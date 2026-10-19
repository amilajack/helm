// SPDX-License-Identifier: GPL-3.0-or-later
import type { WamParameterConfiguration, WamParameterDataMap } from '@webaudiomodules/api';
export const MODULE_ID = 'org.tytel.helm';
export interface Parameter extends WamParameterConfiguration {id:string;label:string;minValue:number;maxValue:number;defaultValue:number;discreteStep:number;nativeStep?:number;display?:{skew:number;offset:number;invert:boolean;multiplier:number;units:string}}
export interface Metadata {parameters:Parameter[];modulationSources:{id:string;destinations:string[]}[]}
export interface Route {source:string;destination:string;amount:number}
export interface PatchInfo {patch_name:string;author:string;folder_name:string;license:string}
export interface State {schemaVersion:1;parameterValues:WamParameterDataMap;modulations:Route[];info:PatchInfo}
export interface Dsp {
  HEAPF32:Float32Array;
  UTF8ToString(pointer:number):string;
  ccall(name:string,result:'number'|'string'|null,types:string[],values:(number|string)[]):number|string;
  _helm_wam_create(rate:number):number;
  _helm_wam_destroy(synth:number):void;
  _helm_wam_callback_begin():void;
  _helm_wam_callback_end():void;
  _helm_wam_prepare_state(synth:number):void;
  _helm_wam_metadata(synth:number):number;
  _helm_wam_set(synth:number,index:number,value:number):number;
  _helm_wam_get(synth:number,index:number):number;
  _helm_wam_clear_routes(synth:number):void;
  _helm_wam_midi(synth:number,status:number,first:number,second:number):void;
  _helm_wam_transport(synth:number,ppq:number,bpm:number):void;
  _helm_wam_render(synth:number,frames:number):number;
  _helm_browser_audio_allocations():number;
  _helm_browser_audio_releases():number;
  _helm_browser_audio_pool_allocations():number;
}
export interface DspOptions {instantiateWasm?:(imports:WebAssembly.Imports,success:(instance:WebAssembly.Instance)=>void)=>WebAssembly.Exports;wasmBinary?:Uint8Array;locateFile?:(name:string)=>string;printErr?:(text:string)=>void;onAbort?:(error:unknown)=>void}
