#!/usr/bin/env python3
"""Generate deterministic, video-only WebM fixtures using the host FFmpeg tool."""
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parent / 'input-pages'
for codec, name in [('libvpx', 'vp8'), ('libvpx-vp9', 'vp9')]:
    command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
               '-f', 'lavfi', '-i', 'color=c=red:s=160x120:r=10:d=1',
               '-f', 'lavfi', '-i', 'color=c=lime:s=160x120:r=10:d=1',
               '-f', 'lavfi', '-i', 'color=c=blue:s=160x120:r=10:d=1',
               '-filter_complex', '[0:v][1:v][2:v]concat=n=3:v=1:a=0,format=yuv420p[v]',
               '-map', '[v]', '-an', '-c:v', codec, '-threads', '1', '-g', '10',
               '-b:v', '200k', '-metadata', 'title=UEFI media integration fixture',
               str(root / ('media-' + name + '.webm'))]
    subprocess.run(command, check=True)
subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
                '-f', 'lavfi', '-i', 'color=c=red:s=640x480:r=10:d=7',
                '-an', '-c:v', 'libvpx', '-threads', '1', '-pix_fmt', 'yuv420p',
                '-b:v', '200k', str(root / 'media-quota.webm')], check=True)
(root / 'media-malformed.webm').write_bytes(b'Invalid WebM input.\n')
