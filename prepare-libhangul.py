#!/usr/bin/env python3
"""Fetch and copy the pinned libhangul source without altering upstream files."""
import json
from pathlib import Path
import shutil
import subprocess

project = Path(__file__).resolve().parent
source = project.parents[1] / 'work/libhangul'
revision = json.loads((project / 'revisions.json').read_text())['libhangul']
if not (source / '.git').exists():
    if source.exists() and any(source.iterdir()):
        raise SystemExit('Refusing to replace a nonempty libhangul directory.')
    source.mkdir(parents=True, exist_ok=True)
    subprocess.run(['git', 'init', '-q', str(source)], check=True)
    subprocess.run(['git', '-C', str(source), 'remote', 'add', 'origin',
                    'https://github.com/libhangul/libhangul.git'], check=True)
    subprocess.run(['git', '-C', str(source), 'fetch', '--depth=1', 'origin', revision], check=True)
    subprocess.run(['git', '-C', str(source), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
if actual != revision:
    raise SystemExit('Existing libhangul checkout differs from revisions.json.')
if (source / 'COPYING').read_bytes() != (project / 'licenses/libhangul-COPYING').read_bytes():
    raise SystemExit('libhangul license text differs from the pinned source.')
if subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--', 'hangul', 'COPYING']):
    raise SystemExit('libhangul source has local changes; record them as a reviewed patch first.')
target = project.parents[1] / 'work/chromium-build/chromium/third_party/blink/uefi/libhangul'
shutil.copytree(source / 'hangul', target / 'hangul', dirs_exist_ok=True)
shutil.copyfile(source / 'COPYING', target / 'COPYING')
print('Prepared unmodified libhangul ' + actual + ' with built-in keyboard tables.')
