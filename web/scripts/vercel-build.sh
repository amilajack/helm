#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

# Some deployment sources omit .git. Recover only the exact deployed commit's
# index for the corresponding-source archive; never replace the build's files.
if [[ $(git rev-parse --show-toplevel 2>/dev/null || true) != "$root" ]]; then
  revision=${VERCEL_GIT_COMMIT_SHA:?Vercel must provide the deployed Git commit}
  if [[ ! "$revision" =~ ^[0-9a-f]{40}$ ]]; then
    echo 'Invalid VERCEL_GIT_COMMIT_SHA' >&2
    exit 1
  fi
  git init .
  git fetch --depth 1 https://github.com/amilajack/helm.git "$revision"
  git read-tree FETCH_HEAD
fi

python3 web/scripts/feature-inventory.py
HELM_BUILD_JOBS=${HELM_BUILD_JOBS:-2} bash web/scripts/build.sh
python3 web/scripts/package-static.py
