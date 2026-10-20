#!/usr/bin/env python3
"""Generate stereo PCM WAV signals for device-output verification."""
from pathlib import Path
import math
import struct
import wave
root = Path(__file__).resolve().parent / 'input-pages'
with wave.open(str(root / 'audio-tone.wav'), 'wb') as output:
    output.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
    samples = bytearray()
    for frame in range(8 * 48000):
        left, right = (440, 660) if frame < 4 * 48000 else (880, 1100)
        samples += struct.pack('<hh', round(10000 * math.sin(2 * math.pi * left * frame / 48000)),
                               round(10000 * math.sin(2 * math.pi * right * frame / 48000)))
    output.writeframes(samples)
(root / 'audio-invalid.wav').write_bytes(b'RIFF' + struct.pack('<I', 4) + b'WAVE')
for name, sample_rate, width, frequency in [('audio-mono8.wav', 22050, 1, 440), ('audio-mono16.wav', 16000, 2, 330)]:
    with wave.open(str(root / name), 'wb') as output:
        output.setparams((1, width, sample_rate, 0, 'NONE', 'not compressed'))
        if width == 1:
            data = bytes(round(128 + 40 * math.sin(2 * math.pi * frequency * frame / sample_rate)) for frame in range(sample_rate))
        else:
            data = b''.join(struct.pack('<h', round(8000 * math.sin(2 * math.pi * frequency * frame / sample_rate))) for frame in range(sample_rate))
        output.writeframes(data)
