const {test,expect}=require('@playwright/test');
test.beforeEach(async({page})=>{page.on('requestfailed',r=>console.log('requestfailed',r.url(),r.failure()));page.on('pageerror',e=>console.log('pageerror',e.message));});
test('original JUCE interface starts and renders the C++ engine',async({page})=>{
  const errors=[];page.on('pageerror',error=>errors.push(error.message));
  page.on('console',m=>{if(m.type()==='error')console.log('browser:',m.text());});
  await page.goto('/helm.html');
  await expect(page.locator('#status')).toHaveText('Ready',{timeout:60000});
  await expect(page.locator('canvas').first()).toBeVisible();
  await page.screenshot({path:require('path').resolve(__dirname,'../../build/wasm/browser.png')});
  const result=await page.evaluate(()=>{
    const M=Module;
    const begin=M._helm_browser_offline_begin(48000);
    const midi=M._helm_browser_midi(0x90,60,100,0);
    const energy=M._helm_browser_offline_render(48000);
    M._helm_browser_offline_end();
    return {begin,midi,energy};
  });
  console.log(result);
  expect(result.begin).toBe(1);expect(result.midi).toBe(1);
  expect(result.energy).toBeGreaterThan(0.001);expect(Number.isFinite(result.energy)).toBe(true);
  expect(errors).toEqual([]);
});
test('AudioWorklet plays, suspends, resumes and reports callback allocations',async({page})=>{
  const errors=[];page.on('pageerror',error=>errors.push(error.message));
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  await page.locator('#enable-audio').click();
  await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_status())).toBe(2);
  await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_blocks())).toBeGreaterThan(20);
  await page.evaluate(()=>Module._helm_browser_midi(0x90,60,100,Module._helm_browser_audio_time()*Module._helm_browser_audio_rate()));
  await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_peak())).toBeGreaterThan(0.001);
  const stats=await page.evaluate(()=>({blocks:Module._helm_browser_audio_blocks(),skipped:Module._helm_browser_audio_skipped(),allocations:Module._helm_browser_audio_allocations(),releases:Module._helm_browser_audio_releases(),pool:Module._helm_browser_audio_pool_allocations()}));
  console.log('live audio',stats);expect(stats.allocations).toBe(0);expect(stats.releases).toBe(0);
  expect(await page.evaluate(()=>Module._helm_browser_offline_begin(48000))).toBe(0);
  await page.evaluate(async()=>{const context=Object.values(emAudio).find(x=>x instanceof AudioContext);await context.suspend();});
  await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_status())).toBe(4);
  await page.locator('#enable-audio').click();
  await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_status())).toBe(2);
  await page.evaluate(()=>Module._helm_browser_panic());
  expect(errors).toEqual([]);
});
test('factory presets load on demand and saved files survive reload',async({page})=>{
  const requests=[];page.on('request',request=>{if(request.url().includes('/objects/'))requests.push(request.url());});
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  expect(requests).toEqual([]);
  const loaded=await page.evaluate(async()=>{
    const path=[...HelmLibrary.entries.keys()][0];
    await HelmLibrary.prepare(path);
    return HelmControl.run(()=>Module.ccall('helm_browser_load_patch','number',['string'],[path]));
  });
  expect(loaded).toBe(1);expect(requests.length).toBe(1);
  await page.evaluate(async()=>{
    const path='/user/patches/User Patches/Keys/Test.helm';
    if(!Module.ccall('helm_browser_save_patch','number',['string'],[path]))throw Error('save failed');
    await HelmBrowser.flush();
  });
  await page.reload();await expect(page.locator('#status')).toHaveText('Ready');
  const result=await page.evaluate(()=>Module.ccall('helm_browser_load_patch','number',['string'],['/user/patches/User Patches/Keys/Test.helm']));
  expect(result).toBe(1);
});
test('all legacy controls remain editable and modulation routing renders safely',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  const result=await page.evaluate(async()=>HelmControl.run(()=>{
    const controls=JSON.parse(Module.ccall('helm_browser_controls','string',[],[]));
    if(!Module._helm_browser_offline_begin(48000))throw Error('offline unavailable');
    for(const control of controls){
      for(const value of [control.min,control.max,control.default]){
        if(!Module.ccall('helm_browser_set_control','number',['string','number'],[control.name,value]))throw Error('set '+control.name);
        const actual=Module.ccall('helm_browser_get_control','number',['string'],[control.name]);
        if(Math.abs(actual-value)>1e-8)throw Error('get '+control.name);
        const energy=Module._helm_browser_offline_render(256);
        if(!Number.isFinite(energy))throw Error('render '+control.name);
      }
    }
    const connect=Module.ccall('helm_browser_modulation','number',['string','string','number'],['mono_lfo_1','cutoff',24]);
    Module._helm_browser_midi(0x90,60,100,0);
    const energy=Module._helm_browser_offline_render(8192);
    const disconnect=Module.ccall('helm_browser_modulation','number',['string','string','number'],['mono_lfo_1','cutoff',0]);
    Module._helm_browser_offline_end();
    return {names:controls.map(c=>c.name).sort(),connect,disconnect,energy};
  }));
  const expected=require('fs').readFileSync(require('path').resolve(__dirname,'../../tests/legacy-parameters.txt'),'utf8').trim().split('\n');
  expect(result.names).toEqual(expected);expect(result.connect).toBe(1);expect(result.disconnect).toBe(1);
  expect(result.energy).toBeGreaterThan(0);
});
test('all factory patches validate and load through the original preset reader',async({page})=>{
  test.setTimeout(180000);
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  const failures=await page.evaluate(async()=>{
    const failures=[];
    for(const path of HelmLibrary.entries.keys()){
      await HelmLibrary.prepare(path);
      const result=await HelmControl.run(()=>Module.ccall('helm_browser_load_patch','number',['string'],[path]));
      if(result!==1)failures.push(path);
    }
    return failures;
  });
  expect(failures).toEqual([]);expect(errors).toEqual([]);
});
test('failed downloads and malformed patches retain current settings',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  await page.route('**/library/objects/**',route=>route.fulfill({status:200,body:'corrupt'}));
  const retained=await page.evaluate(async()=>{
    const before=Module.ccall('helm_browser_get_control','number',['string'],['cutoff']);
    let failed=false;
    try{await HelmLibrary.prepare([...HelmLibrary.entries.keys()][0]);}catch{failed=true;}
    Module.FS.writeFile('/user/invalid.helm','{"synth_version":"0.9.0","settings":{}}');
    const accepted=Module.ccall('helm_browser_load_patch','number',['string'],['/user/invalid.helm']);
    const after=Module.ccall('helm_browser_get_control','number',['string'],['cutoff']);
    return {before,after,failed,accepted};
  });
  expect(retained.failed).toBe(true);expect(retained.accepted).toBe(0);expect(retained.after).toBe(retained.before);
});
test('AudioWorklet closes and restarts repeatedly without reusing a live stack',async({page})=>{
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  for(let i=0;i<3;++i){
    await page.locator('#enable-audio').click();
    await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_status())).toBe(2);
    await page.evaluate(async()=>{const context=Object.values(emAudio).find(x=>x instanceof AudioContext);await context.close();});
    await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_status())).toBe(-1);
  }
  expect(errors).toEqual([]);
});
test('transport, MIDI overflow and original computer keyboard remain functional',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  const state=await page.evaluate(()=>{
    Module._helm_browser_offline_begin(48000);
    Module._helm_browser_transport(1,7,8);
    Module._helm_browser_offline_render(4800);
    const playing=Module._helm_browser_transport_position();
    Module._helm_browser_transport(0,7,8);
    Module._helm_browser_offline_render(4800);
    const paused=Module._helm_browser_transport_position();
    Module._helm_browser_rewind();Module._helm_browser_offline_render(128);
    const rewound=Module._helm_browser_transport_position();
    for(let i=0;i<1100;++i)Module._helm_browser_midi(0x90,60,100,0);
    const overflow=Module._helm_browser_midi_overflows();
    Module._helm_browser_offline_render(128);Module._helm_browser_offline_end();
    return {playing,paused,rewound,overflow};
  });
  expect(state.playing).toBeCloseTo(.2,8);expect(state.paused).toBe(state.playing);
  expect(state.rewound).toBe(0);expect(state.overflow).toBeGreaterThan(0);
  await page.locator('#enable-audio').click();
  await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_status())).toBe(2);
  await page.locator('canvas').first().focus();
  await page.keyboard.down('a');
  await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_peak())).toBeGreaterThan(.001);
  await page.keyboard.up('a');
});
test('native patch picker imports and exports through browser file handles',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  await page.evaluate(async()=>{
    const path=[...HelmLibrary.entries.keys()][0];await HelmLibrary.prepare(path);
    const bytes=Module.FS.readFile(path);
    window.showOpenFilePicker=async()=>[{getFile:async()=>new File([bytes],'Imported.helm')}];
    window.exported=null;
    window.showSaveFilePicker=async()=>({name:'Exported.helm',createWritable:async()=>({write:async data=>window.exported=Array.from(data),close:async()=>{},abort:async()=>{}})});
  });
  await page.locator('#import-patch').click();
  await expect.poll(()=>page.evaluate(()=>Module.FS.analyzePath('/user/patches/User Patches/Imported/Imported.helm').exists)).toBe(true);
  await page.locator('#export-patch').click();
  await expect.poll(()=>page.evaluate(()=>window.exported?.length||0)).toBeGreaterThan(100);
  const exported=await page.evaluate(()=>JSON.parse(new TextDecoder().decode(new Uint8Array(window.exported))));
  expect(exported.synth_version).toBe('0.9.0');expect(exported.settings.modulations).toBeInstanceOf(Array);
});
test('storage failure is visible and retry preserves files',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  const result=await page.evaluate(async()=>{
    await HelmBrowser.flush();
    const original=Module.FS.syncfs;
    Module.FS.writeFile('/user/retry.txt','keep me');
    Module.FS.syncfs=(populate,done)=>done(Error('simulated quota failure'));
    let failed=false;try{await HelmBrowser.flush();}catch{failed=true;}
    const message=document.getElementById('storage-status').textContent;
    Module.FS.syncfs=original;await HelmBrowser.flush();
    return {failed,message,text:Module.FS.readFile('/user/retry.txt',{encoding:'utf8'})};
  });
  expect(result.failed).toBe(true);expect(result.message).toContain('Unable to save');expect(result.text).toBe('keep me');
  await page.reload();await expect(page.locator('#status')).toHaveText('Ready');
  expect(await page.evaluate(()=>Module.FS.readFile('/user/retry.txt',{encoding:'utf8'}))).toBe('keep me');
});
test('JUCE accessibility exposes original controls and edits their values',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  const slider=page.getByRole('slider',{name:'volume',exact:true});
  await expect(slider).toBeAttached();
  await slider.focus();await page.keyboard.press('ArrowRight');
  await expect.poll(()=>page.evaluate(()=>Module.ccall('helm_browser_get_control','number',['string'],['volume']))).not.toBe(.5);
});
test('bank export and transactional import round trip saved patches',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  await page.evaluate(()=>{
    Module.ccall('helm_browser_save_patch','number',['string'],['/user/patches/User Patches/Keys/BankTest.helm']);
    window.bankBytes=null;
    window.showSaveFilePicker=async()=>({name:'Roundtrip.helmbank',createWritable:async()=>({write:async data=>window.bankBytes=Array.from(data),close:async()=>{},abort:async()=>{}})});
    Module.ccall('helm_browser_export_bank',null,['string'],['User Patches']);
  });
  await expect.poll(()=>page.evaluate(()=>window.bankBytes?.length||0)).toBeGreaterThan(100);
  require('fs').writeFileSync(require('path').resolve(__dirname,'../../build/wasm/bank-test.zip'),Buffer.from(await page.evaluate(()=>window.bankBytes)));
  await page.evaluate(()=>{
    window.showOpenFilePicker=async()=>[{getFile:async()=>new File([new Uint8Array(window.bankBytes)],'Roundtrip.helmbank')}];
    Module._helm_browser_import_bank();
  });
  await expect.poll(()=>page.evaluate(()=>Module.FS.analyzePath('/user/patches/Roundtrip/Keys/BankTest.helm').exists)).toBe(true);
  expect(await page.evaluate(()=>Module.ccall('helm_browser_load_patch','number',['string'],['/user/patches/Roundtrip/Keys/BankTest.helm']))).toBe(1);
  const unsafe=require('child_process').execFileSync('python3',['-c',"import io,zipfile,sys;b=io.BytesIO();z=zipfile.ZipFile(b,'w');z.writestr('../escape.helm','{}');z.close();sys.stdout.buffer.write(b.getvalue())"]);
  await page.evaluate(bytes=>{
    window.showOpenFilePicker=async()=>[{getFile:async()=>new File([new Uint8Array(bytes)],'Unsafe.helmbank')}];
    Module._helm_browser_import_bank();
  },Array.from(unsafe));
  await expect(page.locator('#file-status')).toContainText('Unsafe bank archive');
  expect(await page.evaluate(()=>Module.FS.analyzePath('/user/patches/Unsafe').exists)).toBe(false);
  expect(await page.evaluate(()=>Module.FS.analyzePath('/user/escape.helm').exists)).toBe(false);
});
test('canceling a deferred bank export leaves the destination untouched',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  await page.evaluate(()=>{
    Module.ccall('helm_browser_save_patch','number',['string'],['/user/patches/User Patches/Keys/Cancel.helm']);
    window.written=false;
    window.showSaveFilePicker=async()=>({name:'Cancel.helmbank',createWritable:async()=>{window.written=true;throw Error('must not write');}});
    HelmLibrary.prepareBank=()=>new Promise(resolve=>window.finishPreparing=resolve);
    Module.ccall('helm_browser_export_bank',null,['string'],['User Patches']);
  });
  await page.getByRole('button',{name:'Cancel export',exact:true}).click();
  await expect(page.locator('#file-status')).toContainText('Export canceled');
  await page.evaluate(async()=>{window.finishPreparing();await HelmControl.run(()=>{});});
  expect(await page.evaluate(()=>window.written)).toBe(false);
});
test('user files can be downloaded without removing their persistent copy',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  await page.evaluate(()=>{
    Module.FS.writeFile('/user/backup.txt','saved settings');
    window.backup=null;
    window.showSaveFilePicker=async()=>({createWritable:async()=>({write:async data=>window.backup=Array.from(data),close:async()=>{},abort:async()=>{}})});
  });
  await page.getByRole('button',{name:'User files',exact:true}).click();
  await page.getByRole('button',{name:'Download backup.txt',exact:true}).click();
  await expect(page.getByRole('dialog',{name:'User files'}).getByRole('status')).toHaveText('Downloaded backup.txt');
  expect(await page.evaluate(()=>new TextDecoder().decode(new Uint8Array(window.backup)))).toBe('saved settings');
  expect(await page.evaluate(()=>Module.FS.readFile('/user/backup.txt',{encoding:'utf8'}))).toBe('saved settings');
});
test('original Save dialog accepts Unicode text and persists new banks and folders',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  const open=page.getByRole('button',{name:'SAVE',exact:true});
  await open.focus();await page.keyboard.press('Enter');
  const patch=page.getByRole('textbox',{name:'Patch Name',exact:true});
  await expect(patch).toBeAttached();await patch.fill('星🌟');
  await page.getByRole('textbox',{name:'Author',exact:true}).fill('作者');
  const bank=page.getByRole('textbox',{name:'Add Bank',exact:true});
  await bank.fill('Browser Test Bank');
  await page.getByRole('button',{name:'+',exact:true}).first().focus();await page.keyboard.press('Enter');
  const folder=page.getByRole('textbox',{name:'Add Folder',exact:true});
  await folder.fill('Keys');await folder.press('Enter');
  const save=page.getByRole('button',{name:'Save',exact:true});
  await save.focus();await page.keyboard.press('Enter');
  const path='/user/patches/Browser Test Bank/Keys/星🌟.helm';
  await expect.poll(()=>page.evaluate(path=>Module.FS.analyzePath(path).exists,path)).toBe(true);
  const stored=await page.evaluate(path=>JSON.parse(Module.FS.readFile(path,{encoding:'utf8'})),path);
  expect(stored.author).toBe('作者');
  await expect.poll(()=>page.evaluate(()=>Module.FS.analyzePath('/fonts/fallback').exists)).toBe(true);
  await page.evaluate(async()=>{await HelmBrowser.fallbackFonts;await HelmBrowser.flush();});
  await page.reload();await expect(page.locator('#status')).toHaveText('Ready');
  expect(await page.evaluate(path=>Module.ccall('helm_browser_load_patch','number',['string'],[path]),path)).toBe(1);
});
test('factory edits and deletions survive a catalog refresh',async({page})=>{
  await page.goto('/helm.html');await expect(page.locator('#status')).toHaveText('Ready');
  const paths=await page.evaluate(async()=>{
    const [edited,deleted]=HelmLibrary.entries.keys();
    await HelmLibrary.prepare(edited);
    await HelmControl.run(()=>{
      Module.ccall('helm_browser_load_patch','number',['string'],[edited]);
      Module.ccall('helm_browser_set_control','number',['string','number'],['cutoff',70]);
      Module.ccall('helm_browser_save_patch','number',['string'],[edited]);
    });
    Module.FS.unlink(deleted);await HelmBrowser.flush();return {edited,deleted};
  });
  await page.reload();await expect(page.locator('#status')).toHaveText('Ready');
  const result=await page.evaluate(({edited,deleted})=>({
    loaded:Module.ccall('helm_browser_load_patch','number',['string'],[edited]),
    cutoff:Module.ccall('helm_browser_get_control','number',['string'],['cutoff']),
    deleted:!Module.FS.analyzePath(deleted).exists&&!HelmLibrary.entries.has(deleted)
  }),paths);
  expect(result).toEqual({loaded:1,cutoff:70,deleted:true});
});
