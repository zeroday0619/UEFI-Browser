#!/usr/bin/env python3
"""Run the production libhangul adapter under address and undefined-behavior sanitizers."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
subprocess.run(['python3', str(project / 'prepare-libhangul.py')], check=True)
source = project.parents[1] / 'work/libhangul/hangul'
with tempfile.TemporaryDirectory(prefix='uefi-hangul-') as directory:
    output = Path(directory)
    objects = []
    for name in ('hangulctype', 'hangulinputcontext', 'hangulkeyboard'):
        target = output / (name + '.o')
        subprocess.run(['clang', '-g', '-fsanitize=address,undefined',
                        '-DENABLE_EXTERNAL_KEYBOARDS=0', '-c', str(source / (name + '.c')),
                        '-o', str(target)], check=True)
        objects.append(str(target))
    executable = output / 'korean-input'
    subprocess.run(['clang++', '-std=c++20', '-g', '-fsanitize=address,undefined',
                    '-I' + str(project / 'src'), '-I' + str(source),
                    str(project / 'src/blink/korean_input.cc'),
                    str(project / 'tests/korean-input.cc'), *objects, '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
