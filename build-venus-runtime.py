#!/usr/bin/env python3
"""Cross-compile the Mesa Venus runtime against the existing firmware musl ABI."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

project = Path(__file__).resolve().parent
workspace = project.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--jobs', type=int, default=4)
parser.add_argument('--configure-only', action='store_true')
arguments = parser.parse_args()
if arguments.jobs < 1:
    parser.error('--jobs must be positive')
build = project / 'build/venus-runtime'
build.mkdir(parents=True, exist_ok=True)
config = project / 'build/venus-runtime-config'
config.mkdir(parents=True, exist_ok=True)
musl = workspace / 'work/chromium-build/musl-1.2.5'
musl_build = project / 'build/musl/libc'
chromium = workspace / 'work/chromium-build/chromium'
libc = musl_build / 'lib/libc.a'
if not libc.is_file():
    raise SystemExit('Build the existing firmware musl runtime before building Venus.')
environment = os.environ.copy()
python_tools = config / 'python-tools'
python_tools.mkdir(exist_ok=True)
yaml_module = python_tools / 'yaml'
if not yaml_module.exists():
    yaml_module.symlink_to(chromium / 'third_party/pyyaml', target_is_directory=True)
environment['PYTHONPATH'] = os.pathsep.join(map(str, [
    chromium / 'third_party/mako/mako', chromium / 'third_party',
    python_tools,
]))
subprocess.run([sys.executable, '-c', 'import mako, markupsafe; from yaml import SafeLoader'],
               env=environment, check=True)
source = Path(subprocess.check_output([
    sys.executable, str(project / 'prepare-venus.py'), '--target',
    str(project / 'build/venus-runtime-source'),
], text=True).strip())
subprocess.run(['patch', '--batch', '-p1', '-i',
                str(project / 'patches/mesa-venus-uefi.patch')], cwd=source, check=True)
shutil.copyfile(project / 'src/gpu/venus-runtime/renderer.c',
                source / 'src/virtio/vulkan/vn_renderer_uefi.c')
resource = subprocess.check_output(['clang', '-print-resource-dir'], text=True).strip()
(config / 'sysroot').mkdir(exist_ok=True)
common = ['-O2', '-fno-builtin', '-fno-stack-protector', '-mno-red-zone',
          '-fno-pic', '-fno-pie', '-ffunction-sections', '-fdata-sections',
          '--sysroot=' + str(config / 'sysroot'),
          '-nostdinc', '-idirafter', resource + '/include',
          '-isystem', str(musl_build / 'obj/include'),
          '-isystem', str(musl / 'include'),
          '-isystem', str(musl / 'arch/x86_64'),
          '-isystem', str(musl / 'arch/generic'),
          '-I' + str(project / 'runtime')]
cpp = ['-nostdinc++', '-isystem', str(chromium / 'third_party/libc++/src/include'),
       '-isystem', str(chromium / 'third_party/libc++abi/src/include'),
       '-I' + str(chromium / 'buildtools/third_party/libc++'),
       '-DANDROID_HOST_MUSL',
       '-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE',
       '-fno-exceptions', '-fno-rtti']
# Configure probes only link; they never execute in the host OS. The absolute
# dispatcher symbol lets probes resolve the real musl functions without adding
# a hosted libc or silently accepting arbitrary undefined function symbols.
link = ['-nostdlib', '-no-pie', '-Wl,-e,main', str(libc),
        '-Wl,--defsym,firmware_syscall=0']
cross = config / 'cross.ini'
old_cross = cross.read_text() if cross.exists() else None
cross.write_text("[binaries]\nc = 'clang'\ncpp = 'clang++'\nar = 'llvm-ar'\nstrip = 'llvm-strip'\npkg-config = '/bin/false'\n"
    "[host_machine]\nsystem = 'linux'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n"
    "[properties]\nneeds_exe_wrapper = true\nskip_sanity_check = true\n"
    "[built-in options]\n" + '\n'.join(
        key + ' = ' + repr(value) for key, value in {
            'c_args': common, 'cpp_args': cpp + common,
            'c_link_args': link, 'cpp_link_args': link,
        }.items()) + '\n')
options = {
    'uefi': 'true', 'platforms': '[]', 'vulkan-drivers': 'virtio',
    'gallium-drivers': '[]', 'opengl': 'false', 'gles1': 'disabled',
    'gles2': 'disabled', 'egl': 'disabled', 'glx': 'disabled', 'gbm': 'disabled',
    'llvm': 'disabled', 'expat': 'disabled', 'xmlconfig': 'disabled',
    'zlib': 'disabled', 'zstd': 'disabled', 'shader-cache': 'disabled',
    'libunwind': 'disabled', 'valgrind': 'disabled', 'display-info': 'disabled',
    'build-tests': 'false', 'perfetto': 'false', 'video-codecs': '[]',
    'spirv-tools': 'disabled', 'b_staticpic': 'false', 'b_pie': 'false',
    'buildtype': 'release',
}
setup = ['meson', 'setup', str(build), str(source), '--cross-file', str(cross),
         '--wrap-mode=nofallback']
if (build / 'meson-private/coredata.dat').exists():
    setup.append('--wipe' if old_cross != cross.read_text() else '--reconfigure')
subprocess.run(setup + ['-D' + key + '=' + value for key, value in options.items()],
               env=environment, check=True)
if not arguments.configure_only:
    subprocess.run(['ninja', '-C', str(build), '-j', str(arguments.jobs),
                    'src/virtio/vulkan/libvulkan_virtio.a'],
                   env=environment, check=True)
    print(build / 'src/virtio/vulkan/libvulkan_virtio.a')
