#!/usr/bin/env python3
"""Check the generated native/host boundary before linking firmware objects."""
import json
from pathlib import Path
import shlex

project = Path(__file__).resolve().parents[1]
build = project.parents[1] / 'work/chromium-build/chromium/out/uefi-blink-native'


def arguments(path, name):
    prefix = name + ' = '
    for line in path.read_text().splitlines():
        if line.startswith(prefix):
            return set(shlex.split(line[len(prefix):]))
    raise AssertionError(f'Missing {name} in {path}')


native_rule = build / 'obj/v8/v8_libbase.ninja'
host_rule = build / 'clang_x64/obj/v8/v8_libbase.ninja'
native_defines = arguments(native_rule, 'defines')
host_defines = arguments(host_rule, 'defines')
native_flags = arguments(native_rule, 'cflags')
host_flags = arguments(host_rule, 'cflags')
for define in ('-DUEFI_RUNTIME', '-DV8_UEFI_RUNTIME'):
    assert define in native_defines, f'Native V8 is missing {define}'
    assert define not in host_defines, f'Host V8 incorrectly receives {define}'
for flag in ('-nostdinc', '-mno-red-zone', '-fno-stack-protector'):
    assert flag in native_flags, f'Native V8 is missing {flag}'
assert '-nostdinc' not in host_flags, 'Host V8 incorrectly uses firmware headers'
assert not any(flag.startswith('-fstack-protector') for flag in native_flags)
expat_defines = arguments(build / 'obj/third_party/expat/expat.ninja', 'defines')
assert '-DHAVE_GETRANDOM=1' in expat_defines
result = {
    'native_v8_uefi_runtime': True,
    'host_v8_uefi_runtime': False,
    'native_compiler_flags_verified': True,
    'native_expat_getrandom': True,
    'firmware_execution_verified': False,
}
(project / 'validation/blink-native-v8-toolchains.json').write_text(
    json.dumps(result, indent=2) + '\n')
print('PASS: native V8, host snapshot, and Expat compiler configuration boundaries.')
