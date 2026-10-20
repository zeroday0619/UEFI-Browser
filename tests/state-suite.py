#!/usr/bin/env python3
"""Run isolated QEMU graphics, media, network, storage, input and scroll regressions and retain evidence."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import threading
import time
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cpus', type=int, default=4, choices=(1, 4))
parser.add_argument('--workers', type=int, default=2, choices=(1, 2))
parser.add_argument('--https-ca', type=Path, required=True)
parser.add_argument('--prefix', default='state-release')
parser.add_argument('cases', nargs='*')
options = parser.parse_args()
if not options.prefix.replace('-', '').replace('_', '').isalnum():
    parser.error('Prefix must contain letters, digits, hyphens or underscores.')
project = Path(__file__).resolve().parents[1]
os.chdir(project)
cases = options.cases or [
    'webgpu', 'webgl', 'webgl2', 'webgpu-canvas', 'gpu-lifecycle',
    'workers', 'workers-limits', 'audio', 'audio-limits', 'media', 'media-boundaries', 'indexeddb', 'cache', 'websocket', 'storage', 'storage-lifecycle', 'network', 'network-secure',
    'scroll-state', 'scroll-stuck', 'scroll-state-iframe', 'cookies', 'cookies-secure',
    'scroll-gutter-topology', 'scroll-gutter-replacement', 'scroll-gutter-state',
    'scroll-gutter-fallback', 'scroll-gutter-parent-frame', 'scroll-gutter-nested-frame',
    'scroll-gutter-root', 'scroll-gutter-horizontal', 'scroll-layout', 'scroll-controls',
    'scroll-style', 'scroll-input', 'scroll-tree', 'tabs-site', 'frame-policy',
]
if len(cases) != len(set(cases)):
    parser.error('Each scenario must appear once.')
source = project / 'build/blink/esp'
image_hash = hashlib.sha256((source / 'EFI/BOOT/BOOTX64.EFI').read_bytes()).hexdigest()
pending = queue.Queue()
for case in cases:
    pending.put(case)
lock = threading.Lock()
summary = {'image_sha256': image_hash, 'workers': options.workers, 'cpus': options.cpus, 'cases': []}


def save_png(source_path, destination):
    header = source_path.read_bytes().split(b'\n', 3)
    assert header[0] == b'P6' and header[2] == b'255', source_path
    width, height = map(int, header[1].split())
    pixels = header[3]
    assert len(pixels) == width * height * 3, source_path
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    destination.write_bytes(b'\x89PNG\r\n\x1a\n' +
                           chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def worker(index):
    build = project / 'build/state-workers' / options.prefix / str(index)
    shutil.copytree(source, build / 'esp', dirs_exist_ok=True)
    assert hashlib.sha256((build / 'esp/EFI/BOOT/BOOTX64.EFI').read_bytes()).hexdigest() == image_hash
    while True:
        try:
            case = pending.get_nowait()
        except queue.Empty:
            return
        for pattern in ('input-*.ppm', '*results.json'):
            for item in build.glob(pattern):
                item.unlink()
        environment = os.environ.copy()
        environment.update(BUILD_DIRECTORY=str(build), QEMU_TEST_ACCEL='kvm', QEMU_TEST_CPUS=str(options.cpus), INPUT_SCENARIO=case)
        environment.pop('TEST_CA_FILE', None)
        environment.pop('QEMU_TEST_AUDIO_CAPTURE', None)
        if case == 'renderer-recovery':
            import socket
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                environment['QEMU_TEST_GDB_PORT'] = str(listener.getsockname()[1])
            shutil.copy(project / 'build/blink/payload.elf', build / 'payload.elf')
        if case == 'audio': environment['QEMU_TEST_AUDIO_CAPTURE'] = str(build / 'audio-output.wav')
        if case in ('cookies-secure', 'network-secure', 'websocket', 'cache', 'indexeddb', 'media', 'media-boundaries', 'audio', 'audio-limits', 'workers', 'workers-limits', 'webgpu', 'webgl', 'webgl2', 'gpu-lifecycle', 'webgpu-canvas', 'service-capabilities'):
            environment['TEST_CA_FILE'] = str(options.https_ca.resolve())
        if case in ('cache', 'indexeddb', 'media', 'media-boundaries', 'audio', 'audio-limits', 'workers', 'workers-limits', 'webgpu', 'webgl', 'webgl2', 'gpu-lifecycle', 'webgpu-canvas', 'service-capabilities'):
            import runpy
            disk = build / (case + '-profile.img')
            runpy.run_path(str(project / 'tests/fat-image.py'))['create'](disk, build / 'esp/EFI/BOOT/BOOTX64.EFI')
            environment['QEMU_TEST_DISK_IMAGE'] = str(disk)
        output_prefix = project / 'validation'  / (options.prefix + '-' + case)
        started = time.monotonic()
        with Path(str(output_prefix) + '-runner.log').open('w') as output:
            result = subprocess.run(['python3', 'tests/browser-qemu.py'], env=environment,
                                    stdout=output, stderr=subprocess.STDOUT)
        shutil.copy(build / 'browser-serial.log', Path(str(output_prefix) + '.log'))
        for screenshot in build.glob('input-*.ppm'):
            save_png(screenshot, Path(str(output_prefix) + '-' + screenshot.stem + '.png'))
        for measurements in build.glob('*results.json'):
            shutil.copy(measurements, Path(str(output_prefix) + '-' + measurements.name))
        if case == 'audio' and result.returncode == 0:
            shutil.copy(build / 'audio-output.wav', Path(str(output_prefix) + '-raw.wav'))
            check = subprocess.run(['python3', 'tests/check-audio-output.py', str(build / 'audio-output.wav'), str(output_prefix) + '-spectrum.json'])
            if check.returncode: result.returncode = check.returncode
        record = {'scenario': case, 'exit_code': result.returncode, 'seconds': round(time.monotonic() - started, 2)}
        with lock:
            summary['cases'].append(record)
            (project / 'validation' / (options.prefix + '-suite.json')).write_text(json.dumps(summary, indent=2) + '\n')
            print(record, flush=True)


with concurrent.futures.ThreadPoolExecutor(max_workers=options.workers) as executor:
    list(executor.map(worker, range(options.workers)))
if any(case['exit_code'] for case in summary['cases']):
    raise SystemExit(1)
