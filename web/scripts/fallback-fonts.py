#!/usr/bin/env python3
"""Pin the same on-demand CJK/emoji fonts as the Surge migration."""
from pathlib import Path
from urllib.request import urlopen
import hashlib,sys
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
base='https://raw.githubusercontent.com/amilajack/surge/644d2db8c8d062818b2e2436fe386402a6709ea7/resources/fonts-fallback/'
files={
'NotoSansSC-Regular.otf':'faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9',
'NotoSansKR-Regular.otf':'69975a0ac8472717870aefeab0a4d52739308d90856b9955313b2ad5e0148d68',
'NotoEmoji-Regular.ttf':'de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551',
}
for name,digest in files.items():
    path=out/name
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest()==digest:continue
    with urlopen(base+name,timeout=60) as response:data=response.read()
    if hashlib.sha256(data).hexdigest()!=digest:raise RuntimeError('Font checksum mismatch: '+name)
    path.write_bytes(data)
for name in ['LICENSE-NotoSansCJK.txt','OFL-NotoEmoji.txt']:
    path=out/name
    if not path.exists():
        with urlopen(base+name,timeout=60) as response:path.write_bytes(response.read())
