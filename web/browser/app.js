// SPDX-License-Identifier: GPL-3.0-or-later
(() => {
  const pending=[];let timer=null;
  globalThis.HelmControl={
    run(action){return new Promise((resolve,reject)=>{pending.push({action,resolve,reject});pump();});}
  };
  function pump(){
    clearTimeout(timer);timer=null;
    if(!pending.length)return;
    if(!Module._helm_control_enter?.()){timer=setTimeout(pump,4);return;}
    try{for(let i=0;i<128&&pending.length;++i){const task=pending.shift();try{task.resolve(task.action());}catch(error){task.reject(error);}}}
    finally{Module._helm_control_exit();}
    if(pending.length)timer=setTimeout(pump,0);
  }
  globalThis.helmReady=()=>{
    // Browser callbacks can arrive between C++ control ticks. Serialize every
    // mutation through the same engine gate, retaining allocated text arguments.
    for(const name of ['helm_text_update','helm_text_composition','helm_text_format',
      'helm_accessibility_action','helm_accessibility_text','helm_accessibility_composition',
      'helm_accessibility_key','helm_accessibility_character','helm_clipboard_complete']){
      const original=Module['_'+name];
      Module['_'+name]=(...args)=>{
        // Preserve JUCE's handled/unhandled result when ownership is available
        // synchronously, so unhandled keys retain their browser default action.
        let result=1;
        HelmControl.run(()=>{result=original(...args);return result;}).catch(error=>HelmBrowser.reportFile(String(error)));
        return result;
      };
    }
    HelmUserFiles.attach(Module);HelmAccessibility.attach(Module);HelmMidi.attach(Module);HelmTransport.attach(Module);
    document.getElementById('enable-audio').onclick=()=>Module._helm_enable_audio();
    document.getElementById('import-patch').onclick=()=>Module._helm_browser_import_patch();
    document.getElementById('export-patch').onclick=()=>Module._helm_browser_export_patch();
    document.getElementById('status').textContent='Ready';
  };
  if(!crossOriginIsolated){
    Module.noInitialRun=true;
    document.getElementById('status').textContent='Audio requires cross-origin isolation. Use the supplied server or configure COOP and COEP headers.';
  }
  addEventListener('blur',()=>Module._helm_browser_panic?.());
})();
