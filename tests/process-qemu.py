#!/usr/bin/env python3
"""Verify isolated CPU workers and equivalent visible output through QEMU."""
import json
import os
from pathlib import Path
import re
import subprocess

project = Path(__file__).resolve().parents[1]
build = project / 'build/blink'
results = []
reference = None
for processors in (1, 2, 4):
    environment = dict(os.environ, QEMU_TEST_ACCEL='kvm',
                       QEMU_TEST_CPUS=str(processors), INPUT_SCENARIO='settings-preview')
    environment.pop('QEMU_TEST_KEEP_VARIABLES', None)
    output = project / 'validation' / f'process-{processors}cpu.log'
    with output.open('w') as stream:
        subprocess.run(['python3', str(project / 'tests/browser-qemu.py')],
                       env=environment, stdout=stream, stderr=subprocess.STDOUT, check=True)
    text = output.read_text()
    assert 'PASS: EFI loader released payload, stack and TLS' in text
    if processors == 1:
        assert 'no application processors; BSP presentation retained' in text
    else:
        workers = re.findall(r'pid=(\d+) cpu=(\d+) cr3=([0-9a-f]+) private=(\d+) cpl=3', text)
        assert len(workers) == processors-1
        assert len({worker[2] for worker in workers}) == len(workers)
        assert f'PASS: {processors-1} AP workers reached a concurrent user-mode barrier' in text
        for marker in ('supervisor-write fault recovery',
                       'process output RGB/BGR conversion, row partitioning, odd width, stride and guards',
                       'worker CPU reset and firmware descriptor restoration before release',
                       f'first frame presented by {processors-1} isolated AP workers'):
            assert marker in text, marker
        statistics = re.search(r'PROCESS: frames=(\d+) failures=(\d+) workers=(\d+) starts=(\d+) idle_stops=(\d+)', text)
        assert statistics and int(statistics[1]) > 0 and statistics[2] == '0'
        assert int(statistics[4]) == 1 and int(statistics[5]) == 1
        fallback = re.search(r'PROCESS: idle fallback frames=(\d+)', text)
        assert fallback and int(fallback[1]) > 0, 'Idle navigation must avoid synchronous AP restart'
    screenshot = (build / 'input-settings-light.ppm').read_bytes()
    if reference is None:
        reference = screenshot
    else:
        assert screenshot == reference, 'Parallel presentation changed the framebuffer'
    timing = re.search(r'PERF: full_presentations=(\d+) presentation_us=(\d+)', text)
    results.append({'cpus': processors, 'frames': int(timing[1]),
                    'presentation_total_us': int(timing[2]), 'framebuffer_equal': True})
    print(f'PASS: {processors} CPUs, isolation checks, visible settings and teardown', flush=True)
(project / 'validation/process-comparison.json').write_text(json.dumps(results, indent=2)+'\n')
