#!/usr/bin/env python3
"""Exercise the production allocator with fault-injected firmware under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='uefi-allocator-') as directory:
    build = Path(directory)
    flags = ['clang', '-g', '-O1', '-fno-builtin', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(project / 'runtime')]
    symbols = ['malloc', 'calloc', 'realloc', 'aligned_alloc', 'posix_memalign', 'free', 'malloc_usable_size', 'sysconf', '__libc_malloc', '__libc_calloc', '__libc_realloc', '__libc_free']
    subprocess.run(flags + ['-D' + name + '=checked_' + name for name in symbols] + ['-c', str(project / 'src/musl/allocator.c'), '-o', str(build / 'allocator.o')], check=True)
    subprocess.run(flags + [str(project / 'tests/allocator-host.c'), str(build / 'allocator.o'), '-o', str(build / 'allocator-test')], check=True)
    subprocess.run([str(build / 'allocator-test')], check=True)
