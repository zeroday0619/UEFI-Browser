#!/usr/bin/env python3
"""Fetch pinned upstream sources and apply the firmware bring-up patches."""
import concurrent.futures
import hashlib
import json
import pathlib
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile

project = pathlib.Path(__file__).resolve().parent
scratch = project.parents[1] / 'work/chromium-build'
lock = json.loads((project / 'revisions.json').read_text())
scratch.mkdir(parents=True, exist_ok=True)

def run(arguments, directory=None):
    subprocess.run(arguments, cwd=directory, check=True)

def download(name, url):
    destination = scratch / name
    if not destination.exists():
        temporary = destination.with_suffix('.download')
        urllib.request.urlretrieve(url, temporary)
        temporary.rename(destination)
    actual = hashlib.sha256(destination.read_bytes()).hexdigest()
    if actual != lock['archives'][name]:
        raise RuntimeError(f'Checksum mismatch: {destination}')
    return destination

for component, repository in [('skia', 'google/skia'), ('v8', 'v8/v8')]:
    archive = download(component+'.tar.gz', f'https://codeload.github.com/{repository}/tar.gz/{lock[component]}')
    target = scratch / component
    if not target.exists():
        with tarfile.open(archive) as source:
            top = source.getmembers()[0].name.split('/')[0]
            source.extractall(scratch, filter='data')
        (scratch / top).rename(target)

def fetch_dependency(item):
    name, dependency = item
    target = scratch / 'v8' / name
    if (target / '.git').exists():
        revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=target, text=True).strip()
        if revision == dependency['revision']:
            return
        raise RuntimeError(f'Existing dependency has a different revision: {target}')
    target.mkdir(parents=True, exist_ok=True)
    run(['git', 'init', '-q', str(target)])
    run(['git', 'fetch', '-q', '--depth=1', dependency['url'], dependency['revision']], target)
    run(['git', 'checkout', '-q', '--detach', 'FETCH_HEAD'], target)

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
    list(executor.map(fetch_dependency, lock['v8_dependencies'].items()))

def apply_patch(component, name):
    directory = scratch / component
    patch = project / 'patches' / name
    applied = subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)], cwd=directory, capture_output=True)
    if applied.returncode == 0:
        return
    run(['git', 'apply', '--check', str(patch)], directory)
    run(['git', 'apply', str(patch)], directory)

apply_patch('skia', 'skia.patch')
apply_patch('v8', 'v8.patch')
apply_patch('v8/build', 'v8-build.patch')
apply_patch('v8/buildtools', 'v8-buildtools.patch')
toolchain = scratch / 'v8/build/toolchain/uefi'
toolchain.mkdir(exist_ok=True)
shutil.copyfile(project / 'config/uefi-toolchain.gn', toolchain / 'BUILD.gn')
(scratch / 'v8/build/config/gclient_args.gni').write_text('checkout_android = false\ncheckout_ios = false\ncheckout_fuchsia = false\ncheckout_src_internal = false\n')
archive = download('gn.zip', f'https://chrome-infra-packages.appspot.com/dl/gn/gn/linux-amd64/+/git_revision:{lock["gn"]}')
tools = scratch / 'tools'
tools.mkdir(exist_ok=True)
with zipfile.ZipFile(archive) as source:
    (tools / 'gn').write_bytes(source.read('gn'))
(tools / 'gn').chmod(0o755)
for component in ['skia', 'v8']:
    output = scratch / component / 'out/uefi'
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy(project / 'config' / (component+'.gn'), output / 'args.gn')
print(f'Pinned sources prepared in {scratch}')
