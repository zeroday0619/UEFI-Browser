#!/usr/bin/env python3
"""Install Dawn's pinned host Go toolchain for its source generator."""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import tempfile
import urllib.parse
import urllib.request
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('dawn', type=pathlib.Path)
options = parser.parse_args()
dawn = options.dawn.resolve()
match = re.search(r"'dawn_go_version':\s*'([^']+)'", (dawn / 'DEPS').read_text())
if not match:
    raise SystemExit('Dawn Go toolchain pin is absent from DEPS')
version = match[1]
expected = version.split('@', 1)[1]
destination = dawn / 'tools/golang/linux-amd64'
binary = destination / 'bin/go'
if binary.exists():
    actual = subprocess.check_output([str(binary), 'version'], text=True)
    if f'go{expected} linux/amd64' not in actual:
        raise SystemExit('Dawn host Go toolchain does not match its DEPS pin')
    raise SystemExit(0)
if destination.exists() and any(destination.iterdir()):
    raise SystemExit('Incomplete Dawn Go directory requires inspection: ' + str(destination))
destination.parent.mkdir(parents=True, exist_ok=True)
url = ('https://chrome-infra-packages.appspot.com/dl/infra/3pp/tools/go/linux-amd64/+/'
       + urllib.parse.quote(version, safe=':@'))
with tempfile.TemporaryDirectory(dir=destination.parent, prefix='go-download-') as temporary:
    temporary = pathlib.Path(temporary)
    archive = temporary / 'package.zip'
    urllib.request.urlretrieve(url, archive)
    staging = temporary / 'toolchain'
    staging.mkdir()
    with zipfile.ZipFile(archive) as package:
        for entry in package.infolist():
            target = (staging / entry.filename).resolve()
            if not target.is_relative_to(staging):
                raise SystemExit('Unsafe path in Go toolchain archive')
        package.extractall(staging)
        for entry in package.infolist():
            mode = (entry.external_attr >> 16) & 0o777
            if mode and not entry.is_dir():
                (staging / entry.filename).chmod(mode)
    actual = subprocess.check_output([str(staging / 'bin/go'), 'version'], text=True)
    if f'go{expected} linux/amd64' not in actual:
        raise SystemExit('Downloaded Go version does not match Dawn DEPS')
    if destination.exists():
        destination.rmdir()
    shutil.move(str(staging), str(destination))
print(json.dumps({'package': 'infra/3pp/tools/go/linux-amd64', 'version': version}))
