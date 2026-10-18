#!/usr/bin/env python3
"""Boot the native payload and capture its GOP output through QMP."""
import tempfile
import json
import os
import pathlib
import shutil
import socket
import subprocess
import time

project = pathlib.Path(__file__).resolve().parent
build = pathlib.Path(os.environ.get('BUILD_DIRECTORY', project / 'build')).resolve()
socket_directory = tempfile.TemporaryDirectory(prefix='blink-qmp-', dir='/tmp')
monitor = pathlib.Path(socket_directory.name) / 'qmp.sock'
monitor.unlink(missing_ok=True)
shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd', build / 'OVMF_VARS.fd')
log = build / 'serial.log'
log.write_text('')
command = ['qemu-system-x86_64', '-machine', 'q35,accel=tcg', '-m', '1024',
           '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
           '-drive', f'if=pflash,format=raw,file={build / "OVMF_VARS.fd"}',
           '-drive', f'format=raw,file=fat:rw:{build / "esp"}',
           '-netdev', 'user,id=network', '-device', 'virtio-net-pci,netdev=network,romfile=',
           '-device', 'virtio-rng-pci', '-display', 'none', '-serial', f'file:{log}',
           '-qmp', f'unix:{monitor},server=on,wait=off', '-no-reboot']
certificates = project / 'build/ca.esl'
if certificates.exists():
    command += ['-fw_cfg', f'name=etc/edk2/https/cacerts,file={certificates}']
process = subprocess.Popen(command)
try:
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        text = log.read_text(errors='replace')
        if 'FAIL:' in text or 'Exception Type' in text:
            raise RuntimeError(text)
        if 'PASS: engine teardown and pool cleanup' in text:
            break
        if process.poll() is not None:
            raise RuntimeError(f'QEMU exited with {process.returncode}: {text}')
        time.sleep(0.2)
    else:
        raise TimeoutError(text)
    required = os.environ.get('REQUIRED_MARKER')
    if required and required not in text:
        raise RuntimeError('Expected engine result is absent: ' + required)
    with socket.socket(socket.AF_UNIX) as control:
        control.connect(str(monitor))
        stream = control.makefile('rwb', buffering=0)
        json.loads(stream.readline())
        def execute(name, arguments=None):
            stream.write((json.dumps({'execute': name, 'arguments': arguments or {}})+'\n').encode())
            while True:
                result = json.loads(stream.readline())
                if 'error' in result:
                    raise RuntimeError(result)
                if 'return' in result:
                    return result['return']
        execute('qmp_capabilities')
        execute('screendump', {'filename': str(build / 'skia.ppm')})
        execute('send-key', {'keys': [{'type': 'qcode', 'data': 'ret'}]})
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
        text=log.read_text(errors='replace')
        if 'Exception Type' in text or 'FAIL:' in text:
            raise RuntimeError(text)
        if 'PASS: EFI loader released payload, stack and TLS' in text:
            break
        time.sleep(0.1)
    else:
        raise TimeoutError('EFI return path did not complete: '+text)
    print(text)
finally:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()

socket_directory.cleanup()
