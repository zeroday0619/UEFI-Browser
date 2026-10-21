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
parser.add_argument('--build-directory', type=Path, default=Path('build/blink'))
parser.add_argument('--require-broker-probe', action='store_true')
parser.add_argument('--require-media-service', action='store_true')
parser.add_argument('--require-media-queue-probe', action='store_true')
parser.add_argument('--require-cpl3', action='store_true')
parser.add_argument('--require-no-renderer-faults', action='store_true')
parser.add_argument('--require-scroll-wire', action='store_true')
parser.add_argument('--require-cpl3-probe', action='store_true')
parser.add_argument('--require-memory-probe', action='store_true')
parser.add_argument('--require-mozc-probe', action='store_true')
parser.add_argument('cases', nargs='*')
options = parser.parse_args()
if (options.require_media_queue_probe or options.require_media_service or options.require_broker_probe or options.require_cpl3 or options.require_cpl3_probe or options.require_scroll_wire or options.require_memory_probe or options.require_mozc_probe) and options.cpus != 4:
    parser.error('The renderer broker probe requires an AP renderer and --cpus 4.')
if not options.prefix.replace('-', '').replace('_', '').isalnum():
    parser.error('Prefix must contain letters, digits, hyphens or underscores.')
project = Path(__file__).resolve().parents[1]
os.chdir(project)
cases = options.cases or [
    'webgpu', 'gpu-coherency', 'webgl', 'webgl2', 'webgpu-canvas', 'gpu-canvas-perf', 'gpu-canvas-colors', 'gpu-lifecycle', 'gpu-multicontext',
    'workers', 'workers-limits', 'audio', 'audio-limits', 'media', 'media-source', 'mse-audio', 'mse-config', 'mse-rate', 'mse-dispatch', 'mse-batch', 'mse-reservoir', 'mse-queue', 'mse-queue-recovery', 'media-source-boundaries', 'media-boundaries', 'indexeddb', 'cache', 'websocket', 'storage', 'storage-lifecycle', 'network', 'network-secure',
    'scroll-state', 'scroll-stuck', 'scroll-state-iframe', 'cookies', 'cookies-secure',
    'scroll-gutter-topology', 'scroll-gutter-replacement', 'scroll-gutter-state',
    'scroll-gutter-fallback', 'scroll-gutter-parent-frame', 'scroll-gutter-nested-frame',
    'scroll-gutter-root', 'scroll-gutter-horizontal', 'scroll-layout', 'scroll-controls',
    'scroll-style', 'scroll-input', 'scroll-tree', 'scroll-affine', 'scroll-affine-controls', 'scroll-filter', 'scroll-encoding', 'paint-snapshot', 'tabs-site', 'frame-policy',
]
if not options.cases and options.cpus != 4:
    cases.remove('mse-reservoir')
    cases.remove('mse-queue')
    cases.remove('mse-queue-recovery')
if len(cases) != len(set(cases)):
    parser.error('Each scenario must appear once.')
if any(case in cases for case in ('mse-reservoir', 'mse-queue', 'mse-queue-recovery')) and options.cpus != 4:
    parser.error('The retained-audio test requires separate BSP and renderer CPUs.')
if 'media-process' in cases and options.cpus != 4:
    parser.error('The media service test requires separate BSP, renderer and decoder CPUs.')
image_directory = (project / options.build_directory).resolve()
source = image_directory / 'esp'
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
        environment['QEMU_TEST_REQUIRE_CPL3'] = '1' if options.require_cpl3 or options.require_cpl3_probe or options.require_scroll_wire or options.require_memory_probe else '0'
        environment['QEMU_TEST_REQUIRE_CPL3_PROBE'] = '1' if options.require_cpl3_probe else '0'
        environment['QEMU_TEST_REQUIRE_BROKER_PROBE'] = '1' if options.require_broker_probe else '0'
        environment.pop('QEMU_TEST_DISK_IMAGE', None)
        environment.pop('TEST_CA_FILE', None)
        environment.pop('QEMU_TEST_AUDIO_CAPTURE', None)
        if case in ('devtools', 'devtools-async', 'authenticator', 'large-resources', 'response-policy', 'transport-idle'):
            environment['TEST_CA_FILE'] = str(options.https_ca.resolve())
        if case in ('renderer-recovery', 'sandbox', 'scroll-wire-boundary'):
            import socket
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                environment['QEMU_TEST_GDB_PORT'] = str(listener.getsockname()[1])
            shutil.copy(image_directory / 'payload.elf', build / 'payload.elf')
        if case in ('audio', 'mse-audio', 'mse-config', 'mse-reservoir', 'mse-queue', 'mse-queue-recovery', 'youtube-observe'): environment['QEMU_TEST_AUDIO_CAPTURE'] = str(build / 'audio-output.wav')
        if case in ('cookies-secure', 'network-secure', 'websocket', 'cache', 'indexeddb', 'media', 'media-source', 'mse-audio', 'mse-config', 'mse-rate', 'mse-dispatch', 'mse-batch', 'mse-reservoir', 'mse-queue', 'mse-queue-recovery', 'media-source-boundaries', 'media-boundaries', 'audio', 'audio-limits', 'workers', 'workers-limits', 'webgpu', 'gpu-coherency', 'webgl', 'webgl2', 'gpu-lifecycle', 'gpu-multicontext', 'webgpu-canvas', 'gpu-canvas-perf', 'gpu-canvas-colors', 'service-capabilities'):
            environment['TEST_CA_FILE'] = str(options.https_ca.resolve())
        if case in ('devtools', 'devtools-async', 'youtube-observe', 'cache', 'indexeddb', 'media', 'media-source', 'mse-audio', 'mse-config', 'mse-rate', 'mse-dispatch', 'mse-batch', 'mse-reservoir', 'mse-queue', 'mse-queue-recovery', 'media-source-boundaries', 'media-boundaries', 'audio', 'audio-limits', 'workers', 'workers-limits', 'webgpu', 'gpu-coherency', 'webgl', 'webgl2', 'gpu-lifecycle', 'gpu-multicontext', 'webgpu-canvas', 'gpu-canvas-perf', 'gpu-canvas-colors', 'gpu-aquarium', 'gpu-live-webgl', 'service-capabilities'):
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
        if case in ('mse-reservoir', 'mse-queue', 'mse-queue-recovery'):
            shutil.copy(build / 'audio-output.wav', Path(str(output_prefix) + '-raw.wav'))
            check = subprocess.run(['python3', 'tests/check-audio-output.py',
                str(build / 'audio-output.wav'), str(output_prefix) + '-spectrum.json', '--reservoir'])
            if check.returncode: result.returncode = check.returncode
        if case == 'mse-config':
            shutil.copy(build / 'audio-output.wav', Path(str(output_prefix) + '-raw.wav'))
            check = subprocess.run(['python3', 'tests/check-mse-config-output.py',
                str(build / 'audio-output.wav'), str(output_prefix) + '-spectrum.json'])
            if check.returncode: result.returncode = check.returncode
        if case in ('audio', 'mse-audio') and result.returncode == 0:
            shutil.copy(build / 'audio-output.wav', Path(str(output_prefix) + '-raw.wav'))
            check = subprocess.run(['python3', 'tests/check-audio-output.py', str(build / 'audio-output.wav'), str(output_prefix) + '-spectrum.json'] + (['--mse'] if case == 'mse-audio' else []))
            if check.returncode: result.returncode = check.returncode
        record = {'scenario': case, 'exit_code': result.returncode, 'seconds': round(time.monotonic() - started, 2)}
        if options.require_media_service:
            import re
            text = (build / 'browser-serial.log').read_text(errors='replace')
            counts = [int(value) for value in re.findall(
                r'MEDIA SERVICE: production decoded_packets=(\d+) frames=\d+ cpl=3', text)]
            record['media_service_decoded_packets'] = max(counts, default=0)
            record['media_service_configured'] = bool(re.search(
                r'MEDIA SERVICE: Opus stream=\d+ channels=[12] cpl=3', text))
            if not record['media_service_configured'] or record['media_service_decoded_packets'] < 100:
                record['exit_code'] = 1
        if options.require_media_queue_probe:
            import re
            record['media_queue_checks'] = [int(value) for value in re.findall(
                r'MEDIA QUEUE PROBE: passed checks=(\d+) packet_limit=512 byte_limit=4194304 cancellation=verified',
                (build / 'browser-serial.log').read_text(errors='replace'))]
            if not record['media_queue_checks'] or any(value < 40 for value in record['media_queue_checks']):
                record['exit_code'] = 1
        if case == 'renderer-shutdown':
            import re
            text = (build / 'browser-serial.log').read_text(errors='replace')
            shutdown = re.search(r'RENDERER: shutdown completed acknowledged=0 forced=1 elapsed_us=(\d+)', text)
            record['forced_shutdown_microseconds'] = int(shutdown[1]) if shutdown else None
            record['shutdown_nmi'] = 'vector=2' in text
            if (not shutdown or not record['shutdown_nmi'] or
                    not 2000000 <= int(shutdown[1]) <= 10000000 or
                    'RENDERER: joined and released private address space' not in text):
                record['exit_code'] = 1
        if environment.get('QEMU_TEST_VENUS') == '1' and case in (
                'webgpu', 'gpu-coherency', 'webgl', 'webgl2', 'webgpu-canvas', 'gpu-canvas-perf', 'gpu-canvas-colors', 'gpu-live-webgl', 'gpu-lifecycle', 'gpu-multicontext'):
            text = (build / 'browser-serial.log').read_text(errors='replace')
            record['hardware_backend_selected'] = 'GPU: Vulkan backend=Mesa Venus' in text or 'GPU-COMPOSITOR: Skia Vulkan initialized' in text
            record['transport_initialized'] = 'PASS: Mesa Venus runtime transport initialized' in text
            record['firmware_controller_restored'] = 'PASS: Venus firmware controller restored' in text
            import re
            transitions = re.findall(r'VENUS TRANSPORT: context (opened|closed) id=(\d+) active=(\d+)', text)
            record['contexts_opened'] = sum(kind == 'opened' for kind, _, _ in transitions)
            record['contexts_closed'] = sum(kind == 'closed' for kind, _, _ in transitions)
            record['peak_contexts'] = max((int(count) for _, _, count in transitions), default=0)
            record['remaining_contexts'] = int(transitions[-1][2]) if transitions else None
            record['context_loss'] = bool(re.search(r'context closed id=\d+ active=\d+ lost=[1-9]', text))
            outstanding = 0
            valid_transitions = bool(transitions)
            for kind, _, count in transitions:
                outstanding += 1 if kind == 'opened' else -1
                valid_transitions &= outstanding == int(count) and 0 <= outstanding <= 16
            record['context_accounting_valid'] = valid_transitions
            if (not valid_transitions or outstanding or record['context_loss']):
                record['exit_code'] = 1

            if not all(record[name] for name in ('hardware_backend_selected', 'transport_initialized',
                                                 'firmware_controller_restored')):
                record['exit_code'] = 1
        if options.require_no_renderer_faults:
            text = (build / 'browser-serial.log').read_text(errors='replace')
            fault_text = text
            if options.require_cpl3_probe:
                boundary_end = 'PASS: CPL3 JIT executable writes, writable execution and backing-alias writes blocked'
                if boundary_end in text: fault_text = text.split(boundary_end, 1)[1]
            record['unexpected_renderer_fault'] = any(marker in fault_text for marker in (
                'RENDERER: stopped status=', 'RENDERER: runtime abort'))
            if record['unexpected_renderer_fault']:
                record['exit_code'] = 1
        if case == 'devtools':
            text = (build / 'browser-serial.log').read_text(errors='replace')
            fault_text = text
            if options.require_cpl3_probe:
                boundary_end = 'PASS: CPL3 JIT executable writes, writable execution and backing-alias writes blocked'
                if boundary_end in text: fault_text = text.split(boundary_end, 1)[1]
            if 'RENDERER: stopped status=' in fault_text or 'DEVTOOLS: frontend loaded' not in text:
                record['exit_code'] = 1
        if options.require_broker_probe:
            import re
            record['broker_checks'] = [int(value) for value in re.findall(
                r'BROKER PROBE: passed checks=(\d+);',
                (build / 'browser-serial.log').read_text(errors='replace'))]
            if options.require_media_service:
                record['media_broker_checks'] = [int(value) for value in re.findall(
                    r'MEDIA BROKER PROBE: passed checks=(\d+) streams=8 stale_handles=blocked',
                    (build / 'browser-serial.log').read_text(errors='replace'))]
                if not record['media_broker_checks'] or any(count != 32 for count in record['media_broker_checks']):
                    record['exit_code'] = 1
            if not record['broker_checks'] or any(count != 65 for count in record['broker_checks']):
                record['exit_code'] = 1
        if options.require_cpl3 or options.require_cpl3_probe or options.require_scroll_wire or options.require_memory_probe:
            import re
            text=(build / 'browser-serial.log').read_text(errors='replace')
            record['renderer_cpl']=[int(value) for value in re.findall(r'Blink/V8 event loop[^\n]*cpl=(\d+)',text)]
            if not record['renderer_cpl'] or any(value!=3 for value in record['renderer_cpl']): record['exit_code']=1
            if options.require_cpl3_probe:
                record['boundary_cases']=[int(value) for value in re.findall(r'CPL3: case=(\d+)',text)]
                if record['boundary_cases']!=list(range(1,15)): record['exit_code']=1
        if options.require_scroll_wire:
            import re
            text=(build / 'browser-serial.log').read_text(errors='replace')
            record['wire_publications']=len(re.findall(r'COMPOSITOR: scene .* wire=\d+ cpl3=1',text))
            record['wire_self_test']='COMPOSITOR: wire validation self-test passed' in text
            if not record['wire_publications'] or not record['wire_self_test']: record['exit_code']=1
        if options.require_memory_probe:
            import re
            text=(build / 'browser-serial.log').read_text(errors='replace')
            record['memory_probe_cpl']=[int(value) for value in re.findall(
                r'MEMORY: dynamic allocation passed cpl=(\d+) bytes=167772160 limit=\d+ reclaimed=1',text)]
            if 0 not in record['memory_probe_cpl'] or 3 not in record['memory_probe_cpl']: record['exit_code']=1
            record['retained_page_bytes'] = [int(value) for value in re.findall(
                r'MEMORY: renderer page batches reclaimed cycles=4 bytes=67108864 retained=(\d+)', text)]
            if not record['retained_page_bytes'] or any(value >= 67108864 for value in record['retained_page_bytes']):
                record['exit_code'] = 1
            record['sparse_tables_passed'] = 'MEMORY: sparse page-table growth passed mappings=2304' in text
            if not record['sparse_tables_passed']:
                record['exit_code'] = 1
            record['broker_buffer_lookup_us'] = [int(value) for value in re.findall(
                r'MEMORY: broker buffer lookup passed allocations=4096 requests=2048 elapsed_us=(\d+)', text)]
            if not record['broker_buffer_lookup_us']:
                record['exit_code'] = 1
            record['broker_buffer_bounds_passed'] = 'MEMORY: broker buffer bounds, padding, permissions and revocation passed' in text
            if not record['broker_buffer_bounds_passed']:
                record['exit_code'] = 1
            record['reservation_index_us'] = [int(value) for value in re.findall(
                r'MEMORY: reservation index passed mappings=5000 hint_reused=1 elapsed_us=(\d+)', text)]
            if not record['reservation_index_us']:
                record['exit_code'] = 1
            record['reservation_split_passed'] = 'MEMORY: reservation split and fixed replacement preserved neighbors' in text
            if not record['reservation_split_passed']:
                record['exit_code'] = 1
        if options.require_mozc_probe:
            import re
            text = (build / 'browser-serial.log').read_text(errors='replace')
            record['mozc_probe_cpl'] = [int(value) for value in re.findall(
                r'PASS: Mozc firmware romaji, dictionary conversion, candidate selection and commit 日本語 cpl=(\d+)', text)]
            if 0 not in record['mozc_probe_cpl'] or 3 not in record['mozc_probe_cpl']:
                record['exit_code'] = 1
        with lock:
            summary['cases'].append(record)
            (project / 'validation' / (options.prefix + '-suite.json')).write_text(json.dumps(summary, indent=2) + '\n')
            print(record, flush=True)


with concurrent.futures.ThreadPoolExecutor(max_workers=options.workers) as executor:
    list(executor.map(worker, range(options.workers)))
if any(case['exit_code'] for case in summary['cases']):
    raise SystemExit(1)
