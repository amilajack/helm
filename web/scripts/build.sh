#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
if [[ ! -f "$root/web/node_modules/esbuild/package.json" ]]; then
  echo 'Run pnpm install --frozen-lockfile before building the browser targets.' >&2
  exit 1
fi
version=6.0.10
sdk="$root/.toolchains/emsdk-$version"
if [[ ! -d "$sdk/.git" ]]; then
  git clone --depth 1 --branch "$version" https://github.com/emscripten-core/emsdk.git "$sdk"
fi
if [[ $(git -C "$sdk" rev-parse HEAD) != a2b92777574c2feda07994cd4f1079a3dfc151f8 ]]; then
  echo 'Unexpected emsdk revision.' >&2
  exit 1
fi
"$sdk/emsdk" install "$version"
"$sdk/emsdk" activate "$version"
source "$sdk/emsdk_env.sh"
build="$root/build/wasm"
if [[ -f "$build/CMakeCache.txt" ]] && ! python3 - "$build/CMakeCache.txt" "$sdk" <<'PY'
import pathlib,sys
cache=pathlib.Path(sys.argv[1]).read_text()
toolchain=next((line.split('=',1)[1] for line in cache.splitlines() if line.startswith('CMAKE_TOOLCHAIN_FILE:')),'')
raise SystemExit(0 if toolchain and pathlib.Path(toolchain).resolve().is_relative_to(pathlib.Path(sys.argv[2]).resolve()) else 1)
PY
then
  echo 'Toolchain changed; removing only the generated browser build.'
  rm -rf "$build"
fi
emcmake cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel "${HELM_BUILD_JOBS:-4}"
