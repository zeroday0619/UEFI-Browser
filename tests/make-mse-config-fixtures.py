#!/usr/bin/env python3
"""Generate consecutive Opus segments with different channel configurations."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent / 'input-pages'
for name, expression in (
    ('mono', '0.25*sin(2*PI*440*t)'),
    ('stereo', '0.25*sin(2*PI*880*t)|0.25*sin(2*PI*1100*t)'),
):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
        '-f', 'lavfi', '-i', f'aevalsrc={expression}:s=48000:d=8',
        '-c:a', 'libopus', '-b:a', '128k', '-threads', '1',
        str(root / f'media-config-{name}.webm')], check=True)
