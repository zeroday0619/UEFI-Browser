#!/usr/bin/env python3
"""Restore empty pinned Git dependencies named by compiler or Ninja diagnostics."""
import configparser
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
log = Path(sys.argv[2]).read_text(errors='replace')
modules = configparser.ConfigParser()
modules.read(root / '.gitmodules')
urls = {modules[section]['path']: modules[section]['url'] for section in modules.sections()}
links = {}
for entry in subprocess.check_output(['git', '-C', str(root), 'ls-tree', '-rz', 'HEAD']).split(b'\0'):
    if entry.startswith(b'160000 '):
        metadata, name = entry.split(b'\t')
        links[name.decode()] = metadata.split()[2].decode()
missing = re.findall(r"fatal error: '([^']+)' file not found", log)
missing += re.findall(r"ninja: error: '([^']+)',.*missing and no known rule", log)
requested = set()
for filename in missing:
    if filename.startswith('../../'):
        filename = filename[6:]
    for dependency in links:
        if filename.startswith(dependency + '/'):
            requested.add(dependency)
restored = 0
for dependency in sorted(requested):
    directory = root / dependency
    if directory.exists() and any(directory.iterdir()):
        continue
    if dependency not in urls:
        raise SystemExit('Pinned dependency has no configured URL: ' + dependency)
    directory.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, GIT_LFS_SKIP_SMUDGE='1')
    commands = [['init', '-q'], ['remote', 'add', 'origin', urls[dependency]],
                ['fetch', '--depth=1', 'origin', links[dependency]],
                ['checkout', '--detach', 'FETCH_HEAD']]
    for command in commands:
        subprocess.run(['git', '-C', str(directory), *command], env=environment, check=True)
    actual = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != links[dependency]:
        raise SystemExit('Restored dependency revision mismatch: ' + dependency)
    restored += 1
    print('Restored pinned build input: ' + dependency, flush=True)
raise SystemExit(0 if restored else 1)
