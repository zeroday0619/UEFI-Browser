#!/usr/bin/env python3
"""Verify that a single-CPU CPL3 browser refuses privileged rendering."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-directory', type=Path, default=Path('build/blink'))
parser.add_argument('--prefix', default='cpl3-single-cpu')
options = parser.parse_args()
project = Path(__file__).resolve().parents[1]
if not options.prefix.replace('-', '').replace('_', '').isalnum():
    parser.error('Prefix must contain letters, digits, hyphens or underscores.')
image = (project / options.build_directory / 'esp/EFI/BOOT/BOOTX64.EFI').resolve()
image_hash = hashlib.sha256(image.read_bytes()).hexdigest()
log = project / 'validation' / (options.prefix + '.log')
marker = 'FAIL: CPL3 renderer unavailable; privileged rendering is disabled in this build'
with tempfile.TemporaryDirectory(prefix='uefi-cpl3-startup-') as directory:
    temporary = Path(directory)
    (temporary / 'esp/EFI/BOOT').mkdir(parents=True)
    shutil.copy(image, temporary / 'esp/EFI/BOOT/BOOTX64.EFI')
    shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd', temporary / 'variables.fd')
    command = ['qemu-system-x86_64', '-machine', 'q35,accel=kvm', '-m', '4096', '-smp', '1',
               '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive', f'if=pflash,format=raw,file={temporary / "variables.fd"}',
               '-drive', f'format=raw,file=fat:rw:{temporary / "esp"}',
               '-device', 'virtio-rng-pci', '-display', 'none', '-serial', f'file:{log}', '-no-reboot']
    log.write_text('')
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            text = log.read_text(errors='replace') if log.exists() else ''
            if marker in text:
                # Observe the refusal through engine cleanup rather than accepting a transient message.
                if 'Tests finished. Press any key' in text:
                    break
            if 'Exception Type' in text or 'FATAL:' in text or process.poll() is not None:
                raise RuntimeError(text)
            time.sleep(.1)
        else:
            raise TimeoutError(text)
        assert 'Blink/V8 event loop' not in text
        assert 'BLINK: non-composited main frame created' not in text
        assert 'local fallback' not in text
        result = {'image_sha256': image_hash, 'cpus': 1, 'refused_privileged_fallback': True}
        (project / 'validation' / (options.prefix + '.json')).write_text(json.dumps(result, indent=2) + '\n')
        print('PASS: missing AP refuses CPL0 fallback and creates no web frame')
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
