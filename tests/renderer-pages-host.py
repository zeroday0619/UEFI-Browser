#!/usr/bin/env python3
"""Check the production renderer page pool with fault-injected firmware."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='uefi-renderer-pages-') as directory:
    executable = Path(directory) / 'renderer-pages'
    subprocess.run([
        'clang', '-g', '-O1', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '-I' + str(project / 'runtime'),
        str(project / 'tests/renderer-pages-host.c'), '-o', str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
