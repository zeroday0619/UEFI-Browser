#!/usr/bin/env python3
"""Exercise the actual transport's descriptor logic without executing PCI I/O."""
from pathlib import Path
import subprocess
project = Path(__file__).resolve().parents[1]
source = project / 'build/venus-runtime-source/mesa-26.2.5'
output = project / 'build/venus-transport-host'
subprocess.run(['clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-pthread', '-ffunction-sections',
                '-Wl,--gc-sections', '-I' + str(project / 'runtime'),
                '-I' + str(source / 'src'), str(project / 'tests/venus-transport-host.c'),
                '-o', str(output)], check=True)
subprocess.run([str(output)], check=True)
