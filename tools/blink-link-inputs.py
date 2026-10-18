#!/usr/bin/env python3
"""Export native GN runtime archives that are not merged into libbrowser.a."""
import json
from pathlib import Path
import sys
build = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2])
lines = (build / 'obj/third_party/blink/uefi/browser.ninja').read_text().splitlines()
edge = next(line for line in lines if line.startswith('build obj/third_party/blink/uefi/libbrowser.a:'))
archives = [build / 'obj/buildtools/third_party/libc++/libc++.a',
            build / 'obj/buildtools/third_party/libc++abi/libc++abi.a']
archives.extend((build / token).resolve() for token in edge.split() if token.endswith('.rlib'))
for archive in archives:
    if not archive.is_file():
        raise SystemExit('Missing native runtime archive: ' + str(archive))
output.write_text('\n'.join(json.dumps(str(archive)) for archive in dict.fromkeys(archives)) + '\n')
print(f'Added {len(dict.fromkeys(archives))} native C++ and Rust runtime archives.')
