#!/usr/bin/env python3
"""Capture a real Blink EFI navigation; inspect the screenshot before claiming fidelity."""
import tempfile
import json
import os
import re
from pathlib import Path
import shutil
import socket
import subprocess
import time

project = Path(__file__).resolve().parents[1]
build = Path(os.environ.get('BUILD_DIRECTORY', project / 'build/blink')).resolve()
image = build / 'esp/EFI/BOOT/BOOTX64.EFI'
if not image.is_file():
    raise SystemExit('The Blink EFI image has not been linked: ' + str(image))
socket_directory = tempfile.TemporaryDirectory(prefix='blink-qmp-', dir='/tmp')
monitor = Path(socket_directory.name) / 'qmp.sock'
monitor.unlink(missing_ok=True)
variables = build / 'browser-OVMF_VARS.fd'
if not os.environ.get('QEMU_TEST_KEEP_VARIABLES') or not variables.exists():
    shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
log = build / 'browser-serial.log'
log.write_text('')
disk = os.environ.get('QEMU_TEST_DISK_IMAGE')
drive = f'format=raw,file={Path(disk).resolve()}' if disk else f'format=raw,file=fat:rw:{build / "esp"}'
command = ['qemu-system-x86_64', '-machine', 'q35,accel=' + os.environ.get('QEMU_TEST_ACCEL', 'tcg'), '-m', '4096', '-smp', os.environ.get('QEMU_TEST_CPUS', '1'),
    '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
    '-drive', f'if=pflash,format=raw,file={variables}',
    '-drive', drive,
    '-netdev', 'user,id=network', '-device', 'virtio-net-pci,netdev=network,romfile=',
    '-device', 'virtio-mouse-pci', '-device', 'virtio-rng-pci', '-display', 'none', '-serial', f'file:{log}',
    '-qmp', f'unix:{monitor},server=on,wait=off', '-no-reboot',
    '-fw_cfg', f'name=etc/edk2/https/cacerts,file={os.environ.get("TEST_CA_FILE", project / "build/ca.esl")}']
audio_capture = os.environ.get('QEMU_TEST_AUDIO_CAPTURE')
if audio_capture:
    command += ['-audiodev', f'wav,id=audio,path={audio_capture},out.frequency=48000,out.channels=2']
else:
    command += ['-audiodev', 'none,id=audio']
command += ['-device', 'AC97,audiodev=audio']
if os.environ.get('QEMU_TEST_GDB_PORT'):
    port = int(os.environ['QEMU_TEST_GDB_PORT'])
    if not 1 <= port <= 65535:
        raise SystemExit('QEMU_TEST_GDB_PORT must be between 1 and 65535.')
    command += ['-gdb', f'tcp:127.0.0.1:{port}']
process = subprocess.Popen(command)
try:
    required = ['BLINK: non-composited main frame created',
                'BLINK: main document load finished',
                'BLINK: main frame stopped loading',
                'BLINK: first raster frame copied to GOP']
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        text = log.read_text(errors='replace')
        if 'FAIL:' in text or 'Exception Type' in text or process.poll() is not None:
            raise RuntimeError(text)
        if all(marker in text for marker in required):
            for media_type in ('text/css', '(?:text|application)/javascript', 'image/'):
                if os.environ.get('INPUT_SCENARIO') in ('site', 'site-no-wheel') and not re.search(r'BLINK: resource 200 type=' + media_type, text):
                    raise RuntimeError('Required subresource was not received: ' + media_type + '\n' + text)
            break
        time.sleep(0.2)
    else:
        raise TimeoutError(text)
    # A completed load can precede its final animation and paint task.
    time.sleep(2)
    with socket.socket(socket.AF_UNIX) as control:
        control.connect(str(monitor))
        stream = control.makefile('rwb', buffering=0)
        json.loads(stream.readline())
        def execute(name, arguments=None):
            stream.write((json.dumps({'execute': name, 'arguments': arguments or {}}) + '\n').encode())
            while True:
                response = json.loads(stream.readline())
                if 'error' in response:
                    raise RuntimeError(response)
                if 'return' in response:
                    return response['return']
        execute('qmp_capabilities')
        screenshot = build / 'browser-screen.ppm'
        execute('screendump', {'filename': str(screenshot)})
        if os.environ.get('INPUT_SCENARIO'):
            import runpy
            runpy.run_path(str(project / 'tests/input-scenario.py'))['run'](execute, build, log)
        execute('send-key', {'keys': [{'type': 'qcode', 'data': 'f12'}]})
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            text = log.read_text(errors='replace')
            if 'FAIL:' in text or 'Exception Type' in text:
                raise RuntimeError(text)
            if 'Tests finished. Press any key' in text:
                execute('send-key', {'keys': [{'type': 'qcode', 'data': 'ret'}]})
                break
            time.sleep(0.2)
        else:
            raise TimeoutError('Blink shutdown did not finish: ' + text)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        text = log.read_text(errors='replace')
        if 'FAIL:' in text or 'Exception Type' in text:
            raise RuntimeError(text)
        if 'PASS: EFI loader released payload, stack and TLS' in text:
            break
        time.sleep(0.2)
    else:
        raise TimeoutError('EFI return did not finish: ' + text)
    print(text)
    print('Captured Blink framebuffer: ' + str(screenshot))
    print('Visual inspection is required to confirm the site content and rendering fidelity.')
finally:
    if audio_capture and process.poll() is None:
        # Request a normal emulator shutdown before analyzing the captured PCM.
        try:
            with socket.socket(socket.AF_UNIX) as cleanup:
                cleanup.settimeout(3)
                cleanup.connect(str(monitor))
                connection = cleanup.makefile('rwb', buffering=0)
                connection.readline()
                connection.write(b'{"execute":"qmp_capabilities"}\n')
                for attempt in range(32):
                    reply = connection.readline()
                    if not reply: raise EOFError('QMP closed before capabilities response')
                    if b'"return"' in reply: break
                else: raise OSError('QMP capabilities response missing')
                connection.write(b'{"execute":"quit"}\n')
                process.wait(timeout=5)
        except (OSError, EOFError, subprocess.TimeoutExpired):
            pass
    if process.poll() is None: process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()

socket_directory.cleanup()
