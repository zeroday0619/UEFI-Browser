#!/usr/bin/env python3
"""Check the freestanding Venus buffer adapter under ASan and UBSan."""
import json
from pathlib import Path
import subprocess
import tempfile
project = Path(__file__).resolve().parents[1]
version = json.loads((project / 'revisions.json').read_text())['mesa_venus']['version']
headers = project / 'build/venus-source' / ('mesa-' + version) / 'include'
if not headers.exists():
    raise SystemExit('Run python3 prepare-venus.py before this test.')
with tempfile.TemporaryDirectory(prefix='uefi-venus-stream-') as directory:
    executable = Path(directory) / 'venus-stream'
    subprocess.run(['clang', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-isystem', str(headers), str(project / 'tests/venus-stream-host.c'),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
