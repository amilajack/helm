#!/usr/bin/env bash
# Builds the WCLAP bundle (build/wclap/Helm.wclap.tar.gz) with a pinned wasi-sdk.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
if [[ ! -f "$root/web/node_modules/esbuild/package.json" ]]; then
  echo 'Run pnpm install --frozen-lockfile before building the WCLAP target.' >&2
  exit 1
fi
version=34
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) platform=arm64-macos; digest=9c59398106b417f8f14913380fdf0097a8cc0ff4af9eb3ce0065a859e88d49e9 ;;
  Darwin-x86_64) platform=x86_64-macos; digest=87d27fa8adc68dee59bfbf2e22a6d34ef717c34d6bf1d8af2a56fc929d9ce0eb ;;
  Linux-x86_64) platform=x86_64-linux; digest=b761e3a0721dbae9c09a0059e5fdb2bf917d1b4a8a7b430fb3b5aafb0984b2c4 ;;
  Linux-aarch64) platform=arm64-linux; digest=f7e243dff54d60bcc576e94d6166b69f410f2500ae4a9ceef34315be10e77971 ;;
  *) echo "No pinned wasi-sdk for $(uname -s) $(uname -m)." >&2; exit 1 ;;
esac
sdk="$root/.toolchains/wasi-sdk-$version.0-$platform"
if [[ ! -f "$sdk/.helm-verified" ]]; then
  # Recreate only this generated SDK; never trust a partially extracted cache.
  rm -rf "$sdk" "$sdk.tar.gz"
  mkdir -p "$root/.toolchains"
  curl -fsSL -o "$sdk.tar.gz" \
    "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-$version/wasi-sdk-$version.0-$platform.tar.gz"
  actual=$(python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$sdk.tar.gz")
  if [[ "$actual" != "$digest" ]]; then
    echo 'Unexpected wasi-sdk archive digest.' >&2
    rm -f "$sdk.tar.gz"
    exit 1
  fi
  tar -xzf "$sdk.tar.gz" -C "$root/.toolchains"
  rm -f "$sdk.tar.gz"
  touch "$sdk/.helm-verified"
fi
build="$root/build/wclap"
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
  -DWASI_SDK_PREFIX="$sdk" -DCMAKE_TOOLCHAIN_FILE="$sdk/share/cmake/wasi-sdk-p1.cmake"
cmake --build "$build" --parallel "${HELM_BUILD_JOBS:-4}"
