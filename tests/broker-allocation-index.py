#!/usr/bin/env python3
"""Validate broker allocation lookup against an independent linear reference."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='uefi-broker-index-') as directory:
    executable = Path(directory) / 'broker-index'
    subprocess.run(['clang', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(project / 'tests/broker-allocation-index.c'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
