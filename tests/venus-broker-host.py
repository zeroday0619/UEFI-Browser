#!/usr/bin/env python3
"""Validate GPU broker ownership and failure paths with an injected device."""
from pathlib import Path
import subprocess
project = Path(__file__).resolve().parents[1]
output = project / 'build/venus-broker-host'
subprocess.run(['clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-I' + str(project / 'runtime'),
                str(project / 'tests/venus-broker-host.c'), '-o', str(output)], check=True)
subprocess.run([str(output)], check=True)
