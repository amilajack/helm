// SPDX-License-Identifier: GPL-3.0-or-later
import type {HelmNode} from './index';
import type {Metadata,Parameter,State} from './types';
import {readPatch,toPatch} from './state';
function format(value:number,parameter:Parameter){
 const display=parameter.display;if(!display)return value.toPrecision(5);
 let shown=display.skew===1?value*value:display.skew===2?Math.sqrt(value):display.skew===3?2**value:value;
 shown+=display.offset;if(display.invert)shown=1/shown;shown*=display.multiplier;
 return (Number.isFinite(shown)?Number(shown.toPrecision(5)).toString():'∞')+(display.units?' '+display.units:'');
}
export async function createGui(node:HelmNode,context:BaseAudioContext,metadata:Metadata,base:URL){
 const element=document.createElement('section');element.tabIndex=0;element.setAttribute('aria-label','Helm WAM instrument');
 const root=element.attachShadow({mode:'open'});
 root.innerHTML=`<style>
 :host{display:block;width:760px;max-width:100%;background:#202328;color:#eee;font:14px system-ui;padding:16px;box-sizing:border-box}.parameter[hidden]{display:none}
 header{display:flex;gap:10px;align-items:center;flex-wrap:wrap}h2{margin:0;color:#03a9f4}button,input,select{font:inherit}button{cursor:pointer}label{display:flex;gap:8px;align-items:center}section{margin-top:14px}#controls{display:grid;grid-template-columns:repeat(auto-fit,minmax(215px,1fr));gap:10px;max-height:410px;overflow:auto;padding:6px 0}.parameter{display:grid;grid-template-columns:1fr auto;gap:4px}.parameter input{grid-column:1/-1;width:95%}output{font-variant-numeric:tabular-nums;color:#00e676}#keyboard{display:flex;gap:3px}#keyboard button{min-width:29px;min-height:38px}#routes button{margin:3px}#status{min-height:1.4em;color:#ffca80}a{color:#03a9f4}
 </style><header><h2>Helm</h2><span id="patch">init</span><button id="init">Init patch</button><button id="import">Import .helm</button><button id="export">Export .helm</button></header><section><label>Factory preset <select id="presets"><option value="">Choose a preset…</option></select></label><input id="file" type="file" accept=".helm" hidden></section><section id="keyboard" aria-label="Keyboard"></section><section><label>Find parameter <input id="search" type="search" placeholder="Filter parameters"></label><div id="controls"></div></section><section><h3>Modulation routing</h3><label>Source <select id="source"></select></label><label>Destination <select id="destination"></select></label><label>Amount <input id="amount" type="number" min="-1000000" max="1000000" step="any" value="12"></label><button id="add-route">Set route</button><div id="routes"></div></section><p id="status" role="status"></p><a href="../source.tar.gz">Source &amp; licenses</a>`;
 root.querySelector<HTMLAnchorElement>('a')!.href=new URL('../source.tar.gz',base).href;
 const find=<T extends HTMLElement>(selector:string)=>root.querySelector<T>(selector)!;
 const status=find<HTMLParagraphElement>('#status');
 const report=(error:unknown)=>status.textContent=String(error instanceof Error?error.message:error);
 let closed=false,generation=0,busy=false;
 const inputs=new Map<string,{input:HTMLInputElement;output:HTMLOutputElement;row:HTMLLabelElement}>();
 for(const parameter of metadata.parameters){
  const row=document.createElement('label');row.className='parameter';
  const text=document.createElement('span');text.textContent=parameter.label||parameter.id;text.title=parameter.id;
  const output=document.createElement('output');
  const input=document.createElement('input');input.type='range';input.min=String(parameter.minValue);input.max=String(parameter.maxValue);input.step=parameter.nativeStep?'1':'any';input.value=String(parameter.defaultValue);input.setAttribute('aria-label',parameter.id);
  output.textContent=format(Number(input.value),parameter);input.setAttribute('aria-valuetext',output.textContent);
  input.oninput=()=>{++generation;const value=input.valueAsNumber;output.textContent=format(value,parameter);input.setAttribute('aria-valuetext',output.textContent);try{node.scheduleEvents({type:'wam-automation',time:context.currentTime,data:{id:parameter.id,value,normalized:false}});}catch(error){report(error);}};
  row.append(text,output,input);find('#controls').append(row);inputs.set(parameter.id,{input,output,row});
 }
 find<HTMLInputElement>('#search').oninput=event=>{const text=(event.target as HTMLInputElement).value.toLowerCase();for(const [id,entry]of inputs)entry.row.hidden=!id.includes(text)&&!entry.row.textContent?.toLowerCase().includes(text);};
 const stop=()=>{for(const note of pressed)send(0x80,note,0);pressed.clear();send(0xb0,123,0);};
 const pressed=new Set<number>();
 const send=(statusByte:number,note:number,velocity:number)=>{if(!closed&&!node._destroyed)node.scheduleEvents({type:'wam-midi',time:context.currentTime,data:{bytes:[statusByte,note,velocity]}});};
 const names=['C','C♯','D','D♯','E','F','F♯','G','G♯','A','A♯','B','C'];
 for(let i=0;i<13;++i){const note=60+i,button=document.createElement('button');button.textContent=names[i]!;button.setAttribute('aria-label','Play '+names[i]+(i===12?'5':'4'));button.onpointerdown=event=>{button.setPointerCapture(event.pointerId);pressed.add(note);send(0x90,note,100);};const release=()=>{if(pressed.delete(note))send(0x80,note,0);};button.onpointerup=release;button.onpointercancel=release;find('#keyboard').append(button);}
 const keys='awsedftgyhujk';
 root.addEventListener('keydown',event=>{const e=event as KeyboardEvent;if(e.repeat||e.ctrlKey||e.metaKey||e.altKey||(e.target as HTMLElement).matches('input,select,textarea'))return;const index=keys.indexOf(e.key.toLowerCase());if(index>=0){e.preventDefault();pressed.add(60+index);send(0x90,60+index,100);}});
 root.addEventListener('keyup',event=>{const e=event as KeyboardEvent;const index=keys.indexOf(e.key.toLowerCase());if(index>=0&&pressed.delete(60+index)){e.preventDefault();send(0x80,60+index,0);}});
 element.addEventListener('focusout',event=>{if(!element.contains(event.relatedTarget as Node))stop();});
 let routeSignature='';
 const display=(state:State)=>{
  find('#patch').textContent=state.info.patch_name||'init';
  for(const [id,value]of Object.entries(state.parameterValues)){const control=inputs.get(id);if(control&&root.activeElement!==control.input){control.input.value=String(value.value);control.output.textContent=format(value.value,metadata.parameters.find(p=>p.id===id)!);control.input.setAttribute('aria-valuetext',control.output.textContent);}}
  const signature=JSON.stringify(state.modulations);if(signature===routeSignature)return;routeSignature=signature;
  const routes=find('#routes');routes.replaceChildren();
  for(const route of state.modulations){const remove=document.createElement('button');remove.textContent=route.source+' → '+route.destination+' ('+route.amount+') ×';remove.onclick=()=>void node.getState().then(current=>node.setState({...current,modulations:current.modulations.filter(r=>r.source!==route.source||r.destination!==route.destination)})).then(refresh).catch(report);routes.append(remove);}
 };
 const refresh=async()=>{if(closed||busy||node._destroyed)return;busy=true;try{display(await node.getState());}catch(error){report(error);}finally{busy=false;}};
 const timer=setInterval(()=>void refresh(),250);void refresh();
 const source=find<HTMLSelectElement>('#source'),destination=find<HTMLSelectElement>('#destination');
 for(const mod of metadata.modulationSources)source.add(new Option(mod.id,mod.id));
 const destinations=()=>{destination.replaceChildren();for(const id of metadata.modulationSources.find(mod=>mod.id===source.value)!.destinations)destination.add(new Option(id,id));};source.onchange=destinations;destinations();
 find<HTMLButtonElement>('#add-route').onclick=()=>{++generation;const amount=find<HTMLInputElement>('#amount').valueAsNumber;void node.getState().then(current=>node.setState({...current,modulations:[...current.modulations.filter(r=>r.source!==source.value||r.destination!==destination.value),{source:source.value,destination:destination.value,amount}]})).then(refresh).catch(report);};
 find<HTMLButtonElement>('#init').onclick=()=>{++generation;stop();void node.setState({schemaVersion:1,parameterValues:Object.fromEntries(metadata.parameters.map(p=>[p.id,{id:p.id,value:p.defaultValue,normalized:false}])),modulations:[],info:{patch_name:'init',author:'',folder_name:'',license:''}}).then(refresh).catch(report);};
 const picker=find<HTMLInputElement>('#file');find<HTMLButtonElement>('#import').onclick=()=>picker.click();
 picker.onchange=()=>{const file=picker.files?.[0];if(!file)return;const token=++generation;void (async()=>{if(file.size>1048576)throw Error('Patch exceeds 1 MiB');const patch=readPatch(JSON.parse(await file.text()),metadata);if(token!==generation||closed)return;await node.setState(patch);status.textContent='Loaded '+file.name;await refresh();})().catch(report);picker.value='';};
 find<HTMLButtonElement>('#export').onclick=()=>void node.getState().then(state=>{const url=URL.createObjectURL(new Blob([JSON.stringify(toPatch(state),null,2)],{type:'application/json'}));const link=document.createElement('a');link.href=url;link.download=(state.info.patch_name||'Helm')+'.helm';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}).catch(report);
 const presets=find<HTMLSelectElement>('#presets');
 type Entry={path:string;sha256:string;size:number};let catalog:Entry[]=[];
 void fetch(new URL('../library/manifest.json',base)).then(response=>{if(!response.ok)throw Error('Preset catalog unavailable');return response.json();}).then(data=>{catalog=data.patches;for(const entry of catalog)presets.add(new Option(entry.path.replace(/\.helm$/,''),entry.sha256+':'+entry.path));}).catch(report);
 presets.onchange=()=>{const entry=catalog.find(p=>p.sha256+':'+p.path===presets.value);if(!entry)return;const token=++generation;status.textContent='Loading preset…';void (async()=>{const response=await fetch(new URL('../library/objects/'+entry.sha256,base));if(!response.ok)throw Error('Preset download failed');const bytes=await response.arrayBuffer();const digest=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),n=>n.toString(16).padStart(2,'0')).join('');if(bytes.byteLength!==entry.size||digest!==entry.sha256)throw Error('Preset integrity check failed');const patch=readPatch(JSON.parse(new TextDecoder().decode(bytes)),metadata);if(token!==generation||closed)return;await node.setState(patch);status.textContent='Loaded '+entry.path;await refresh();})().catch(report);};
 element.addEventListener('helm-destroy',()=>{if(closed)return;stop();closed=true;++generation;clearInterval(timer);});
 return element;
}
