// SPDX-License-Identifier: GPL-3.0-or-later
import {initializeWamHost} from '@webaudiomodules/sdk';
import Helm from './index.js';
export {initializeWamHost};
const status=document.querySelector<HTMLElement>('#status');
if(status){
 let context:AudioContext|undefined;let group:string|undefined;let loading=false;
 const entry=document.querySelector<HTMLAnchorElement>('#entry')!;entry.href=new URL('../../wam/index.js',import.meta.url).href;entry.textContent=entry.href;
 document.querySelector<HTMLButtonElement>('#load')!.onclick=()=>void(async()=>{
  if(loading)return;loading=true;status.textContent='Loading Helm…';
  try{
   context??=new AudioContext();await context.resume();
   if(!group)[group]=await initializeWamHost(context);
   const plugin=await Helm.createInstance(group!,context);
   plugin.audioNode.connect(context.destination);
   const section=document.createElement('section'),actions=document.createElement('div');
   const note=document.createElement('button');note.textContent='Play C4';note.onclick=()=>{const time=context!.currentTime;plugin.audioNode.scheduleEvents({type:'wam-midi',time,data:{bytes:[0x90,60,100]}},{type:'wam-midi',time:time+.4,data:{bytes:[0x80,60,0]}});};
   const remove=document.createElement('button');remove.textContent='Remove instance';
   const gui=await plugin.createGui();
   remove.onclick=()=>{plugin.destroyGui(gui);plugin.audioNode.disconnect();plugin.audioNode.destroy();section.remove();};
   actions.append(note,remove);section.append(actions,gui);document.querySelector('#plugins')!.append(section);status.textContent='Ready';
  }catch(error){status.textContent=String(error);}finally{loading=false;}
 })();
}
