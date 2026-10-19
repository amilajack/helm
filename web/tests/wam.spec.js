const {test,expect}=require('@playwright/test');
test('WAM 2 creates isolated instruments and renders timestamped MIDI offline',async({page})=>{
 await page.goto('/wam/host.html');
 const result=await page.evaluate(async()=>{
  const {default:Helm}=await import('/wam/index.js');const {initializeWamHost}=await import('/wam/host.js');
  if(!Helm.isWebAudioModuleConstructor)throw Error('Not a WAM 2 constructor');
  const context=new OfflineAudioContext(4,48000,48000);const [group]=await initializeWamHost(context);
  const [a,b]=await Promise.all([Helm.createInstance(group,context),Helm.createInstance(group,context)]);
  const info=await a.audioNode.getParameterInfo();
  await a.audioNode.setParameterValues({cutoff:{id:'cutoff',value:55,normalized:false}});
  const cutoffA=(await a.audioNode.getParameterValues(false,'cutoff')).cutoff.value;
  const cutoffB=(await b.audioNode.getParameterValues(false,'cutoff')).cutoff.value;
  const saved=await a.audioNode.getState();saved.modulations=[{source:'mono_lfo_1',destination:'cutoff',amount:12}];
  await a.audioNode.setState(saved);const restored=await a.audioNode.getState();
  const merger=new ChannelMergerNode(context,{numberOfInputs:4});merger.connect(context.destination);
  for(const [index,plugin]of [a,b].entries()){
   const splitter=new ChannelSplitterNode(context,{numberOfOutputs:2});plugin.audioNode.connect(splitter);
   splitter.connect(merger,0,index*2);splitter.connect(merger,1,index*2+1);
   plugin.audioNode.scheduleEvents({type:'wam-midi',time:.1+index*.2,data:{bytes:[0x90,60+index*7,100]}},{type:'wam-midi',time:.6,data:{bytes:[0x80,60+index*7,0]}});
  }
  const audio=await context.startRendering();const channels=[0,2].map(c=>Array.from(audio.getChannelData(c)));
  const energy=channels.map(samples=>samples.reduce((sum,x)=>sum+x*x,0));
  const before=channels.map((samples,c)=>samples.slice(0,(.1+c*.2)*48000).reduce((sum,x)=>sum+x*x,0));
  a.audioNode.destroy();b.audioNode.destroy();
  return {names:Object.keys(info).sort(),cutoffA,cutoffB,restored,energy,before,unique:a.instanceId!==b.instanceId};
 });
 const expected=require('fs').readFileSync(require('path').resolve(__dirname,'../../tests/legacy-parameters.txt'),'utf8').trim().split('\n');
 expect(result.names).toEqual(expected);expect(result.unique).toBe(true);expect(result.cutoffA).toBe(55);expect(result.cutoffB).toBe(80);
 expect(result.restored.modulations).toEqual([{source:'mono_lfo_1',destination:'cutoff',amount:12}]);
 for(const energy of result.energy)expect(energy).toBeGreaterThan(.001);
 expect(result.before).toEqual([0,0]);
});
test('WAM 2 handles automation, invalid state and live callback allocation audit',async({page})=>{
 const errors=[];page.on('pageerror',error=>errors.push(error.message));
 await page.goto('/wam/host.html');await page.locator('#load').click();
 await expect(page.locator('section[aria-label="Helm WAM instrument"]')).toBeAttached();
 const result=await page.evaluate(async()=>{
  const {default:Helm}=await import('/wam/index.js');const {initializeWamHost}=await import('/wam/host.js');
  const context=new AudioContext();await context.resume();const [group]=await initializeWamHost(context);
  const plugin=await Helm.createInstance(group,context),node=plugin.audioNode;
  const analyser=new AnalyserNode(context,{fftSize:256});node.connect(analyser);analyser.connect(context.destination);
  const time=context.currentTime;
  node.scheduleEvents({type:'wam-automation',time:time+.05,data:{id:'cutoff',value:.5,normalized:true}},{type:'wam-midi',time:time+.05,data:{bytes:[0x90,60,100]}});
  const data=new Float32Array(256);let peak=0;
  for(let i=0;i<20;++i){await new Promise(resolve=>setTimeout(resolve,20));analyser.getFloatTimeDomainData(data);peak=Math.max(peak,...data.map(Math.abs));}
  const cutoff=(await node.getParameterValues(false,'cutoff')).cutoff.value;
  await node.setParameterValues(Object.fromEntries(Object.entries({osc_1_unison_voices:8,osc_2_unison_voices:4,polyphony:8,filter_on:1,distortion_on:1,formant_on:1,delay_on:1,reverb_on:1,noise_volume:.4}).map(([id,value])=>[id,{id,value,normalized:false}])));
  for(const note of [64,67,71])node.scheduleEvents({type:'wam-midi',time:context.currentTime,data:{bytes:[0x90,note,100]}});
  node.scheduleEvents({type:'wam-transport',time:context.currentTime,data:{currentBar:2,currentBarStarted:context.currentTime,tempo:90,timeSigNumerator:7,timeSigDenominator:8,playing:true}});
  await new Promise(resolve=>setTimeout(resolve,200));
  const tempo=(await node.getParameterValues(false,'beats_per_minute')).beats_per_minute.value;
  const before=await node.getState();const invalid=structuredClone(before);invalid.parameterValues.cutoff.value=NaN;
  let rejected=false;try{await node.setState(invalid);}catch{rejected=true;}
  const retained=(await node.getParameterValues(false,'cutoff')).cutoff.value;
  const stats=await node.getStats();node.destroy();await context.close();return {peak,cutoff,retained,rejected,stats,tempo};
 });
 expect(result.peak).toBeGreaterThan(.001);expect(result.rejected).toBe(true);expect(result.retained).toBe(result.cutoff);
 expect(result.cutoff).toBe(63.5);expect(result.tempo).toBe(1.5);expect(result.stats.allocations).toBe(0);expect(result.stats.releases).toBe(0);expect(errors).toEqual([]);
});
test('WAM preset conversion matches the original reader for every factory patch',async({page})=>{
 test.setTimeout(180000);
 await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
 const failures=await page.evaluate(async()=>{
  const {readPatch,toPatch}=await import('/wam/state.js');const metadata=await(await fetch('/wam/parameters.json')).json();
  const failures=[];
  for(const path of HelmLibrary.entries.keys()){
   try{
    await HelmLibrary.prepare(path);const patch=JSON.parse(Module.FS.readFile(path,{encoding:'utf8'}));
    const converted=toPatch(readPatch(patch,metadata));
    const original=await HelmControl.run(()=>{
     if(Module.ccall('helm_browser_load_patch','number',['string'],[path])!==1)throw Error('reader failed');
     if(Module.ccall('helm_browser_save_patch','number',['string'],['/user/wam-compare.helm'])!==1)throw Error('save failed');
     return JSON.parse(Module.FS.readFile('/user/wam-compare.helm',{encoding:'utf8'}));
    });
    for(const info of metadata.parameters)if(Math.abs(original.settings[info.id]-converted.settings[info.id])>1e-7)throw Error(info.id+': '+original.settings[info.id]+' vs '+converted.settings[info.id]);
    const key=r=>r.source+'|'+r.destination;
    const a=original.settings.modulations.slice().sort((a,b)=>key(a).localeCompare(key(b))),b=converted.settings.modulations.slice().sort((a,b)=>key(a).localeCompare(key(b)));
    if(a.length!==b.length||a.some((r,i)=>key(r)!==key(b[i])||Math.abs(r.amount-b[i].amount)>1e-7))throw Error('modulation mismatch');
   }catch(error){failures.push(path+': '+error.message);}
  }
  return failures;
 });
 expect(failures).toEqual([]);
});
test('WAM GUI filters controls, loads presets and removes multiple instances',async({page})=>{
 await page.goto('/wam/host.html');await page.locator('#load').click();
 const instrument=page.locator('section[aria-label="Helm WAM instrument"]');await expect(instrument).toHaveCount(1);
 await instrument.getByRole('searchbox',{name:'Find parameter'}).fill('cutoff');
 await expect(instrument.getByRole('slider')).toHaveCount(1);
 const presets=instrument.getByRole('combobox',{name:'Factory preset'});
 await expect.poll(()=>presets.locator('option').count()).toBeGreaterThan(100);
 await presets.selectOption({index:1});await expect(instrument.locator('#status')).toContainText('Loaded');
 await page.locator('#load').click();await expect(instrument).toHaveCount(2);
 await page.getByRole('button',{name:'Remove instance'}).first().click();await expect(instrument).toHaveCount(1);
 await page.getByRole('button',{name:'Remove instance'}).click();await expect(instrument).toHaveCount(0);
});
test('WAM 2 loads across origins in a host without SharedArrayBuffer',async({page})=>{
 const http=require('http');
 const server=http.createServer((_request,response)=>{response.setHeader('Content-Type','text/html');response.end('<!doctype html><title>External WAM host</title><button>Start host</button>');});
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 try{
  await page.goto('http://127.0.0.1:'+server.address().port);await page.getByRole('button').click();
  const result=await page.evaluate(async()=>{
   const base='http://127.0.0.1:8137/wam/';
   const {default:Helm}=await import(base+'index.js');const {initializeWamHost}=await import(base+'host.js');
   const context=new AudioContext();await context.resume();const [group]=await initializeWamHost(context);
   const plugin=await Helm.createInstance(group,context);const analyser=new AnalyserNode(context,{fftSize:256});plugin.audioNode.connect(analyser);analyser.connect(context.destination);
   const gui=await plugin.createGui();document.body.append(gui);
   const sourceOrigin=new URL(gui.shadowRoot.querySelector('a').href).origin;
   plugin.audioNode.scheduleEvents({type:'wam-midi',time:context.currentTime,data:{bytes:[0x90,60,100]}});
   const samples=new Float32Array(256);let peak=0;for(let i=0;i<20;++i){await new Promise(resolve=>setTimeout(resolve,20));analyser.getFloatTimeDomainData(samples);peak=Math.max(peak,...samples.map(Math.abs));}
   plugin.destroyGui(gui);plugin.audioNode.destroy();await context.close();return {isolated:crossOriginIsolated,peak,sourceOrigin};
  });
  expect(result.isolated).toBe(false);expect(result.peak).toBeGreaterThan(.001);expect(result.sourceOrigin).toBe('http://127.0.0.1:8137');
 }finally{await new Promise(resolve=>server.close(resolve));}
});
