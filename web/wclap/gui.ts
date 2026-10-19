// SPDX-License-Identifier: GPL-3.0-or-later
// Helm's GUI inside a CLAP webview (clap.webview/3). Messages are UTF-8 JSON in
// ArrayBuffers: received as MessageEvents, sent with window.parent.postMessage.
import {createGui,type HelmController} from '../wam/gui';
import type {Metadata,State} from '../wam/types';
type Reply={type:string;rid?:number;state?:State;metadata?:Metadata;message?:string};
const encoder=new TextEncoder(),decoder=new TextDecoder();
const pending=new Map<number,{resolve:(reply:Reply)=>void;reject:(error:Error)=>void;timer:ReturnType<typeof setTimeout>}>();
let nextId=1,closed=false;
function post(message:object){
 const bytes=encoder.encode(JSON.stringify(message));
 window.parent.postMessage(bytes.buffer,'*');
}
function request(message:object):Promise<Reply>{
 if(closed)return Promise.reject(Error('Helm has been closed'));
 const rid=nextId++;
 return new Promise((resolve,reject)=>{
  const timer=setTimeout(()=>{pending.delete(rid);reject(Error('Helm plugin request timed out'));},30000);
  pending.set(rid,{resolve,reject,timer});
  post({...message,rid});
 });
}
function decode(data:unknown):Reply|undefined{
 let text:string;
 if(data instanceof ArrayBuffer)text=decoder.decode(data);
 else if(ArrayBuffer.isView(data))text=decoder.decode(data);
 else if(typeof data==='string')text=data;
 else return;
 try{const value=JSON.parse(text);return value&&typeof value==='object'&&typeof value.type==='string'?value as Reply:undefined;}
 catch{return;}
}
window.addEventListener('message',event=>{
 const reply=decode(event.data);if(!reply||typeof reply.rid!=='number')return;
 const entry=pending.get(reply.rid);if(!entry)return;
 pending.delete(reply.rid);clearTimeout(entry.timer);
 if(reply.type==='error')entry.reject(Error(reply.message||'Helm plugin error'));else entry.resolve(reply);
});
window.addEventListener('pagehide',()=>{closed=true;for(const entry of pending.values()){clearTimeout(entry.timer);entry.reject(Error('Helm has been closed'));}pending.clear();});
const controller:HelmController={
 automate:(id,value)=>post({type:'param',id,value}),
 gesture:(id,begin)=>post({type:'gesture',id,begin}),
 midi:bytes=>post({type:'midi',bytes}),
 getState:async()=>{const reply=await request({type:'getState'});if(!reply.state)throw Error('Missing Helm state');return reply.state;},
 setState:async state=>{await request({type:'setState',state});},
 destroyed:()=>closed,
};
async function start(){
 const status=document.getElementById('status')!;
 try{
  const reply=await request({type:'metadata'});
  if(!reply.metadata)throw Error('Missing Helm parameter metadata');
  const gui=await createGui(controller,reply.metadata,{library:new URL('library/',location.href),source:new URL('https://github.com/amilajack/helm'),label:'Helm CLAP instrument'});
  gui.style.width='100%';status.replaceWith(gui);gui.focus();
  window.addEventListener('pagehide',()=>gui.dispatchEvent(new Event('helm-destroy')));
 }catch(error){status.textContent='Helm could not start: '+(error instanceof Error?error.message:String(error));}
}
void start();
