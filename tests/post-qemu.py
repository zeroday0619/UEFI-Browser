#!/usr/bin/env python3
"""Verify loader POST success and rejection of a corrupted embedded ELF image."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import zlib

project=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-directory',type=Path,default=Path('build/blink'))
parser.add_argument('--prefix',default='post')
parser.add_argument('--require-watchdog-off',action='store_true')
options=parser.parse_args()
if not options.prefix.replace('-','').replace('_','').isalnum():
    parser.error('Prefix must contain letters, digits, hyphens or underscores.')
original=(project/options.build_directory/'esp/EFI/BOOT/BOOTX64.EFI').read_bytes()
results=[]
for damaged in (False,True):
    label='invalid-elf' if damaged else 'success'
    payload=bytearray(original)
    if damaged:
        offset=payload.index(b'\x7fELF\x02\x01\x01')
        assert struct.unpack_from('<HH',payload,offset+16)==(2,62)
        payload[offset]=0
    log=project/('validation/'+options.prefix+'-'+label+'.log')
    with tempfile.TemporaryDirectory(prefix='uefi-post-') as directory:
        temporary=Path(directory)
        (temporary/'esp/EFI/BOOT').mkdir(parents=True)
        (temporary/'esp/EFI/BOOT/BOOTX64.EFI').write_bytes(payload)
        shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd',temporary/'variables.fd')
        monitor=temporary/'qmp'
        command=['qemu-system-x86_64','-machine','q35,accel=kvm','-m','4096','-smp','4',
                 '-drive','if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                 '-drive',f'if=pflash,format=raw,file={temporary / "variables.fd"}',
                 '-drive',f'format=raw,file=fat:rw:{temporary / "esp"}',
                 '-device','virtio-mouse-pci','-device','virtio-rng-pci',
                 '-display','none','-serial',f'file:{log}','-qmp',f'unix:{monitor},server=on,wait=off','-no-reboot']
        log.write_text('')
        process=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        try:
            deadline=time.monotonic()+60
            while not monitor.exists():
                if process.poll() is not None or time.monotonic()>deadline: raise RuntimeError('QMP did not start.')
                time.sleep(.02)
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(10)
                connection.connect(str(monitor))
                stream=connection.makefile('rwb',buffering=0)
                json.loads(stream.readline())
                def execute(name,arguments=None):
                    stream.write((json.dumps({'execute':name,'arguments':arguments or {}})+'\n').encode())
                    while True:
                        reply=json.loads(stream.readline())
                        if 'error' in reply: raise RuntimeError(reply)
                        if 'return' in reply: return reply['return']
                execute('qmp_capabilities')
                marker='Startup stopped. Press a key' if damaged else 'POST complete. Starting UEFI BROWSER'
                while time.monotonic()<deadline:
                    text=log.read_text(errors='replace')
                    if marker in text: break
                    if process.poll() is not None or 'Exception Type' in text: raise RuntimeError(text)
                    time.sleep(.02)
                else: raise TimeoutError(text)
                execute('stop')
                if options.require_watchdog_off:
                    assert '[PASS] Firmware boot watchdog disabled' in text
                for check in ('CPU long mode, SSE2 and execute-disable','64 KiB allocation, pattern read/write and release','Firmware HTTP driver','Pointer and wheel input driver'):
                    assert '[PASS] '+check in text,check
                if damaged:
                    assert '[FAIL] ELF image format or unsupported segment - status 0x8000000000000003' in text
                    assert 'POST complete.' not in text and 'Running musl checks' not in text
                else:
                    assert '[PASS] ELF image structure, bounds and executable entry' in text
                    assert '[PASS] Payload memory, stack and thread-local storage' in text
                screenshot=temporary/'post.ppm'
                execute('screendump',{'filename':str(screenshot)})
                header=screenshot.read_bytes().split(b'\n',3)
                assert header[0]==b'P6' and header[2]==b'255'
                width,height=map(int,header[1].split())
                pixels=header[3]
                assert len(pixels)==width*height*3
                def chunk(kind,data):
                    return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
                rows=b''.join(b'\0'+pixels[row*width*3:(row+1)*width*3] for row in range(height))
                (project/('validation/'+options.prefix+'-'+label+'.png')).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))
                results.append({'case':label,'image_sha256':hashlib.sha256(payload).hexdigest(),'passed':True,'execution_blocked':damaged})
        finally:
            if process.poll() is None: process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
(project/('validation/'+options.prefix+'-qemu.json')).write_text(json.dumps(results,indent=2)+'\n')
print('PASS: loader POST success and invalid-ELF rejection; screenshots captured for text review.')
