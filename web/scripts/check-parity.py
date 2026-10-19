#!/usr/bin/env python3
"""Compare production native and Wasm DSP sample-by-sample with seeded randomness."""
import argparse,array,json,math,pathlib,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--native',default=str(root/'build/dsp-native/HelmDspParity'))
parser.add_argument('--wasm',default=str(root/'build/dsp-wasm/HelmDspParity.js'))
parser.add_argument('--node',default='node')
args=parser.parse_args()
cases=list(range(11))+list(range(20,27))+list(range(30,34))+list(range(40,47))+list(range(50,63))
matrix=[(case,48000,128,24000) for case in cases]
matrix += [(case,rate,block,8192) for case in (0,44,56) for rate in (44100,96000) for block in (1,64,255,256)]
results=[]
with tempfile.TemporaryDirectory(prefix='helm-parity-') as directory:
    for case,rate,block,frames in matrix:
        values=[]
        for name,command in [('native',[args.native]),('wasm',[args.node,args.wasm]),('repeat',[args.node,args.wasm])]:
            path=pathlib.Path(directory)/f'{name}.f32'
            subprocess.run(command+[str(path),str(case),str(rate),str(block),str(frames)],check=True,capture_output=True)
            audio=array.array('f');audio.frombytes(path.read_bytes());values.append(audio)
        native,wasm,repeat=values
        assert len(native)==len(wasm)==frames*2
        assert wasm==repeat, f'Non-deterministic render: {case,rate,block}'
        assert all(math.isfinite(x) for audio in values for x in audio)
        energy=sum(x*x for x in native)
        assert energy>1e-8,f'Silent fixture: {case,rate,block}'
        relative=math.sqrt(sum((a-b)**2 for a,b in zip(native,wasm))/energy)
        peak=max(abs(a-b) for a,b in zip(native,wasm))
        result=dict(case=case,rate=rate,block=block,frames=frames,relativeRms=relative,maxDifference=peak)
        results.append(result)
        assert relative<=1e-5, result
        print(f'case {case:2} / {rate} Hz / {block:3} samples: relative RMS {relative:.3g}',flush=True)
(root/'build/wasm/parity-results.json').write_text(json.dumps(results,indent=2)+'\n')
print(f'{len(results)} native/Wasm comparisons passed (relative RMS <= 1e-5; deterministic repeats).')
