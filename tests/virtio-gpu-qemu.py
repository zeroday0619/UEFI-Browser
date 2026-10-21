#!/usr/bin/env python3
"""Boot a standalone UEFI Virtio-GPU control-queue probe in an isolated QEMU guest."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--backend', choices=('venus', 'software'), default='venus')
parser.add_argument('--icd', type=Path)
parser.add_argument('--image', type=Path)
parser.add_argument('--venus-commands', action='store_true')
parser.add_argument('--gpu-queue', action='store_true')
parser.add_argument('--render', action='store_true')
parser.add_argument('--prefix', default='virtio-gpu-probe')
options = parser.parse_args()
if not options.prefix or any(character not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for character in options.prefix):
    parser.error('The prefix must contain only letters, digits, hyphens and underscores.')
project = Path(__file__).resolve().parents[1]
image = options.image or project / 'build/virtio-gpu-probe/esp/EFI/BOOT/BOOTX64.EFI'
log = project / ('validation/' + options.prefix + '.log')
if log.exists():
    parser.error('The validation prefix already has a log; choose a new prefix.')
report = {'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
          'backend': options.backend, 'hardware_rendering_verified': False}
environment = os.environ.copy()
if options.icd:
    if not options.icd.is_file():
        parser.error('The ICD manifest does not exist.')
    environment['VK_DRIVER_FILES'] = str(options.icd.resolve())
with tempfile.TemporaryDirectory(prefix='uefi-virtio-gpu-') as directory:
    temporary = Path(directory)
    boot = temporary / 'esp/EFI/BOOT'
    boot.mkdir(parents=True)
    shutil.copy(image, boot / 'BOOTX64.EFI')
    shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd', temporary / 'variables.fd')
    monitor = temporary / 'qmp'
    arguments = ['qemu-system-x86_64', '-machine', 'q35,memory-backend=memory',
                 '-accel', 'kvm,honor-guest-pat=on', '-m', '1024M', '-smp', '1',
                 '-object', 'memory-backend-memfd,id=memory,size=1024M,share=on',
                 '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                 '-drive', 'if=pflash,format=raw,file=' + str(temporary / 'variables.fd'),
                 '-drive', 'format=raw,file=fat:rw:' + str(temporary / 'esp'),
                 '-vga', 'std', '-net', 'none', '-serial', 'file:' + str(log),
                 '-qmp', 'unix:' + str(monitor) + ',server=on,wait=off', '-no-reboot']
    if options.backend == 'venus':
        arguments += ['-display', 'egl-headless,rendernode=/dev/dri/renderD128',
                      '-device', 'virtio-gpu-gl-pci,blob=on,venus=on,hostmem=512M']
    else:
        arguments += ['-display', 'none', '-device', 'virtio-gpu-pci']
    report['arguments'] = arguments
    with (temporary / 'stderr').open('w+') as errors:
        process = subprocess.Popen(arguments, env=environment, stdout=subprocess.DEVNULL, stderr=errors)
        try:
            expected = ('PASS: UEFI Virtio control queue and Venus capability payload'
                        if options.backend == 'venus' else 'FAIL: required Venus transport features absent')
            deadline = time.monotonic() + 180
            while True:
                content = log.read_text(errors='replace') if log.exists() else ''
                if expected in content and 'VIRTIO GPU: reset acknowledged' in content:
                    report['expected_marker'] = expected
                    report['reset_cleanup'] = True
                    report['dma_released'] = 'VIRTIO GPU: DMA backing released' in content
                    report['queue_wraps'] = 'completed three ring wraps' in content
                    report['context_lifecycle'] = 'identifier reused three times' in content
                    report['guest_blob_lifecycle'] = 'blob created, attached, detached and released three times' in content
                    report['index_wrap'] = 'crossed the 16-bit index boundary' in content
                    fences = re.search(r'validated_fences=0x([0-9a-f]+)', content)
                    report['validated_fences'] = int(fences[1], 16) if fences else 0
                    if options.backend == 'venus' and not (
                            report['dma_released'] and report['queue_wraps'] and
                            report['context_lifecycle'] and report['guest_blob_lifecycle'] and
                            report['index_wrap'] and report['validated_fences'] >= 65558):
                        raise RuntimeError('Missing DMA, queue, context or fence evidence.')
                    if (options.venus_commands or options.gpu_queue or options.render) and options.backend == 'venus':
                        marker = 'PASS: Venus Vulkan physical devices queried and instance destroyed'
                        report['vulkan_instance_cycles'] = content.count(marker)
                        report['physical_devices'] = re.findall(r'VENUS: physical_device_name=(.+)', content)
                        report['device_types'] = [int(value, 16) for value in re.findall(r'VENUS: device_type=0x([0-9a-f]+)', content)]
                        if report['vulkan_instance_cycles'] != 3 or not report['physical_devices']:
                            raise RuntimeError('Missing Venus Vulkan command evidence.')
                        if options.gpu_queue or options.render:
                            report['gpu_queue_cycles'] = content.count('PASS: Venus graphics queue executed GPU timestamp commands')
                            starts = [int(value, 16) for value in re.findall(r'gpu_timestamp_begin=0x([0-9a-f]+)', content)]
                            ends = [int(value, 16) for value in re.findall(r'gpu_timestamp_end=0x([0-9a-f]+)', content)]
                            report['gpu_timestamps'] = list(zip(starts, ends))
                            if (report['gpu_queue_cycles'] != 3 or len(starts) != 3 or len(ends) != 3 or
                                    not all(0 < start < end for start, end in zip(starts, ends))):
                                raise RuntimeError('Missing completed GPU timestamp queries.')
                            report['gpu_queue_execution_verified'] = True
                    break
                if process.poll() is not None or 'Exception Type' in content or 'ASSERT' in content:
                    raise RuntimeError('The guest stopped before completing the probe.')
                if 'FAIL:' in content and expected not in content:
                    raise RuntimeError('The guest reported a probe failure.')
                if time.monotonic() >= deadline:
                    raise TimeoutError('The UEFI GPU probe timed out.')
                time.sleep(.1)
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(10)
                connection.connect(str(monitor))
                stream = connection.makefile('rwb', buffering=0)
                json.loads(stream.readline())
                def execute(name, arguments=None):
                    request = {'execute': name, 'id': name}
                    if arguments is not None:
                        request['arguments'] = arguments
                    stream.write((json.dumps(request) + '\n').encode())
                    while True:
                        line = stream.readline()
                        if not line:
                            raise RuntimeError('QMP disconnected.')
                        reply = json.loads(line)
                        if reply.get('id') != name:
                            continue
                        if 'error' in reply:
                            raise RuntimeError(str(reply['error']))
                        return reply['return']
                execute('qmp_capabilities')
                if options.render and options.backend == 'venus':
                    if (content.count('PASS: Venus GPU triangle pixels validated') != 3 or
                            content.count('PASS: Venus GPU image presented through independent GOP framebuffer') != 3 or
                            content.count('PASS: Venus render resources destroyed') != 3):
                        raise RuntimeError('Missing GPU rendering, presentation or teardown evidence.')
                    screenshot = project / ('validation/' + options.prefix + '.ppm')
                    execute('screendump', {'filename': str(screenshot)})
                    pixel_report = project / ('validation/' + options.prefix + '-pixels.json')
                    subprocess.run(['python3', str(project / 'tests/venus-render-output.py'), str(screenshot), str(pixel_report)], check=True)
                    report['render_pixels'] = json.loads(pixel_report.read_text())
                    report['gpu_rendering_verified'] = True
                    report['hardware_rendering_verified'] = bool(report.get('device_types')) and report['device_types'][0] in (1, 2)
                execute('quit')
            process.wait(timeout=10)
            if process.returncode != 0:
                raise RuntimeError('QEMU did not shut down successfully.')
            report['passed'] = True
        except Exception as error:
            report['passed'] = False
            report['failure'] = str(error)
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
            (project / ('validation/' + options.prefix + '.json')).write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: value for key, value in report.items() if key not in ('arguments',)}, indent=2))
if not report['passed']:
    raise SystemExit(1)
