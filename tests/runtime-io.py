#!/usr/bin/env python3
"""Verify firmware TCP and persistent file services across two QEMU boots."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--prefix', default='runtime-io')
options = parser.parse_args()
project = Path(__file__).resolve().parents[1]
build = options.build.resolve()
application = build / 'esp/EFI/BOOT/BOOTX64.EFI'
disk = build / (options.prefix + '.img')
module_spec = importlib.util.spec_from_file_location('fat_image', project / 'tests/fat-image.py')
fat_image = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(fat_image)
# Exclusive creation prevents a validation run from erasing an existing profile.
fat_image.create(disk, application)
environment = os.environ.copy()
environment.update(BUILD_DIRECTORY=str(build), QEMU_TEST_ACCEL='kvm', QEMU_TEST_CPUS='4',
                   QEMU_TEST_DISK_IMAGE=str(disk))
environment.pop('INPUT_SCENARIO', None)
results = {'image_sha256': hashlib.sha256(application.read_bytes()).hexdigest(), 'boots': []}
for generation in (1, 2):
    prefix = project / 'validation' / f'{options.prefix}-{generation}'
    with Path(str(prefix) + '-runner.log').open('w') as output:
        completed = subprocess.run(['python3', 'tests/browser-qemu.py'], cwd=project,
                                   env=environment, stdout=output, stderr=subprocess.STDOUT,
                                   timeout=400)
    serial = build / 'browser-serial.log'
    shutil.copy(serial, Path(str(prefix) + '.log'))
    text = serial.read_text(errors='replace')
    assert completed.returncode == 0, f'QEMU failed: {prefix}'
    assert 'FAIL:' not in text and 'PASS: EFI loader released payload, stack and TLS' in text
    for owner in (0, 1):
        assert f'IO PROBE: file_generation={generation} owner={owner}' in text
        assert f'IO PROBE: TCP nonblocking connect, binary send, poll, receive and EOF passed owner={owner}' in text
    assert 'IO PROBE: renderer cannot use browser-owned descriptors' in text
    files = {}
    for name in ('browser-probe.dat', 'renderer-probe.dat'):
        data = fat_image.read_file(disk, 'EFI/UEFIBrowser/Profile/' + name)
        assert data == generation.to_bytes(4, 'little'), (name, data)
        files[name] = data.hex()
    assert fat_image.read_file(disk, 'EFI/UEFIBrowser/Profile/browser-owned.dat') == b''
    try:
        fat_image.read_file(disk, 'EFI/UEFIBrowser/Profile/transient-probe.dat')
    except FileNotFoundError:
        pass
    else:
        raise AssertionError('Deleted file survived the boot')
    results['boots'].append({'generation': generation, 'files': files, 'deleted_file_absent': True})
    print(f'Boot {generation}: TCP, descriptor ownership and persistent files passed.', flush=True)
(project / 'validation' / (options.prefix + '-results.json')).write_text(json.dumps(results, indent=2) + '\n')
