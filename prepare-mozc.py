#!/usr/bin/env python3
"""Generate a Mozc firmware library target from the pinned upstream build graph."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bazel', type=Path)
options = parser.parse_args()
project = Path(__file__).resolve().parent
subprocess.run(['python3', str(project / 'fetch-mozc.py')], check=True)
source = project.parents[1] / 'work/mozc/src'
chromium = project.parents[1] / 'work/chromium-build/chromium'
target = chromium / 'third_party/blink/uefi/mozc'
for name in ('mozc-protobuf-build.patch', 'mozc-abseil-build.patch', 'mozc-entropy.patch'):
    patch = project / 'patches' / name
    if subprocess.run(['git', '-C', str(chromium), 'apply', '--reverse', '--check', str(patch)],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        subprocess.run(['git', '-C', str(chromium), 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', str(chromium), 'apply', str(patch)], check=True)
# Host generators must run before their tables are copied into the firmware build.
bazel = str(options.bazel.resolve()) if options.bazel else subprocess.check_output(
    ['python3', str(project / 'tools/fetch-bazel.py')], text=True).strip()
subprocess.run([bazel, 'build', '//session:session_handler_main', '--config', 'release_build', '--jobs=16'], cwd=source, check=True)
query = 'mnemonic("CppCompile", deps(//session:session_handler) union deps(//engine) union deps(//data_manager/oss:oss_data_manager))'
actions = json.loads(subprocess.check_output([bazel, 'aquery', query, '--config', 'release_build', '--output=jsonproto'], cwd=source))
files = set()
for action in actions['actions']:
    arguments = action['arguments']
    if '-c' not in arguments:
        continue
    path = arguments[arguments.index('-c') + 1]
    output = arguments[arguments.index('-o') + 1]
    if path.startswith('external/') or '/external/' in path or '-exec/' in output:
        continue
    files.add(path)
target.mkdir(parents=True, exist_ok=True)
for path in source.iterdir():
    if path.is_dir() and not path.is_symlink() and not path.name.startswith('.'):
        shutil.copytree(path, target / 'source' / path.name, dirs_exist_ok=True)
shutil.copyfile(source / 'version.bzl', target / 'source/version.bzl')
generated = target / 'generated'
generated.mkdir(exist_ok=True)
for path in (source / 'bazel-bin').rglob('*'):
    relative = path.relative_to(source / 'bazel-bin')
    if 'external' in relative.parts or '_objs' in relative.parts:
        continue
    if path.is_file() and path.suffix in ('.inc', '.h') and not path.name.endswith('.pb.h'):
        destination = generated / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
protoc = chromium / 'out/uefi-blink-native/clang_x64/protoc'
firmware_sources = []
for path in sorted(files):
    if path.startswith('bazel-out/'):
        relative = path.split('/bin/', 1)[1]
        if not relative.endswith('.pb.cc'):
            raise SystemExit('Unexpected generated compilation unit: ' + path)
        proto = relative[:-6] + '.proto'
        with tempfile.TemporaryDirectory(prefix='uefi-mozc-proto-') as directory:
            subprocess.run([str(protoc), '--proto_path=' + str(source),
                            '--proto_path=' + str(chromium / 'third_party/protobuf/src'),
                            '--cpp_out=' + directory, str(source / proto)], check=True)
            for output in Path(directory).rglob('*'):
                if not output.is_file():
                    continue
                destination = generated / output.relative_to(directory)
                data = output.read_bytes()
                if not destination.exists() or destination.read_bytes() != data:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(data)
        firmware_sources.append('generated/' + relative)
    elif path != 'session/session_watch_dog.cc':
        firmware_sources.append('source/' + path)
groups = {}
for path in firmware_sources:
    name = str(Path(path).parent).replace('/', '_')
    groups.setdefault(name, []).append(path)
configuration = """config("compiler") {
  include_dirs = [ "source", "generated" ]
  cflags = [ "-iquote" + rebase_path("source", root_build_dir), "-iquote" + rebase_path("generated", root_build_dir) ]
  defines = [ "UEFI_MOZC", "MOZC_BUILD", "MOZC_USE_SVS_JAPANESE", "MOZC_DISABLE_SESSION_WATCHDOG", "MOZC_DATASET_MAGIC_NUMBER_LENGTH=7" ]
  cflags_cc = [ "-funsigned-char", "-Wno-deprecated-declarations", "-Wno-nullability-completeness" ]
}
"""
for name, units in sorted(groups.items()):
    configuration += ('source_set(' + json.dumps(name) + ') {\n  sources = ' + json.dumps(units, indent=2) +
        '\n  configs += [ ":compiler" ]\n  deps = [ "//third_party/abseil-cpp:uefi_mozc", "//third_party/protobuf:protobuf_full", "//third_party/zlib" ]\n}\n')
configuration += 'source_set("adapter") {\n  sources = [ "../src/blink/japanese_input.cc", "../src/blink/japanese_probe.cc" ]\n  configs += [ ":compiler" ]\n  deps = [ "//third_party/abseil-cpp:uefi_mozc", "//third_party/protobuf:protobuf_full" ]\n}\n'
configuration += 'group("engine") {\n  deps = ' + json.dumps([':adapter'] + [':' + name for name in sorted(groups)], indent=2) + '\n}\n'
(target / 'BUILD.gn').write_text(configuration)
(project / 'build/mozc-sources.json').write_text(json.dumps(firmware_sources, indent=2) + '\n')
print('Generated Mozc firmware compilation target: third_party/blink/uefi/mozc:engine')
print('Firmware compilation, linkage and Japanese input execution remain separate validation steps.')
