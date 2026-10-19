#!/usr/bin/env python3
"""Pin the shared source surface reviewed for the browser port; fail on drift."""
from pathlib import Path
import argparse,hashlib,json,re
root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser();parser.add_argument('--update',action='store_true');args=parser.parse_args()
manifest=(root/'cmake/Sources.cmake').read_text()
common=manifest.split('set(HELM_COMMON_SOURCES',1)[1].split(')',1)[0].split()
paths=common+[p.relative_to(root).as_posix() for p in (root/'src/browser').iterdir() if p.is_file()]
entries=[]
for name in sorted(paths):
    text=(root/name).read_text()
    entries.append({'path':name,'sha256':hashlib.sha256(text.encode()).hexdigest(),
      'methods':sorted(set(re.findall(r'\b([A-Za-z_][\w]*::[A-Za-z_][\w]*)\s*\(',text)))})
current={'schema':1,'reference':'amilajack/surge@644d2db8c8d062818b2e2436fe386402a6709ea7','files':entries}
path=root/'web/feature-review.json'
if args.update:path.write_text(json.dumps(current,indent=2)+'\n')
elif not path.exists() or json.loads(path.read_text())!=current:raise SystemExit('Browser source inventory changed. Review affected behavior, update tests, then run feature-inventory.py --update.')
print(f'{len(entries)} shared/browser translation units and headers accounted for.')
