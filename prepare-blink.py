#!/usr/bin/env python3
"""Prepare the pinned Blink embedding compile check without building a guest OS."""
import argparse
import configparser
import json
import os
import pathlib
import re
import runpy
import shutil
import subprocess

project = pathlib.Path(__file__).resolve().parent
root = project.parents[1] / 'work/chromium-build/chromium'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--resolve-inputs', action='store_true',
                    help='Fetch missing tracked inputs and exact Git dependencies required by GN.')
parser.add_argument('--mozc', action='store_true', help='Include the experimental Mozc firmware engine.')
parser.add_argument('--firmware', action='store_true', help='Generate the musl firmware object build graph.')
options = parser.parse_args()
subprocess.run([str(project / 'fetch-blink.sh')], check=True)
subprocess.run(['python3', str(project / 'prepare-libhangul.py')], check=True)
if options.resolve_inputs:
    subprocess.run(['git', '-C', str(root), 'sparse-checkout', 'add', '--skip-checks',
                    'build_overrides', 'tools/rust', 'tools/clang'], check=True)
revision = json.loads((project / 'revisions.json').read_text())['chromium']
actual = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
if actual != revision:
    raise SystemExit('Chromium revision does not match revisions.json')

(target := root / 'third_party/blink/uefi').mkdir(exist_ok=True)
for name in ('src/blink', 'src/network', 'src/process', 'runtime'):
    shutil.copytree(project / name, target / name, dirs_exist_ok=True)
version_fields = dict(line.split('=', 1) for line in (root / 'chrome/VERSION').read_text().splitlines())
chromium_version = '.'.join(version_fields[key] for key in ('MAJOR', 'MINOR', 'BUILD', 'PATCH'))
(target / 'src/blink/browser_build_info.h').write_text(
    '// Generated from the verified Chromium checkout.\n'
    '#define UEFI_CHROMIUM_VERSION ' + json.dumps(chromium_version) + '\n'
    '#define UEFI_CHROMIUM_REVISION ' + json.dumps(actual) + '\n')
subprocess.run(['python3', str(project / 'tools/embed-ca.py'),
                os.environ.get('UEFI_CA_BUNDLE', str(project / 'trust/mozilla-ca.pem')),
                str(target / 'runtime/mozilla_roots.h')], check=True)
if options.firmware:
    (project / 'build').mkdir(exist_ok=True)
    subprocess.run(['python3', str(project / 'tools/ca_bundle.py'),
                    os.environ.get('UEFI_CA_BUNDLE', str(project / 'trust/mozilla-ca.pem')),
                    str(project / 'build/ca.esl')], check=True)
if options.mozc:
    protoc = root / 'out/uefi-blink-native/clang_x64/protoc'
    if not protoc.is_file():
        # Mozc messages must use Chromium's Protobuf ABI rather than the host Bazel version.
        bootstrap = ['python3', str(project / 'prepare-blink.py'), '--firmware']
        if options.resolve_inputs:
            bootstrap.append('--resolve-inputs')
        subprocess.run(bootstrap, check=True)
        subprocess.run(['ninja', '-C', str(protoc.parent.parent), 'clang_x64/protoc'], check=True)
    subprocess.run(['python3', str(project / 'prepare-mozc.py')], check=True)
shutil.copyfile(project / 'config/blink-navigation.gn', target / 'BUILD.gn')
shutil.copyfile(project / 'config/blink-gclient.gni', root / 'build/config/gclient_args.gni')
build = root / ('out/uefi-blink-native' if options.firmware else 'out/uefi-blink')
build.mkdir(parents=True, exist_ok=True)
shutil.copyfile(project / 'config/blink-host.gn', build / 'args.gn')
with (build / 'args.gn').open('a') as output:
    output.write('\nuefi_enable_mozc = ' + ('true' if options.mozc else 'false') + '\n')
if options.firmware:
    toolchain = root / 'build/toolchain/uefi'
    toolchain.mkdir(exist_ok=True)
    shutil.copyfile(project / 'config/uefi-toolchain.gn', toolchain / 'BUILD.gn')
    resource = subprocess.check_output(['/usr/bin/clang', '-print-resource-dir'], text=True).strip()
    workspace = str(project.parents[1])
    configuration = (project / 'config/blink-firmware.gn').read_text()
    configuration = configuration.replace('@WORKSPACE@', workspace).replace('@CLANG_RESOURCE@', resource)
    with (build / 'args.gn').open('a') as output:
        output.write('\n' + configuration)

patch = project / 'patches/blink-build.patch'
# Validate each file before applying new hunks so an earlier patch version
# can be upgraded without overwriting unrelated checkout changes.
pending = []
for section in patch.read_text().split('diff --git ')[1:]:
    content = 'diff --git ' + section
    if subprocess.run(['git', '-C', str(root), 'apply', '--reverse', '--check', '-'],
                      input=content, text=True, stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode:
        subprocess.run(['git', '-C', str(root), 'apply', '--check', '-'],
                       input=content, text=True, check=True)
        pending.append(content)
for content in pending:
    subprocess.run(['git', '-C', str(root), 'apply', '-'], input=content,
                   text=True, check=True)
for section in (project / 'patches/blink-devtools.patch').read_text().split('diff --git ')[1:]:
    content = 'diff --git ' + section
    if subprocess.run(['git', '-C', str(root), 'apply', '--reverse', '--check', '-'],
                      input=content, text=True, stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode:
        subprocess.run(['git', '-C', str(root), 'apply', '--check', '-'], input=content, text=True, check=True)
        subprocess.run(['git', '-C', str(root), 'apply', '-'], input=content, text=True, check=True)
opus_patch = project / 'patches/blink-opus-service.patch'
if subprocess.run(['git', '-C', str(root), 'apply', '--reverse', '--check', str(opus_patch)],
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
    subprocess.run(['git', '-C', str(root), 'apply', '--check', str(opus_patch)], check=True)
    subprocess.run(['git', '-C', str(root), 'apply', str(opus_patch)], check=True)

# The host snapshot generator keeps its native address-space behavior.
v8 = root / 'v8'
for name in ('blink-v8.patch', 'blink-v8-jit.patch', 'blink-v8-metadata.patch', 'blink-v8-inspector-diagnostics.patch'):
    v8_patch = project / 'patches' / name
    if (v8 / 'BUILD.gn').exists():
        if subprocess.run(['patch', '--batch', '--force', '--silent', '--dry-run', '--reverse',
                           '-p1', '-i', str(v8_patch)], cwd=v8,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            subprocess.run(['patch', '--batch', '--forward', '--dry-run', '-p1',
                            '-i', str(v8_patch)], cwd=v8, check=True)
            subprocess.run(['patch', '--batch', '--forward', '-p1',
                            '-i', str(v8_patch)], cwd=v8, check=True)
modules = configparser.ConfigParser()
modules.read(root / '.gitmodules')
urls = {modules[section]['path']: modules[section]['url'] for section in modules.sections()}
links = {}
for entry in subprocess.check_output(['git', '-C', str(root), 'ls-tree', '-rz', 'HEAD']).split(b'\0'):
    if entry.startswith(b'160000 '):
        metadata, name = entry.split(b'\t')
        links[name.decode()] = metadata.split()[2].decode()

for name, expected in links.items():
    directory = root / name
    if (directory / '.git').exists():
        actual = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != expected:
            raise SystemExit('Dependency revision mismatch: ' + name)

def fetch_dependency(dependency):
    destination = root / dependency
    if (destination / '.git').exists():
        return
    if destination.exists() and any(destination.iterdir()):
        raise SystemExit('Refusing to replace a nonempty dependency: ' + dependency)
    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(['git', 'init', '-q', str(destination)], check=True)
    subprocess.run(['git', '-C', str(destination), 'remote', 'add', 'origin', urls[dependency]], check=True)
    subprocess.run(['git', '-C', str(destination), 'fetch', '--depth=1', 'origin', links[dependency]], check=True)
    subprocess.run(['git', '-C', str(destination), 'checkout', '--detach', 'FETCH_HEAD'], check=True)

if options.resolve_inputs:
    fetch_dependency('third_party/depot_tools')
    fetch_dependency('third_party/dawn')
    # The Skia archive has no nested Git metadata; use its pinned Chromium gitlink.
    lastchange = runpy.run_path(str(root / 'build/util/lastchange.py'))
    skia_header = 'skia/ext/skia_commit_hash.h'
    lastchange['WriteIfChanged'](str(root / skia_header),
        lastchange['GetHeaderContents'](skia_header, 'SKIA_COMMIT_HASH',
                                        links['third_party/skia']))
    subprocess.run(['/usr/bin/python3', str(root / 'build/util/lastchange.py'),
                    '-m', 'DAWN_COMMIT_HASH', '-s', str(root / 'third_party/dawn'),
                    '--revision', 'gpu/webgpu/DAWN_VERSION',
                    '--header', 'gpu/webgpu/dawn_commit_hash.h'], cwd=root, check=True)
    for arguments in (['-o', str(root / 'build/util/LASTCHANGE')],
                      ['--minibranch-out', str(root / 'build/util/MINIBRANCH')]):
        subprocess.run(['/usr/bin/python3', str(root / 'build/util/lastchange.py'), *arguments],
                       cwd=root, check=True)

if options.resolve_inputs:
    fetch_dependency('third_party/swiftshader')
    fetch_dependency('third_party/angle')
for package in ('dawn', 'swiftshader', 'angle'):
    dependency_patch = project / ('patches/blink-' + package + '.patch')
    dependency = root / 'third_party' / package
    if subprocess.run(['patch', '--batch', '--silent', '--dry-run', '--reverse',
                       '-p1', '-i', str(dependency_patch)], cwd=dependency,
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        subprocess.run(['patch', '--batch', '--forward', '--dry-run', '-p1',
                        '-i', str(dependency_patch)], cwd=dependency, check=True)
        subprocess.run(['patch', '--batch', '--forward', '-p1',
                        '-i', str(dependency_patch)], cwd=dependency, check=True)
subprocess.run(['python3', str(project / 'tools/prepare-software-gpu.py'),
                str(root)], check=True)

command = [str(root.parent / 'tools/gn'), 'gen', str(build), '--root=' + str(root),
           '--dotfile=' + str(project / ('config/blink-firmware-dotfile.gn' if options.firmware else 'config/blink-dotfile.gn'))]
resolved = set()
for attempt in range(100):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (project / ('validation/blink-native-generation.log' if options.firmware else 'validation/blink-generation.log')).write_text(result.stdout)
    print(result.stdout, flush=True)
    if result.returncode == 0:
        print('GN generation passed. No Blink compilation or firmware execution has been performed.')
        print('Adapter object check: python3 ' + str(project / 'check-blink-api.py') + ' --resolve-inputs')
        break
    if not options.resolve_inputs:
        raise SystemExit(result.returncode)
    match = re.search(r'(?:Unable to load|I resolved this to) "([^"]+)"', result.stdout)
    if not match:
        raise SystemExit(result.returncode)
    name = pathlib.Path(match[1]).relative_to(root).as_posix()
    if name in resolved:
        raise SystemExit('Input remains unavailable after preparation: ' + name)
    resolved.add(name)
    if name == 'third_party/rust-toolchain/VERSION':
        subprocess.run(['/usr/bin/python3', str(root / 'tools/rust/update_rust.py')], check=True)
        continue
    dependency = next((path for path in links if name == path or name.startswith(path + '/')), None)
    if dependency:
        if (root / dependency / '.git').exists():
            raise SystemExit('Missing input in an existing dependency: ' + name)
        fetch_dependency(dependency)
    else:
        tracked = subprocess.run(['git', '-C', str(root), 'cat-file', '-e', 'HEAD:' + name]).returncode == 0
        if not tracked or (root / name).exists():
            raise SystemExit('Missing generated or external input: ' + name)
        prefix = pathlib.PurePosixPath(name).parent.as_posix()
        subprocess.run(['git', '-C', str(root), 'sparse-checkout', 'add', '--skip-checks', prefix], check=True)
else:
    raise SystemExit('Dependency preparation limit reached; inspect blink-generation.log')

webrtc = root / 'third_party/webrtc'
webrtc_patch = project / 'patches/blink-webrtc.patch'
if (webrtc / 'rtc_base/platform_thread_types.cc').exists():
    if subprocess.run(['git', '-C', str(webrtc), 'apply', '--reverse', '--check', str(webrtc_patch)],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        subprocess.run(['git', '-C', str(webrtc), 'apply', '--check', str(webrtc_patch)], check=True)
        subprocess.run(['git', '-C', str(webrtc), 'apply', str(webrtc_patch)], check=True)

libsync = root / 'third_party/libsync/src'
libsync_patch = project / 'patches/blink-libsync.patch'
if (libsync / 'include/ndk/sync.h').exists():
    if subprocess.run(['git', '-C', str(libsync), 'apply', '--reverse', '--check', str(libsync_patch)],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        subprocess.run(['git', '-C', str(libsync), 'apply', '--check', str(libsync_patch)], check=True)
        subprocess.run(['git', '-C', str(libsync), 'apply', str(libsync_patch)], check=True)
