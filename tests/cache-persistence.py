#!/usr/bin/env python3
"""Verify Cache Storage across QEMU boots and a corrupt latest snapshot."""
import argparse
import hashlib
import importlib.util
import json
import mmap
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

project = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--prefix', default='cache-persistence')
parser.add_argument('--https-ca', type=Path, required=True)
options = parser.parse_args()
if not options.prefix.replace('-', '').replace('_', '').isalnum():
    parser.error('Prefix must contain letters, digits, hyphens or underscores.')
build = project / 'build' / options.prefix
application = build / 'esp/EFI/BOOT/BOOTX64.EFI'
application.parent.mkdir(parents=True, exist_ok=True)
shutil.copy(project / 'build/blink/esp/EFI/BOOT/BOOTX64.EFI', application)
spec = importlib.util.spec_from_file_location('fat_image', project / 'tests/fat-image.py')
fat_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fat_image)
disk = build / 'profile.img'
fat_image.create(disk, application)
environment = os.environ.copy()
environment.update(BUILD_DIRECTORY=str(build), QEMU_TEST_ACCEL='kvm', QEMU_TEST_CPUS='4',
                   QEMU_TEST_DISK_IMAGE=str(disk), INPUT_SCENARIO='cache',
                   TEST_CA_FILE=str(options.https_ca.resolve()))
results = {'image_sha256': hashlib.sha256(application.read_bytes()).hexdigest(), 'boots': []}
expected_fallback = None
marker = b'UEFICS01:158.0.8110.0\0'
for generation in (1, 2, 3):
    prefix = project / 'validation' / f'{options.prefix}-{generation}'
    with Path(str(prefix) + '-runner.log').open('w') as output:
        completed = subprocess.run(['python3', 'tests/browser-qemu.py'], cwd=project, env=environment,
                                   stdout=output, stderr=subprocess.STDOUT, timeout=400)
    serial = build / 'browser-serial.log'
    shutil.copy(serial, Path(str(prefix) + '.log'))
    text = serial.read_text(errors='replace')
    assert completed.returncode == 0, prefix
    assert 'CACHE PASS checks=35' in text and 'CACHE FAIL' not in text
    assert f'CACHE GENERATION {generation}' in text
    if expected_fallback is not None:
        loaded = re.search(r'CACHE: loaded generation=(\d+)', text)
        assert loaded and int(loaded[1]) == expected_fallback, (loaded, expected_fallback)
    snapshots = {}
    directory = fat_image.read_file(disk, 'EFI/UEFIBrowser/Profile')
    long_parts = {}
    for offset in range(0, len(directory), 32):
        entry = directory[offset:offset + 32]
        if not entry[0]: break
        if entry[0] == 0xe5: long_parts = {}; continue
        if entry[11] == 15:
            long_parts[entry[0] & 31] = entry[1:11] + entry[14:26] + entry[28:32]
            continue
        name = b''.join(long_parts[index] for index in sorted(long_parts)).decode('utf-16-le').split('\0')[0].rstrip('\uffff')
        long_parts = {}
        if not name.startswith('cache-'): continue
        data = fat_image.read_file(disk, 'EFI/UEFIBrowser/Profile/' + name)
        assert data.startswith(marker) and hashlib.sha256(data[:-32]).digest() == data[-32:]
        snapshots[name] = (struct.unpack_from('<Q', data, len(marker))[0], data)
    assert len(snapshots) >= 2
    results['boots'].append({'counter': generation, 'checks': 35,
                            'valid_snapshots': len(snapshots), 'fallback_generation': expected_fallback})
    if generation == 2:
        latest = max(snapshots, key=lambda name: snapshots[name][0])
        alternate = latest[:-1] + str(1 - int(latest[-1]))
        expected_fallback = snapshots[alternate][0]
        payload = snapshots[latest][1]
        with disk.open('r+b') as image, mmap.mmap(image.fileno(), 0) as mapping:
            position = fat_image.file_byte_offset(disk, 'EFI/UEFIBrowser/Profile/' + latest, len(payload) - 1)
            assert position >= (fat_image.PARTITION + fat_image.DATA_SECTOR) * fat_image.SECTOR
            assert mapping[position] == payload[-1]
            mapping[position] ^= 1
            mapping.flush()
        results['corrupted_snapshot'] = {'filename': latest, 'generation': snapshots[latest][0],
                                         'expected_fallback_generation': expected_fallback}
    (project / 'validation' / (options.prefix + '-results.json')).write_text(json.dumps(results, indent=2) + '\n')
    print(f'Boot {generation}: Cache Storage retained; 35 checks passed.', flush=True)
