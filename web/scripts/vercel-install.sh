#!/usr/bin/env bash
set -euo pipefail
# Vercel's Amazon Linux build image supplies make, Git and a C++ compiler.
if ! command -v cmake >/dev/null || ! command -v python3 >/dev/null; then
  dnf install -y cmake python3
fi
cmake --version
python3 --version
