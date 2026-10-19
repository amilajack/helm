#!/usr/bin/env python3
"""Emit the same resource symbols as juceaide without running a cross-built tool."""
from pathlib import Path
import re, sys
root, out = map(Path, sys.argv[1:]); out.mkdir(parents=True, exist_ok=True)
manifest=(root/'cmake/Sources.cmake').read_text().split('set(HELM_RESOURCES',1)[1].split(')',1)[0]
header=['#pragma once\nnamespace BinaryData {']; source=['#include "BinaryData.h"\nnamespace BinaryData {']
for relative in manifest.split():
    path=root/relative; name=re.sub(r'[^a-zA-Z0-9_]', '_', path.name.replace('-', '')); data=path.read_bytes()
    header += [f'extern const char* {name};', f'const int {name}Size = {len(data)};']
    source += [f'static const unsigned char data_{name}[] = {{'+','.join(map(str,data))+',0,0};', f'const char* {name} = reinterpret_cast<const char*>(data_{name});']
header+=['}'];source+=['}']
for name,lines in [('BinaryData.h',header),('BinaryData.cpp',source)]:
    path=out/name; text='\n'.join(lines)+'\n'
    if not path.exists() or path.read_text()!=text: path.write_text(text)
