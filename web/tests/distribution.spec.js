const {test,expect}=require('@playwright/test');
const fs=require('fs'),path=require('path'),crypto=require('crypto');
const root=path.resolve(__dirname,'../../build/web-dist');
const version=fs.readFileSync(path.join(root,'index.html'),'utf8').match(/url=([a-f0-9]+)\/helm.html/)[1];
const directory=path.join(root,version);
test('distributed assets and corresponding source match the integrity manifest',()=>{
  const manifest=JSON.parse(fs.readFileSync(path.join(directory,'integrity.json')));
  expect(manifest.version).toBe(version);
  for(const [name,expected]of Object.entries(manifest.files)){
    expect(crypto.createHash('sha256').update(fs.readFileSync(path.join(directory,name))).digest('hex'),name).toBe(expected);
  }
  const names=require('child_process').execFileSync('tar',['-tzf',path.join(directory,'source.tar.gz')],{maxBuffer:16*1024*1024}).toString().split('\n');
  for(const required of ['CMakeLists.txt','src/browser/BrowserAudio.cpp','web/scripts/build.sh','web/scripts/prepare-juce.py',
    'web/feature-review.json','third_party/JUCE9/CMakeLists.txt','third_party/JUCE9/LICENSE.md',
    'third_party/JUCE9/modules/juce_core/system/juce_TargetPlatform.h','SOURCE-DISTRIBUTION.json'])expect(names).toContain('helm/'+required);
  expect(names.some(name=>/\/(node_modules|\.git|\.toolchains)\//.test(name))).toBe(false);
});
test('versioned static site loads its original editor and real audio processor',async({page})=>{
  const failures=[],errors=[];
  page.on('requestfailed',request=>failures.push(request.url()));
  page.on('pageerror',error=>errors.push(error.message));
  await page.goto('/');await expect(page.locator('#status')).toHaveText('Ready',{timeout:60000});
  await expect(page).toHaveURL(new RegExp('/'+version+'/helm.html$'));
  expect(await page.evaluate(()=>crossOriginIsolated)).toBe(true);
  await expect(page.getByRole('link',{name:'Source & licenses'})).toHaveAttribute('href','source.tar.gz');
  await page.locator('#enable-audio').click();
  await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_status())).toBe(2);
  await page.evaluate(()=>Module._helm_browser_midi(0x90,60,100,Module._helm_browser_audio_time()*Module._helm_browser_audio_rate()));
  await expect.poll(()=>page.evaluate(()=>Module._helm_browser_audio_peak())).toBeGreaterThan(.001);
  expect(failures).toEqual([]);expect(errors).toEqual([]);
});
