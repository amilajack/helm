// SPDX-License-Identifier: GPL-3.0-or-later
import type {WamParameterDataMap} from '@webaudiomodules/api';
import type {Metadata,PatchInfo,Route,State} from './types';
const record=(value:unknown):value is Record<string,unknown>=>!!value&&typeof value==='object'&&!Array.isArray(value);
const older=(version:string,reference:string)=>{
 const a=version.split('.').map(Number),b=reference.split('.').map(Number);
 for(let i=0;i<Math.max(a.length,b.length);++i){const difference=(a[i]||0)-(b[i]||0);if(difference)return difference<0;}
 return true;
};
export function parameters(input:unknown,metadata:Metadata):WamParameterDataMap {
 if(!record(input))throw Error('Expected a parameter map');
 const result:WamParameterDataMap={};const known=new Map(metadata.parameters.map(p=>[p.id,p]));
 for(const [id,entry]of Object.entries(input)){
  const info=known.get(id);
  if(!info||!record(entry)||entry.id!==id||typeof entry.value!=='number'||!Number.isFinite(entry.value))throw Error('Invalid parameter: '+id);
  const normalized=entry.normalized===true;
  if(entry.normalized!==undefined&&typeof entry.normalized!=='boolean')throw Error('Invalid normalization flag');
  const value=normalized?info.minValue+entry.value*(info.maxValue-info.minValue):entry.value;
  if(value<info.minValue||value>info.maxValue)throw Error('Parameter outside range: '+id);
  if(info.discreteStep&&Math.abs(value-Math.round(value))>1e-6)throw Error('Expected an integer: '+id);
  result[id]={id,value:info.discreteStep?Math.round(value):value,normalized:false};
 }
 return result;
}
export function routes(input:unknown,metadata:Metadata):Route[] {
 if(!Array.isArray(input)||input.length>256)throw Error('Invalid modulation list');
 const sources=new Map(metadata.modulationSources.map(source=>[source.id,new Set(source.destinations)]));
 const seen=new Set<string>();
 return input.map(route=>{
  if(!record(route)||typeof route.source!=='string'||typeof route.destination!=='string'||typeof route.amount!=='number'||!Number.isFinite(route.amount)||Math.abs(route.amount)>1e6||!sources.get(route.source)?.has(route.destination))throw Error('Invalid modulation route');
  const key=route.source+'\0'+route.destination;if(seen.has(key))throw Error('Duplicate modulation route');seen.add(key);
  return {source:route.source,destination:route.destination,amount:route.amount};
 });
}
function patchInfo(input:Record<string,unknown>):PatchInfo {
 const result={} as PatchInfo;
 for(const key of ['patch_name','author','folder_name','license'] as const){const value=input[key]??'';if(typeof value!=='string'||value.length>4096)throw Error('Invalid patch information');result[key]=value;}
 return result;
}
// These transforms follow LoadSave::varToState, including the old oscillator,
// filter, distortion, wet/dry and BPM representations. Missing legacy fields
// convert to zero there; current missing controls use their normal defaults.
export function readPatch(input:unknown,metadata:Metadata):State {
 if(!record(input))throw Error('Invalid Helm patch');
 const version=input.synth_version??'0.4.1';
 if(typeof version!=='string'||!/^\d+(\.\d+){1,3}$/.test(version))throw Error('Invalid synth version');
 const raw=older(version,'0.4.1')?input:input.settings;
 if(!record(raw)||!Array.isArray(raw.modulations))throw Error('Invalid patch settings');
 const settings={...raw};let modulations=raw.modulations.map(route=>{
  if(!record(route))throw Error('Invalid modulation');return {...route};
 });
 const number=(name:string)=>{const value=settings[name]??0;if(typeof value!=='number'&&typeof value!=='boolean')throw Error('Invalid patch value');const result=Number(value);if(!Number.isFinite(result)||Math.abs(result)>1e6)throw Error('Invalid patch number');return result;};
 if(older(version,'0.5.0')){
  if('osc_mix'in settings){const mix=number('osc_mix');settings.osc_1_volume=Math.sqrt(1-mix);settings.osc_2_volume=Math.sqrt(mix);delete settings.osc_mix;}
  modulations=modulations.flatMap(route=>route.destination==='osc_mix'?[{...route,destination:'osc_1_volume',amount:-Number(route.amount)},{...route,destination:'osc_2_volume',amount:Number(route.amount)}]:[route]);
 }
 if(older(version,'0.7.2')&&number('stutter_on')){settings.stutter_resample_sync=0;settings.stutter_sync=0;}
 if(older(version,'0.8.6')){
  for(const oscillator of [1,2]){const id='osc_'+oscillator+'_volume',voices=number('osc_'+oscillator+'_unison_voices');settings[id]=Math.sqrt(number(id)**2*Math.sqrt(.5/((voices+1)/2)));}
  settings.sub_volume=Math.sqrt(.5)*number('sub_volume');
  if(older(version,'0.5.0')){settings.sub_octave=1;settings.cutoff=number('cutoff')-number('keytrack')*12;}
  const filter=number('filter_type');
  if(filter>=6)settings.filter_on=0;
  else if(filter>=3){settings.filter_shelf=filter>=5?1:filter>=4?2:0;settings.filter_on=1;settings.filter_style=2;}
  else{if(filter>=2)settings.filter_blend=1;else if(filter>=1)settings.filter_blend=2;settings.filter_on=1;settings.filter_style=0;}
  settings.distortion_on=1;settings.distortion_type=0;settings.distortion_mix=1;
  settings.distortion_drive=number('filter_saturation')+(filter>=6?0:12);
  if(filter<6)settings.filter_drive=-12;
  modulations=modulations.map(route=>route.destination==='filter_saturation'?{...route,destination:'distortion_drive'}:route);
  let volume=number('volume');
  for(const effect of ['delay','reverb']){const wet=number(effect+'_dry_wet');if(number(effect+'_on')&&wet!==0&&wet!==1){const ratio=wet/(1-wet),next=1-1/(1+ratio*ratio);settings[effect+'_dry_wet']=next;volume*=Math.sqrt(wet/Math.sqrt(next));}}
  settings.volume=volume;settings.beats_per_minute=number('beats_per_minute')/60;
 }
 const values:WamParameterDataMap={};
 for(const info of metadata.parameters)values[info.id]={id:info.id,value:info.id in settings?number(info.id):info.defaultValue,normalized:false};
 return {schemaVersion:1,parameterValues:parameters(values,metadata),modulations:routes(modulations,metadata),info:patchInfo(input)};
}
export function state(input:unknown,metadata:Metadata):State {
 if(!record(input))throw Error('Invalid WAM state');
 if('settings'in input||'synth_version'in input)return readPatch(input,metadata);
 if(input.schemaVersion!==1||!record(input.info))throw Error('Unsupported Helm WAM state');
 const values=parameters(input.parameterValues,metadata);
 if(Object.keys(values).length!==metadata.parameters.length)throw Error('Incomplete Helm WAM state');
 return {schemaVersion:1,parameterValues:values,modulations:routes(input.modulations,metadata),info:patchInfo(input.info)};
}
export function toPatch(value:State){
 return {synth_version:'0.9.0',...value.info,settings:{...Object.fromEntries(Object.entries(value.parameterValues).map(([id,p])=>[id,p.value])),modulations:value.modulations}};
}
