#!/usr/bin/env python3
"""Verify JIT permissions and actual CPL3 fault containment in QEMU."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-directory', type=Path, default=Path('build/jit-boundary'))
parser.add_argument('--prefix', default='jit-wx-boundary')
parser.add_argument('--require-transition-benchmark', action='store_true')
options = parser.parse_args()
if not options.prefix.replace('-', '').replace('_', '').isalnum():
    parser.error('Prefix must contain letters, digits, hyphens or underscores.')
project = Path(__file__).resolve().parents[1]
image = (project / options.build_directory / 'esp/EFI/BOOT/BOOTX64.EFI').resolve()
log = project / 'validation' / (options.prefix + '.log')
record = {'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
          'passed': False}
with tempfile.TemporaryDirectory(prefix='uefi-jit-boundary-') as directory:
    temporary = Path(directory)
    (temporary / 'esp/EFI/BOOT').mkdir(parents=True)
    shutil.copy(image, temporary / 'esp/EFI/BOOT/BOOTX64.EFI')
    shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd', temporary / 'variables.fd')
    command = ['qemu-system-x86_64', '-machine', 'q35,accel=kvm',
               '-m', '2048', '-smp', '2',
               '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive', f'if=pflash,format=raw,file={temporary / "variables.fd"}',
               '-drive', f'format=raw,file=fat:rw:{temporary / "esp"}',
               '-device', 'virtio-rng-pci', '-display', 'none',
               '-serial', f'file:{log}', '-no-reboot']
    log.write_text('')
    with (project / 'validation' / (options.prefix + '-runner.log')).open('w') as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                text = log.read_text(errors='replace')
                if 'FAIL:' in text or 'Exception Type' in text:
                    raise RuntimeError(text)
                if 'Tests finished. Press any key' in text:
                    break
                if process.poll() is not None:
                    raise RuntimeError('QEMU stopped before runtime completion.')
                time.sleep(.1)
            else:
                raise TimeoutError(text)
            cases = [tuple(int(value, 16 if index in (2, 3) else 10)
                           for index, value in enumerate(match))
                     for match in re.findall(
                         r'CPL3: case=(\d+) vector=(\d+) error=([0-9a-f]+) cs=([0-9a-f]+)',
                         text)]
            assert [case[0] for case in cases] == list(range(1, 15)), cases
            assert all(case[3] & 3 == 3 for case in cases), cases
            assert [(case[1], case[2]) for case in cases[-3:]] == [(14, 7), (14, 21), (14, 7)], cases
            assert 'PASS: JIT RX execution, RW/NX writes, alias exclusion, nested scopes, TLS switch and reuse' in text
            assert 'PASS: JIT discard preserves RX, clears contents and reuses backing' in text
            assert 'PASS: CPL3 JIT executable writes, writable execution and backing-alias writes blocked' in text
            transitions = [(int(sample), int(elapsed)) for sample, elapsed in re.findall(
                r'JIT PROBE: transition_pairs=1000 aliases=66 sample=(\d+) elapsed_us=(\d+)', text)]
            if options.require_transition_benchmark:
                assert [sample for sample, _ in transitions] == [0, 1, 2], transitions
                assert all(elapsed > 0 for _, elapsed in transitions), transitions
            record.update(passed=True, cases=cases,
                          transition_pairs=1000 if transitions else None,
                          transition_aliases=66 if transitions else None,
                          transition_samples_us=[elapsed for _, elapsed in transitions])
            print('PASS: 14 real CPL3 boundary cases, JIT execution, per-thread scopes and three protected-memory faults')
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            (project / 'validation' / (options.prefix + '.json')).write_text(
                json.dumps(record, indent=2) + '\n')
