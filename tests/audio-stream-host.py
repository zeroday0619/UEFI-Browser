#!/usr/bin/env python3
"""Exercise the production DMA queue against a bounded AC97 register model."""
from pathlib import Path
import subprocess
project = Path(__file__).resolve().parents[1]
build = project / 'build/audio-stream-host'
build.mkdir(parents=True, exist_ok=True)
source = (project / 'src/audio/ac97.c').read_text()
start = source.index('typedef struct {')
end = source.index('static Clip clips[8];')
(build / 'audio-stream-types.h').write_text(source[start:end])
subprocess.run(['clang','-std=c11','-g','-O1','-fsanitize=address,undefined',
 '-fno-omit-frame-pointer','-I'+str(build),'-I'+str(project/'runtime'),
 str(project/'tests/audio-stream-host.c'),'-o',str(build/'check')],check=True)
subprocess.run([str(build/'check')],check=True)
