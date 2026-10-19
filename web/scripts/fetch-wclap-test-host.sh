#!/usr/bin/env bash
# Fetches the WebCLAP browser test host (and the submodules it serves) at a pinned
# commit into build/wclap-test-host, for web/tests/wclap.spec.js.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
host="$root/build/wclap-test-host"
revision=b42ade615bef2c989f96400a7b0b2ef15cabd396
if [[ ! -d "$host/.git" ]] || [[ $(git -C "$host" rev-parse HEAD) != "$revision" ]]; then
  rm -rf "$host"
  git init -q "$host"
  git -C "$host" fetch -q --depth 1 https://github.com/WebCLAP/browser-test-host.git "$revision"
  git -C "$host" checkout -q FETCH_HEAD
fi
# Submodule revisions are pinned by the host commit above.
git -C "$host" submodule update -q --init --depth 1 clap-audionode/wclap-js examples
git -C "$host/clap-audionode/wclap-js" submodule update -q --init --depth 1 es6/wasi
echo "$host"
