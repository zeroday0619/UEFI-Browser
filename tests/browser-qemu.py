#!/usr/bin/env python3
"""Capture a real Blink EFI navigation; inspect the screenshot before claiming fidelity."""
import tempfile
import json
import os
import re
from pathlib import Path
import shutil
import socket
import subprocess
import time

project = Path(__file__).resolve().parents[1]
build = Path(os.environ.get('BUILD_DIRECTORY', project / 'build/blink')).resolve()
image = build / 'esp/EFI/BOOT/BOOTX64.EFI'
if not image.is_file():
    raise SystemExit('The Blink EFI image has not been linked: ' + str(image))
socket_directory = tempfile.TemporaryDirectory(prefix='blink-qmp-', dir='/tmp')
monitor = Path(socket_directory.name) / 'qmp.sock'
monitor.unlink(missing_ok=True)
variables = build / 'browser-OVMF_VARS.fd'
if not os.environ.get('QEMU_TEST_KEEP_VARIABLES') or not variables.exists():
    shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
log = build / 'browser-serial.log'
log.write_text('')
(build / 'qmp-events.jsonl').write_text('')
disk = os.environ.get('QEMU_TEST_DISK_IMAGE')
drive = f'format=raw,file={Path(disk).resolve()}' if disk else f'format=raw,file=fat:rw:{build / "esp"}'
command = [os.environ.get('QEMU_BINARY', 'qemu-system-x86_64'), '-machine', 'q35,accel=' + os.environ.get('QEMU_TEST_ACCEL', 'tcg'), '-m', '4096', '-smp', os.environ.get('QEMU_TEST_CPUS', '1'),
    '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
    '-drive', f'if=pflash,format=raw,file={variables}',
    '-drive', drive,
    '-netdev', 'user,id=network', '-device', 'virtio-net-pci,netdev=network,romfile=',
    '-device', 'virtio-mouse-pci', '-device', 'virtio-rng-pci', '-display', 'none', '-serial', f'file:{log}',
    '-qmp', f'unix:{monitor},server=on,wait=off', '-no-reboot',
    '-fw_cfg', f'name=etc/edk2/https/cacerts,file={os.environ.get("TEST_CA_FILE", project / "build/ca.esl")}']
if os.environ.get('QEMU_BINARY'):
    command[1:1] = ['-L', '/usr/share/qemu', '-L', '/usr/share/seabios']
if os.environ.get('QEMU_TEST_VENUS') == '1':
    if os.environ.get('QEMU_TEST_ACCEL') != 'kvm':
        raise SystemExit('Venus validation requires KVM.')
    command[command.index('-machine')+1] = 'q35,memory-backend=memory'
    command[command.index('-display')+1] = 'egl-headless,rendernode=/dev/dri/renderD128'
    command += ['-accel', 'kvm,honor-guest-pat=on', '-vga', 'std',
                '-object', 'memory-backend-memfd,id=memory,size=4G,share=on',
                '-device', 'virtio-gpu-gl-pci,id=venus,blob=on,venus=on,hostmem=512M']
audio_capture = os.environ.get('QEMU_TEST_AUDIO_CAPTURE')
if audio_capture:
    command += ['-audiodev', f'wav,id=audio,path={audio_capture},out.frequency=48000,out.channels=2']
else:
    command += ['-audiodev', 'none,id=audio']
command += ['-device', 'AC97,audiodev=audio']
if os.environ.get('QEMU_TEST_CPU_RESET_LOG') == '1':
    command += ['-d', 'cpu_reset,guest_errors', '-D', str(build / 'qemu-reset.log')]
if os.environ.get('QEMU_TEST_GDB_PORT'):
    port = int(os.environ['QEMU_TEST_GDB_PORT'])
    if not 1 <= port <= 65535:
        raise SystemExit('QEMU_TEST_GDB_PORT must be between 1 and 65535.')
    command += ['-gdb', f'tcp:127.0.0.1:{port}']
display_name = 'uefi-gpu-' + str(os.getpid())
gpu_capture = os.environ.get('QEMU_TEST_GPU_COMPOSITION') == '1' or os.environ.get('QEMU_TEST_SDL_CAPTURE') == '1'
if gpu_capture:
    command[command.index('-display')+1] = 'sdl,gl=on'
    command[command.index('-vga')+1] = 'none'
    command[command.index('virtio-gpu-gl-pci,id=venus,blob=on,venus=on,hostmem=512M')] = 'virtio-vga-gl,id=venus,blob=on,venus=on,hostmem=512M'
    if os.environ.get('QEMU_TEST_GPU_COMPOSITION') == '1':
        index = command.index('virtio-vga-gl,id=venus,blob=on,venus=on,hostmem=512M')
        command[index] += ',uefi-scanout-modifier=on'
    command += ['-global', 'virtio-vga-gl.romfile=/usr/share/seabios/vgabios-virtio.bin']
    command += ['-name', display_name]
    os.environ['SDL_VIDEODRIVER'] = 'x11'
    os.environ['SDL_VIDEO_X11_FORCE_EGL'] = '1'
    from qemu_test_desktop import prepare
    desktop = prepare()
    os.environ['SDL_VIDEO_X11_WMCLASS'] = desktop['window_class']
    os.environ['SDL_MOUSE_FOCUS_CLICKTHROUGH'] = '0'
    (build / 'test-desktop.json').write_text(json.dumps(desktop, indent=2) + '\n')
process = subprocess.Popen(command)
stream = None
try:
    required = ['BLINK: non-composited main frame created',
                'BLINK: main document load finished',
                'BLINK: main frame stopped loading',
                'BLINK: first raster frame copied to GOP']
    if os.environ.get('QEMU_TEST_GPU_COMPOSITION') == '1':
        required[-1] = 'GPU-COMPOSITOR: first frame presented; cpu_readback_bytes=0'
    if os.environ.get('QEMU_TEST_REQUIRE_BROKER_PROBE') == '1':
        required.append('BROKER PROBE: passed checks=65;')
    if os.environ.get('QEMU_TEST_REQUIRE_CPL3') == '1':
        required.append('cpl=3')
    if os.environ.get('QEMU_TEST_REQUIRE_WATCHDOG_OFF') == '1':
        required.append('Firmware boot watchdog disabled')
    if os.environ.get('QEMU_TEST_REQUIRE_CPL3_PROBE') == '1':
        required.append('PASS: CPL3 payload entry, fixed syscall ABI')
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        text = log.read_text(errors='replace')
        if (os.environ.get('QEMU_TEST_GPU_COMPOSITION') == '1' and 'GPU-COMPOSITOR: initialization failed' in text) or 'FAIL:' in text or 'Exception Type' in text or process.poll() is not None:
            raise RuntimeError(text)
        if os.environ.get('QEMU_TEST_REQUIRE_CPL3_PROBE') != '1' and any(
                marker in text for marker in ('RENDERER: stopped status=', 'Fatal process out of memory')):
            raise RuntimeError('Renderer failed before browser startup:\n' + text)
        if all(marker in text for marker in required):
            for media_type in ('text/css', '(?:text|application)/javascript', 'image/'):
                if os.environ.get('INPUT_SCENARIO') in ('site', 'site-no-wheel') and not re.search(r'BLINK: resource 200 type=' + media_type, text):
                    raise RuntimeError('Required subresource was not received: ' + media_type + '\n' + text)
            break
        time.sleep(0.2)
    else:
        raise TimeoutError(text)
    # A completed load can precede its final animation and paint task.
    time.sleep(2)
    with socket.socket(socket.AF_UNIX) as control:
        control.connect(str(monitor))
        stream = control.makefile('rwb', buffering=0)
        json.loads(stream.readline())
        def execute(name, arguments=None):
            if name == 'screendump' and gpu_capture:
                # QEMU 11.1 exposes no QMP software surface for DMA-BUF scanout.
                # Capture the SDL GPU window itself rather than the unrelated GOP console.
                failure = ''
                for attempt in range(3):
                    windows = subprocess.check_output(['xwininfo', '-root', '-tree'], text=True)
                    matches = re.findall(r'(0x[0-9a-f]+) "QEMU \(' + re.escape(display_name) + r'-0\)', windows)
                    if len(matches) > 1:
                        raise RuntimeError('The direct GPU display window is ambiguous: ' + windows)
                    if matches:
                        target = Path(arguments['filename'])
                        target.unlink(missing_ok=True)
                        captured = subprocess.run(['import', '-window', matches[0], '-depth', '8',
                                                   'ppm:' + str(target)], capture_output=True, text=True)
                        if captured.returncode == 0 and target.is_file():
                            return {}
                        failure = captured.stderr
                    else:
                        failure = 'The direct GPU display window was not found: ' + windows
                    if process.poll() is not None:
                        raise RuntimeError('QEMU exited during GPU capture: ' + failure)
                    # Re-enumeration avoids reusing a window identifier after an SDL transition.
                    if attempt < 2:
                        time.sleep(.2)
                raise RuntimeError('The direct GPU display capture failed after three attempts: ' + failure)
            stream.write((json.dumps({'execute': name, 'arguments': arguments or {}}) + '\n').encode())
            while True:
                response = json.loads(stream.readline())
                if 'event' in response:
                    with (build / 'qmp-events.jsonl').open('a') as events:
                        events.write(json.dumps(response) + '\n')
                if 'error' in response:
                    raise RuntimeError(response)
                if 'return' in response:
                    return response['return']
        execute('qmp_capabilities')
        screenshot = build / 'browser-screen.ppm'
        execute('screendump', {'filename': str(screenshot)})
        if gpu_capture:
            header = screenshot.read_bytes().split(b'\n', 3)
            pixels = header[3]
            if len(set(pixels[index:index+3] for index in range(0, len(pixels), 3))) < 16:
                raise AssertionError('The GPU display contains no visible browser interface.')
        if os.environ.get('INPUT_SCENARIO'):
            import runpy
            runpy.run_path(str(project / 'tests/input-scenario.py'))['run'](execute, build, log, process.poll)
        execute('send-key', {'keys': [{'type': 'qcode', 'data': 'f12'}]})
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            text = log.read_text(errors='replace')
            if 'FAIL:' in text or 'Exception Type' in text:
                raise RuntimeError(text)
            if 'Tests finished. Press any key' in text:
                execute('send-key', {'keys': [{'type': 'qcode', 'data': 'ret'}]})
                break
            time.sleep(0.2)
        else:
            raise TimeoutError('Blink shutdown did not finish: ' + text)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        text = log.read_text(errors='replace')
        if 'FAIL:' in text or 'Exception Type' in text:
            raise RuntimeError(text)
        if 'PASS: EFI loader released payload, stack and TLS' in text:
            break
        time.sleep(0.2)
    else:
        raise TimeoutError('EFI return did not finish: ' + text)
    print(text)
    print('Captured Blink framebuffer: ' + str(screenshot))
    print('Visual inspection is required to confirm the site content and rendering fidelity.')
except Exception:
    print('QEMU process status at failure: ' + str(process.poll()), flush=True)
    if os.environ.get('QEMU_TEST_FAILURE_BACKTRACE') == '1' and process.poll() is None:
        port = os.environ.get('QEMU_TEST_GDB_PORT')
        symbols = build / 'payload.elf'
        if port and symbols.is_file():
            # Inspect the failing guest before teardown without changing its registers.
            try:
                with (build / 'failure-backtrace.log').open('w') as output:
                    subprocess.run(['gdb', '-q', '-nx', '-batch', str(symbols),
                        '-ex', 'set pagination off',
                        '-ex', 'target remote 127.0.0.1:' + port,
                        '-ex', 'info threads', '-ex', 'thread apply all bt 24',
                        '-ex', 'thread apply all info registers',
                        '-ex', 'thread apply all x/64gx $rsp',
                        '-ex', 'thread apply all x/12i $rip',
                        '-ex', 'thread 1',
                        '-ex', 'x/10gx *(unsigned long long*)&channel',
                        '-ex', 'detach'], stdout=output, stderr=subprocess.STDOUT,
                        timeout=30, check=True)
            except (OSError, subprocess.SubprocessError) as error:
                print('Failure backtrace could not be captured:', error, flush=True)
    raise
finally:
    # QEMU must release the original QMP client before accepting the shutdown client.
    if stream is not None:
        stream.close()
    if audio_capture and process.poll() is None:
        # Request a normal emulator shutdown before analyzing the captured PCM.
        try:
            with socket.socket(socket.AF_UNIX) as cleanup:
                cleanup.settimeout(3)
                cleanup.connect(str(monitor))
                connection = cleanup.makefile('rwb', buffering=0)
                connection.readline()
                connection.write(b'{"execute":"qmp_capabilities"}\n')
                for attempt in range(32):
                    reply = connection.readline()
                    if not reply: raise EOFError('QMP closed before capabilities response')
                    if b'"return"' in reply: break
                else: raise OSError('QMP capabilities response missing')
                connection.write(b'{"execute":"quit"}\n')
                process.wait(timeout=5)
        except (OSError, EOFError, subprocess.TimeoutExpired):
            pass
    if process.poll() is None: process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()

socket_directory.cleanup()
