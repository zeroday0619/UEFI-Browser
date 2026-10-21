#!/usr/bin/env python3
"""Check host initialization for a future UEFI Virtio-GPU Venus backend."""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--render-node', type=Path, default=Path('/dev/dri/renderD128'))
parser.add_argument('--icd', type=Path)
parser.add_argument('--report', type=Path, required=True)
options = parser.parse_args()
qemu = shutil.which('qemu-system-x86_64')
if not qemu:
    parser.error('qemu-system-x86_64 is required.')
if not options.render_node.exists():
    parser.error('The render node does not exist.')
if options.icd and not options.icd.is_file():
    parser.error('The Vulkan ICD manifest does not exist.')
environment = os.environ.copy()
if options.icd:
    environment['VK_DRIVER_FILES'] = str(options.icd.resolve())
report = {'qemu_device_created': False, 'guest_execution_verified': False,
          'hardware_rendering_verified': False, 'render_node': str(options.render_node),
          'icd_manifest': str(options.icd.resolve()) if options.icd else None,
          'scope': 'Paused QEMU device initialization only; no guest GPU command is submitted.'}
process = None
try:
    report['qemu_version'] = subprocess.check_output([qemu, '--version'], text=True, timeout=10).splitlines()[0]
    with tempfile.TemporaryDirectory(prefix='uefi-venus-host-') as directory:
        temporary = Path(directory)
        monitor = temporary / 'qmp'
        arguments = [qemu, '-machine', 'q35,memory-backend=memory',
                     '-accel', 'kvm,honor-guest-pat=on', '-m', '512M',
                     '-object', 'memory-backend-memfd,id=memory,size=512M,share=on',
                     '-nodefaults', '-S', '-display',
                     'egl-headless,rendernode=' + str(options.render_node),
                     '-device', 'virtio-gpu-gl-pci,blob=on,venus=on,hostmem=512M',
                     '-qmp', 'unix:' + str(monitor) + ',server=on,wait=off']
        report['arguments'] = arguments
        with (temporary / 'stderr').open('w+') as errors:
            process = subprocess.Popen(arguments, env=environment, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=errors)
            try:
                deadline = time.monotonic() + 30
                while not monitor.exists():
                    if process.poll() is not None:
                        raise RuntimeError('QEMU exited before exposing its monitor.')
                    if time.monotonic() >= deadline:
                        raise TimeoutError('QEMU monitor initialization timed out.')
                    time.sleep(.05)
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(30)
                    connection.connect(str(monitor))
                    stream = connection.makefile('rwb', buffering=0)
                    greeting = json.loads(stream.readline())
                    if 'QMP' not in greeting:
                        raise RuntimeError('Missing QMP greeting.')
                    def execute(name):
                        stream.write((json.dumps({'execute': name, 'id': name}) + '\n').encode())
                        while True:
                            line = stream.readline()
                            if not line:
                                raise RuntimeError('QMP disconnected during ' + name)
                            reply = json.loads(line)
                            if reply.get('id') != name:
                                continue
                            if 'error' in reply:
                                raise RuntimeError(str(reply['error']))
                            return reply['return']
                    execute('qmp_capabilities')
                    report['status'] = execute('query-status')
                    report['pci'] = execute('query-pci')
                    devices = [device for bus in report['pci'] for device in bus['devices']]
                    gpu = [device for device in devices if device.get('id', {}).get('vendor') == 0x1af4
                           and device.get('id', {}).get('device') == 0x1050]
                    if not gpu or report['status']['running']:
                        raise RuntimeError('Expected a paused Virtio-GPU device.')
                    report['qemu_device_created'] = True
                    execute('quit')
                    process.wait(timeout=10)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                errors.seek(0)
                report['stderr'] = errors.read()
                report['exit_code'] = process.returncode
except Exception as error:
    report['failure'] = str(error)
finally:
    options.report.parent.mkdir(parents=True, exist_ok=True)
    options.report.write_text(json.dumps(report, indent=2) + '\n')
if report.get('failure'):
    raise SystemExit('FAIL: ' + report['failure'] + '\n' + report.get('stderr', ''))
print('PASS: paused QEMU Venus device initialized; guest and hardware rendering remain unverified.')
