#!/usr/bin/env python3
"""Build and exercise the production Mozc adapter on the host with an isolated profile."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bazel', type=Path, required=True)
options = parser.parse_args()
project = Path(__file__).resolve().parents[1]
subprocess.run(['python3', str(project / 'fetch-mozc.py')], check=True)
source = project.parents[1] / 'work/mozc/src'
for name in ('japanese_input.cc', 'japanese_input.h'):
    shutil.copyfile(project / 'src/blink' / name, source / 'ios' / name)
shutil.copyfile(project / 'tests/japanese-input.cc', source / 'ios/japanese_input_test.cc')
build = source / 'ios/BUILD.bazel'
original = subprocess.check_output(['git', '-C', str(source), 'show', 'HEAD:src/ios/BUILD.bazel'], text=True)
addition = '''
# This local host test compiles the downstream firmware adapter against the real engine.
mozc_cc_binary(
    name = "uefi_input_test",
    srcs = ["japanese_input.cc", "japanese_input.h", "japanese_input_test.cc"],
    deps = ["//engine", "//data_manager/oss:oss_data_manager", "//session:session_handler",
            "//protocol:commands_cc_proto", "//base:system_util", "//config:config_handler"],
)
'''
if build.read_text() not in (original, original + addition):
    raise SystemExit('Refusing to overwrite an unrelated Mozc iOS build edit.')
build.write_text(original + addition)
try:
    subprocess.run([str(options.bazel.resolve()), 'build', '//ios:uefi_input_test',
                    '--config', 'release_build', '--jobs=16'], cwd=source, check=True)
    with tempfile.TemporaryDirectory(prefix='uefi-mozc-profile-') as profile:
        subprocess.run([str(source / 'bazel-bin/ios/uefi_input_test'), profile], check=True)
finally:
    build.write_text(original)
