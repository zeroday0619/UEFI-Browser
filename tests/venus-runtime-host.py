#!/usr/bin/env python3
"""Run the actual Venus backend with deterministic host transport fault injection."""
import json
from pathlib import Path
import shlex
import subprocess

project = Path(__file__).resolve().parents[1]
build = project / 'build/venus-runtime'
commands = json.loads((build / 'compile_commands.json').read_text())
entry = next(value for value in commands if value['file'].endswith('vn_renderer_uefi.c'))
arguments = shlex.split(entry['command'])
flags = [value for value in arguments if value.startswith(('-I', '-D'))
         and value not in ('-DNDEBUG', '-D_FORTIFY_SOURCE=2')]
output = project / 'build/venus-runtime-host'
source = project / 'build/venus-runtime-source/mesa-26.2.5/src/virtio/vulkan'
subprocess.run(['clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-pthread', *flags,
                '-I' + str(source), str(project / 'tests/venus-runtime-host.c'),
                '-o', str(output)], cwd=build, check=True)
subprocess.run([str(output)], check=True)
