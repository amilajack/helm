// SPDX-License-Identifier: GPL-3.0-or-later
if (typeof window !== 'undefined') {
  const library = globalThis.HelmLibrary = {
    entries: new Map(), loaded: new Set(), requests: new Map(), token: 0,
    async prepare(path) {
      const entry=this.entries.get(path);
      if(!entry || this.loaded.has(path))return;
      if(this.requests.has(path))return this.requests.get(path);
      const request=(async()=>{
        const response=await fetch('library/objects/'+entry.sha256);
        if(!response.ok)throw Error('Preset download failed: '+response.status);
        const bytes=new Uint8Array(await response.arrayBuffer());
        const digest=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),b=>b.toString(16).padStart(2,'0')).join('');
        if(bytes.length!==entry.size || digest!==entry.sha256)throw Error('Preset integrity check failed');
        FS.writeFile(path,bytes);this.loaded.add(path);
      })().finally(()=>this.requests.delete(path));
      this.requests.set(path,request);return request;
    },
    ensure(path) {
      const token=++this.token;
      if(!this.entries.has(path) || this.loaded.has(path))return true;
      HelmBrowser.reportFile('Loading preset…');
      this.prepare(path).then(async()=>{
        if(token!==this.token)return;
        await HelmControl.run(()=>{
          if(token!==this.token)return;
          if(!Module.ccall('helm_browser_load_patch','number',['string'],[path]))throw Error('Invalid preset');
        });
        HelmBrowser.reportFile('');
      }).catch(error=>HelmBrowser.reportFile(error.message+'. Current patch retained.'));
      return false;
    },
    async prepareBank(bank) {
      const paths=[...this.entries.keys()].filter(path=>path.startsWith('/user/patches/'+bank+'/'));
      // Bound download concurrency instead of opening hundreds of connections.
      for(let i=0;i<paths.length;i+=8)await Promise.all(paths.slice(i,i+8).map(path=>this.prepare(path)));
    }
  };
  Module.preRun=Module.preRun||[];
  Module.preRun.push(()=>{
    addRunDependency('helm-factory-catalog');
    Promise.resolve(HelmBrowser.ready).then(async()=>{
      const response=await fetch('library/manifest.json');
      if(!response.ok)throw Error('Unable to download factory catalog');
      const manifest=await response.json();
      if(manifest.version!==1 || !Array.isArray(manifest.patches))throw Error('Invalid factory catalog');
      FS.mkdirTree('/user/patches');
      const installedPath='/user/.factory-catalog.json';
      let installed=[];
      if(FS.analyzePath(installedPath).exists) installed=JSON.parse(FS.readFile(installedPath,{encoding:'utf8'}));
      const known=new Set(installed);

      for(const entry of manifest.patches){
        if(typeof entry.path!=='string' || entry.path.split('/').some(x=>!x||x==='.'||x==='..') || entry.path.includes('\\') || !/^[0-9a-f]{64}$/.test(entry.sha256) || !Number.isInteger(entry.size) || entry.size>1048576)throw Error('Invalid factory catalog entry');
        const path='/user/patches/'+entry.path;
        FS.mkdirTree(path.slice(0,path.lastIndexOf('/')));
        const exists=FS.analyzePath(path).exists;
        // As on desktop, the library is a user-editable copy. Preserve edits and
        // deletions across reload; only new factory entries get placeholders.
        if(!exists && known.has(entry.path))continue;
        if(!exists)FS.writeFile(path,new Uint8Array());
        else if(FS.stat(path).size)library.loaded.add(path);
        library.entries.set(path,entry);

      }
      FS.writeFile(installedPath,JSON.stringify(manifest.patches.map(entry=>entry.path)));
    }).catch(error=>{Module.noInitialRun=true;document.getElementById('status').textContent=error.message+'. Reload to retry.';})
      .finally(()=>removeRunDependency('helm-factory-catalog'));
  });
}
