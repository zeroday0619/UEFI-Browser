#!/usr/bin/env python3
"""Verify the AC97 device output captured by QEMU's WAV audio backend."""
import argparse
from array import array
import json
import math
from pathlib import Path
import sys
import struct
import hashlib
import wave
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('wave',type=Path)
parser.add_argument('report',type=Path)
options=parser.parse_args()
raw = options.wave.read_bytes()
assert raw[:4] == b'RIFF' and raw[8:16] == b'WAVEfmt ' and raw[36:40] == b'data', 'Unexpected QEMU WAV header'
assert struct.unpack_from('<I', raw, 16)[0] == 16 and struct.unpack_from('<H', raw, 20)[0] == 1
channels,rate,byte_rate,alignment,bits = struct.unpack_from('<HIIHH', raw, 22)
assert (channels,rate,byte_rate,alignment,bits) == (2,48000,192000,4,16)
payload = raw[44:]
assert len(payload) % alignment == 0
riff_size, data_size = struct.unpack_from('<I',raw,4)[0], struct.unpack_from('<I',raw,40)[0]
assert (riff_size,data_size) in ((0,0),(len(raw)-8,len(payload))), 'Inconsistent WAV sizes'
frames = len(payload)//alignment
data = array('h',payload)
# QEMU 11.1.2 can leave zero size fields even after a normal QMP quit. Preserve
# the raw capture and write a separate playable container with identical PCM.
normalized = options.report.with_name(options.report.name.replace('-spectrum.json','-playable.wav'))
with wave.open(str(normalized),'wb') as destination:
    destination.setparams((channels,2,rate,0,'NONE','not compressed'))
    destination.writeframes(payload)
with wave.open(str(normalized),'rb') as playable:
    assert playable.readframes(playable.getnframes()) == payload, 'Normalized container changed PCM bytes'
if sys.byteorder!='little': data.byteswap()
window=4800
records=[]
for start in range(0,frames-window+1,window):
    samples=[data[(start+index)*2:(start+index)*2+2] for index in range(window)]
    rms=[math.sqrt(sum(pair[channel]**2 for pair in samples)/window) for channel in range(2)]
    frequencies=[]
    for channel in range(2):
        crossings=sum(samples[index-1][channel]<=0<samples[index][channel] for index in range(1,window))
        frequencies.append(crossings*rate/window)
    records.append({'start_seconds':round(start/rate,3),'rms':rms,'frequency':frequencies})
def tone(record,left,right,low,high):
    return all(low<value<high for value in record['rms']) and abs(record['frequency'][0]-left)<=15 and abs(record['frequency'][1]-right)<=15
first=[record for record in records if tone(record,440,660,5000,8500)]
second=[record for record in records if tone(record,880,1100,5000,8500)]
quiet=[record for record in records if tone(record,880,1100,900,2500)]
mono8=[record for record in records if tone(record,440,440,5000,8500)]
mono16=[record for record in records if tone(record,330,330,4500,6500)]
silence=[record for record in records if all(value<2 for value in record['rms'])]
report={'raw_header_finalized': data_size != 0, 'raw_capture_sha256':hashlib.sha256(raw).hexdigest(), 'pcm_sha256':hashlib.sha256(payload).hexdigest(), 'sample_rate':rate,'channels':channels,'frames':frames,'duration_seconds':frames/rate,
        'first_stereo_tone_windows':len(first),'second_stereo_tone_windows':len(second),
        'mono8_windows':len(mono8),'mono16_windows':len(mono16),'attenuated_tone_windows':len(quiet),'silent_windows':len(silence),'windows':records}
options.report.write_text(json.dumps(report,indent=2)+'\n')
assert len(mono8)>=6 and len(mono16)>=6, 'Mono conversion or native sample-rate playback failed'
assert len(first)>=12, 'Missing first stereo tone during normal and busy-JavaScript playback'
assert len(second)>=4, 'Missing second stereo tone after seeking'
assert len(quiet)>=3, 'Volume attenuation not observed in device output'
assert len([record for record in silence if second and quiet and second[0]['start_seconds'] < record['start_seconds'] < quiet[0]['start_seconds']])>=3, 'Mute not observed between loud and quiet output'
assert first[0]['start_seconds']<second[0]['start_seconds']<quiet[0]['start_seconds'], 'Unexpected tone order'
print('PASS: actual AC97 stereo output, seek frequencies, mute and volume attenuation')
