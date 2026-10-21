#!/usr/bin/env python3
"""Exercise production POST watchdog handling with bounded firmware callbacks."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='post-watchdog-') as directory:
    executable = Path(directory) / 'check'
    subprocess.run(['clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I' + str(project / 'runtime'),
                    str(project / 'tests/post-watchdog-host.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
