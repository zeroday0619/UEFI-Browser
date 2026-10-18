#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd "$project/../.." && pwd)
python3 "$project/prepare.py"
"$project/build-musl.sh"
mkdir -p "$project/build/musl/uapi"
for header in linux asm-generic; do
 ln -sfn "/usr/include/$header" "$project/build/musl/uapi/$header"
done
ln -sfn /usr/include/x86_64-linux-gnu/asm "$project/build/musl/uapi/asm"
python3 - "$project" "$root" "$(clang -print-resource-dir)" <<'PY'
import pathlib
import sys
project, root, resource = sys.argv[1:]
for component in ['v8', 'skia']:
    text = (pathlib.Path(project) / f'config/{component}-musl.gn').read_text()
    text = text.replace('@WORKSPACE@', root).replace('@CLANG_RESOURCE@', resource)
    output = pathlib.Path(root) / 'work/chromium-build' / component / 'out/musl'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'args.gn').write_text(text)
PY
"$root/work/chromium-build/tools/gn" gen "$root/work/chromium-build/v8/out/musl" --root="$root/work/chromium-build/v8"
ninja -C "$root/work/chromium-build/v8/out/musl" -j"${JOBS:-12}" v8_monolith buildtools/third_party/libc++:libc++
"$root/work/chromium-build/tools/gn" gen "$root/work/chromium-build/skia/out/musl" --root="$root/work/chromium-build/skia"
ninja -C "$root/work/chromium-build/skia/out/musl" -j"${JOBS:-12}" skia
ENABLE_V8=1 ENABLE_SKIA=1 BUILD_DIRECTORY="$project/build/v8-musl" "$project/build-musl.sh"
BUILD_DIRECTORY="$project/build/v8-musl" python3 "$project/validate-binary.py"
