#!/usr/bin/env python3
"""Generate clear VP8 and Opus fixtures with identifiable stereo tones."""
from pathlib import Path
import subprocess
import json
root = Path(__file__).resolve().parent / 'input-pages'
base = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y']
video = root / 'media-mse-video.webm'
audio = root / 'media-opus.webm'
subprocess.run(base + ['-f','lavfi','-i','color=c=red:s=160x120:r=10:d=2',
 '-f','lavfi','-i','color=c=lime:s=160x120:r=10:d=2',
 '-f','lavfi','-i','color=c=blue:s=160x120:r=10:d=2',
 '-filter_complex','[0:v][1:v][2:v]concat=n=3:v=1:a=0,format=yuv420p[v]',
 '-map','[v]','-an','-c:v','libvpx','-threads','1','-g','10','-b:v','200k',str(video)],check=True)
expression = 'aevalsrc=0.25*sin(2*PI*if(lt(t\\,2)\\,440\\,if(lt(t\\,4)\\,660\\,880))*t)|0.25*sin(2*PI*if(lt(t\\,2)\\,660\\,if(lt(t\\,4)\\,880\\,1100))*t):s=48000:d=6'
subprocess.run(base + ['-f','lavfi','-i',expression,'-c:a','libopus','-b:a','128k','-threads','1',str(audio)],check=True)
subprocess.run(base + ['-itsoffset','0.007','-i',str(video),'-i',str(audio),'-map','0:v','-map','1:a','-c','copy',str(root/'media-mse-av.webm')],check=True)

packets = json.loads(subprocess.check_output(['ffprobe','-v','error','-show_packets',
 '-show_entries','packet=stream_index,pts_time','-of','json',str(root/'media-mse-av.webm')]))['packets']
# FFmpeg reports decoded timestamps; WebM ordering applies before Opus pre-skip.
values = [float(item['pts_time']) + (.0065 if item['stream_index'] == 1 else 0) for item in packets]
assert all(left <= right for left, right in zip(values, values[1:])), 'Unordered coded WebM timestamps'

# The long fixture exceeds both the retained PCM ring and the blocking-script interval.
long_expression = 'aevalsrc=0.25*sin(2*PI*if(lt(t\\,10)\\,440\\,880)*t)|0.25*sin(2*PI*if(lt(t\\,10)\\,660\\,1100)*t):s=48000:d=20'
subprocess.run(base + ['-f','lavfi','-i',long_expression,'-c:a','libopus','-b:a','128k','-threads','1',str(root/'media-opus-long.webm')],check=True)
