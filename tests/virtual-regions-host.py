#!/usr/bin/env python3
"""Check the production reservation index against an independent bitmap model."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='uefi-virtual-regions-') as directory:
    executable = Path(directory) / 'virtual-regions'
    subprocess.run([
        'clang', '-g', '-O1', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', str(project / 'tests/virtual-regions-host.c'),
        '-o', str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
