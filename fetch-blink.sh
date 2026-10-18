#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd "$project/../.." && pwd)
target="$root/work/chromium-build/chromium"
revision=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["chromium"])' "$project/revisions.json")
if [[ -d "$target/.git" ]]; then
 [[ $(git -C "$target" rev-parse HEAD) == "$revision" ]] || { printf '%s\n' 'Existing Chromium checkout has a different revision.' >&2; exit 1; }
 exit 0
fi
mkdir -p "$target"
git -C "$target" init -q
git -C "$target" remote add origin https://chromium.googlesource.com/chromium/src.git
git -C "$target" config remote.origin.promisor true
git -C "$target" config remote.origin.partialclonefilter blob:none
git -C "$target" fetch --depth=1 --filter=blob:none origin "$revision"
git -C "$target" sparse-checkout init --cone
git -C "$target" sparse-checkout set third_party/blink base build buildtools ui mojo content
git -C "$target" checkout --detach FETCH_HEAD
