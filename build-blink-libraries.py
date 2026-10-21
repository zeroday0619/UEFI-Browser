#!/usr/bin/env python3
"""Build the Blink dependency graph; this does not produce a bootable browser."""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess

project = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mozc', action='store_true', help='Build the experimental Mozc firmware engine.')
parser.add_argument('--host', action='store_true', help='Build Linux host libraries instead of musl firmware objects.')
parser.add_argument('--resolve-inputs', action='store_true', help='Resolve the pinned GN inputs before building.')
parser.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
options = parser.parse_args()
if options.jobs < 1:
    parser.error('--jobs must be positive')
preparation = ['python3', str(project / 'prepare-blink.py')]
if options.mozc: preparation.append('--mozc')
if not options.host:
    preparation.append('--firmware')
if options.resolve_inputs:
    preparation.append('--resolve-inputs')
subprocess.run(preparation, check=True)
subprocess.run(['python3', str(project / 'tools/prepare-dawn-go.py'),
                str(project.parents[1] / 'work/chromium-build/chromium/third_party/dawn')],
               check=True)
configuration = 'uefi-blink' if options.host else 'uefi-blink-native'
build = project.parents[1] / 'work/chromium-build/chromium/out' / configuration
log_path = project / 'validation' / ('blink-library-build.log' if options.host else 'blink-native-library-build.log')
target = 'third_party/blink/uefi:' + ('navigation' if options.host else 'browser')
command = ['ninja', '-C', str(build), '-j' + str(options.jobs), target]
status_path = project / 'validation' / ('blink-build-status.json' if options.host else 'blink-native-build-status.json')
status = {
    'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'command': command,
    'state': 'building',
    'blink_firmware_linked': False,
    'website_rendered': False,
}
def save_status():
    temporary = status_path.with_suffix('.json.next')
    temporary.write_text(json.dumps(status, indent=2) + '\n')
    temporary.replace(status_path)
save_status()
print('Building ' + configuration + '; log: ' + str(log_path), flush=True)
with log_path.open('w') as log:
    while True:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        log.flush()
        if not result.returncode or not options.resolve_inputs:
            break
        resolution = subprocess.run(['python3',
            str(project / 'tools/resolve-build-inputs.py'),
            str(build.parent.parent), str(log_path)])
        if resolution.returncode:
            break
        log.write('\nResuming after pinned source input restoration.\n')
        log.flush()
status.update(state='failed' if result.returncode else 'libraries_built',
              exit_code=result.returncode,
              finished_at=datetime.datetime.now(datetime.timezone.utc).isoformat())
save_status()
if result.returncode:
    print('Blink library build failed. No EFI image was produced. See ' + str(log_path), flush=True)
    raise SystemExit(result.returncode)
print('Blink dependency graph built. Final EFI linkage and execution remain unverified.', flush=True)
