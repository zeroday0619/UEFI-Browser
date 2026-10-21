#!/usr/bin/env python3
"""Generate immutable resource tables from Chromium's generated GRIT packs."""
import argparse
import gzip
from pathlib import Path
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('chromium', type=Path)
parser.add_argument('output', type=Path)
options = parser.parse_args()
sys.path.insert(0, str(options.chromium / 'tools/grit'))
from grit.format import data_pack

resource_directory = options.chromium / 'out/uefi-blink-native/gen/third_party/blink'
resources = {}
for name in ('public/resources/blink_resources.pak',
             'public/resources/blink_image_resources_100_percent.pak',
             'public/resources/inspector_overlay_resources.pak',
             'renderer/modules/media_controls/resources/media_controls_resources_100_percent.pak'):
    contents = data_pack.ReadDataPack(str(resource_directory / name))
    overlap = resources.keys() & contents.resources.keys()
    if overlap:
        raise SystemExit(f'Duplicate resource IDs in {name}: {sorted(overlap)}')
    for identifier, data in contents.resources.items():
        # The Inspector pack stores gzip scripts; Blink's platform API consumes decoded source.
        if name.endswith('/inspector_overlay_resources.pak') and data.startswith(b'\x1f\x8b'):
            data = gzip.decompress(data)
        resources[identifier] = data
with options.output.open('w') as output:
    output.write('/* Generated from the pinned Chromium GRIT resource packs. */\n')
    output.write('#include <stddef.h>\n#include <stdint.h>\n')
    for identifier, data in sorted(resources.items()):
        output.write(f'static const unsigned char resource_{identifier}[] = {{\n')
        for position in range(0, len(data), 24):
            output.write(','.join(str(byte) for byte in data[position:position + 24]) + ',\n')
        output.write('0};\n')
    output.write('const unsigned char *firmware_resource(int identifier, size_t *size) {\n switch (identifier) {\n')
    for identifier, data in sorted(resources.items()):
        output.write(f' case {identifier}: *size = {len(data)}; return resource_{identifier};\n')
    output.write(' default: *size = 0; return NULL;\n }\n}\n')
print(f'Embedded {len(resources)} Blink resources ({sum(map(len, resources.values()))} bytes).')
