#!/usr/bin/env python3
"""Reject hosted imports and syscall instructions in the firmware payload."""
import pathlib
import os
import re
import struct
import subprocess

project=pathlib.Path(__file__).resolve().parent
build=pathlib.Path(os.environ.get('BUILD_DIRECTORY', project/'build')).resolve()
payload=build/'payload.elf'
image=build/'esp/EFI/BOOT/BOOTX64.EFI'
def output(*arguments):
    return subprocess.check_output(arguments,text=True)
data=payload.read_bytes()
assert data[:6]==b'\x7fELF\x02\x01'
assert struct.unpack_from('<HH',data,16)==(2,62)
entry,program_offset=struct.unpack_from('<QQ',data,24)
program_size,program_count=struct.unpack_from('<HH',data,54)
assert program_size==56
loads=[]
for index in range(program_count):
    kind,flags,offset,address,physical,file_size,memory_size,alignment=struct.unpack_from('<IIQQQQQQ',data,program_offset+index*program_size)
    assert kind not in (2,3), 'Dynamic loader segment found.'
    if kind==1:
        assert 0x10000000<=address<address+memory_size<=0x20000000
        assert offset+file_size<=len(data) and file_size<=memory_size
        loads.append((address,address+memory_size,flags))
assert any(start<=entry<end and flags&1 for start,end,flags in loads)
# Static ELF links resolve absent weak hooks to zero without a dynamic loader.
undefined = output('llvm-nm', '--undefined-only', str(payload)).splitlines()
optional_hooks = []
for line in undefined:
    fields = line.split()
    assert len(fields) == 2 and fields[0] in ('w', 'v'), 'Required unresolved symbol: ' + line
    optional_hooks.append(fields[1])
if optional_hooks:
    print('Optional unresolved weak hooks: ' + ', '.join(optional_hooks))
assembly=output('llvm-objdump','-d',str(payload))
assert not re.search(r'\t(?:syscall|sysenter)\b',assembly), 'Host system-call instruction found.'
pe=image.read_bytes()
header=struct.unpack_from('<I',pe,0x3c)[0]
assert pe[header:header+4]==b'PE\0\0'
assert struct.unpack_from('<H',pe,header+4)[0]==0x8664
assert struct.unpack_from('<H',pe,header+24)[0]==0x20b
assert struct.unpack_from('<H',pe,header+24+68)[0]==10
imports=output('llvm-readobj' ,'--coff-imports',str(image))
assert 'Import {' not in imports, 'PE imports found.'
print('PASS: static x86-64 EFI image, bounded ELF loads, no required unresolved symbols or syscall instructions')
