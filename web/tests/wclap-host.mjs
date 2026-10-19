// SPDX-License-Identifier: GPL-3.0-or-later
// A deliberately small wasm32 WCLAP host for tests. Host callbacks are placed in the
// plugin's function table through one-function forwarding modules, and every
// CLAP structure is laid out by hand for wasm32 (pointers are 4 bytes).
const encoder=new TextEncoder(),decoder=new TextDecoder();
const TYPE={i32:0x7f,i64:0x7e,f64:0x7c};
function leb(n){const out=[];do{let b=n&127;n>>>=7;if(n)b|=128;out.push(b);}while(n);return out;}
function section(id,bytes){return [id,...leb(bytes.length),...bytes];}
function str(s){const b=[...encoder.encode(s)];return [...leb(b.length),...b];}
// Builds a module that imports `env.f` with the given signature and re-exports it,
// turning a JS function into a Wasm function that a funcref table accepts.
function forwarder(params,results,fn){
 const type=[0x60,...leb(params.length),...params.map(p=>TYPE[p]),...leb(results.length),...results.map(r=>TYPE[r])];
 const bytes=new Uint8Array([0,0x61,0x73,0x6d,1,0,0,0,
  ...section(1,[1,...type]),
  ...section(2,[1,...str('env'),...str('f'),0,0]),
  ...section(7,[1,...str('f'),0,0])]);
 return new WebAssembly.Instance(new WebAssembly.Module(bytes),{env:{f:fn}}).exports.f;
}
export const EVENT={NOTE_ON:0,NOTE_OFF:1,PARAM_VALUE:5,MIDI:10};

export class WclapHost{
 constructor(instance){
  this.exports=instance.exports;this.table=instance.exports.__indirect_function_table;
  this.messages=[];this.flushRequests=0;
  this.entry=instance.exports.clap_entry.value;
 }
 get view(){return new DataView(this.exports.memory.buffer);}
 get bytes(){return new Uint8Array(this.exports.memory.buffer);}
 u32(p){return this.view.getUint32(p,true);}
 malloc(size){const p=this.exports.malloc(size);if(!p)throw Error('WCLAP malloc failed');this.bytes.fill(0,p,p+size);return p;}
 cstring(text){const b=encoder.encode(text+'\0');const p=this.malloc(b.length);this.bytes.set(b,p);return p;}
 readString(p){if(!p)return null;const b=this.bytes;let e=p;while(b[e])++e;return decoder.decode(b.slice(p,e));}
 fn(pointer){const f=this.table.get(pointer);if(!f)throw Error('Null CLAP function pointer');return f;}
 add(params,results,js){const index=this.table.grow(1);this.table.set(index,forwarder(params,results,js));return index;}
 init(path){
  const version=[this.u32(this.entry),this.u32(this.entry+4),this.u32(this.entry+8)];
  if(version[0]!==1)throw Error('Unsupported CLAP version '+version.join('.'));
  if(!this.fn(this.u32(this.entry+12))(this.cstring(path)))throw Error('clap_entry.init failed');
  this.factory=this.fn(this.u32(this.entry+20))(this.cstring('clap.plugin-factory'));
  if(!this.factory)throw Error('Missing plugin factory');
  this.#createHost();
 }
 descriptors(){
  const count=this.fn(this.u32(this.factory))(this.factory),result=[];
  for(let i=0;i<count;++i){
   const d=this.fn(this.u32(this.factory+4))(this.factory,i);
   const features=[];for(let f=this.u32(d+44);this.u32(f);f+=4)features.push(this.readString(this.u32(f)));
   result.push({id:this.readString(this.u32(d+12)),name:this.readString(this.u32(d+16)),version:this.readString(this.u32(d+36)),features});
  }
  return result;
 }
 #createHost(){
  const webview=this.malloc(4);
  this.view.setUint32(webview,this.add(['i32','i32','i32'],['i32'],(_host,buffer,size)=>{
   this.messages.push(decoder.decode(this.bytes.slice(buffer,buffer+size)));return 1;}),true);
  const params=this.malloc(12);
  this.view.setUint32(params,this.add(['i32','i32'],[],()=>{}),true);
  this.view.setUint32(params+4,this.add(['i32','i32','i32'],[],()=>{}),true);
  this.view.setUint32(params+8,this.add(['i32'],[],()=>{++this.flushRequests;}),true);
  const state=this.malloc(4);this.view.setUint32(state,this.add(['i32'],[],()=>{}),true);
  const extensions={'clap.webview/3':webview,'clap.params':params,'clap.state':state};
  const host=this.host=this.malloc(48);const v=this.view;
  v.setUint32(host,1,true);v.setUint32(host+4,2,true);v.setUint32(host+8,0,true);
  for(const [offset,text] of [[16,'Helm WCLAP tests'],[20,'Helm'],[24,''],[28,'1']])v.setUint32(host+offset,this.cstring(text),true);
  this.view.setUint32(host+32,this.add(['i32','i32'],['i32'],(_host,id)=>extensions[this.readString(id)]??0),true);
  for(const offset of [36,40,44])this.view.setUint32(host+offset,this.add(['i32'],[],()=>{}),true);
 }
 create(id,sampleRate=48000){
  const plugin=this.fn(this.u32(this.factory+8))(this.factory,this.host,this.cstring(id));
  if(!plugin)throw Error('create_plugin failed');
  const call=(offset,...args)=>this.fn(this.u32(plugin+offset))(plugin,...args);
  if(!call(8))throw Error('plugin.init failed');
  if(!call(16,sampleRate,1,4096)||!call(24))throw Error('plugin activation failed');
  return new WclapPlugin(this,plugin,call);
 }
}

class WclapPlugin{
 constructor(host,pointer,call){
  this.host=host;this.pointer=pointer;this.call=call;
  const h=host;
  this.ext=id=>call(40,h.cstring(id));
  // Reusable process structures: two 256-frame float channels and event lists.
  this.frames=256;this.left=h.malloc(this.frames*4);this.right=h.malloc(this.frames*4);
  this.channels=h.malloc(8);h.view.setUint32(this.channels,this.left,true);h.view.setUint32(this.channels+4,this.right,true);
  this.output=h.malloc(24);h.view.setUint32(this.output,this.channels,true);h.view.setUint32(this.output+8,2,true);
  this.eventStorage=h.malloc(64*48);this.eventPointers=[];
  this.inEvents=h.malloc(12);
  h.view.setUint32(this.inEvents+4,h.add(['i32'],['i32'],()=>this.eventPointers.length),true);
  h.view.setUint32(this.inEvents+8,h.add(['i32','i32'],['i32'],(_list,index)=>this.eventPointers[index]??0),true);
  this.outEvents=h.malloc(8);
  h.view.setUint32(this.outEvents+4,h.add(['i32','i32'],['i32'],()=>1),true);
  this.process=h.malloc(40);
 }
 #event(index,type,time,size){
  const h=this.host,p=this.eventStorage+index*48;h.bytes.fill(0,p,p+48);
  h.view.setUint32(p,size,true);h.view.setUint32(p+4,time,true);h.view.setUint16(p+8,0,true);h.view.setUint16(p+10,type,true);
  return p;
 }
 // events: [{time, type:'note-on'|'note-off', key, velocity} | {time, type:'param', id, value}]
 renderBlock(frames,events=[]){
  if(frames>this.frames)throw Error('Block too large');
  const h=this.host;this.eventPointers=[];
  events.slice().sort((a,b)=>a.time-b.time).forEach((event,i)=>{
   if(event.type==='param'){
    const p=this.#event(i,EVENT.PARAM_VALUE,event.time,48);
    h.view.setUint32(p+16,event.id,true);h.view.setInt32(p+24,-1,true);h.view.setInt16(p+28,-1,true);h.view.setInt16(p+30,-1,true);h.view.setInt16(p+32,-1,true);
    h.view.setFloat64(p+40,event.value,true);this.eventPointers.push(p);
   }else{
    const p=this.#event(i,event.type==='note-on'?EVENT.NOTE_ON:EVENT.NOTE_OFF,event.time,40);
    h.view.setInt32(p+16,-1,true);h.view.setInt16(p+20,0,true);h.view.setInt16(p+22,0,true);h.view.setInt16(p+24,event.key,true);
    h.view.setFloat64(p+32,event.velocity??0.8,true);this.eventPointers.push(p);
   }
  });
  const v=h.view,p=this.process;h.bytes.fill(0,p,p+40);
  v.setUint32(p+8,frames,true);v.setUint32(p+20,this.output,true);v.setUint32(p+28,1,true);
  v.setUint32(p+32,this.inEvents,true);v.setUint32(p+36,this.outEvents,true);
  if(this.call(36,p)!==1)throw Error('process failed');
  return [new Float32Array(h.exports.memory.buffer,this.left,frames).slice(),new Float32Array(h.exports.memory.buffer,this.right,frames).slice()];
 }
 render(frames,events=[]){
  const left=new Float32Array(frames),right=new Float32Array(frames);
  for(let start=0;start<frames;start+=this.frames){
   const count=Math.min(this.frames,frames-start);
   const block=events.filter(e=>e.time>=start&&e.time<start+count).map(e=>({...e,time:e.time-start}));
   const [l,r]=this.renderBlock(count,block);left.set(l,start);right.set(r,start);
  }
  return [left,right];
 }
 send(message){
  const h=this.host,webview=this.ext('clap.webview/3');if(!webview)throw Error('No webview extension');
  const bytes=encoder.encode(JSON.stringify(message)),p=h.malloc(bytes.length+1);h.bytes.set(bytes,p);
  h.messages=[];const ok=h.fn(h.u32(webview+8))(this.pointer,p,bytes.length);h.exports.free(p);
  if(!ok)throw Error('Webview message rejected: '+JSON.stringify(message));
  return h.messages.map(m=>JSON.parse(m));
 }
 resource(path){
  const h=this.host,webview=this.ext('clap.webview/3');const chunks=[];
  const stream=h.malloc(8);
  h.view.setUint32(stream+4,h.add(['i32','i32','i64'],['i64'],(_s,buffer,size)=>{chunks.push(h.bytes.slice(buffer,buffer+Number(size)));return size;}),true);
  const mime=h.malloc(128);
  const ok=h.fn(h.u32(webview+4))(this.pointer,h.cstring(path),mime,128,stream);
  if(!ok)return null;
  const data=new Uint8Array(chunks.reduce((n,c)=>n+c.length,0));let at=0;for(const c of chunks){data.set(c,at);at+=c.length;}
  return {mime:h.readString(mime),data};
 }
 save(){
  const h=this.host,state=this.ext('clap.state');const chunks=[];
  const stream=h.malloc(8);
  h.view.setUint32(stream+4,h.add(['i32','i32','i64'],['i64'],(_s,buffer,size)=>{chunks.push(h.bytes.slice(buffer,buffer+Number(size)));return size;}),true);
  if(!h.fn(h.u32(state))(this.pointer,stream))throw Error('state.save failed');
  return decoder.decode(Buffer.concat(chunks));
 }
 load(text){
  const h=this.host,state=this.ext('clap.state'),bytes=encoder.encode(text);let at=0;
  const stream=h.malloc(8);
  h.view.setUint32(stream+4,h.add(['i32','i32','i64'],['i64'],(_s,buffer,size)=>{
   const count=Math.min(Number(size),bytes.length-at,7);h.bytes.set(bytes.subarray(at,at+count),buffer);at+=count;return BigInt(count);}),true);
  return !!h.fn(h.u32(state+4))(this.pointer,stream);
 }
 paramValue(id){const h=this.host,params=this.ext('clap.params'),out=h.malloc(8);
  if(!h.fn(h.u32(params+8))(this.pointer,id,out))throw Error('get_value failed');return h.view.getFloat64(out,true);}
 destroy(){this.call(28);this.call(20);this.call(12);}
}
