#!/usr/bin/env python3
"""Validate a QEMU screendump against the Vulkan triangle fixture and save PNG."""
import argparse
import json
from pathlib import Path
import struct
import zlib
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('image', type=Path)
parser.add_argument('report', type=Path)
options = parser.parse_args()
header = options.image.read_bytes().split(b'\n', 3)
assert header[0] == b'P6' and header[2] == b'255', 'Unexpected QEMU screenshot format'
width, height = map(int, header[1].split())
pixels = header[3]
assert width >= 544 and height >= 608 and len(pixels) == width * height * 3

def pixel(x, y):
    offset = (y * width + x) * 3
    return tuple(pixels[offset:offset+3])

interior = background = 0
for y in range(128):
    for x in range(128):
        actual = pixel(32 + x * 4 + 2, 96 + y * 4 + 2)
        blue = (y + 0.5 - 16) / 96
        green = ((x + 0.5 - 16) - 48 * blue) / 96
        red = 1 - green - blue
        if min(red, green, blue) > 0.02:
            expected = tuple(value * 255 for value in (red, green, blue))
            assert all(abs(value - reference) <= 2 for value, reference in zip(actual, expected)), (x, y, actual, expected)
            interior += 1
        elif min(red, green, blue) < -0.02:
            assert actual == (16, 32, 48), (x, y, actual)
            background += 1
assert interior >= 3500 and background >= 10000
assert pixel(0, 0) == (12, 18, 28), 'Missing independent framebuffer presentation'

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
rows = b''.join(b'\0' + pixels[y*width*3:(y+1)*width*3] for y in range(height))
output = options.image.with_suffix('.png')
output.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                   chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
report = {'width': width, 'height': height, 'triangle_pixels': interior, 'background_pixels': background,
          'screenshot': str(output), 'passed': True}
options.report.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
