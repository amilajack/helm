// SPDX-License-Identifier: GPL-3.0-or-later
import getWamProcessor from '@webaudiomodules/sdk/src/WamProcessor.js';
import getWamParameterInfo from '@webaudiomodules/sdk/src/WamParameterInfo.js';
import type {WamMidiData,WamParameterData,WamParameterInfoMap,WamTransportData} from '@webaudiomodules/api';
import createDsp from './dsp.mjs';
import {state as validateState} from './state';
import {MODULE_ID,type Dsp,type Metadata,type State} from './types';
declare const sampleRate:number;
declare const currentTime:number;
declare function registerProcessor(name:string,processor:typeof HelmProcessor):void;
const Base=getWamProcessor(MODULE_ID),ParameterInfo=getWamParameterInfo(MODULE_ID);
class HelmProcessor extends Base {
 private metadata:Metadata;
 private indexes:Map<string,number>;
 private dsp?:Dsp;
 private synth=0;
 private ready:Promise<void>;
 private saved:Pick<State,'modulations'|'info'>={modulations:[],info:{patch_name:'init',author:'',folder_name:'',license:''}};
 private transport?:WamTransportData;
 private resync=false;
 constructor(options:AudioWorkletNodeOptions){
  super(options);
  const bootstrap=options.processorOptions as {metadata:Metadata;wasmModule:WebAssembly.Module};
  this.metadata=bootstrap.metadata;
  this.indexes=new Map(this.metadata.parameters.map((p,i)=>[p.id,i]));
  this.ready=createDsp({instantiateWasm(imports,success){const instance=new WebAssembly.Instance(bootstrap.wasmModule,imports);success(instance);return instance.exports;},printErr:message=>console.error(message)})
   .then(dsp=>{if(this._destroyed)return;this.dsp=dsp;this.synth=dsp._helm_wam_create(sampleRate);if(!this.synth)throw Error('Helm DSP initialization failed');this.port.postMessage({helmReady:true});})
   .catch(error=>{this.port.postMessage({helmError:String(error)});throw error;});
  // Initialization errors are reported to the node, even before its RPC starts.
  void this.ready.catch(()=>{});
 }
 override _generateWamParameterInfo():WamParameterInfoMap {
  return Object.fromEntries(this.metadata.parameters.map(info=>[info.id,new ParameterInfo(info.id,info)]));
 }
 override async _onMessage(message:MessageEvent){
  const request=message.data?.request;
  try{
   if(request==='initialize/processor')await this.ready;
   if(this._destroyed)throw Error('Helm has been destroyed');
   if(request==='helm/stats'){
    this.port.postMessage({id:message.data.id,response:request,content:{allocations:this.dsp?._helm_browser_audio_allocations()??0,releases:this.dsp?._helm_browser_audio_releases()??0,pool:this.dsp?._helm_browser_audio_pool_allocations()??0}});return;
   }
   await super._onMessage(message);
  }catch(error){this.port.postMessage({id:message.data?.id,response:request,error:String(error)});}
 }
 override _setParameterValue(update:WamParameterData,_interpolate:boolean){
  const info=this._parameterInfo[update.id];if(!info||!Number.isFinite(update.value))return;
  let value=update.normalized?info.denormalize(update.value):update.value;
  value=Math.max(info.minValue,Math.min(info.maxValue,value));
  if(info.discreteStep)value=Math.round(value);
  // Use timestamped WAM processing slices and the synth's native smoothing.
  // A parameter event takes effect at its sample offset, without an extra ramp.
  super._setParameterValue({id:update.id,value,normalized:false},false);
  this.dsp?._helm_wam_set(this.synth,this.indexes.get(update.id)!,value);
 }
 override _interpolateParameterValues(_start:number,_end:number){}
 override _setState(input:unknown){
  const next=validateState(input,this.metadata);
  this.dsp!._helm_wam_clear_routes(this.synth);
  for(const route of next.modulations){
   const ok=this.dsp!.ccall('helm_wam_route','number',['number','string','string','number'],[this.synth,route.source,route.destination,route.amount]);
   if(!ok)throw Error('Unable to construct modulation route');
  }
  this._setParameterValues(next.parameterValues,false);
  this.saved={modulations:next.modulations,info:next.info};
  // Adopt arp/routing changes before accepting the next performance event.
  this.dsp!._helm_wam_prepare_state(this.synth);
 }
 override _getState():State{return {schemaVersion:1,parameterValues:this._getParameterValues(false),...this.saved};}
 override _onMidi(data:WamMidiData){const b=data.bytes;this.dsp?._helm_wam_midi(this.synth,b[0],b[1],b[2]??0);}
 override _onMpe(data:WamMidiData){this._onMidi(data);}
 override _onTransport(data:WamTransportData){
  if(!Number.isFinite(data.tempo)||data.tempo<20||data.tempo>300||!Number.isFinite(data.currentBar)||!Number.isFinite(data.currentBarStarted)||!Number.isInteger(data.timeSigNumerator)||data.timeSigNumerator<1||data.timeSigNumerator>32||![1,2,4,8,16,32].includes(data.timeSigDenominator))return;
  this.transport={...data};this.resync=true;
  this._setParameterValue({id:'beats_per_minute',value:data.tempo/60,normalized:false},false);
 }
 override _process(start:number,end:number,_inputs:Float32Array[][],outputs:Float32Array[][]){
  const out=outputs[0],dsp=this.dsp;if(!dsp||!this.synth||!out?.[0]||!out[1])return;
  const transport=this.transport;
  if(transport&&(transport.playing||this.resync)){
   const elapsed=currentTime+start/sampleRate-transport.currentBarStarted;
   const ppq=transport.currentBar*transport.timeSigNumerator*4/transport.timeSigDenominator+elapsed*transport.tempo/60;
   dsp._helm_wam_transport(this.synth,ppq,transport.tempo);this.resync=false;
  }
  const pointer=dsp._helm_wam_render(this.synth,end-start)/4;
  for(let i=0;i<end-start;++i){out[0][start+i]=dsp.HEAPF32[pointer+i]!;out[1][start+i]=dsp.HEAPF32[pointer+256+i]!;}
 }
 override process(inputs:Float32Array[][],outputs:Float32Array[][],parameters:Record<string,Float32Array>){
  if(this._destroyed)return false;
  if(!this.dsp||!this.synth)return true;
  this.dsp._helm_wam_callback_begin();
  try{return super.process(inputs,outputs,parameters);}
  finally{this.dsp._helm_wam_callback_end();}
 }
 override destroy(){if(this.synth){this.dsp!._helm_wam_destroy(this.synth);this.synth=0;}super.destroy();}
}
registerProcessor(MODULE_ID,HelmProcessor);
