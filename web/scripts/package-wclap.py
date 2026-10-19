#!/usr/bin/env python3
"""Pack Helm.wclap/ into a reproducible .wclap.tar.gz with licenses at the root."""
from pathlib import Path
import gzip,shutil,sys,tarfile
root=Path(__file__).resolve().parents[2]
bundle,output,clap_license=map(Path,sys.argv[1:4])
if not (bundle/'module.wasm').is_file():raise SystemExit('Build HelmWclap first: module.wasm is missing')
shutil.copy2(root/'COPYING',bundle/'COPYING')
shutil.copy2(clap_license,bundle/'CLAP-LICENSE.txt')
(bundle/'SOURCE.txt').write_text(
 'Helm is free software under the GNU GPL version 3 or later (see COPYING).\n'
 'Corresponding source: https://github.com/amilajack/helm, and source.tar.gz next to\n'
 'each hosted release. CLAP headers: MIT, https://github.com/free-audio/clap (1.2.10).\n'
 'Built with wasi-sdk 34. module.wasm contains wasi-libc (MIT/Apache-2.0 WITH LLVM-exception,\n'
 'including musl, MIT) and libc++ (Apache-2.0 WITH LLVM-exception):\n'
 'https://github.com/WebAssembly/wasi-sdk and https://github.com/WebAssembly/wasi-libc\n')
def normalized(info):
    info.mtime=0;info.uid=info.gid=0;info.uname=info.gname=''
    info.mode=0o755 if info.isdir() else 0o644
    return info
# Files sit at the archive root, as WCLAP hosts expect `module.wasm` there.
files=sorted(p for p in bundle.rglob('*') if p.is_file() and not p.name.startswith('.'))
temporary=output.with_suffix('.partial')
with temporary.open('wb') as raw, gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0) as compressed, \
        tarfile.open(fileobj=compressed,mode='w',format=tarfile.USTAR_FORMAT) as archive:
    for p in files:archive.add(p,arcname=p.relative_to(bundle).as_posix(),filter=normalized)
temporary.replace(output)
print(output)
