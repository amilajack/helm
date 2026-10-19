// Helm's WCLAP in the WebCLAP browser test host: AudioWorklet playback, the
// clap.webview/3 GUI, factory presets from the bundle and host parameters.
// Run with HELM_TEST_WCLAP=1 after build-wclap.sh and fetch-wclap-test-host.sh.
const {test,expect}=require('@playwright/test');
test('WebCLAP host plays Helm and drives its webview GUI',async({page})=>{
  const errors=[];page.on('pageerror',error=>errors.push(error.message));
  const url='http://127.0.0.1:8138/?module='+encodeURIComponent('http://127.0.0.1:8137/Helm.wclap.tar.gz');
  // The host proxies webview resources through a service worker that controls the page after a reload.
  await page.goto(url);await page.evaluate(()=>navigator.serviceWorker.ready);
  await page.reload();await page.mouse.click(600,500);
  await page.waitForFunction(()=>globalThis.effectNode,null,{timeout:30000});
  const info=await page.evaluate(async()=>({name:effectNode.descriptor.name,features:effectNode.descriptor.features,params:(await effectNode.getParams()).length}));
  expect(info).toEqual({name:'Helm',features:['instrument','synthesizer','stereo'],params:139});
  await expect.poll(()=>page.frames().some(frame=>frame.url().endsWith('/get_resource/index.html')),{timeout:30000}).toBe(true);
  const frame=page.frames().find(frame=>frame.url().endsWith('/get_resource/index.html'));
  const gui=frame.locator('section[aria-label="Helm CLAP instrument"]');
  await expect(gui).toBeAttached({timeout:30000});
  // Factory preset: GUI → get_resource → bundle files in the host's WASI filesystem.
  await expect(gui.locator('#presets option')).not.toHaveCount(1,{timeout:30000});
  await gui.locator('#presets').selectOption({index:5});
  await expect(gui.locator('#status')).toHaveText(/^Loaded /,{timeout:30000});
  // A GUI edit becomes the host-visible parameter value.
  await gui.locator('input[aria-label="cutoff"]').evaluate(input=>{input.value='42';input.dispatchEvent(new Event('input'));input.dispatchEvent(new Event('change'));});
  await expect.poll(()=>page.evaluate(async()=>{const p=(await effectNode.getParams()).find(p=>p.name==='Filter Cutoff');return (await effectNode.getParam(p.id)).value;})).toBe(42);
  // A GUI keyboard note is audible at the plugin output.
  await page.evaluate(()=>{const analyser=effectNode.context.createAnalyser();effectNode.connect(analyser);globalThis.helmPeak=0;
    globalThis.helmMeter=setInterval(()=>{const data=new Float32Array(analyser.fftSize);analyser.getFloatTimeDomainData(data);for(const x of data)helmPeak=Math.max(helmPeak,Math.abs(x));},20);});
  const key=gui.locator('#keyboard button').first();
  await key.dispatchEvent('pointerdown',{pointerId:1});
  await expect.poll(()=>page.evaluate(()=>helmPeak),{timeout:10000}).toBeGreaterThan(.001);
  await key.dispatchEvent('pointerup',{pointerId:1});
  const state=await page.evaluate(async()=>{const s=await effectNode.saveState();return JSON.parse(new TextDecoder().decode(ArrayBuffer.isView(s)?s:new Uint8Array(s)));});
  expect(state.schemaVersion).toBe(1);expect(state.parameterValues.cutoff.value).toBe(42);
  expect(errors).toEqual([]);
});
