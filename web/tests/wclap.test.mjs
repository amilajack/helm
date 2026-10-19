// SPDX-License-Identifier: GPL-3.0-or-later
// Loads build/wclap/Helm.wclap through a minimal wasm32 WCLAP host (wclap-host.mjs)
// and Node's WASI, with the bundle mounted the way browser hosts mount it.
// Run: node --test web/tests/wclap.test.mjs (after web/scripts/build-wclap.sh).
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync,existsSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import {resolve,dirname} from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';
import {WASI} from 'node:wasi';
import {WclapHost} from './wclap-host.mjs';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'../..');
const build=process.env.HELM_WCLAP_BUILD?resolve(process.env.HELM_WCLAP_BUILD):root+'/build/wclap';
const bundle=build+'/Helm.wclap';
const legacy=readFileSync(root+'/tests/legacy-parameters.txt','utf8').trim().split('\n');
const fnv1a=text=>{let h=0x811c9dc5;for(const c of Buffer.from(text))h=Math.imul(h^c,0x01000193)>>>0;return h;};
const module=new WebAssembly.Module(readFileSync(bundle+'/module.wasm'));
const pluginPath='/plugin/helm';

async function load(){
 const wasi=new WASI({version:'preview1',preopens:{[pluginPath]:bundle},returnOnExit:true});
 const instance=await WebAssembly.instantiate(module,wasi.getImportObject());
 wasi.initialize(instance);
 const host=new WclapHost(instance);host.init(pluginPath);
 return host;
}
const energy=(samples,from=0,to=samples.length)=>{let sum=0;for(let i=from;i<to;++i)sum+=samples[i]**2;return sum;};
const sequence=[{time:4800,type:'note-on',key:60},{time:9000,type:'note-on',key:67},{time:24000,type:'note-off',key:60},{time:30000,type:'note-off',key:67}];

test('module follows the WCLAP ABI',()=>{
 const exports=Object.fromEntries(WebAssembly.Module.exports(module).map(e=>[e.name,e.kind]));
 assert.equal(exports.clap_entry,'global');assert.equal(exports.malloc,'function');
 assert.equal(exports.memory,'memory');assert.equal(exports._initialize,'function');
 assert.equal(Object.values(exports).filter(kind=>kind==='table').length,1);
 for(const entry of WebAssembly.Module.imports(module))assert.equal(entry.module,'wasi_snapshot_preview1',entry.name);
 const archive=build+'/Helm.wclap.tar.gz';
 const names=execFileSync('tar',['-tzf',archive],{encoding:'utf8'}).trim().split('\n');
 for(const name of ['module.wasm','COPYING','SOURCE.txt','library/manifest.json'])assert.ok(names.includes(name),name);
 assert.ok(names.every(name=>!name.startsWith('/')&&!name.includes('..')));
});

test('descriptor, parameters and sample-accurate notes',async()=>{
 const host=await load();
 const [descriptor]=host.descriptors();
 assert.equal(descriptor.id,'org.tytel.helm');assert.deepEqual(descriptor.features,['instrument','synthesizer','stereo']);
 const plugin=host.create('org.tytel.helm');
 const [{metadata}]=plugin.send({type:'metadata',rid:1});
 assert.deepEqual(metadata.parameters.map(p=>p.id).sort(),[...legacy].sort());
 assert.equal(plugin.paramValue(fnv1a('cutoff')),80);
 const [left,right]=plugin.render(48000,sequence);
 assert.equal(energy(left,0,4800)+energy(right,0,4800),0);
 assert.ok(energy(left,4800,24000)>1e-3);
 assert.ok(left.every(Number.isFinite)&&right.every(Number.isFinite));
 plugin.destroy();
});

test('state round trips between instances and rejects invalid data',async()=>{
 const host=await load();
 const a=host.create('org.tytel.helm');
 a.renderBlock(64,[{time:0,type:'param',id:fnv1a('cutoff'),value:55}]);
 const [{state}]=a.send({type:'getState',rid:2});
 assert.equal(state.parameterValues.cutoff.value,55);
 state.modulations=[{source:'mono_lfo_1',destination:'cutoff',amount:12}];
 state.info.patch_name='Round trip';
 assert.equal(a.send({type:'setState',rid:3,state})[0].type,'state');
 assert.equal(a.send({type:'setState',rid:4,state:{...state,modulations:[{source:'nope',destination:'cutoff',amount:1}]}})[0].type,'error');
 const saved=a.save();
 const b=host.create('org.tytel.helm');
 assert.ok(b.load(saved));
 assert.equal(b.paramValue(fnv1a('cutoff')),55);
 const [{state:restored}]=b.send({type:'getState',rid:5});
 assert.deepEqual(restored.modulations,state.modulations);assert.equal(restored.info.patch_name,'Round trip');
 assert.equal(b.save(),saved);
 assert.ok(!b.load('{"schemaVersion":1'));assert.ok(!b.load(''));
 // GUI edits reach the engine and the host is asked to flush them.
 const before=host.flushRequests;b.send({type:'param',id:'cutoff',value:30});
 assert.ok(host.flushRequests>before);assert.equal(b.paramValue(fnv1a('cutoff')),30);
 assert.throws(()=>b.send({type:'param',id:'cutoff',value:1e9}));
 a.destroy();b.destroy();
});

test('webview resources come from the module and the bundle',async()=>{
 const host=await load();const plugin=host.create('org.tytel.helm');
 const page=plugin.resource('/index.html');assert.match(page.mime,/^text\/html/);
 assert.match(new TextDecoder().decode(page.data),/gui\.js/);
 assert.match(plugin.resource('/gui.js').mime,/javascript/);
 const manifest=JSON.parse(new TextDecoder().decode(plugin.resource('/library/manifest.json').data));
 const first=manifest.patches[0];
 const object=plugin.resource('/library/objects/'+first.sha256);
 assert.equal(object.data.length,first.size);
 for(const path of ['/../COPYING','/library/../module.wasm','/module.wasm','/library/objects/abc','library/manifest.json'])
  assert.equal(plugin.resource(path),null,path);
 plugin.destroy();
});

// Same block boundaries as the WCLAP render: 256-frame blocks split at events.
async function renderWam(){
 const output=root+'/build/wasm/web/wam';
 const {default:createDsp}=await import(pathToFileURL(output+'/dsp.mjs').href);
 const dsp=await createDsp({wasmBinary:readFileSync(output+'/dsp.wasm')});
 const synth=dsp._helm_wam_create(48000);const left=new Float32Array(48000);
 const midi={'note-on':[0x90,102],'note-off':[0x80,0]};
 for(let start=0;start<48000;start+=256){
  const end=Math.min(start+256,48000);let frame=start;
  const events=sequence.filter(e=>e.time>=start&&e.time<end);
  for(const event of [...events,null]){
   const until=event?event.time:end;
   if(until>frame){const p=dsp._helm_wam_render(synth,until-frame)/4;left.set(dsp.HEAPF32.subarray(p,p+until-frame),frame);frame=until;}
   if(event)dsp._helm_wam_midi(synth,midi[event.type][0],event.key,midi[event.type][1]);
  }
 }
 dsp._helm_wam_destroy(synth);return left;
}
test('WCLAP renders the same audio as the WAM build',{skip:!existsSync(root+'/build/wasm/web/wam/dsp.mjs')&&'WAM build missing'},async()=>{
 const host=await load();const plugin=host.create('org.tytel.helm');
 // CLAP velocity 102/127 matches MIDI velocity 102 exactly.
 const [left]=plugin.render(48000,sequence.map(e=>({...e,velocity:102/127})));
 const reference=await renderWam();
 let error=0;for(let i=0;i<left.length;++i)error+=(left[i]-reference[i])**2;
 const relative=Math.sqrt(error/Math.max(energy(reference),1e-30));
 assert.ok(energy(reference)>1e-3,'reference render is silent');
 assert.ok(relative<=1e-5,'relative RMS '+relative);
 plugin.destroy();
});
