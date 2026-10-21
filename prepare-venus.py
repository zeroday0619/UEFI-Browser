#!/usr/bin/env python3
"""Prepare hash-pinned Mesa sources for the freestanding Venus transport."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request

project = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--target', type=Path, default=project / 'build/venus-source')
arguments = parser.parse_args()
revision = json.loads((project / 'revisions.json').read_text())['mesa_venus']
cache = project.parents[1] / 'work/mesa-venus-download'
cache.mkdir(parents=True, exist_ok=True)
archive = cache / ('mesa-' + revision['version'] + '.tar.xz')
if not archive.exists():
    with tempfile.NamedTemporaryFile(dir=cache, delete=False) as output:
        temporary = Path(output.name)
        try:
            with urllib.request.urlopen(revision['url'], timeout=60) as response:
                shutil.copyfileobj(response, output)
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    temporary.replace(archive)
if hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest() != revision['sha256']:
    raise SystemExit('Mesa source archive SHA-256 differs from revisions.json.')
# Extraction on every preparation prevents unrecorded source edits from entering the build.
target = arguments.target.resolve()
target.mkdir(parents=True, exist_ok=True)
with tarfile.open(archive) as source:
    source.extractall(target, filter='data')
root = target / ('mesa-' + revision['version'])
for name in ('MIT', 'Apache-2.0'):
    if (root / 'licenses' / name).read_bytes() != (project / 'licenses' / ('Mesa-' + name + '.txt')).read_bytes():
        raise SystemExit('Mesa license differs from the recorded license text.')
protocol = root / 'src/virtio/venus-protocol/vn_protocol_driver.h'
if 'git-' + revision['generated_protocol_revision'] not in protocol.read_text():
    raise SystemExit('Unexpected generated Venus protocol revision.')
print(root)
