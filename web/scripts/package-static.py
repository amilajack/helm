#!/usr/bin/env python3
"""Package a content-addressed static site with matching corresponding source."""
from pathlib import Path
import argparse,gzip,hashlib,io,json,shutil,subprocess,tarfile
root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--build',type=Path,default=root/'build/wasm')
parser.add_argument('--output',type=Path,default=root/'build/web-dist')
parser.add_argument('--juce',type=Path)
parser.add_argument('--wclap',type=Path,default=root/'build/wclap')
args=parser.parse_args();web=args.build/'web'
juce=args.juce
if juce is None and (args.build/'juce-source-path.txt').exists():
    juce=Path((args.build/'juce-source-path.txt').read_text())
if juce is None:
    cache=(args.build/'CMakeCache.txt').read_text()
    override=next((line.split('=',1)[1] for line in cache.splitlines() if line.startswith('FETCHCONTENT_SOURCE_DIR_JUCE_SOURCE:')),'')
    juce=Path(override) if override else args.build/'_deps/juce_source-src'
if not (juce/'modules/juce_core/juce_core.h').is_file():raise SystemExit('Pass --juce pointing to the pinned JUCE source')
for dependency in ['sdk','api']:
    if not (root/'web/node_modules/@webaudiomodules'/dependency/'package.json').is_file():
        raise SystemExit('Install WAM dependencies with pnpm install --frozen-lockfile before packaging')
files=[p for p in web.iterdir() if p.suffix in ['.html','.js','.wasm']]
files += [p for folder in ['library','fonts-fallback'] for p in (web/folder).rglob('*') if p.is_file()]
files += [p for p in (web/'wam').iterdir() if p.is_file() and p.suffix in ['.html','.js','.mjs','.json','.wasm','.txt']]
if not all((web/name).exists() for name in ['helm.html','helm.js','helm.wasm']):raise SystemExit('Build HelmBrowser first')
wclap_bundle=args.wclap/'Helm.wclap.tar.gz'
if not wclap_bundle.is_file():raise SystemExit('Build the WCLAP bundle first: web/scripts/build-wclap.sh')
clap=Path((args.wclap/'clap-source-path.txt').read_text())
if not (clap/'include/clap/clap.h').is_file():raise SystemExit('Missing pinned CLAP headers; rebuild the WCLAP target')
# Source files include uncommitted implementation changes but exclude ignored
# toolchains/build products. A source snapshot is bundled with every release.
paths=subprocess.check_output(['git','ls-files','-z','--cached','--others','--exclude-standard'],cwd=root).decode().split('\0')
source_files=sorted({root/p for p in paths if p and (root/p).is_file()})
source_digest=hashlib.sha256()
for p in source_files:source_digest.update(p.relative_to(root).as_posix().encode()+b'\0'+p.read_bytes())
manifest={p.relative_to(web).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(files)}
manifest['wclap/Helm.wclap.tar.gz']=hashlib.sha256(wclap_bundle.read_bytes()).hexdigest()
version=hashlib.sha256(json.dumps({'files':manifest,'source':source_digest.hexdigest()},sort_keys=True).encode()).hexdigest()[:20]
destination=args.output/version;destination.mkdir(parents=True,exist_ok=True)
for p in files:
    target=destination/p.relative_to(web);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
(destination/'wclap').mkdir(exist_ok=True);shutil.copy2(wclap_bundle,destination/'wclap/Helm.wclap.tar.gz')
source=destination/'source.tar.gz'
def normalized(info):
    info.mtime=0;info.uid=info.gid=0;info.uname=info.gname=''
    info.mode=0o755 if info.mode & 0o111 else 0o644
    return info
with source.open('wb') as raw, gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0) as compressed, tarfile.open(fileobj=compressed,mode='w') as archive:
    for p in source_files:archive.add(p,arcname='helm/'+p.relative_to(root).as_posix(),filter=normalized)
    for p in sorted(juce.rglob('*')):
        if p.is_file() and '.git' not in p.relative_to(juce).parts:
            archive.add(p,arcname='helm/third_party/JUCE9/'+p.relative_to(juce).as_posix(),filter=normalized)
    for p in sorted(clap.rglob('*')):
        if p.is_file() and '.git' not in p.relative_to(clap).parts:
            archive.add(p,arcname='helm/third_party/clap/'+p.relative_to(clap).as_posix(),filter=normalized)
    for dependency in ['sdk','api']:
        directory=root/'web/node_modules/@webaudiomodules'/dependency
        for p in sorted(directory.rglob('*')):
            if p.is_file():archive.add(p,arcname='helm/third_party/wam-'+dependency+'/'+p.relative_to(directory).as_posix(),filter=normalized)
    metadata=json.dumps({'sourceSha256':source_digest.hexdigest(),'juce':'3fb5cbc0340758596dac39f7c8de47d562b4896e','wamSdk':'0.0.12','wamApi':'2.0.0-alpha.6','clap':'195b42a004144fab0b3cf95e9c067187d15365b7','wasiSdk':'34'}).encode()
    info=tarfile.TarInfo('helm/SOURCE-DISTRIBUTION.json');info.size=len(metadata);archive.addfile(info,io.BytesIO(metadata))
for name,path in [('Helm-LICENSE.txt',root/'COPYING'),('JUCE-LICENSE.md',juce/'LICENSE.md')]:
    if not path.exists() and name.startswith('Helm'):path=root/'LICENSE'
    shutil.copy2(path,destination/name)
    manifest[name]=hashlib.sha256((destination/name).read_bytes()).hexdigest()
manifest['source.tar.gz']=hashlib.sha256(source.read_bytes()).hexdigest()
(destination/'integrity.json').write_text(json.dumps({'version':version,'files':manifest},indent=2)+'\n')
html=(destination/'helm.html').read_text().replace('https://github.com/amilajack/helm','source.tar.gz')
(destination/'helm.html').write_text(html)
# Recompute the HTML digest after installing its local source-download link.
manifest['helm.html']=hashlib.sha256((destination/'helm.html').read_bytes()).hexdigest()
(destination/'integrity.json').write_text(json.dumps({'version':version,'files':manifest},indent=2)+'\n')
stable=args.output/'wam';stable.mkdir(exist_ok=True)
(stable/'index.js').write_text(f'export {{ default }} from "../{version}/wam/index.js";\n')
shutil.copy2(destination/'wam/descriptor.json',stable/'descriptor.json')
stable_wclap=args.output/'wclap';stable_wclap.mkdir(exist_ok=True)
shutil.copy2(destination/'wclap/Helm.wclap.tar.gz',stable_wclap/'Helm.wclap.tar.gz')
(stable/'host.html').write_text(f'<!doctype html><meta charset="utf-8"><meta http-equiv="refresh" content="0;url=../{version}/wam/host.html"><title>Helm WAM 2</title>\n')
args.output.joinpath('index.html').write_text(f'<!doctype html><meta charset="utf-8"><meta http-equiv="refresh" content="0;url={version}/helm.html"><title>Helm</title><a href="{version}/helm.html">Open Helm</a>\n')
headers={'Cross-Origin-Opener-Policy':'same-origin','Cross-Origin-Embedder-Policy':'require-corp','Cross-Origin-Resource-Policy':'same-origin','Access-Control-Allow-Origin':'*'}
args.output.joinpath('_headers').write_text('/*\n'+''.join(f'  {k}: {v}\n' for k,v in headers.items())+'  Cache-Control: no-cache\n/'+version+'/*\n  Cache-Control: public, max-age=31536000, immutable\n')
args.output.joinpath('vercel.json').write_text(json.dumps({'headers':[{'source':'/(.*)','headers':[{'key':k,'value':v} for k,v in headers.items()]},{'source':'/'+version+'/(.*)','headers':[{'key':'Cache-Control','value':'public, max-age=31536000, immutable'}]},{'source':'/','headers':[{'key':'Cache-Control','value':'no-cache'}]}]},indent=2)+'\n')
print(destination)
