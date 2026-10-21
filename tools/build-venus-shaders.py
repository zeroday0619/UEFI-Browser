#!/usr/bin/env python3
"""Compile the standalone Venus rendering fixture into SPIR-V word arrays."""
import argparse
from pathlib import Path
import struct
import subprocess
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
options = parser.parse_args()
project = Path(__file__).resolve().parents[1]
options.output.mkdir(parents=True, exist_ok=True)
arrays = []
for stage, name in [('vert', 'vertex'), ('frag', 'fragment')]:
    source = project / ('src/gpu/venus/shaders/triangle.' + stage)
    binary = options.output / ('triangle.' + stage + '.spv')
    subprocess.run(['glslc', '--target-env=vulkan1.1', '-O', str(source), '-o', str(binary)], check=True)
    data = binary.read_bytes()
    if len(data) % 4 or len(data) > 3000 or struct.unpack_from('<I', data)[0] != 0x07230203:
        raise SystemExit('Unexpected shader module size or SPIR-V header.')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    arrays.append('static const uint32_t venus_' + name + '_shader[] = {\n' +
                  '\n'.join('  ' + ','.join(f'0x{value:08x}' for value in words[index:index+8]) + ','
                            for index in range(0, len(words), 8)) + '\n};\n')
(options.output / 'venus-shaders.h').write_text(''.join(arrays))
