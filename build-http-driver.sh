#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$project/../.." && pwd)
source_tree="$root/work/edk2-wheel"
revision=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["edk2"])' "$project/revisions.json")
mkdir -p "$root/work" "$project/build/http-driver" "$project/validation" "$project/licenses"
if [[ ! -d "$source_tree/.git" ]]; then
 git clone --depth 1 --branch edk2-stable202608 https://github.com/tianocore/edk2.git "$source_tree"
fi
[[ $(git -C "$source_tree" rev-parse HEAD) == "$revision" ]] || {
 printf '%s\n' 'EDK II source revision does not match the pinned HTTP driver revision.' >&2
 exit 1
}
patch="$project/patches/edk2-http-cooperative.patch"
if ! git -C "$source_tree" apply --reverse --check --ignore-space-change "$patch" 2>/dev/null; then
 git -C "$source_tree" apply --check --ignore-space-change "$patch"
 git -C "$source_tree" apply --ignore-space-change "$patch"
fi
# The package metadata references this pinned header-only build input.
git -C "$source_tree" submodule update --init --depth 1 MdePkg/Library/MipiSysTLib/mipisyst
make -C "$source_tree/BaseTools/Source/C" -j"${JOBS:-8}" APPLICATIONS=GenFw CC=gcc > "$project/validation/http-driver-tools.log" 2>&1
cp "$project/config/http.dsc" "$source_tree/UefiBrowserHttp.dsc"
(
 cd "$source_tree"
 export PYTHON_COMMAND=/usr/bin/python3
 export WORKSPACE="$source_tree"
 export EDK_TOOLS_PATH="$source_tree/BaseTools"
 # The upstream environment script reads unset optional variables.
 set +u
 source edksetup.sh BaseTools
 set -u
 build -p UefiBrowserHttp.dsc -a X64 -b RELEASE -t GCC -n "${JOBS:-8}"
) > "$project/validation/http-driver-build.log" 2>&1
cp "$source_tree/Build/UefiBrowserHttp/RELEASE_GCC/X64/HttpDxe.efi" "$project/build/http-driver/HttpDxe.efi"
cp "$source_tree/License.txt" "$project/licenses/EDK-II.txt"
printf '%s\n' "$revision" > "$project/build/http-driver/revision.txt"
printf '%s\n' 'Built pinned EDK II cooperative HTTP driver.'
