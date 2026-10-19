#!/usr/bin/env python3
"""Factory manifest and content-addressed objects; patches download on demand."""
from pathlib import Path
import hashlib,json,sys
root,out=map(Path,sys.argv[1:]);(out/'objects').mkdir(parents=True,exist_ok=True)
entries=[]
for path in sorted(root.rglob('*.helm')):
    data=path.read_bytes();digest=hashlib.sha256(data).hexdigest();target=out/'objects'/digest
    if not target.exists(): target.write_bytes(data)
    entries.append({'path':path.relative_to(root).as_posix(),'sha256':digest,'size':len(data)})
(out/'manifest.json').write_text(json.dumps({'version':1,'patches':entries},separators=(',',':'))+'\n')

for path in root.rglob('LICENSE'):
    target=out/'licenses'/path.relative_to(root)
    target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(path.read_bytes())
