#!/usr/bin/env python3
"""Capture the real UEFI console after an intentionally fatal BSP runtime abort."""
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

project = Path(__file__).resolve().parents[1]
image = project / 'build/panic-probe/esp/EFI/BOOT/BOOTX64.EFI'
log = project / 'validation/panic-qemu.log'
output = project / 'validation/panic-qemu.png'
with tempfile.TemporaryDirectory(prefix='uefi-panic-') as directory:
    temporary = Path(directory)
    (temporary / 'esp/EFI/BOOT').mkdir(parents=True)
    shutil.copy(image, temporary / 'esp/EFI/BOOT/BOOTX64.EFI')
    shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd', temporary / 'variables.fd')
    monitor = temporary / 'qmp'
    command = ['qemu-system-x86_64', '-machine', 'q35,accel=kvm', '-m', '2048', '-smp', '2',
               '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive', f'if=pflash,format=raw,file={temporary / "variables.fd"}',
               '-drive', f'format=raw,file=fat:rw:{temporary / "esp"}',
               '-device', 'virtio-rng-pci', '-display', 'none', '-serial', f'file:{log}',
               '-qmp', f'unix:{monitor},server=on,wait=off', '-no-reboot']
    log.write_text('')
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            text = log.read_text(errors='replace')
            if 'PANIC PROBE: intentional privileged abort' in text and 'FAIL: musl runtime aborted' in text:
                break
            if process.poll() is not None or 'Exception Type' in text:
                raise RuntimeError(text)
            time.sleep(.1)
        else:
            raise TimeoutError(text)
        time.sleep(1)
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(10)
            connection.connect(str(monitor))
            stream = connection.makefile('rwb', buffering=0)
            json.loads(stream.readline())
            def execute(name, arguments=None):
                stream.write((json.dumps({'execute': name, 'arguments': arguments or {}})+'\n').encode())
                while True:
                    reply = json.loads(stream.readline())
                    if 'error' in reply: raise RuntimeError(reply)
                    if 'return' in reply: return reply['return']
            execute('qmp_capabilities')
            screenshot = temporary / 'panic.ppm'
            execute('screendump', {'filename': str(screenshot)})
            raw = screenshot.read_bytes()
            time.sleep(.5)
            execute('screendump', {'filename': str(screenshot)})
            assert raw == screenshot.read_bytes(), 'The halted diagnostic screen changed.'
            header = raw.split(b'\n', 3)
            assert header[0] == b'P6' and header[2] == b'255'
            width, height = map(int, header[1].split())
            pixels = header[3]
            assert len(pixels) == width*height*3
            blue = sum(pixels[index+2] > pixels[index]+64 for index in range(0,len(pixels),3))
            assert blue > width*height//2, 'The fatal console background was not displayed.'
            def chunk(kind, data):
                return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
            rows = b''.join(b'\0'+pixels[row*width*3:(row+1)*width*3] for row in range(height))
            output.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))
            result = {'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(), 'intentional_abort':True, 'stable_screen':True, 'blue_pixels':blue, 'width':width, 'height':height, 'text_review':'Requires visual inspection of panic-qemu.png.'}
            (project / 'validation/panic-qemu.json').write_text(json.dumps(result,indent=2)+'\n')
            print('PASS: intentional abort reached a stable fatal console; inspect the captured text.')
    finally:
        if process.poll() is None: process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
