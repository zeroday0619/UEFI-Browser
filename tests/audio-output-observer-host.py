#!/usr/bin/env python3
"""Check that the timed audio verifier rejects padding, stalls and delayed bursts."""
import io
import math
from pathlib import Path
import runpy
import struct
import wave

module = runpy.run_path(str(Path(__file__).with_name('audio-output-observer.py')))


def capture(silent_after=None):
    pcm = bytearray()
    for index in range(72000):
        left = round(8000 * math.sin(index * math.tau * 440 / 48000))
        right = round(8000 * math.sin(index * math.tau * 660 / 48000))
        if silent_after is not None and index >= silent_after * 48000:
            left = right = 0
        pcm.extend(struct.pack('<hh', left, right))
    buffer = io.BytesIO()
    with wave.open(buffer, 'wb') as output:
        output.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
        output.writeframes(pcm)
    return buffer.getvalue()


def observe(raw, mode='paced'):
    samples = []
    for index in range(76):
        seconds = index * .02
        output_time = min(seconds, .4) if mode == 'stalled' else seconds
        if mode == 'burst':
            output_time = 0 if seconds < 1.25 else 1.5
        byte_count = 4096 * int(output_time * 192000 / 4096)
        samples.append({'seconds': seconds, 'bytes': 44 + byte_count})
    module['annotate_pcm'](samples, raw)
    return module['evaluate'](samples, 0, 1.5)


raw = capture()
assert all(observe(raw)['checks'].values())
padding = observe(capture(silent_after=.4))
assert padding['checks']['pcm_output_realtime']
assert not padding['checks']['signal_output_realtime']
assert not padding['checks']['signal_gap_bounded']
for mode in ('stalled', 'burst'):
    result = observe(raw, mode)
    assert not result['checks']['signal_gap_bounded']
for samples in ([{'seconds': 0, 'bytes': 40}],
                [{'seconds': 0, 'bytes': len(raw) + 4}],
                [{'seconds': 0, 'bytes': 45}],
                [{'seconds': 0, 'bytes': 48}, {'seconds': .02, 'bytes': 44}]):
    try:
        module['annotate_pcm'](samples, raw)
    except AssertionError:
        pass
    else:
        raise AssertionError('Invalid capture boundary was accepted')
print('PASS: paced tones accepted; silence padding, underrun, delayed burst and invalid capture boundaries rejected')
