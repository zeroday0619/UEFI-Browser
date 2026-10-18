#!/usr/bin/env python3
"""Prepare the pinned musl source with a firmware syscall entry point."""
import hashlib
import json
import pathlib
import shutil
import tarfile
import urllib.request

project = pathlib.Path(__file__).resolve().parent
scratch = project.parents[1] / 'work/chromium-build'
lock = json.loads((project / 'revisions.json').read_text())
name = f'musl-{lock["musl"]}'
archive = scratch / (name + '.tar.gz')
scratch.mkdir(parents=True, exist_ok=True)
if not archive.exists():
    temporary = archive.with_suffix('.download')
    with urllib.request.urlopen(f'https://musl.libc.org/releases/{archive.name}', timeout=60) as response:
        with temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
    temporary.rename(archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != lock['archives'][archive.name]:
    raise RuntimeError('musl archive checksum mismatch')
target = scratch / name
if not target.exists():
    with tarfile.open(archive) as source:
        source.extractall(scratch, filter='data')
replacement = (project / 'src/musl/syscall_arch.h').read_bytes()
header = target / 'arch/x86_64/syscall_arch.h'
if header.read_bytes() != replacement:
    header.write_bytes(replacement)
# Handwritten assembly does not inherit the compiler's -mno-red-zone flag.
assembly = target / 'src/math/x86_64/exp2l.s'
original = "\tmovl $0xc2820000,-4(%rsp)\n\tflds -4(%rsp)"
replacement = "\tsub $8,%rsp\n\tmovl $0xc2820000,(%rsp)\n\tflds (%rsp)\n\tadd $8,%rsp"
text = assembly.read_text()
if original in text:
    assembly.write_text(text.replace(original, replacement))
elif replacement not in text:
    raise RuntimeError('Unexpected musl expm1l assembly')
print(target)
