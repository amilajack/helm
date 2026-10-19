// SPDX-License-Identifier: GPL-3.0-or-later
import {WebAudioModule,WamNode} from '@webaudiomodules/sdk';
import type {WamEvent,WamParameterDataMap,WamParameterInfoMap} from '@webaudiomodules/api';
import {MODULE_ID,type Metadata,type State} from './types';
import {parameters,state as validateState} from './state';
const base=new URL('./',import.meta.url);
const registrations=new WeakMap<BaseAudioContext,Promise<void>>();
let assets:Promise<{metadata:Metadata;wasmModule:WebAssembly.Module}>|undefined;
async function loadAssets(){
 return assets??=(async()=>{
  const [description,binary]=await Promise.all([fetch(new URL('parameters.json',base)),fetch(new URL('dsp.wasm',base))]);
  if(!description.ok||!binary.ok)throw Error('Unable to download Helm WAM assets');
  return {metadata:await description.json() as Metadata,wasmModule:await WebAssembly.compile(await binary.arrayBuffer())};
 })().catch(error=>{assets=undefined;throw error;});
}
export class HelmNode extends WamNode {
 private metadata:Metadata;
 private rejects=new Map<number,(error:Error)=>void>();
 private timers=new Map<number,ReturnType<typeof setTimeout>>();
 private resolveReady!:()=>void;
 private rejectReady!:(error:Error)=>void;
 private readyTimer:ReturnType<typeof setTimeout>;
 private failure?:Error;
 readonly ready:Promise<void>;
 constructor(module:Helm,metadata:Metadata,wasmModule:WebAssembly.Module){
  super(module,{numberOfInputs:0,numberOfOutputs:1,outputChannelCount:[2],processorOptions:{metadata,wasmModule,useSab:false}});
  this.metadata=metadata;
  this.ready=new Promise((resolve,reject)=>{this.resolveReady=resolve;this.rejectReady=reject;});
  this.readyTimer=setTimeout(()=>this.fail(Error('Helm DSP initialization timed out')),30000);
  AudioWorkletNode.prototype.addEventListener.call(this,'processorerror',()=>this.fail(new Error('Helm audio processor stopped')));
 }
 private fail(error:Error){this.failure=error;clearTimeout(this.readyTimer);this.rejectReady(error);for(const reject of this.rejects.values())reject(error);this.rejects.clear();for(const timer of this.timers.values())clearTimeout(timer);this.timers.clear();}
 private rpc<T>(request:string,content?:unknown):Promise<T>{
  if(this._destroyed)return Promise.reject(Error('Helm has been destroyed'));
  if(this.failure)return Promise.reject(this.failure);
  const id=this._generateMessageId();
  return new Promise((resolve,reject)=>{
   const cleanup=()=>{clearTimeout(this.timers.get(id));this.timers.delete(id);this.rejects.delete(id);delete this._pendingResponses[id];};
   this._pendingResponses[id]=(value:T)=>{cleanup();resolve(value);};
   this.rejects.set(id,error=>{cleanup();reject(error);});
   this.timers.set(id,setTimeout(()=>this.rejects.get(id)?.(Error('Helm processor request timed out')),30000));
   this.port.postMessage({id,request,content});
  });
 }
 override _onMessage(message:MessageEvent){
  if(message.data.helmReady){clearTimeout(this.readyTimer);this.resolveReady();return;}
  if(message.data.helmError){this.fail(Error(message.data.helmError));return;}
  if(message.data.error){this.rejects.get(message.data.id)?.(Error(message.data.error));return;}
  super._onMessage(message);
 }
 override _initialize(){return this.rpc<void>('initialize/processor');}
 override getParameterInfo(...ids:string[]){return this.rpc<WamParameterInfoMap>('get/parameterInfo',{parameterIds:ids});}
 override getParameterValues(normalized=false,...ids:string[]){return this.rpc<WamParameterDataMap>('get/parameterValues',{normalized,parameterIds:ids});}
 override async setParameterValues(input:WamParameterDataMap){return this.rpc<void>('set/parameterValues',{parameterValues:parameters(input,this.metadata)});}
 override getState(){return this.rpc<State>('get/state');}
 override async setState(input:unknown){const state=validateState(input,this.metadata);return this.rpc<void>('set/state',{state});}
 getStats(){return this.rpc<{allocations:number;releases:number;pool:number}>('helm/stats');}
 override scheduleEvents(...events:WamEvent[]){
  if(this._destroyed)return;
  for(const event of events){
   if(event.time!==undefined&&(!Number.isFinite(event.time)||event.time<0))throw Error('Invalid WAM event timestamp');
   if(event.type==='wam-automation')parameters({[event.data.id]:event.data},this.metadata);
   if(event.type==='wam-midi'||event.type==='wam-mpe'){
    const b=event.data.bytes;if(b.length<2||b.length>3||b.some((v,i)=>!Number.isInteger(v)||v<0||v>(i?127:239))||b[0]<128)throw Error('Invalid MIDI event');
   }
  }
  super.scheduleEvents(...events);
 }
 override destroy(){if(this._destroyed)return;this.fail(Error('Helm has been destroyed'));super.destroy();}
}
export default class Helm extends WebAudioModule<HelmNode> {
 private readonly uniqueId=MODULE_ID+'-'+crypto.randomUUID();
 private guis=new Set<HTMLElement>();
 override get instanceId(){return this.uniqueId;}
 override async initialize(initialState?:unknown){
  this._descriptorUrl=new URL('descriptor.json',base).href;
  await this._loadDescriptor();
  this.audioNode=await this.createAudioNode(initialState);this.initialized=true;return this;
 }
 override async createAudioNode(initialState?:unknown){
  const asset=await loadAssets();
  let registered=registrations.get(this.audioContext);
  if(!registered){registered=(async()=>{await WamNode.addModules(this.audioContext,MODULE_ID);await this.audioContext.audioWorklet.addModule(new URL('processor.js',base).href);})().catch(error=>{registrations.delete(this.audioContext);throw error;});registrations.set(this.audioContext,registered);}
  await registered;
  const node=new HelmNode(this,asset.metadata,asset.wasmModule);
  try{await node.ready;await node._initialize();if(initialState!==undefined)await node.setState(initialState);return node;}
  catch(error){node.destroy();throw error;}
 }
 override async createGui(){
  const {createGui}=await import('./gui.js');const node=this.audioNode,context=this.audioContext;
  const gui=await createGui({
   automate:(id,value)=>node.scheduleEvents({type:'wam-automation',time:context.currentTime,data:{id,value,normalized:false}}),
   midi:bytes=>node.scheduleEvents({type:'wam-midi',time:context.currentTime,data:{bytes:bytes as [number,number,number]}}),
   getState:()=>node.getState(),setState:state=>node.setState(state),destroyed:()=>node._destroyed,
  },(await loadAssets()).metadata,{library:new URL('../library/',base),source:new URL('../source.tar.gz',base)});
  this.guis.add(gui);return gui;
 }
 override destroyGui(gui:HTMLElement){gui.dispatchEvent(new Event('helm-destroy'));gui.remove();this.guis.delete(gui);}
}
