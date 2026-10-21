#!/usr/bin/env python3
"""Require both full tone segments in QEMU PCM across an Opus config change."""
from array import array
import json
import math
from pathlib import Path
import struct
import sys

raw = Path(sys.argv[1]).read_bytes()
assert raw[:4] == b'RIFF' and raw[8:16] == b'WAVEfmt ' and raw[36:40] == b'data'
assert struct.unpack_from('<HHIIHH', raw, 20) == (1, 2, 48000, 192000, 4, 16)
assert (len(raw) - 44) % 4 == 0
samples = array('h', raw[44:])
if sys.byteorder != 'little':
    samples.byteswap()
records = []
for start in range(0, len(samples) - 9599, 9600):
    channels = [samples[start + channel:start + 9600:2] for channel in (0, 1)]
    rms = [math.sqrt(sum(value * value for value in channel) / 4800) for channel in channels]
    frequency = [10 * sum(left <= 0 < right for left, right in zip(channel, channel[1:])) for channel in channels]
    tone = next((name for name, expected in (('mono', (440, 440)), ('stereo', (880, 1100)))
        if all(4500 < value < 9000 for value in rms) and
        all(abs(actual - target) <= 20 for actual, target in zip(frequency, expected))), None)
    records.append({'seconds': start / 96000, 'tone': tone, 'rms': rms, 'frequency': frequency})
durations = {name: sum(record['tone'] == name for record in records) / 10 for name in ('mono', 'stereo')}
report = {'tone_seconds': durations, 'checks': {name: 7.6 <= duration <= 8.2 for name, duration in durations.items()}, 'windows': records}
Path(sys.argv[2]).write_text(json.dumps(report, indent=2) + '\n')
assert all(report['checks'].values()), durations
print('PASS: complete mono and stereo PCM across the Opus configuration transition')
