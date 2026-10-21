#!/usr/bin/env python3
"""Check the firmware ICD's reachable link closure without executing host code."""
import hashlib
import json
from pathlib import Path
import subprocess

project = Path(__file__).resolve().parents[1]
build = project / 'build/venus-runtime'
chromium = project.parents[1] / 'work/chromium-build/chromium'
native = chromium / 'out/uefi-blink-native/obj/buildtools/third_party'
archive = build / 'src/virtio/vulkan/libvulkan_virtio.a'
libraries = [native / 'libc++/libc++.a', native / 'libc++abi/libc++abi.a',
             project / 'build/musl/libc/lib/libc.a']
output = build / 'venus-icd-linked.o'
roots = ['vk_icdGetInstanceProcAddr', 'vk_icdNegotiateLoaderICDInterfaceVersion',
         'vk_icdGetPhysicalDeviceProcAddr']
subprocess.run(['ld.lld', '-r', '--gc-sections',
                *[argument for name in roots for argument in ('-u', name)],
                '--whole-archive', str(archive), '--no-whole-archive',
                '--start-group', *map(str, libraries), '--end-group',
                '-o', str(output)], check=True)
symbols = subprocess.check_output(['llvm-nm', str(output)], text=True)
undefined = sorted(line.split()[-1] for line in symbols.splitlines()
                   if line.split()[:1] == ['U'])
expected = sorted(['firmware_syscall', 'firmware_venus_open', 'firmware_venus_close',
                   'firmware_venus_submit', 'firmware_venus_poll',
                   'firmware_venus_release_fence', 'firmware_venus_create_blob',
                   'firmware_venus_destroy_blob'])
if undefined != expected:
    raise SystemExit('Unexpected firmware link boundary: ' + repr(undefined))
defined = {line.split()[-1] for line in symbols.splitlines() if len(line.split()) == 3}
if not set(roots) <= defined:
    raise SystemExit('An ICD entrypoint is missing.')
header = subprocess.check_output(['llvm-readelf', '-h', str(output)], text=True)
if 'REL (Relocatable file)' not in header or 'Advanced Micro Devices X86-64' not in header:
    raise SystemExit('The firmware link result has an unexpected ELF target.')
if '@GLIBC' in symbols:
    raise SystemExit('The firmware link result contains a hosted glibc symbol.')
report = {
    'result': 'pass', 'scope': 'cross-compiled ICD link closure; no firmware execution',
    'entrypoints': roots, 'unresolved_service_functions': undefined,
    'inputs': {str(path.relative_to(project.parents[1])):
               hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
               for path in [archive, *libraries]},
    'relocatable_object_sha256': hashlib.file_digest(output.open('rb'), 'sha256').hexdigest(),
}
(project / 'validation/venus-runtime-link.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: firmware ICD entrypoints resolve with musl and Chromium libc++; only eight declared service functions remain')
