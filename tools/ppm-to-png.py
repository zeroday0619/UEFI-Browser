#!/usr/bin/env python3
"""Convert a QEMU RGB framebuffer capture to PNG without changing its pixels."""
import argparse
import pathlib
import struct
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('input', type=pathlib.Path)
parser.add_argument('output', type=pathlib.Path)
arguments = parser.parse_args()
magic, dimensions, maximum, pixels = arguments.input.read_bytes().split(b'\n', 3)
if magic != b'P6' or maximum != b'255':
    raise SystemExit('Expected an 8-bit binary RGB PPM capture.')
width, height = map(int, dimensions.split())
if len(pixels) != width * height * 3:
    raise SystemExit('The framebuffer byte count does not match its dimensions.')
def chunk(name, data):
    return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
rows = b''.join(b'\0' + pixels[row * width * 3:(row + 1) * width * 3] for row in range(height))
arguments.output.write_bytes(b'\x89PNG\r\n\x1a\n' +
    chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
    chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
