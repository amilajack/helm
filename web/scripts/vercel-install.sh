#!/usr/bin/env bash
set -euo pipefail
# Vercel's Amazon Linux build image supplies make, Git and a C++ compiler.
if ! command -v cmake >/dev/null || ! command -v python3 >/dev/null; then
  dnf install -y cmake python3
fi
cmake --version
python3 --version
if command -v pnpm >/dev/null; then
  pnpm install --frozen-lockfile
else
  npx --yes pnpm@12.14.0 install --frozen-lockfile
fi
