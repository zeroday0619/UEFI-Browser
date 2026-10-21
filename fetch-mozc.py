#!/usr/bin/env python3
"""Fetch the approved Mozc source and dictionary at the recorded revision."""
import json
from pathlib import Path
import subprocess

project = Path(__file__).resolve().parent
destination = project.parents[1] / 'work/mozc'
revision = json.loads((project / 'revisions.json').read_text())['mozc']
if not (destination / '.git').exists():
    if destination.exists() and any(destination.iterdir()):
        raise SystemExit('Refusing to replace a nonempty Mozc directory.')
    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(['git', 'init', '-q', str(destination)], check=True)
    subprocess.run(['git', '-C', str(destination), 'remote', 'add', 'origin',
                    'https://github.com/google/mozc.git'], check=True)
    subprocess.run(['git', '-C', str(destination), 'fetch', '--depth=1', 'origin', revision], check=True)
    subprocess.run(['git', '-C', str(destination), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
if actual != revision:
    raise SystemExit('Existing Mozc checkout differs from revisions.json.')
if subprocess.check_output(['git', '-C', str(destination), 'status', '--porcelain', '--untracked-files=no']):
    raise SystemExit('Mozc has tracked local changes; record them as a reviewed patch first.')
for source, notice in [('LICENSE', 'Mozc-LICENSE'),
                       ('src/data/dictionary_oss/README.txt', 'Mozc-dictionary-README.txt')]:
    if (destination / source).read_bytes() != (project / 'licenses' / notice).read_bytes():
        raise SystemExit('Mozc notice mismatch: ' + source)
print('Pinned Mozc source and dictionary notices verified: ' + actual)
print('Source preparation does not establish an EFI port or Japanese input support.')
