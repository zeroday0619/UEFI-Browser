#!/usr/bin/env python3
"""Execute the musl/Mesa Venus ICD in an isolated UEFI guest using the host GPU."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

project = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--prefix', required=True)
parser.add_argument('--cpl3', action='store_true')
parser.add_argument('--image', type=Path, default=project / 'build/venus-runtime-efi/esp/EFI/BOOT/BOOTX64.EFI')
parser.add_argument('--icd', type=Path, default=Path('/etc/vulkan/icd.d/nvidia_icd.json'))
options = parser.parse_args()
if not re.fullmatch(r'[A-Za-z0-9_-]+', options.prefix):
    parser.error('Invalid validation prefix.')
log = project / 'validation' / (options.prefix + '.log')
if log.exists():
    parser.error('Choose an unused validation prefix.')
report = {'image_sha256': hashlib.sha256(options.image.read_bytes()).hexdigest(),
          'passed': False, 'mesa_runtime_execution': False}
environment = os.environ.copy()
environment['VK_DRIVER_FILES'] = str(options.icd.resolve(strict=True))
with tempfile.TemporaryDirectory(prefix='uefi-mesa-runtime-') as directory:
    temporary = Path(directory)
    boot = temporary / 'esp/EFI/BOOT'
    boot.mkdir(parents=True)
    shutil.copyfile(options.image, boot / 'BOOTX64.EFI')
    shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', temporary / 'variables.fd')
    monitor = temporary / 'qmp'
    arguments = ['qemu-system-x86_64', '-machine', 'q35,memory-backend=memory',
        '-accel', 'kvm,honor-guest-pat=on', '-m', '1024M', '-smp', '2' if options.cpl3 else '1',
        '-object', 'memory-backend-memfd,id=memory,size=1024M,share=on',
        '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
        '-drive', 'if=pflash,format=raw,file=' + str(temporary / 'variables.fd'),
        '-drive', 'format=raw,file=fat:rw:' + str(temporary / 'esp'),
        '-vga', 'std', '-net', 'none', '-device', 'virtio-rng-pci', '-serial', 'file:' + str(log),
        '-qmp', 'unix:' + str(monitor) + ',server=on,wait=off', '-no-reboot',
        '-display', 'egl-headless,rendernode=/dev/dri/renderD128',
        '-device', 'virtio-gpu-gl-pci,blob=on,venus=on,hostmem=512M']
    report['arguments'] = arguments
    with (temporary / 'stderr').open('w+') as errors:
        process = subprocess.Popen(arguments, env=environment, stdout=subprocess.DEVNULL, stderr=errors)
        try:
            deadline = time.monotonic() + 120
            while True:
                content = log.read_text(errors='replace') if log.exists() else ''
                if ('PASS: protected Mesa Venus runtime and broker cleanup' if options.cpl3 else
                    'PASS: UEFI Mesa Venus runtime Vulkan execution') in content:
                    break
                if process.poll() is not None or any(marker in content for marker in
                        ('FAIL:', 'Exception Type', 'ASSERT', 'runtime aborted')):
                    raise RuntimeError('Guest or host failed before completing the Mesa runtime probe.')
                if time.monotonic() > deadline:
                    raise TimeoutError('Mesa runtime probe timed out.')
                time.sleep(.1)
            if options.cpl3 and 'VENUS PROTECTED: runtime cpl=3' not in content:
                raise RuntimeError('Missing unprivileged execution evidence.')
            if options.cpl3 and content.count('PASS: Venus CPL3 session, resource, range, quota, fence, NX and alias boundaries') != 3:
                raise RuntimeError('Missing device boundary checks.')
            report['cpl3'] = options.cpl3
            report['devices'] = re.findall(r'MESA RUNTIME: device=(.+) type=(\d+) vendor=([0-9a-f]+)', content)
            report['timestamps'] = [[int(value) for value in match] for match in
                re.findall(r'timestamps=(\d+),(\d+) validated_bytes=65536', content)]
            report['cycles'] = content.count('PASS: Mesa runtime Vulkan queue, GPU buffer readback and teardown')
            report['shader_cycles'] = content.count('PASS: Mesa runtime shader rendering and presentation')
            report['clock_waits'] = content.count('PASS: Mesa monotonic relative and absolute waits')
            report['resets'] = content.count('PASS: Venus runtime transport reset and DMA cleanup')
            report['fences'] = [[int(value) for value in match] for match in
                re.findall(r'VENUS TRANSPORT: completed=(\d+) submitted=(\d+)', content)]
            if (report['cycles'] != 3 or report['shader_cycles'] != 3 or report['clock_waits'] != 3 or report['resets'] != 3 or len(report['timestamps']) != 3 or
                len(report['devices']) != 3 or len(report['fences']) != 3 or
                not all(0 < first < last for first, last in report['timestamps']) or
                not all(int(device[1]) in (1, 2) for device in report['devices']) or
                not all(done == submitted and done > 10 for done, submitted in report['fences'])):
                raise RuntimeError('Missing queue execution, readback or cleanup evidence.')
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(10)
                connection.connect(str(monitor))
                stream = connection.makefile('rwb', buffering=0)
                json.loads(stream.readline())
                screenshot = project / 'validation' / (options.prefix + '.ppm')
                for name in ('qmp_capabilities', 'screendump', 'quit'):
                    command = {'execute': name, 'id': name}
                    if name == 'screendump': command['arguments'] = {'filename': str(screenshot)}
                    stream.write((json.dumps(command) + '\n').encode())
                    while True:
                        line = stream.readline()
                        if not line:
                            raise RuntimeError('QMP disconnected.')
                        reply = json.loads(line)
                        if reply.get('id') == name:
                            if 'error' in reply: raise RuntimeError(str(reply['error']))
                            break
            process.wait(timeout=10)
            if process.returncode:
                raise RuntimeError('QEMU exited unsuccessfully.')
            pixels = project / 'validation' / (options.prefix + '-pixels.json')
            subprocess.run([sys.executable, str(project / 'tests/venus-render-output.py'),
                            str(screenshot), str(pixels)], check=True)
            report['presentation'] = json.loads(pixels.read_text())
            report['passed'] = report['mesa_runtime_execution'] = True
        except Exception as error:
            report['failure'] = str(error)
        finally:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            errors.seek(0)
            report['stderr'] = errors.read()
            report['exit_code'] = process.returncode
            (project / 'validation' / (options.prefix + '.json')).write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: value for key, value in report.items() if key != 'arguments'}, indent=2))
raise SystemExit(0 if report['passed'] else 1)
