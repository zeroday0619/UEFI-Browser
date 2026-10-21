"""Exercise live page controls through QEMU input devices, without DOM injection."""
import os
import re
import time

def devtools_tab_row(image, left, right):
    width, height, pixels = image
    rows = []
    for y in range(85, min(height, 160)):
        # A banner focus border spans beyond either tab; the selected underline does not.
        outside = (max(0, left - 8), min(width - 1, right + 8))
        if any((lambda red, green, blue: blue > 180 and red < 120 and blue > green + 40)(
                *pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]) for x in outside):
            continue
        consecutive = 0
        for x in range(left, min(width, right)):
            red, green, blue = pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
            consecutive = consecutive + 1 if blue > 180 and red < 120 and blue > green + 40 else 0
            if consecutive >= 35:
                rows.append(y)
                break
    return max(rows) - 10 if rows else None


def devtools_console_prompt(image):
    width, height, pixels = image
    selected_row = devtools_tab_row(image, 114, 170)
    if selected_row is None:
        return None
    rows = []
    for y in range(selected_row + 30, height - 5):
        for x in range(15, min(width, 24)):
            red, green, blue = pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
            if blue > 200 and red < 180 and blue > green + 40:
                rows.append(y)
                break
    return max(rows) - 2 if len(rows) >= 3 else None


def run(execute, build, log, process_status=None):
    def after_boundary_tests(content):
        if os.environ.get('QEMU_TEST_REQUIRE_CPL3_PROBE') == '1':
            marker = 'PASS: CPL3 JIT executable writes, writable execution and backing-alias writes blocked'
            if marker in content:
                return content.split(marker, 1)[1]
        return content
    def healthy():
        if process_status is not None:
            status = process_status()
            if status is not None:
                raise RuntimeError('QEMU exited during input scenario: ' + str(status))
        text = log.read_text(errors='replace')
        if any(marker in text for marker in ('AUTHENTICATOR FAILURE', 'LARGE RESOURCE FAILURE', 'RESPONSE POLICY FAILURE', 'TRANSPORT IDLE FAILURE')):
            raise RuntimeError(text)
        if os.environ.get('INPUT_SCENARIO') in ('devtools', 'devtools-async', 'youtube-observe', 'media-process') and 'RENDERER: stopped status=' in after_boundary_tests(text):
            raise RuntimeError('Renderer stopped during input scenario:\n' + text)
        if 'Fatal process out of memory' in text:
            raise RuntimeError('Renderer exhausted memory:\n' + text)
        if 'DEVTOOLS FIXTURE FAILURE' in text or 'DEVTOOLS ASYNC FAILURE' in text or 'MSE RATE FAILURE' in text or 'MSE DISPATCH FAILURE' in text or 'MSE RESERVOIR FAILURE' in text or 'MSE CONFIG FAILURE' in text:
            raise RuntimeError(text)
        if any(marker in text for marker in ('FATAL:', 'FAIL:', 'Exception Type', 'IDB FAIL', 'MEDIA FAILURE', 'MSE FAILURE', 'MSE LIMIT FAILURE', 'MSE AUDIO FAILURE', 'AUDIO FAILURE', 'WORKERS FAILURE', 'WEBGPU FAILURE', 'GPU MULTICONTEXT FAILURE', 'WEBGL FAILURE', 'WEBGL2 FAILURE', 'RENDERER: runtime abort')):
            raise RuntimeError(text)
        return text
    def snapshot(name):
        time.sleep(1)
        healthy()
        path = build / ('input-' + name + '.ppm')
        execute('screendump', {'filename': str(path)})
        data = path.read_bytes().split(b'\n', 3)
        width, height = map(int, data[1].split())
        return width, height, data[3]
    def pixel(image, x, y):
        width, _, data = image
        offset = (y * width + x) * 3
        return sum(data[offset:offset + 3])
    position = [640, 400]
    def move(x, y, page=True):
        if page: y += 80
        while position != [x, y]:
            dx = max(-100, min(100, x-position[0]))
            dy = max(-100, min(100, y-position[1]))
            execute('input-send-event', {'events': [
                {'type':'rel','data':{'axis':'x','value':dx}},
                {'type':'rel','data':{'axis':'y','value':dy}}]})
            position[0] += dx
            position[1] += dy
            time.sleep(.08)
    def click(x, y, page=True):
        move(x, y, page)
        if os.environ.get('INPUT_POINTER_TRACE') == '1':
            print(f'QMP: click x={x} y={y + (80 if page else 0)}', flush=True)
        for down in (True, False):
            execute('input-send-event', {'events':[{'type':'btn','data':{'button':'left','down':down}}]})
            time.sleep(.15)
    def key(*keys):
        execute('send-key', {'keys':[{'type':'qcode','data':name} for name in keys]})
        time.sleep(.3)
    def wait_marker(marker, timeout=30, offset=0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            content = healthy()
            fault_content = after_boundary_tests(content)
            if os.environ.get('INPUT_SCENARIO') in ('webgpu', 'gpu-coherency', 'webgl', 'webgl2', 'webgpu-canvas', 'gpu-multicontext', 'devtools') and 'RENDERER: stopped status=' in fault_content:
                raise AssertionError('Scenario execution stopped the renderer:\n' + content)
            if marker in content[offset:]: return
            time.sleep(.2)
        raise AssertionError('Missing page result: ' + marker + '\n' + healthy())
    def wheel(direction, ticks=1):
        for _ in range(ticks):
            for down in (True, False):
                execute('input-send-event', {'events':[{'type':'btn','data':{'button':'wheel-' + direction,'down':down}}]})
                time.sleep(.15)
    def type_address(text):
        for character in text:
            if character == ':': key('shift', 'semicolon')
            elif character == '/': key('slash')
            elif character == '.': key('dot')
            elif character == '-': key('minus')
            elif character == '_': key('shift','minus')
            elif character == '?': key('shift', 'slash')
            elif character == '=': key('equal')
            elif character.isupper(): key('shift',character.lower())
            else: key(character)
    if os.environ.get('INPUT_SCENARIO') == 'paint-snapshot':
        key('ctrl', 'l'); type_address('http://10.0.2.2:8765/paint-snapshot.html'); key('ret')
        wait_marker('PAINT SNAPSHOT READY')
        initial = snapshot('paint-snapshot-initial')
        click(90, 30); wait_marker('PAINT SNAPSHOT CHANGED')
        changed = snapshot('paint-snapshot-changed')
        assert initial[2] != changed[2], 'Style mutation did not repaint'
        move(1200, 500); wheel('down', 8)
        wait_marker('PAINT SNAPSHOT SCROLL 640')
        scrolled = snapshot('paint-snapshot-scrolled')
        assert changed[2] != scrolled[2], 'Root scrolling did not repaint'
        print('PASS: effect and iframe snapshots captured; cross-image pixel comparison is required')
        return
    if os.environ.get('INPUT_SCENARIO') == 'scroll-encoding':
        import json
        variant = '?capture=1' if os.environ.get('SCROLL_ENCODING_CAPTURE') == '1' else ''
        key('ctrl', 'l'); type_address('http://10.0.2.2:8765/scroll-encoding.html' + variant); key('ret')
        wait_marker('SCROLL ENCODING READY', timeout=60)
        snapshot('scroll-encoding-ready')
        click(100, 35); wait_marker('SCROLL ENCODING BEGIN')
        started = time.monotonic()
        click(300, 35); wait_marker('SCROLL ENCODING CLICK')
        click_seconds = time.monotonic() - started
        click(510, 35); key('a'); wait_marker('SCROLL ENCODING INPUT a')
        # Hiding a busy tab cancels its uncommitted records; resuming must capture a new scene.
        key('ctrl', 't'); key('ctrl', '1')
        wait_marker('SCROLL ENCODING PASS', timeout=45)
        assert 'SCROLL ENCODING FAILURE' not in healthy()
        result = json.loads(re.findall(r'SCROLL ENCODING RESULT (\{[^\n]+\})', healthy())[-1])
        result['mouse_delivery_and_poll_seconds'] = click_seconds
        result['keyboard_delivery'] = True
        result['hide_and_resume'] = True
        slices = [int(value) for value in re.findall(r'PERF: scroll_encode_us=\d+ slices=(\d+)', healthy())]
        result['encoding_slices'] = slices
        if os.environ.get('SCROLL_ENCODING_REQUIRE_SLICES') == '1':
            assert any(value > 1 for value in slices), 'The fixture did not exercise encoding continuations'
        snapshot('scroll-encoding-complete')
        (build / 'scroll-encoding-results.json').write_text(json.dumps(result, indent=2))
        key('ctrl', 'w')
        print('PASS: scroll encoding under paint load, mouse, keyboard and tab cancellation', json.dumps(result))
        return
    if os.environ.get('INPUT_SCENARIO') == 'transport-idle':
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/transport-idle.html'); key('ret')
        wait_marker('TRANSPORT IDLE PASS checks=4', timeout=60)
        snapshot('transport-idle-complete')
        print('PASS: configured HTTPS header/body idle timeout and following request')
        return
    if os.environ.get('INPUT_SCENARIO') == 'media-process':
        key('ctrl', 'l'); type_address('http://10.0.2.2:8765/media-process.html'); key('ret')
        wait_marker('MEDIA PROCESS FIXTURE READY', timeout=60)
        click(120, 100)
        wait_marker('MEDIA PROCESS BUSY BEGIN')
        for cycle in (1, 2):
            key('f7')
            wait_marker(f'MEDIA SERVICE: probe passed cycle={cycle} decoded_packets=100 cpl=3 private_data=1 private_tls=1 supervisor_fault=14', timeout=10)
            assert healthy().count('MEDIA SERVICE: denied_capabilities=5 fault_error=5 ') == cycle
            assert 'MEDIA PROCESS BUSY END' not in healthy(), 'Media completion did not precede the blocked renderer task'
        wait_marker('MEDIA PROCESS BUSY END', timeout=20)
        shutdown = re.search(r'MEDIA SERVICE: immediate shutdown completed elapsed_ms=(\d+)', healthy())
        assert shutdown and int(shutdown[1]) < 2000
        click(450, 100)
        wait_marker('MEDIA PROCESS PAGE RESPONSIVE')
        snapshot('media-process-recovered')
        print('PASS: two independent CPL3 Opus service lifecycles during blocked JavaScript, packet bound, private TLS/data, supervisor faults and surviving page input')
        return
    if os.environ.get('INPUT_SCENARIO') in ('mse-queue', 'mse-queue-recovery'):
        import json
        offset = len(healthy())
        if os.environ.get('INPUT_SCENARIO') == 'mse-queue-recovery':
            key('ctrl', 'l'); type_address('https://10.0.2.2:8767/media-queue.html'); key('ret')
            wait_marker('MSE RESERVOIR READY', timeout=60, offset=offset)
            click(450, 205)
            wait_marker('MSE RESERVOIR BLOCK BEGIN', timeout=20, offset=offset)
            key('shift', 'esc'); wait_marker('MONITOR: opened', offset=offset)
            key('delete')
            wait_marker('RENDERER: recovery launched', timeout=20, offset=offset)
            assert re.search(r'MEDIA QUEUE: released streams=1 queued_bytes=[1-9][0-9]*', healthy()[offset:])
            key('esc')
            offset = len(healthy())
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/media-queue.html'); key('ret')
        wait_marker('MSE RESERVOIR READY', timeout=60, offset=offset)
        click(450, 205)
        import runpy
        from pathlib import Path
        output = build / 'audio-output-observation-results.json'
        runpy.run_path(str(Path(__file__).with_name('audio-output-observer.py')))['run'](
            lambda: healthy()[offset:], build / 'audio-output.wav', output)
        observation = json.loads(output.read_text())
        assert observation['interior_seconds'] > 5.5
        assert .95 <= observation['signal_to_wall_clock_ratio'] <= 1.05
        content = healthy()[offset:].split('MSE RESERVOIR BLOCK BEGIN', 1)[1].split('MSE RESERVOIR BLOCK END', 1)[0]
        counts = [int(value) for value in re.findall(r'MEDIA SERVICE: production decoded_packets=(\d+)', content)]
        assert len(counts) >= 2 and counts[-1] - counts[0] >= 100, counts
        observation['decode_count_markers_during_block'] = counts
        output.write_text(json.dumps(observation, indent=2))
        wait_marker('MSE QUEUE PASS', timeout=30, offset=offset)
        snapshot('mse-queue-complete')
        print('PASS: autonomous Opus decoding and timed PCM during six-second JavaScript, pause, resume and seek')
        return
    if os.environ.get('INPUT_SCENARIO') == 'mse-reservoir':
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/media-reservoir.html'); key('ret')
        wait_marker('MSE RESERVOIR READY', timeout=60)
        click(450, 205)
        import runpy
        from pathlib import Path
        runpy.run_path(str(Path(__file__).with_name('audio-output-observer.py')))['run'](
            healthy, build / 'audio-output.wav', build / 'audio-output-observation-results.json')
        wait_marker('MSE RESERVOIR PASS', timeout=30)
        snapshot('mse-reservoir-complete')
        print('PASS: retained audio during 1.5-second JavaScript, pause and resume')
        return
    if os.environ.get('INPUT_SCENARIO') in ('mse-dispatch', 'mse-batch'):
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/media-dispatch.html')
        if os.environ.get('INPUT_SCENARIO') == 'mse-batch':
            key('shift', 'slash'); type_address('queue'); key('equal'); type_address('4')
        key('ret')
        wait_marker('MSE DISPATCH READY', timeout=60)
        click(450, 205)
        wait_marker('MSE DISPATCH PASS', timeout=30)
        if os.environ.get('INPUT_SCENARIO') == 'mse-batch':
            assert '"queuedTaskSources":4' in healthy()
        snapshot('mse-dispatch-complete')
        print('PASS: MSE audio maintains real-time progression under repeated JavaScript load')
        return
    if os.environ.get('INPUT_SCENARIO') == 'mse-rate':
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/media-rate.html'); key('ret')
        wait_marker('MSE RATE READY', timeout=60)
        click(450, 205)
        wait_marker('MSE RATE PASS', timeout=30)
        snapshot('mse-rate-complete')
        print('PASS: MSE zero-rate clock retention and playback resume')
        return
    if os.environ.get('INPUT_SCENARIO') == 'response-policy':
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/response-policy.html'); key('ret')
        wait_marker('RESPONSE POLICY PASS checks=35', timeout=180)
        assert 'NETWORK POLICY: CORP blocked' in healthy()
        assert 'NETWORK POLICY: ORB blocked' in healthy()
        snapshot('response-policy-complete')
        print('PASS: cross-origin resources, ORB, CORP, redirects, COEP and worker policies')
        return
    if os.environ.get('INPUT_SCENARIO') == 'large-resources':
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/large-resources.html'); key('ret')
        wait_marker('LARGE RESOURCE PASS checks=4', timeout=120)
        snapshot('large-resources-complete')
        print('PASS: large script execution, chunked bytes, bounded rejection and recovery')
        return
    if os.environ.get('INPUT_SCENARIO') == 'authenticator':
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/authenticator.html'); key('ret')
        wait_marker('AUTHENTICATOR PASS checks=6')
        snapshot('authenticator-complete')
        print('PASS: unavailable authenticator queries, concurrency and frame teardown')
        return
    if os.environ.get('INPUT_SCENARIO') == 'youtube-observe':
        # This diagnostic records the live player without treating a loaded page as playback.
        import json
        if os.environ.get('YOUTUBE_PLAYBACK_PROBE') == '1' and os.environ.get('YOUTUBE_INSPECT') != '1':
            raise ValueError('YOUTUBE_PLAYBACK_PROBE requires YOUTUBE_INSPECT=1.')
        key('ctrl', 'l')
        type_address('https://www.youtube.com/watch')
        key('shift', 'slash'); key('v'); key('equal')
        type_address('aqz-KE-bpKQ'); key('ret')
        observations = []
        started = time.monotonic()
        observation_times = (10, 30, 60, 90, 120, 180) if os.environ.get('YOUTUBE_LONG') == '1' else (10, 30, 60, 90)
        if 'YOUTUBE_OBSERVE_SECONDS' in os.environ:
            duration = int(os.environ['YOUTUBE_OBSERVE_SECONDS'])
            if not 180 <= duration <= 1800 or duration % 30:
                raise ValueError('YOUTUBE_OBSERVE_SECONDS must be a multiple of 30 between 180 and 1800.')
            observation_times = (10, 30, 60, 90, 120, 180) + tuple(range(210, duration + 1, 30))
        for elapsed in observation_times:
            while time.monotonic() - started < elapsed:
                healthy()
                time.sleep(.2)
            image = snapshot('youtube-' + str(elapsed))
            text = healthy()
            observations.append({'seconds': elapsed, 'width': image[0], 'height': image[1],
                'mse_sources': text.count('MSE: source initialized'),
                'renderer_stopped': 'RENDERER: stopped status=' in after_boundary_tests(text)})
            (build / 'youtube-observation-results.json').write_text(json.dumps(observations, indent=2))
        if os.environ.get('YOUTUBE_INSPECT') == '1':
            click(image[0] - 96, 24, page=False)
            wait_marker('DEVTOOLS: frontend loaded', timeout=90)
            deadline = time.monotonic() + 60
            while True:
                toolbar = snapshot('youtube-inspector-toolbar')
                selected_row = devtools_tab_row(toolbar, 38, 108)
                if selected_row is not None:
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError('YouTube inspector tab strip was not visible')
            # The optional language banner moves the tab strip by forty pixels.
            clicked_row = selected_row
            console_timeout = int(os.environ.get('YOUTUBE_CONSOLE_TIMEOUT', '30'))
            if not 30 <= console_timeout <= 180:
                raise ValueError('YOUTUBE_CONSOLE_TIMEOUT must be between 30 and 180 seconds.')
            console_started = time.monotonic()
            def save_console_readiness(ready):
                (build / 'youtube-console-readiness-results.json').write_text(json.dumps({
                    'ready': ready, 'seconds': time.monotonic() - console_started,
                    'timeout_seconds': console_timeout, 'clicked_row': clicked_row,
                }, indent=2) + '\n')
            click(143, clicked_row, page=False)
            move(300, 24, page=False)
            last_console_click = time.monotonic()
            deadline = time.monotonic() + console_timeout
            while True:
                console = snapshot('youtube-console-ready')
                prompt_y = devtools_console_prompt(console)
                if prompt_y is not None:
                    save_console_readiness(True)
                    click(70, prompt_y, page=False)
                    break
                # A late language banner can move the tab after the first click.
                updated_row = devtools_tab_row(console, 38, 108)
                if updated_row is not None and (updated_row != clicked_row or time.monotonic() - last_console_click >= 3):
                    clicked_row = updated_row
                    click(143, clicked_row, page=False)
                    move(300, 24, page=False)
                    last_console_click = time.monotonic()
                if time.monotonic() >= deadline:
                    save_console_readiness(False)
                    raise AssertionError('YouTube inspector Console prompt was not visible')
            query_offset = len(healthy())
            expression = 'console.log("YTSTATE",movie_player.getPlayerState(),document.querySelector("video")?.currentTime,document.querySelector("video")?.readyState,document.querySelector("video")?.error)'
            punctuation = {'"': ('shift', 'apostrophe'), '.': ('dot',),
                ',': ('comma',), '(': ('shift', '9'), ')': ('shift', '0'),
                '_': ('shift', 'minus'), '?': ('shift', 'slash')}
            for character in expression:
                keys = punctuation.get(character, ('shift', character.lower()) if character.isupper() else (character,))
                key(*keys)
            time.sleep(1)
            key('ret')
            try:
                wait_marker('YTSTATE ', timeout=45, offset=query_offset)
            except AssertionError:
                snapshot('youtube-console-query-failure')
                raise
            snapshot('youtube-inspected')
            if os.environ.get('YOUTUBE_PLAYBACK_PROBE') == '1':
                import runpy
                from pathlib import Path
                runpy.run_path(str(Path(__file__).with_name('youtube-playback.py')))['run'](
                    key, healthy, snapshot, build)
        print('OBSERVED: live YouTube page; playback requires visual and captured-audio verification')
        return
    if os.environ.get('INPUT_SCENARIO') == 'devtools-async':
        import json
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/devtools-async.html'); key('ret')
        wait_marker('DEVTOOLS ASYNC READY', timeout=60)
        initial = snapshot('devtools-async-ready')
        click(initial[0] - 96, 24, page=False)
        wait_marker('DEVTOOLS: frontend loaded', timeout=90)
        time.sleep(10)
        key('ctrl', '1')
        started = time.monotonic()
        click(200, 125)
        wait_marker('DEVTOOLS ASYNC START')
        wait_marker('DEVTOOLS ASYNC PASS', timeout=180)
        milliseconds = float(re.search(r'DEVTOOLS ASYNC PASS milliseconds=([0-9.]+)', healthy())[1])
        (build / 'devtools-async-results.json').write_text(json.dumps({
            'promise_steps': 2048, 'compiled_functions': 1024,
            'guest_milliseconds': milliseconds,
            'host_seconds': round(time.monotonic() - started, 3)}, indent=2))
        snapshot('devtools-async-complete')
        print('PASS: inspector-attached Promise workload, checksum and completion; guest milliseconds=', milliseconds)
        return
    if os.environ.get('INPUT_SCENARIO') == 'devtools':
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/devtools.html'); key('ret')
        wait_marker('DEVTOOLS FIXTURE READY')
        launcher = snapshot('devtools-launcher')
        tools_x = launcher[0] - 96
        # Native clicks must enter the inspector without relying on its shortcut.
        click(tools_x, 24, page=False)
        try:
            wait_marker('DEVTOOLS: frontend loaded', timeout=90)
        except Exception:
            execute('screendump', {'filename': str(build / 'input-devtools-failed.ppm')})
            raise
        time.sleep(5)
        created = healthy().count('TABS: created id=')
        click(tools_x, 24, page=False)
        assert healthy().count('TABS: created id=') == created
        key('ctrl', 't'); key('ctrl', 't')
        created = healthy().count('TABS: created id=')
        key('ctrl', '1')
        click(tools_x, 24, page=False)
        time.sleep(1)
        assert healthy().count('TABS: created id=') == created
        assert healthy().count('DEVTOOLS: frontend loaded') == 1
        snapshot('devtools-reused-at-tab-limit')
        key('ctrl', '4'); key('ctrl', 'w')
        key('ctrl', '3'); key('ctrl', 'w')
        snapshot('devtools-elements')
        click(350, 141)
        time.sleep(2)
        snapshot('devtools-styles')
        click(143, 54)
        # The native cursor must not obscure the selected-tab underline.
        move(300, 24, page=False)
        # The lazy Console editor must expose its prompt before it receives keys.
        deadline = time.monotonic() + 30
        last_console_click = time.monotonic()
        while True:
            healthy()
            path = build / 'input-devtools-console-ready.ppm'
            execute('screendump', {'filename': str(path)})
            header = path.read_bytes().split(b'\n', 3)
            width, height = map(int, header[1].split())
            pixels = header[3]
            console = (width, height, pixels)
            prompt_y = devtools_console_prompt(console)
            if prompt_y is not None:
                click(70, prompt_y, page=False)
                break
            updated_row = devtools_tab_row(console, 38, 108)
            if updated_row is not None and time.monotonic() - last_console_click >= 3:
                click(143, updated_row, page=False)
                move(300, 24, page=False)
                last_console_click = time.monotonic()
            if time.monotonic() >= deadline:
                raise AssertionError('DevTools Console did not display an input prompt')
            time.sleep(.2)
        for character in 'text.value="debugged";heading.style.color="red"':
            if character == '"': key('shift', 'apostrophe')
            elif character == '=': key('equal')
            elif character == ';': key('semicolon')
            elif character == '.': key('dot')
            else: key(character)
        key('ret')
        time.sleep(2)
        snapshot('devtools-console')
        key('ctrl', '1')
        click(470, 125)
        wait_marker('DEVTOOLS INSPECTED {"value":"debugged","color":"rgb(255, 0, 0)"}')
        snapshot('devtools-inspected')
        key('ctrl', '2')
        click(280, 54)
        time.sleep(2)
        snapshot('devtools-network')
        key('ctrl', '1')
        click(470, 180)
        wait_marker('DEVTOOLS: debugger paused')
        assert 'DEVTOOLS BREAKPOINT RESUMED' not in healthy()
        key('ctrl', '2')
        click(210, 54)
        # Sources loads its editor lazily on first use in the firmware frontend.
        time.sleep(8)
        snapshot('devtools-paused')
        pauses = healthy().count('DEVTOOLS: debugger paused')
        key('f10')
        deadline = time.monotonic() + 30
        while healthy().count('DEVTOOLS: debugger paused') <= pauses:
            if time.monotonic() >= deadline:
                raise AssertionError('DevTools step-over did not pause on the next statement')
            time.sleep(.2)
        assert 'DEVTOOLS BREAKPOINT RESUMED' not in healthy()
        time.sleep(3)
        snapshot('devtools-stepped')
        key('f8')
        wait_marker('DEVTOOLS BREAKPOINT RESUMED')
        wait_marker('DEVTOOLS: debugger resumed')
        snapshot('devtools-resumed')
        key('ctrl', 'w')
        # Opening from the process monitor must reveal the resulting tools tab.
        click(launcher[0] - 70, 64, page=False)
        wait_marker('MONITOR: opened')
        click(tools_x, 24, page=False)
        wait_marker('MONITOR: closed')
        deadline = time.monotonic() + 60
        while healthy().count('DEVTOOLS: frontend loaded') < 2:
            if time.monotonic() >= deadline: raise AssertionError('DevTools did not reopen')
            time.sleep(.2)
        key('ctrl', '1'); key('ctrl', 'w')
        wait_marker('DEVTOOLS: target session detached')
        time.sleep(2)
        snapshot('devtools-target-closed')
        print('PASS: DevTools toolbar launch, tab-limit reuse, monitor exit, console DOM/CSS mutation, debugger pause/step/resume, reopening and target detachment; DOM and network screenshots require inspection')
        return
    if os.environ.get('INPUT_SCENARIO') in ('ime', 'ime-japanese'):
        import runpy
        from pathlib import Path
        name = 'japanese-ime-scenario.py' if os.environ.get('INPUT_SCENARIO') == 'ime-japanese' else 'ime-scenario.py'
        runpy.run_path(str(Path(__file__).with_name(name)))['run'](
            key, click, type_address, wait_marker, snapshot, healthy)
        return
    if os.environ.get('INPUT_SCENARIO') == 'scroll-wire-boundary':
        import subprocess
        import tempfile
        import json
        assert os.environ.get('QEMU_TEST_REQUIRE_CPL3_PROBE')=='1'
        key('ctrl','l'); type_address('http://10.0.2.2:8765/renderer-blocking.html'); key('ret')
        wait_marker('ISOLATION ready'); wait_marker('cpl3=1'); time.sleep(2)
        symbols=subprocess.check_output(['llvm-nm','--defined-only',str(build/'payload.elf')],text=True)
        match=re.search(r'^([0-9a-fA-F]+) [Tt] renderer_boundary_scroll_wire$',symbols,re.M)
        assert match
        offset=len(healthy())
        commands='set pagination off\ntarget remote 127.0.0.1:'+os.environ['QEMU_TEST_GDB_PORT']+'\nthread 2\npython\n'
        commands+="if int(gdb.parse_and_eval('$cs')) & 3 == 3:\n    gdb.execute('set $rip = 0x"+match.group(1)+"')\n    print('WIRE_PROBE_CPL3')\nelse:\n    print('RETRY')\nend\ndetach\n"
        with tempfile.NamedTemporaryFile(mode='w',suffix='.gdb') as command_file:
            command_file.write(commands); command_file.flush()
            for attempt in range(40):
                result=subprocess.run(['gdb','-q','-nx','-batch','-x',command_file.name],text=True,capture_output=True,timeout=10,check=True)
                if 'WIRE_PROBE_CPL3' in result.stdout:
                    print(result.stdout,flush=True); break
                time.sleep(.05)
            else: raise AssertionError('Renderer never reached CPL3')
        deadline=time.monotonic()+30
        while True:
            text=healthy()[offset:]
            if text.count('COMPOSITOR: wire rejected')==5 and re.search(r'COMPOSITOR: scene .* cpl3=1',text): break
            assert time.monotonic()<deadline,text
            assert 'RENDERER: stopped' not in text,text
            time.sleep(.05)
        move(1100,500); before=snapshot('wire-boundary-before-scroll')
        wheel('down'); after=snapshot('wire-boundary-after-scroll')
        assert before[2]!=after[2] and 'COMPOSITOR: async scroll' in healthy()[offset:]
        assert 'ISOLATION scroll' not in healthy()[offset:]
        key('shift','esc'); key('delete'); wait_marker('RENDERER: recovery launched')
        key('esc'); recovered=len(healthy()); click(260,24,False)
        deadline=time.monotonic()+30
        while 'ISOLATION ready' not in healthy()[recovered:]:
            assert time.monotonic()<deadline,healthy()[recovered:]
            time.sleep(.1)
        time.sleep(1); click(280,50)
        wait_marker('ISOLATION click', timeout=5, offset=recovered)
        result={'producer_cpl':3,'rejected_publications':5,'untrusted_pointer_fields_ignored':True,
                'valid_cache_after_rejection':True,'scroll_while_producer_busy':True,'recovery_click':True}
        (build/'scroll-wire-boundary-results.json').write_text(json.dumps(result,indent=2))
        print('PASS: CPL3 hostile scroll publications',json.dumps(result)); return
    if os.environ.get('INPUT_SCENARIO') == 'sandbox':
        import subprocess
        import tempfile
        assert os.environ.get('QEMU_TEST_REQUIRE_CPL3_PROBE')=='1'
        key('ctrl','l'); type_address('http://10.0.2.2:8765/renderer-blocking.html'); key('ret')
        wait_marker('ISOLATION ready'); time.sleep(1)
        click(280,50); wait_marker('ISOLATION click')
        snapshot('sandbox-before')
        symbols=subprocess.check_output(['llvm-nm','--defined-only',str(build/'payload.elf')],text=True)
        for symbol,expected_error in (('renderer_boundary_attack',5),('renderer_boundary_attack_table',7)):
            match=re.search(r'^([0-9a-fA-F]+) [Tt] '+symbol+r'$',symbols,re.M)
            assert match, symbol
            previous=healthy(); offset=len(previous)
            command_text='set pagination off\ntarget remote 127.0.0.1:'+os.environ['QEMU_TEST_GDB_PORT']+'\nthread 2\npython\n'
            command_text+="if int(gdb.parse_and_eval('$cs')) & 3 == 3:\n    gdb.execute('set $rip = 0x"+match.group(1)+"')\n    print('CPL3_FAULT_ARMED')\nelse:\n    print('CPL3_RETRY')\nend\ndetach\n"
            armed=False
            with tempfile.NamedTemporaryFile(mode='w',suffix='.gdb') as command_file:
                command_file.write(command_text); command_file.flush()
                for attempt in range(30):
                    result=subprocess.run(['gdb','-q','-nx','-batch','-x',command_file.name],text=True,capture_output=True,timeout=10,check=True)
                    if 'CPL3_FAULT_ARMED' in result.stdout:
                        armed=True; print(result.stdout,flush=True); break
                    time.sleep(.05)
            assert armed, 'The AP never reached CPL3 for fault injection.'
            deadline=time.monotonic()+20
            while 'RENDERER: recovery launched' not in healthy()[offset:]:
                assert time.monotonic()<deadline, healthy()[offset:]
                time.sleep(.05)
            fault=re.search(r'vector=14 error=([0-9a-f]+) rip=([0-9a-f]+) cs=([0-9a-f]+) address=([0-9a-f]+)',healthy()[offset:])
            assert fault and int(fault[1],16)==expected_error and int(fault[3],16)&3==3, healthy()[offset:]
            assert int(fault[4],16)!=0, fault.groups()
            click(260,24,False)
            deadline=time.monotonic()+30
            while healthy()[offset:].count('ISOLATION ready')<1:
                assert time.monotonic()<deadline, healthy()[offset:]
                time.sleep(.1)
            time.sleep(1); click(280,50)
            wait_marker('ISOLATION click', timeout=5, offset=offset)
            snapshot('sandbox-recovered-'+symbol)
        print('PASS: live CPL3 website input, corrupted channel recovery, supervisor read denial, page-table write denial and two interactive recoveries',flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'gpu-canvas-colors':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/gpu-canvas-colors.html'); key('ret')
        wait_marker('GPU COLORS READY', timeout=120)
        time.sleep(2)
        image=snapshot('gpu-canvas-colors'); width,height,pixels=image
        for x,y,expected in [(60,80,(0,255,0)),(60,140,(128,0,0)),(210,80,(0,255,0)),(210,140,(128,0,0)),(360,140,(128,0,0)),(60,280,(0,0,128)),(210,280,(0,0,128)),(360,280,(0,0,128)),(680,70,(255,0,255)),(700,90,(255,255,255)),(760,70,(0,0,0)),(60,460,(0,255,0)),(210,460,(0,255,0)),(360,460,(0,255,0))]:
            offset=((y+80)*width+x)*3; actual=tuple(pixels[offset:offset+3])
            assert all(abs(a-b)<=2 for a,b in zip(actual,expected)),(x,y,expected,actual)
        if os.environ.get('QEMU_TEST_REQUIRE_GPU_CANVAS')=='1':
            assert healthy().count('GPU-CANVAS: imported token=')>=10
            assert 'GPU-CANVAS: readback api=' not in healthy()
        print('PASS: GPU canvas orientation, opaque/premultiplied/straight alpha, RGBA/BGRA/float16, CSS transform, clipping and foreground occlusion',flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'gpu-live-webgl':
        import json
        key('ctrl','l'); type_address('https://webglfundamentals.org/webgl/webgl-3d-perspective.html')
        offset=len(healthy()); started=time.monotonic(); key('ret')
        marker='GPU-CANVAS: imported token=' if os.environ.get('QEMU_TEST_REQUIRE_GPU_CANVAS')=='1' else 'GPU-CANVAS: readback api=webgl'
        wait_marker(marker,timeout=120,offset=offset)
        first_ms=round((time.monotonic()-started)*1000,2)
        wait_marker('BLINK: main document load finished',timeout=60,offset=offset)
        # SDL focus can deliver relative motion before the scenario starts.
        # Clamp to the screen corner instead of assuming the initial pointer position.
        execute('input-send-event',{'events':[
            {'type':'rel','data':{'axis':'x','value':-4096}},
            {'type':'rel','data':{'axis':'y','value':-4096}}]})
        time.sleep(.25);position[:]=[0,0]
        move(640,400,page=False)
        first=snapshot('live-webgl-loaded')
        def changed(previous,name,started):
            deadline=started+10
            while time.monotonic()<deadline:
                path=build/('input-'+name+'.ppm')
                execute('screendump',{'filename':str(path)})
                header=path.read_bytes().split(b'\n',3); width,height=map(int,header[1].split())
                current=(width,height,header[3])
                # Exclude the native toolbar and the website's slider panel.
                count=sum(previous[2][index:index+3]!=current[2][index:index+3]
                    for y in range(100,height) for x in range(0,900)
                    for index in [(y*width+x)*3])
                if count>1000:return current,round((time.monotonic()-started)*1000,2),count
                healthy();time.sleep(.02)
            raise AssertionError('The live WebGL object did not respond to input')
        move(1145,23); started=time.monotonic()
        for down in (True,False):execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
        second,click_ms,click_pixels=changed(first,'live-webgl-click',started)
        started=time.monotonic();execute('send-key',{'keys':[{'type':'qcode','data':'home'}]})
        third,key_ms,key_pixels=changed(second,'live-webgl-key',started)
        content=healthy()[offset:]
        result={'first_gpu_frame_ms':first_ms,'click_visible_ms':click_ms,'key_visible_ms':key_ms,
            'click_changed_pixels':click_pixels,'key_changed_pixels':key_pixels,
            'imports':content.count('GPU-CANVAS: imported token='),
            'readback_bytes':sum(map(int,re.findall(r'GPU-CANVAS: readback api=webgl bytes=(\d+)',content)))}
        if os.environ.get('QEMU_TEST_REQUIRE_GPU_CANVAS')=='1':assert result['readback_bytes']==0,result
        (build/'gpu-live-webgl-results.json').write_text(json.dumps(result,indent=2)+'\n')
        offset=len(healthy());key('ctrl','l');type_address('about:blank');key('ret')
        wait_marker('BLINK: main frame stopped loading',timeout=30,offset=offset)
        print('PASS: live HTTPS WebGL geometry responds to slider click and keyboard '+json.dumps(result),flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'gpu-aquarium':
        import json
        key('ctrl','l'); type_address('https://webglsamples.org/aquarium/aquarium.html')
        offset=len(healthy()); started=time.monotonic(); key('ret')
        marker='GPU-CANVAS: imported token=' if os.environ.get('QEMU_TEST_REQUIRE_GPU_CANVAS')=='1' else 'GPU-CANVAS: readback api=webgl'
        wait_marker(marker,timeout=150,offset=offset)
        first_ms=round((time.monotonic()-started)*1000,2)
        time.sleep(8)
        first=snapshot('aquarium-before')
        click(65,155)
        second=snapshot('aquarium-after')
        width,height,pixels=second
        changed=sum(first[2][index:index+3]!=pixels[index:index+3] for index in range((height//3)*width*3,len(pixels),3))
        assert changed>1000, 'Aquarium pixels did not animate'
        content=healthy()[offset:]
        result={'url':'https://webglsamples.org/aquarium/aquarium.html','first_gpu_frame_ms':first_ms,'changed_pixels':changed,
            'exports':content.count('GPU-CANVAS: exported token='),'imports':content.count('GPU-CANVAS: imported token='),
            'readback_bytes':sum(map(int,re.findall(r'GPU-CANVAS: readback api=webgl bytes=(\d+)',content)))}
        if os.environ.get('QEMU_TEST_REQUIRE_GPU_CANVAS')=='1':assert result['readback_bytes']==0,result
        (build/'gpu-aquarium-results.json').write_text(json.dumps(result,indent=2)+'\n')
        offset=len(healthy())
        key('ctrl','l');type_address('about:blank');key('ret')
        wait_marker('BLINK: main frame stopped loading',timeout=30,offset=offset)
        print('PASS: live WebGL Aquarium animation and navigation '+json.dumps(result),flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'gpu-canvas-perf':
        import json
        key('ctrl','l'); type_address('https://10.0.2.2:8767/gpu-canvas-perf.html'); key('ret')
        wait_marker('GPU PERF READY', timeout=120)
        samples=[]
        def measured(marker, action):
            offset=len(healthy()); started=time.monotonic(); action()
            deadline=started+30
            while marker not in healthy()[offset:]:
                assert time.monotonic()<deadline, 'Missing benchmark input: '+marker
                time.sleep(.01)
            samples.append({'event':marker,'host_delivery_to_log_ms':round((time.monotonic()-started)*1000,2)})
        move(90,35)
        def press():
            for down in (True,False): execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
        measured('GPU PERF INPUT click',press)
        click(300,35)
        measured('GPU PERF INPUT key a',lambda:execute('send-key',{'keys':[{'type':'qcode','data':'a'}]}))
        move(800,500)
        def scroll():
            for down in (True,False): execute('input-send-event',{'events':[{'type':'btn','data':{'button':'wheel-down','down':down}}]})
        measured('GPU PERF INPUT scroll',scroll)
        wait_marker('GPU PERF DONE', timeout=180)
        text=healthy()
        assert 'GPU PERF FAILURE' not in text, text
        result=json.loads(re.search(r'GPU PERF DONE (\{[^\n]+\})',text)[1])
        result['load_ms']=float(re.search(r'GPU PERF READY load_ms=([\d.]+)',text)[1])
        result['inputs']=samples
        result['exports']=len(re.findall(r'GPU-CANVAS: exported token=',text))
        result['imports']=len(re.findall(r'GPU-CANVAS: imported token=',text))
        result['readback_bytes']=sum(map(int,re.findall(r'GPU-CANVAS: readback api=\w+ bytes=(\d+)',text)))
        if os.environ.get('QEMU_TEST_REQUIRE_GPU_CANVAS')=='1':
            assert result['exports']>0 and result['imports']>0, result
            assert result['readback_bytes']==0, result
        (build/'gpu-canvas-perf-results.json').write_text(json.dumps(result,indent=2)+'\n')
        snapshot('gpu-canvas-perf')
        print('PASS: animated WebGL2/WebGPU canvases and live click, keyboard and wheel input '+json.dumps(result),flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'webgpu-canvas':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/webgpu-canvas.html'); key('ret')
        wait_marker('WEBGPU CANVAS PASS checks=11', timeout=150)
        width, height, pixels = snapshot('webgpu-canvas')
        offset = (350 * width + 160) * 3
        assert tuple(pixels[offset:offset + 3]) == (0, 0, 255)
        print('PASS: WebGPU canvas formats, alpha modes, resize, reconfiguration and presentation')
        return
    if os.environ.get('INPUT_SCENARIO') == 'gpu-multicontext':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/gpu-multicontext.html'); key('ret')
        wait_marker('GPU MULTICONTEXT PASS checks=14', timeout=150)
        assert 'GPU MULTICONTEXT FAILURE' not in healthy()
        width, height, pixels = snapshot('gpu-multicontext')
        for color in (bytes((0, 255, 0)), bytes((0, 0, 255))):
            assert sum(pixels[index:index+3] == color for index in range(0, len(pixels), 3)) >= 8000, color
        if os.environ.get('QEMU_TEST_VENUS') == '1':
            counts = [int(count) for count in re.findall(r'VENUS TRANSPORT: context opened id=\d+ active=(\d+)', healthy())]
            assert counts and max(counts) >= 3 + (os.environ.get('QEMU_TEST_GPU_COMPOSITION') == '1'), counts
        print('PASS: simultaneous WebGL1/WebGL2 and frame WebGPU compute, context loss, removal and recreation')
        return
    if os.environ.get('INPUT_SCENARIO') == 'gpu-lifecycle':
        cycles = 8
        for cycle in range(cycles):
            for version, marker in (('webgpu', 'WEBGPU PASS checks=13'), ('webgl2', 'WEBGL2 PASS checks=8')):
                previous = healthy().count(marker)
                key('ctrl','l'); type_address('https://10.0.2.2:8767/' + version + '.html'); key('ret')
                deadline = time.monotonic() + 150
                while healthy().count(marker) <= previous:
                    assert time.monotonic() < deadline, (version, cycle, healthy())
                    assert 'RENDERER: stopped status=' not in after_boundary_tests(healthy()), healthy()
                    time.sleep(.2)
                width, height, pixels = snapshot('lifecycle-' + str(cycle) + '-' + version)
                for x, y, expected in ((40, 240, (0, 255, 0)), (160, 350, (255, 0, 0))):
                    offset = (y * width + x) * 3
                    assert tuple(pixels[offset:offset + 3]) == expected, (version, cycle, x, y)
        if os.environ.get('QEMU_TEST_VENUS') == '1':
            transitions = re.findall(r'VENUS TRANSPORT: context (opened|closed) id=(\d+) active=(\d+)', healthy())
            assert transitions and max(int(count) for _, _, count in transitions) <= 2 + (os.environ.get('QEMU_TEST_GPU_COMPOSITION') == '1'), transitions
            assert sum(kind == 'closed' for kind, _, _ in transitions) >= cycles, transitions
            assert int(transitions[-1][2]) == 1 + (os.environ.get('QEMU_TEST_GPU_COMPOSITION') == '1'), transitions
        print('PASS: eight WebGPU/WebGL2 navigation cycles, bounded live contexts, device teardown and visible frames')
        return
    if os.environ.get('INPUT_SCENARIO') in ('webgl', 'webgl2'):
        version = os.environ['INPUT_SCENARIO']
        key('ctrl','l'); type_address('https://10.0.2.2:8767/' + version + '.html'); key('ret')
        wait_marker('WEBGL2 PASS checks=8' if version == 'webgl2' else 'WEBGL PASS checks=6', timeout=150)
        time.sleep(.5)
        width, height, pixels = snapshot(version)
        for x, y, expected in ((40, 240, (0, 255, 0)), (160, 350, (255, 0, 0))):
            offset = (y * width + x) * 3
            assert tuple(pixels[offset:offset + 3]) == expected, (x, y, pixels[offset:offset + 3])
        print('PASS: WebGL clear, GLSL compilation, triangle rasterization, readback and validation')
        return
    if os.environ.get('INPUT_SCENARIO') in ('webgpu', 'gpu-coherency'):
        coherence = os.environ.get('INPUT_SCENARIO') == 'gpu-coherency'
        key('ctrl','l'); type_address('https://10.0.2.2:8767/webgpu.html' + ('?coherence_cycles=32' if coherence else '')); key('ret')
        wait_marker('WEBGPU PASS checks=13', timeout=150)
        assert 'WEBGPU FAILURE' not in healthy()
        if coherence: assert 'WEBGPU COHERENCY PASS cycles=32' in healthy()
        time.sleep(.5)
        width, height, pixels = snapshot('webgpu')
        for x, y, expected in ((40, 240, (0, 255, 0)), (160, 350, (255, 0, 0))):
            offset = (y * width + x) * 3
            assert tuple(pixels[offset:offset + 3]) == expected, (x, y, pixels[offset:offset + 3])
        print('PASS: WebGPU compute, shader rasterization, readback, validation and device recreation')
        return
    if os.environ.get('INPUT_SCENARIO') == 'workers-limits':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/workers-limits.html'); key('ret')
        wait_marker('WORKERS LIMIT PASS checks=3', timeout=90)
        snapshot('workers-limits')
        print('PASS: worker capacity, recovery and early termination')
        return
    if os.environ.get('INPUT_SCENARIO') == 'workers':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/workers.html'); key('ret')
        wait_marker('WORKERS PASS checks=15', timeout=90)
        snapshot('workers')
        print('PASS: dedicated worker execution, transfers, fetch, timer, close and recreation')
        return
    if os.environ.get('INPUT_SCENARIO') == 'audio-limits':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/audio-limits.html'); key('ret')
        wait_marker('AUDIO LIMIT PASS checks=11', timeout=120)
        snapshot('audio-limits')
        print('PASS: bounded audio resources, malformed input and cleanup recovery')
        return
    if os.environ.get('INPUT_SCENARIO') == 'audio':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/audio.html'); key('ret')
        wait_marker('AUDIO READY', timeout=90)
        click(480, 220)
        wait_marker('AUDIO PASS checks=15', timeout=60)
        snapshot('audio-complete')
        print('PASS: audio DMA playback, pause, seek, volume, end and reload; waveform validation follows QEMU shutdown')
        return
    if os.environ.get('INPUT_SCENARIO') == 'mse-config':
        key('ctrl', 'l'); type_address('https://10.0.2.2:8767/media-config.html'); key('ret')
        wait_marker('MSE CONFIG READY', timeout=60)
        click(450, 205)
        wait_marker('MSE CONFIG PASS', timeout=45)
        assert 'MSE CONFIG FAILURE' not in healthy()
        snapshot('mse-config-complete')
        print('PASS: MSE mono/stereo transition clock; complete PCM validation follows shutdown')
        return
    if os.environ.get('INPUT_SCENARIO') == 'mse-audio':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/media-source-audio.html'); key('ret')
        wait_marker('MSE AUDIO READY', timeout=90)
        click(480, 220)
        for marker, name, channel in [('MSE AUDIO GREEN', 'mse-audio-green', 1), ('MSE AUDIO BLUE', 'mse-audio-blue', 2)]:
            wait_marker(marker, timeout=40)
            width, height, pixels = snapshot(name)
            for x, y in ((80, 260), (190, 330), (300, 400)):
                color = pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
                assert color[channel] > 180 and all(color[index] < 60 for index in range(3) if index != channel), (name, list(color))
        wait_marker('MSE AUDIO PASS checks=17', timeout=90)
        snapshot('mse-audio-complete')
        print('PASS: MSE Opus and VP8 playback, pause, seek, separate source buffers and audio-only playback')
        return
    if os.environ.get('INPUT_SCENARIO') == 'media-source-boundaries':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/media-source-boundaries.html'); key('ret')
        wait_marker('MSE LIMIT PASS checks=14', timeout=150)
        assert 'MSE LIMIT FAILURE' not in healthy()
        snapshot('mse-boundaries')
        print('PASS: MSE abort, append window, sequence, config change, malformed data, frame recycling and pending detach')
        return
    if os.environ.get('INPUT_SCENARIO') == 'media-source':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/media-source.html'); key('ret')
        wait_marker('MSE READY', timeout=90)
        def mse_pixels(name, channel):
            width, height, pixels = snapshot(name)
            for x, y in ((80, 260), (190, 330), (300, 400)):
                offset = (y * width + x) * 3
                color = pixels[offset:offset + 3]
                assert color[channel] > 180 and all(color[index] < 60 for index in range(3) if index != channel), (name, x, y, list(color))
        mse_pixels('mse-red', 0)
        click(480, 220)
        wait_marker('MSE GREEN', timeout=40)
        mse_pixels('mse-green', 1)
        wait_marker('MSE BLUE', timeout=40)
        mse_pixels('mse-blue', 2)
        wait_marker('MSE PASS checks=22', timeout=90)
        assert 'MSE FAILURE' not in healthy()
        snapshot('mse-complete')
        print('PASS: MSE split append, streaming, underflow recovery, seek, removal, EOS, replay and reattach')
        return
    if os.environ.get('INPUT_SCENARIO') == 'media-boundaries':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/media-boundaries.html'); key('ret')
        wait_marker('MEDIA BOUNDARIES PASS checks=14', timeout=120)
        snapshot('media-boundaries')
        print('PASS: media capability reporting, load failures, origin checks, quota and recovery')
        return
    if os.environ.get('INPUT_SCENARIO') == 'media':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/media.html'); key('ret')
        wait_marker('MEDIA READY', timeout=90)
        def video_pixels(name, channel):
            width, height, pixels = snapshot(name)
            for x, y in ((80, 260), (190, 330), (300, 400)):
                offset = (y * width + x) * 3
                color = pixels[offset:offset + 3]
                assert color[channel] > 180 and all(color[index] < 60 for index in range(3) if index != channel), (name, x, y, list(color))
        video_pixels('media-red', 0)
        click(480, 220)
        wait_marker('MEDIA GREEN', timeout=30)
        video_pixels('media-green', 1)
        wait_marker('MEDIA BLUE', timeout=30)
        video_pixels('media-blue', 2)
        wait_marker('MEDIA PASS checks=20', timeout=40)
        assert 'MEDIA FAILURE' not in healthy()
        snapshot('media-complete')
        print('PASS: VP8/VP9 decode, playback, pause, seek, rate and canvas pixel readback')
        return
    if os.environ.get('INPUT_SCENARIO') == 'service-capabilities':
        import json
        key('ctrl','l'); type_address('https://10.0.2.2:8767/service-capabilities.html'); key('ret')
        wait_marker('SERVICE PROBES COMPLETE', timeout=80)
        records = re.findall(r'SERVICE RESULT (\{[^\n]+\})', healthy())
        assert len(records) == 6, records
        assert all('timeout' not in json.loads(record)['details'] for record in records), records
        assert any(json.loads(record)['name'] == 'indexeddb' and json.loads(record)['status'] == 'working' for record in records), records
        assert any(json.loads(record)['name'] == 'video-webm' and json.loads(record)['status'] == 'working' for record in records), records
        assert any(json.loads(record)['name'] == 'media' and json.loads(record)['status'] == 'working' for record in records), records
        assert any(json.loads(record)['name'] == 'dedicated-worker' and json.loads(record)['status'] == 'working' for record in records), records
        (build / 'service-capabilities-results.json').write_text(json.dumps([json.loads(record) for record in records], indent=2) + '\n')
        snapshot('service-capabilities')
        print('Service probes completed; per-API results record availability, not overall support.')
        return
    if os.environ.get('INPUT_SCENARIO') == 'indexeddb':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/indexeddb.html'); key('ret')
        wait_marker('IDB PASS checks=37', timeout=160)
        assert 'IDB FAIL' not in healthy()
        snapshot('indexeddb-checks')
        print('PASS: IndexedDB transactions, indexes, cursors and Blob values')
        return
    if os.environ.get('INPUT_SCENARIO') == 'cache':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/cache.html'); key('ret')
        wait_marker('CACHE PASS checks=35', timeout=120)
        snapshot('cache-checks')
        print('PASS: Cache Storage response bodies, matching, batches, origins and Blob transfer')
        return
    if os.environ.get('INPUT_SCENARIO') == 'storage-lifecycle':
        def wait_storage(count, session):
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                content = healthy()
                assert 'STORAGE FAIL' not in content
                records = re.findall(r'STORAGE GENERATION local=(\d+) session=(\d+)', content)
                if content.count('STORAGE PASS checks=14') >= count:
                    assert len(records) == count and int(records[-1][1]) == session, records
                    return int(records[-1][0])
                time.sleep(.2)
            raise AssertionError('Storage lifecycle timed out')
        key('ctrl','l'); type_address('http://10.0.2.2:8765/storage.html'); key('ret')
        first = wait_storage(1, 1)
        click(260,24,False); assert wait_storage(2, 2) == first + 1
        key('ctrl','t'); key('ctrl','l'); type_address('http://10.0.2.2:8765/storage.html'); key('ret')
        assert wait_storage(3, 1) == first + 2
        key('ctrl','w'); click(260,24,False); assert wait_storage(4, 3) == first + 3
        snapshot('storage-lifecycle')
        print('PASS: reload retains session storage; new tabs isolate sessions and share local storage')
        return
    if os.environ.get('INPUT_SCENARIO') == 'websocket':
        key('ctrl','l'); type_address('http://10.0.2.2:8765/websocket.html'); key('ret')
        wait_marker('WEBSOCKET PASS checks=17', 100)
        assert 'WEBSOCKET FAIL' not in healthy()
        snapshot('websocket-checks')
        print('PASS: WebSocket text, binary, fragmentation, ping, close, TLS and protocol rejection')
        return
    if os.environ.get('INPUT_SCENARIO') == 'storage':
        key('ctrl','l'); type_address('http://10.0.2.2:8765/storage.html'); key('ret')
        wait_marker('STORAGE PASS checks=14')
        assert 'STORAGE FAIL' not in healthy()
        snapshot('storage-checks')
        print('PASS: local/session storage, events, origins, Unicode and quota')
        return
    if os.environ.get('INPUT_SCENARIO') in ('network', 'network-secure'):
        secure = os.environ['INPUT_SCENARIO'] == 'network-secure'
        key('ctrl','l'); type_address(('https://10.0.2.2:8767' if secure else 'http://10.0.2.2:8765') + '/network.html'); key('ret')
        wait_marker('NETWORK PASS checks=34')
        assert 'NETWORK FAIL' not in healthy()
        snapshot('network-checks')
        click(120,170)
        wait_marker('NETWORK FORM ')
        import json
        record = json.loads(healthy().split('NETWORK FORM ')[-1].splitlines()[0])
        assert record['method'] == 'POST', record
        assert record['body'] == 'message=hello+world&unicode=%ED%95%9C%EA%B8%80', record
        snapshot('network-form')
        print('PASS: network methods, request bodies, CORS, redirects, XHR and POST form')
        return
    if os.environ.get('INPUT_SCENARIO') == 'cookies-secure':
        key('ctrl','l'); type_address('https://10.0.2.2:8767/cookie-secure'); key('ret')
        wait_marker('COOKIE SECURE PASS checks=8')
        assert 'COOKIE FAIL' not in healthy()
        key('ctrl','l'); type_address('http://10.0.2.2:8765/cookie-navigation-check'); key('ret')
        wait_marker('COOKIE navigation=')
        line=healthy().split('COOKIE navigation=')[-1].splitlines()[0]
        assert 'secureServer=' not in line and '__Host-secure=' not in line, line
        print('PASS: HTTPS Secure, HttpOnly, prefixes and plaintext exclusion')
        return
    if os.environ.get('INPUT_SCENARIO') == 'cookies':
        key('ctrl','l'); type_address('http://10.0.2.2:8765/cookie-test'); key('ret')
        wait_marker('COOKIE PASS checks=15')
        assert 'COOKIE FAIL' not in healthy()
        key('ctrl','l'); type_address('http://10.0.2.2:8765/cookie-navigation-check'); key('ret')
        wait_marker('COOKIE navigation=')
        line=healthy().split('COOKIE navigation=')[-1].splitlines()[0]
        assert 'server=private' in line and 'public=visible' in line, line
        assert 'script=hello' not in line, 'Conservative top-level Lax context sent a Strict cookie'
        print('PASS: cookie script, HTTP, credentials, scope and navigation integration')
        return
    if os.environ.get('INPUT_SCENARIO') in ('scroll-state','scroll-stuck','scroll-state-iframe'):
        import runpy
        runpy.run_path(str(__import__('pathlib').Path(__file__).parent/'scroll-state.py'))['run'](
            execute,build,healthy,move,click,key,wait_marker,type_address)
        return
    if os.environ.get('INPUT_SCENARIO') == 'frame-policy':
        import runpy
        runpy.run_path(str(__import__('pathlib').Path(__file__).parent/'frame-policy.py'))['run'](key,wait_marker,type_address)
        return
    if os.environ.get('INPUT_SCENARIO') == 'scroll-gutter-fallback':
        import runpy
        runpy.run_path(str(__import__('pathlib').Path(__file__).parent/'scroll-gutter-fallback.py'))['run'](
            execute,build,healthy,move,click,key,wait_marker,type_address)
        return
    if os.environ.get('INPUT_SCENARIO') in ('scroll-gutter-state', 'scroll-gutter-topology', 'scroll-gutter-replacement', 'scroll-gutter', 'scroll-gutter-root', 'scroll-gutter-horizontal', 'scroll-gutter-container', 'scroll-gutter-iframe', 'scroll-gutter-srcdoc', 'scroll-gutter-nested-frame', 'scroll-gutter-parent-frame'):
        import runpy
        runpy.run_path(str(__import__('pathlib').Path(__file__).parent/'scroll-gutter.py'))['run'](
            execute,build,healthy,move,click,key,wait_marker,type_address)
        return
    if os.environ.get('INPUT_SCENARIO') == 'scroll-layout':
        import runpy
        runpy.run_path(str(__import__('pathlib').Path(__file__).parent/'scroll-layout.py'))['run'](
            execute,build,healthy,move,click,key,wait_marker,type_address)
        return
    if os.environ.get('INPUT_SCENARIO') == 'scroll-filter':
        import runpy
        runpy.run_path(str(__import__('pathlib').Path(__file__).parent/'scroll-filter.py'))['run'](
            execute,build,healthy,move,click,key,wait_marker,type_address)
        return
    if os.environ.get('INPUT_SCENARIO') in ('scroll-affine', 'scroll-affine-controls'):
        import runpy
        runpy.run_path(str(__import__('pathlib').Path(__file__).parent/'scroll-affine.py'))['run'](
            execute,build,healthy,move,click,key,wait_marker,type_address,
            horizontal=os.environ.get('INPUT_SCENARIO') == 'scroll-affine-controls')
        return
    if os.environ.get('INPUT_SCENARIO') == 'scroll-style':
        import runpy
        runpy.run_path(str(__import__('pathlib').Path(__file__).parent/'scroll-style.py'))['run'](
            execute,build,healthy,move,click,key,wait_marker,type_address)
        return
    if os.environ.get('INPUT_SCENARIO') == 'scroll-controls':
        import runpy
        runpy.run_path(str(__import__('pathlib').Path(__file__).parent/'scroll-controls.py'))['run'](
            execute,build,healthy,move,click,key,wait_marker,type_address)
        return
    if os.environ.get('INPUT_SCENARIO') == 'scroll-input':
        import json
        key('ctrl','l'); type_address('http://10.0.2.2:8765/scroll-input.html'); key('ret')
        wait_marker('INPUT ready'); wait_marker('COMPOSITOR: scene pictures='); time.sleep(2)
        def capture(name):
            path=build/('input-input-'+name+'.ppm'); execute('screendump',{'filename':str(path)})
            parts=path.read_bytes().split(b'\n',3)
            return int(parts[1].split()[0]),int(parts[1].split()[1]),parts[3]
        def crop(image,left,top,right,bottom):
            width,_,pixels=image
            return b''.join(pixels[(row*width+left)*3:(row*width+right)*3] for row in range(top,bottom))
        def press(down):
            execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
            time.sleep(.1)
        def check_busy(mode):
            assert 'INPUT '+mode+' end' not in healthy(), 'JavaScript finished before input verification'
        def key_position(mode,keys,expected):
            start=len(healthy()); key(*keys); text=healthy()[start:]
            assert re.search(r'COMPOSITOR: async key sequence=\d+ position='+str(expected)+r' ',text),text
            check_busy(mode)
        click(85,45); wait_marker('INPUT root begin')
        root_start=len(healthy())
        before=capture('root-before')
        started=time.monotonic()
        execute('send-key',{'keys':[{'type':'qcode','data':'down'}]})
        deadline=started+1
        while time.monotonic()<deadline:
            first=capture('first-key-busy')
            if crop(first,550,130,650,250)!=crop(before,550,130,650,250): break
            time.sleep(.005)
        else: raise AssertionError('Keyboard scroll did not update pixels within one second')
        first_key_ms=(time.monotonic()-started)*1000
        check_busy('root')
        for keys,expected in [(('pgdn',),670),(('home',),0),(('end',),13280),
                              (('up',),13240),(('home',),0),(('spc',),630),(('shift','spc'),0)]:
            key_position('root',keys,expected)
        move(500,500); start=len(healthy()); wheel('down')
        assert re.search(r'COMPOSITOR: async scroll sequence=\d+ position=80 ',healthy()[start:]),healthy()[start:]
        check_busy('root'); passive=capture('passive-busy')
        assert crop(passive,400,350,650,650)!=crop(before,400,350,650,650)
        assert 'INPUT passive cancelable=' not in healthy()[root_start:], 'Listener ran inside the long task'
        move(150,250); start=len(healthy()); wheel('down'); check_busy('root')
        assert 'COMPOSITOR: async scroll' not in healthy()[start:], 'Cancelable region scrolled before dispatch'
        move(1272,35); start=len(healthy()); press(True); move(1272,350); press(False)
        check_busy('root'); root_drag=capture('root-drag-busy')
        assert crop(root_drag,1265,96,1280,775)!=crop(before,1265,96,1280,775), 'Root thumb did not move during JavaScript'
        root_positions=re.findall(r'COMPOSITOR: async drag sequence=\d+ position=(\d+) element=(\d+)',healthy()[start:])
        assert root_positions and int(root_positions[-1][0])>5000,healthy()[start:]
        move(1142,225); start=len(healthy()); press(True); move(1142,450); press(False)
        check_busy('root'); nested_drag=capture('nested-drag-busy')
        assert crop(nested_drag,1135,296,1150,565)!=crop(root_drag,1135,296,1150,565), 'Nested thumb did not move during JavaScript'
        nested_positions=re.findall(r'COMPOSITOR: async drag sequence=\d+ position=(\d+) element=(\d+)',healthy()[start:])
        assert nested_positions and int(nested_positions[-1][0])>7000,healthy()[start:]
        assert nested_positions[-1][1]!=root_positions[-1][1]
        assert crop(nested_drag,750,330,1100,550)!=crop(root_drag,750,330,1100,550)
        wait_marker('INPUT root end'); wait_marker('INPUT passive cancelable=false'); wait_marker('INPUT wheel canceled=true')
        wait_marker('INPUT root position='+root_positions[-1][0]); wait_marker('INPUT nested position='+nested_positions[-1][0]); time.sleep(2)
        reconciled=capture('reconciled')
        left=crop(nested_drag,400,80,1280,700); right=crop(reconciled,400,80,1280,700)
        mismatch=sum(abs(a-b)>2 for a,b in zip(left,right))/len(left)
        assert mismatch<.02,('Scrollbar reconciliation differs',mismatch)
        click(210,45); wait_marker('INPUT inside begin')
        for keys,expected in [(('pgdn',),262),(('home',),0),(('end',),9700),(('up',),9660),
                              (('home',),0),(('spc',),262),(('shift','spc'),0)]:
            key_position('inside',keys,expected)
        key_position('inside',('pgdn',),262); capture('nested-key-busy')
        wait_marker('INPUT inside end'); wait_marker('INPUT nested position=262'); time.sleep(1)
        click(335,45); wait_marker('INPUT cancel begin'); start=len(healthy()); key('pgdn'); check_busy('cancel')
        assert 'COMPOSITOR: async key' not in healthy()[start:], 'Cancelable keyboard listener was bypassed'
        wait_marker('INPUT cancel end'); wait_marker('INPUT key canceled=true'); time.sleep(1)
        click(460,45); wait_marker('INPUT edit begin'); start=len(healthy()); key('end'); key('a'); check_busy('edit')
        assert 'COMPOSITOR: async key' not in healthy()[start:], 'Editable control scrolled the document'
        wait_marker('INPUT edit end'); wait_marker('INPUT editor=unchangeda')
        click(585,45); wait_marker('INPUT selectmode begin'); start=len(healthy()); key('end'); check_busy('selectmode')
        assert 'COMPOSITOR: async key' not in healthy()[start:], 'Select control scrolled the document'
        wait_marker('INPUT selectmode end'); wait_marker('INPUT choice=C')
        result={'first_key_capture_ms':round(first_key_ms,2),'select_control_preserved':True,'javascript_block_ms':12000,'passive_wheel_during_javascript':True,'cancelable_wheel_preserved':True,
                'root_keyboard_commands':8,'nested_keyboard_commands':8,'root_drag_position':int(root_positions[-1][0]),
                'nested_drag_position':int(nested_positions[-1][0]),'reconciliation_difference_fraction':mismatch,
                'cancelable_keyboard_preserved':True,'editable_control_preserved':True}
        (build/'scroll-input-results.json').write_text(json.dumps(result,indent=2))
        print('PASS: asynchronous passive wheel, keyboard and scrollbar input',json.dumps(result)); return
    if os.environ.get('INPUT_SCENARIO') == 'entropy':
        key('ctrl','l'); type_address('http://10.0.2.2:8765/entropy.html'); key('ret')
        wait_marker('ENTROPY complete 65536'); print('PASS: renderer ArrayBuffer entropy'); return
    if os.environ.get('INPUT_SCENARIO') in ('scroll-site','scroll-site-input'):
        import json
        url='https://docs.python.org/3/library/asyncio-eventloop.html'
        key('ctrl','l'); type_address(url); start=len(healthy()); key('ret')
        deadline=time.monotonic()+120
        while time.monotonic()<deadline:
            text=healthy()[start:]
            if 'BLINK: navigation HTTP status=200' in text and 'BLINK: main frame stopped loading' in text and 'COMPOSITOR: scene pictures=' in text: break
            time.sleep(.2)
        else: raise AssertionError('Real website did not publish an asynchronous scene: '+healthy()[start:])
        time.sleep(3); before=snapshot('scroll-site-before')
        move(150,500); start=len(healthy()); wheel('down',1)
        nested=snapshot('scroll-site-sidebar')
        nested_log=healthy()[start:]
        assert 'COMPOSITOR: async scroll' in nested_log, nested_log
        assert nested!=before, 'Real sidebar did not move'
        move(900,500); start=len(healthy()); wheel('down',24)
        body=snapshot('scroll-site-document')
        body_log=healthy()[start:]
        assert 'COMPOSITOR: async scroll' in body_log, body_log
        assert body!=nested, 'Real document did not move'
        nested_ids=set(re.findall(r'COMPOSITOR: async scroll .*element=(\d+)',nested_log))
        root_ids=set(re.findall(r'COMPOSITOR: async scroll .*element=(\d+)',body_log))
        assert nested_ids and root_ids and nested_ids.isdisjoint(root_ids), ('Sidebar did not scroll independently',nested_ids,root_ids)
        result={'url':url,'nested_elements':sorted(nested_ids),'root_elements':sorted(root_ids),'nested_async_events':nested_log.count('COMPOSITOR: async scroll'),
                'root_async_events':body_log.count('COMPOSITOR: async scroll')}
        if os.environ.get('INPUT_SCENARIO') == 'scroll-site-input':
            def thumb_center(image,x,top,bottom):
                width,_,pixels=image; runs=[]; start=None
                for y in range(top,bottom):
                    offset=(y*width+x)*3; color=pixels[offset:offset+3]
                    thumb=170<=min(color)<=220 and max(color)-min(color)<6
                    if thumb and start is None: start=y
                    if not thumb and start is not None:
                        if y-start>=8: runs.append((y-start,(y+start)//2))
                        start=None
                assert runs, ('No native scrollbar thumb',x,top,bottom)
                return max(runs)[1]
            drag_ids=[]
            for name,x,end in [('document',1272,450),('sidebar',316,700)]:
                screen=snapshot('site-input-'+name+'-before')
                center=thumb_center(screen,x,96,775)
                move(x,center,page=False); start=len(healthy())
                execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]})
                time.sleep(.1); move(x,end,page=False)
                execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
                time.sleep(.3)
                samples=re.findall(r'COMPOSITOR: async drag sequence=\d+ position=(\d+) element=(\d+)',healthy()[start:])
                assert samples, ('Live scrollbar did not use the compositor',name,healthy()[start:])
                drag_ids.append(samples[-1][1]); snapshot('site-input-'+name+'-after')
            assert len(set(drag_ids))==2, drag_ids
            result['scrollbar_drag_elements']=drag_ids
        (build/'scroll-site-results.json').write_text(json.dumps(result,indent=2))
        print('PASS: real website asynchronous scrolling',json.dumps(result)); return
    if os.environ.get('INPUT_SCENARIO') == 'scroll-tree':
        import json
        key('ctrl','l'); type_address('http://10.0.2.2:8765/scroll-tree.html'); key('ret')
        wait_marker('SCENE ready'); wait_marker('COMPOSITOR: scene pictures='); time.sleep(2)
        def capture(name):
            path=build/('input-'+name+'.ppm'); execute('screendump',{'filename':str(path)})
            parts=path.read_bytes().split(b'\n',3)
            return int(parts[1].split()[0]),int(parts[1].split()[1]),parts[3]
        def crop(image,left,top,right,bottom):
            width,_,pixels=image
            return b''.join(pixels[(row*width+left)*3:(row*width+right)*3] for row in range(top,bottom))
        def fast_wheel(ticks):
            for _ in range(ticks):
                for down in (True,False):
                    execute('input-send-event',{'events':[{'type':'btn','data':{'button':'wheel-down','down':down}}]})
                    time.sleep(.015)
        click(100,45); wait_marker('SCENE begin'); move(1000,400)
        before=capture('tree-before'); started=time.monotonic()
        for down in (True,False):
            execute('input-send-event',{'events':[{'type':'btn','data':{'button':'wheel-down','down':down}}]})
        deadline=started+1
        while time.monotonic()<deadline:
            first=capture('tree-first-wheel')
            if crop(first,720,350,950,550)!=crop(before,720,350,950,550): break
            time.sleep(.005)
        else: raise AssertionError('Nested scrolling did not update within one second')
        latency=(time.monotonic()-started)*1000
        fast_wheel(54)
        nested=capture('tree-nested-busy')
        assert 'SCENE end' not in healthy()
        assert crop(nested,800,350,801,351)==bytes((238,170,34)), 'Nested distant marker was not rasterized'
        assert crop(nested,1100,315,1101,316)==bytes((34,170,102)), 'Nested sticky header moved'
        assert 'SCENE nested 4400' not in healthy(), 'DOM ran inside the long task'
        move(500,500); fast_wheel(64)
        root=capture('tree-root-busy')
        assert 'SCENE end' not in healthy()
        assert crop(root,500,90,501,91)==bytes((136,68,204)), 'Root sticky did not attach to the viewport'
        assert crop(root,500,200,501,201)==bytes((184,55,40)), 'Root distant marker was not rasterized'
        assert crop(root,800,350,801,351)==bytes((238,170,34)), 'Root scroll changed fixed nested content'
        wait_marker('SCENE end'); wait_marker('SCENE root 5120'); wait_marker('SCENE nested 4400')
        wait_marker('acknowledgement=119'); time.sleep(1)
        reconciled=capture('tree-reconciled')
        first=crop(root,300,80,1100,700); second=crop(reconciled,300,80,1100,700)
        mismatch=sum(abs(a-b)>2 for a,b in zip(first,second))/len(first)
        assert mismatch<.02, ('Scene reconciliation differs',mismatch)
        result={'root_scroll_px':5120,'nested_scroll_px':4400,'javascript_block_ms':12000,
                'first_scroll_capture_ms':round(latency,2),
                'sticky_root_and_nested':True,'reconciliation_difference_fraction':mismatch}
        for name,x,expected in [('boundary',205,80),('contain',285,9700)]:
            click(x,45); wait_marker('SCENE '+name+' begin'); move(1000,400)
            start=len(healthy()); fast_wheel(1)
            time.sleep(.1); tail=healthy()[start:]
            assert 'SCENE '+name+' end' not in tail
            assert re.search(r'COMPOSITOR: async scroll sequence=\d+ position='+str(expected)+r' ',tail),tail
            wait_marker('SCENE '+name+' end'); time.sleep(1)
        result['boundary_chain_and_containment']=True
        (build/'scroll-tree-results.json').write_text(json.dumps(result,indent=2))
        print('PASS: nested, sticky and cache-independent scrolling',json.dumps(result)); return
    if os.environ.get('INPUT_SCENARIO') == 'renderer-exit':
        key('ctrl','l'); type_address('http://10.0.2.2:8765/renderer-infinite.html'); key('ret')
        wait_marker('RECOVERY fixture ready'); time.sleep(1)
        click(130,40); wait_marker('RECOVERY infinite begin')
        print('Renderer is executing infinite JavaScript before the F12 shutdown test')
        return
    if os.environ.get('INPUT_SCENARIO') == 'renderer-scroll':
        import json
        key('ctrl','l'); type_address('http://10.0.2.2:8765/renderer-blocking.html'); key('ret')
        wait_marker('ISOLATION ready'); wait_marker('COMPOSITOR: scene pictures=')
        time.sleep(2)
        click(120,50); wait_marker('ISOLATION begin'); move(1100,500)
        def capture(name):
            path=build/('input-'+name+'.ppm')
            execute('screendump',{'filename':str(path)})
            parts=path.read_bytes().split(b'\n',3)
            width,height=map(int,parts[1].split())
            return width,height,parts[3]
        def crop(image,left,top,right,bottom):
            width,_,pixels=image
            return b''.join(pixels[(row*width+left)*3:(row*width+right)*3] for row in range(top,bottom))
        baseline=capture('scroll-before')
        started=time.monotonic()
        execute('input-send-event',{'events':[{'type':'btn','data':{'button':'wheel-down','down':True}}]})
        execute('input-send-event',{'events':[{'type':'btn','data':{'button':'wheel-down','down':False}}]})
        deadline=started+1
        while time.monotonic()<deadline:
            changed=capture('scroll-during-javascript')
            if crop(changed,50,400,1000,700)!=crop(baseline,50,400,1000,700): break
            time.sleep(.005)
        else: raise AssertionError('Page pixels did not scroll during JavaScript')
        latency=(time.monotonic()-started)*1000
        assert 'ISOLATION end' not in healthy(), 'JavaScript completed before visual scrolling'
        assert crop(changed,25,110,715,165)==crop(baseline,25,110,715,165), 'Fixed controls moved during asynchronous scrolling'
        assert 'ISOLATION scroll 80' not in healthy(), 'DOM scroll unexpectedly ran inside the long task'
        for _ in range(12):
            for down in (True,False):
                execute('input-send-event',{'events':[{'type':'btn','data':{'button':'wheel-down','down':down}}]})
                time.sleep(.02)
        changed=capture('scroll-offscreen-during-javascript')
        assert 'ISOLATION end' not in healthy()
        assert crop(changed,900,200,901,201)==bytes((184,55,40)), 'Offscreen content was not present in the raster cache'
        assert crop(changed,25,110,715,165)==crop(baseline,25,110,715,165)
        wait_marker('ISOLATION end'); wait_marker('ISOLATION scroll 1040')
        wait_marker('acknowledgement=13')
        time.sleep(.5)
        reconciled=capture('scroll-reconciled')
        before=crop(changed,25,180,1000,700); after=crop(reconciled,25,180,1000,700)
        mismatched=sum(abs(a-b)>2 for a,b in zip(before,after))/len(before)
        assert mismatched<.02, ('Reconciliation changed the composed page',mismatched)
        result={'scroll_visible_ms':round(latency,2),'reconciliation_channel_difference_fraction':mismatched,
                'fixed_controls_unchanged':True,'offscreen_content_visible':True,'scroll_distance_px':1040,'javascript_block_ms':6000}
        (build/'renderer-scroll-results.json').write_text(json.dumps(result,indent=2))
        print('PASS: asynchronous scroll during JavaScript',json.dumps(result))
        return
    if os.environ.get('INPUT_SCENARIO') == 'renderer-shutdown':
        key('ctrl', 'l'); type_address('http://10.0.2.2:8765/renderer-infinite.html'); key('ret')
        wait_marker('RECOVERY fixture ready'); time.sleep(2)
        click(130, 40); wait_marker('RECOVERY infinite begin')
        snapshot('renderer-shutdown-infinite')
        # The outer harness sends F12 while the renderer remains in JavaScript.
        return
    if os.environ.get('INPUT_SCENARIO') == 'renderer-recovery':
        key('ctrl','l'); type_address('http://10.0.2.2:8765/renderer-infinite.html'); key('ret')
        wait_marker('RECOVERY fixture ready'); time.sleep(2)
        key('ctrl','t'); key('ctrl','1')
        assert 'TABS: selected id=1 count=2' in healthy()
        click(130,40); wait_marker('RECOVERY infinite begin')
        key('shift','esc'); wait_marker('MONITOR: opened')
        started=time.monotonic()
        execute('send-key',{'keys':[{'type':'qcode','data':'delete'}]})
        deadline=time.monotonic()+5
        while time.monotonic()<deadline and 'RENDERER: recovery launched' not in healthy(): time.sleep(.005)
        assert 'RENDERER: recovery launched' in healthy()
        print('RECOVERY termination_ms',round((time.monotonic()-started)*1000,2))
        assert 'vector=2' in healthy(), 'NMI did not interrupt the renderer'
        assert re.search(r'detached renderer jobs=[1-9]',healthy()), 'The termination test did not cover an active network request'
        key('esc'); selection_start=len(healthy()); key('ctrl','2')
        assert 'TABS: selected id=2 count=2' in healthy()[selection_start:]
        key('ctrl','1'); key('ctrl','l'); type_address('http://10.0.2.2:8765/renderer-blocking.html'); key('ret')
        wait_marker('ISOLATION ready'); time.sleep(2)
        click(280,50); wait_marker('ISOLATION click')
        if os.environ.get('QEMU_TEST_GDB_PORT'):
            import subprocess
            offset=len(healthy())
            subprocess.run(['gdb','-q','-batch','-ex','set pagination off',
                '-ex','target remote 127.0.0.1:'+os.environ['QEMU_TEST_GDB_PORT'],
                '-ex','thread 2','-ex','set $rip = 0xffff800000001000','-ex','detach'],check=True)
            deadline=time.monotonic()+20
            while time.monotonic()<deadline and 'RENDERER: recovery launched' not in healthy()[offset:]: time.sleep(.1)
            assert 'vector=14' in healthy()[offset:], healthy()[offset:]
            assert 'RENDERER: recovery launched' in healthy()[offset:]
            key('ctrl','l'); type_address('http://10.0.2.2:8765/renderer-blocking.html'); key('ret')
            deadline=time.monotonic()+20
            while time.monotonic()<deadline and 'ISOLATION ready' not in healthy()[offset:]: time.sleep(.1)
            assert 'ISOLATION ready' in healthy()[offset:]
            time.sleep(2); click(280,50)
            wait_marker('ISOLATION click', timeout=5, offset=offset)
        offset=len(healthy())
        click(120,50)
        deadline=time.monotonic()+10
        while time.monotonic()<deadline and 'ISOLATION begin' not in healthy()[offset:]: time.sleep(.02)
        assert 'ISOLATION begin' in healthy()[offset:]
        key('shift','esc'); click(120,750,False)
        deadline=time.monotonic()+10
        while time.monotonic()<deadline and 'RENDERER: recovery launched' not in healthy()[offset:]: time.sleep(.02)
        assert 'RENDERER: recovery launched' in healthy()[offset:]
        assert 'ISOLATION end' not in healthy()[offset:]
        wait_marker('BLINK: non-composited main frame created', timeout=20, offset=offset)
        wait_marker('RENDERER: recovery publication discarded slot=1', timeout=20, offset=offset)
        key('esc'); snapshot('renderer-recovery-message')
        click(260,24,False)
        deadline=time.monotonic()+20
        while time.monotonic()<deadline and 'ISOLATION ready' not in healthy()[offset:]: time.sleep(.02)
        assert 'ISOLATION ready' in healthy()[offset:]
        time.sleep(1); click(280,50)
        wait_marker('ISOLATION click', timeout=5, offset=offset)
        if os.environ.get('QEMU_TEST_GDB_PORT'):
            disassembly=subprocess.check_output(['llvm-objdump','-d','--disassemble-symbols=abort',str(build/'payload.elf')],text=True)
            invalid=re.search(r'^\s*([0-9a-f]+):.*\bud2\b',disassembly,re.M)
            assert invalid, disassembly
            offset=len(healthy())
            subprocess.run(['gdb','-q','-batch','-ex','target remote 127.0.0.1:'+os.environ['QEMU_TEST_GDB_PORT'],
                '-ex','thread 2','-ex','set $rip = 0x'+invalid.group(1),'-ex','detach'],check=True)
            deadline=time.monotonic()+20
            while time.monotonic()<deadline and 'RENDERER: recovery launched' not in healthy()[offset:]: time.sleep(.02)
            assert 'vector=6' in healthy()[offset:]
            assert 'RENDERER: recovery launched' in healthy()[offset:]
            click(260,24,False)
            deadline=time.monotonic()+20
            while time.monotonic()<deadline and 'ISOLATION ready' not in healthy()[offset:]: time.sleep(.02)
            assert 'ISOLATION ready' in healthy()[offset:]
            time.sleep(1); click(280,50)
            wait_marker('ISOLATION click', timeout=5, offset=offset)
            disassembly=subprocess.check_output(['llvm-objdump','-d','--disassemble-symbols=_ZN5angle13BreakDebuggerEv',str(build/'payload.elf')],text=True)
            breakpoint=re.search(r'^\s*([0-9a-f]+):.*\bint3\b',disassembly,re.M)
            assert breakpoint, disassembly
            offset=len(healthy())
            subprocess.run(['gdb','-q','-batch','-ex','target remote 127.0.0.1:'+os.environ['QEMU_TEST_GDB_PORT'],
                '-ex','thread 2','-ex','set $rip = 0x'+breakpoint.group(1),'-ex','detach'],check=True)
            deadline=time.monotonic()+20
            while time.monotonic()<deadline and 'RENDERER: recovery launched' not in healthy()[offset:]: time.sleep(.02)
            assert 'vector=3' in healthy()[offset:]
            assert 'RENDERER: recovery launched' in healthy()[offset:]
            click(260,24,False)
            deadline=time.monotonic()+20
            while time.monotonic()<deadline and 'ISOLATION ready' not in healthy()[offset:]: time.sleep(.02)
            assert 'ISOLATION ready' in healthy()[offset:]
            time.sleep(1); click(280,50)
            wait_marker('ISOLATION click', timeout=5, offset=offset)
        tracked=re.findall(r'reclaimed broker allocations=([0-9]+) tracked=([0-9]+)',healthy())
        assert len(tracked)==(5 if os.environ.get('QEMU_TEST_GDB_PORT') else 2) and all(freed==owned for freed,owned in tracked), tracked
        selection_start=len(healthy()); key('ctrl','2')
        assert 'TABS: selected id=2 count=2' in healthy()[selection_start:]
        key('ctrl','1'); snapshot('renderer-recovered')
        print('PASS: infinite JavaScript termination, monitor button, page fault, invalid opcode, breakpoint and renderer recovery')
        return
    if os.environ.get('INPUT_SCENARIO') == 'renderer-blocking':
        import json
        type_address('http://10.0.2.2:8765/renderer-blocking.html')
        key('ret')
        wait_marker('ISOLATION ready')
        snapshot('renderer-ready')
        click(120, 50)
        wait_marker('ISOLATION begin')
        started=time.monotonic()
        # The native address field must paint while the renderer is still in JavaScript.
        click(600,24,False)
        def address_pixels(name):
            path=build/('input-'+name+'.ppm')
            execute('screendump',{'filename':str(path)})
            parts=path.read_bytes().split(b'\n',3)
            width=int(parts[1].split()[0]); data=parts[3]
            return b''.join(data[(y*width+300)*3:(y*width+420)*3] for y in range(10,38))
        before=address_pixels('renderer-address-selected')
        pressed=time.monotonic()
        execute('send-key',{'keys':[{'type':'qcode','data':'x'}]})
        deadline=pressed+1
        while address_pixels('renderer-address-during-javascript')==before:
            if time.monotonic()>deadline: raise AssertionError('Native address did not paint within one second')
            time.sleep(.005)
        painted=time.monotonic()
        assert 'ISOLATION end' not in healthy(), 'Native UI observation occurred after JavaScript returned'
        results={'native_address_observed_while_javascript_running':True,
                 'native_key_to_paint_ms':round((painted-pressed)*1000,2),
                 'address_observation_ms':round((painted-started)*1000,2)}
        key('esc')
        click(280,50)
        click(600,50)
        key('y')
        wheel('down')
        wait_marker('ISOLATION end')
        wait_marker('ISOLATION click')
        wait_marker('ISOLATION key y')
        wait_marker('ISOLATION scroll ')
        wait_marker('ISOLATION network delivered')
        interval=healthy().split('ISOLATION begin',1)[1].split('ISOLATION end',1)[0]
        assert 'NETWORK_SERVICE: completed status=0 bytes=9' in interval, 'Network service waited for JavaScript'
        results['network_completed_during_javascript']=True
        snapshot('renderer-after-javascript')
        results['page_events_delivered_after_javascript']=True
        (build/'renderer-blocking-results.json').write_text(json.dumps(results,indent=2))
        print('PASS: native UI remained active and queued page input survived a six-second JavaScript task')
        return
    if os.environ.get('INPUT_SCENARIO') == 'chrome-identity':
        for hostname in ('google.com', 'google.com.evil.test', '10.0.2.2'):
            key('ctrl', 'l')
            type_address('https://' + hostname + ':8766/chrome-identity')
            key('ret')
            wait_marker('CHROME_IDENTITY PASS ' + hostname + ' ')
        return
    if os.environ.get('INPUT_SCENARIO') == 'parallel':
        key('ctrl', 'l')
        type_address('http://10.0.2.2:8765/parallel.html')
        key('ret')
        wait_marker('PARALLEL_READY')
        click(60, 18)
        wait_marker('PARALLEL_INPUT click')
        click(220, 18)
        key('x')
        wait_marker('PARALLEL_INPUT key')
        assert 'PARALLEL_DONE slow ' not in healthy(), 'Input waited for the slow response'
        wait_marker('PARALLEL_DONE fast ')
        wait_marker('PARALLEL_DONE slow ')
        values = {name: int(value) for name, value in re.findall(r'PARALLEL_DONE (fast|slow) (\d+)', healthy())}
        assert values['fast'] < 1500 and values['slow'] >= 3000, values
        return
    if os.environ.get('INPUT_SCENARIO') == 'client-hints':
        for scheme, port in [('https', '8766'), ('http', '8765')]:
            key('ctrl', 'l')
            type_address(scheme + '://10.0.2.2:' + port + '/hints')
            key('ret')
            wait_marker('CLIENT_HINTS_TEST ' + scheme.upper() + ' PASS')
        key('ctrl', 'l')
        type_address('https://10.0.2.2:8766/hints-policy')
        key('ret')
        wait_marker('CLIENT_HINTS_TEST POLICY PASS')
        print('PASS: secure Client Hints, insecure suppression, and Permissions Policy verified.')
        return
    if os.environ.get('INPUT_SCENARIO') == 'identity':
        key('ctrl', 'l')
        type_address('http://10.0.2.2:8765/identity')
        key('ret')
        wait_marker('USER_AGENT_TEST PASS ')
        print('HTTP document, subresource, and navigator User-Agent match.')
        return
    if os.environ.get('INPUT_SCENARIO') == 'tabs':
        def fresh(marker, offset):
            deadline = time.monotonic() + 30
            while marker not in healthy()[offset:]:
                if time.monotonic() > deadline: raise AssertionError(marker + '\n' + healthy()[offset:])
                time.sleep(.05)
        type_address('http://10.0.2.2:8765/tabs.html')
        key('ret')
        wait_marker('TABS_TEST external script loaded')
        click(100, 125)
        key('x')
        click(100, 215)
        wait_marker('TABS_TEST count=1 text=x')
        move(800, 500)
        wheel('down', 3)
        before_scroll = snapshot('tabs-scroll-before')
        key('ctrl', 't')
        wait_marker('TABS: created id=2 count=2')
        type_address('http://10.0.2.2:8765/index.html')
        key('ret')
        wait_marker('INPUT_TEST first document loaded')
        click(520, 125)
        wait_marker('INPUT_TEST button activated')
        key('ctrl', 'shift', 'tab')
        move(800, 500)
        after_scroll = snapshot('tabs-scroll-after')
        offset = before_scroll[0] * 80 * 3
        assert before_scroll[2][offset:] == after_scroll[2][offset:], 'Tab scroll position or page pixels changed'
        key('ctrl', 'home')
        click(100, 215)
        wait_marker('TABS_TEST count=2 text=x')
        snapshot('tabs-preserved')
        wait_marker('TABS_TEST visibility=hidden')
        wait_marker('TABS_TEST visibility=visible')
        # Closing the background frame must not remove the surviving frame's loader.
        click(425, 64, False)
        wait_marker('TABS: closed id=2 remaining=1')
        offset = len(healthy())
        click(260, 24, False)
        fresh('TABS_TEST external script loaded', offset)
        click(100, 292)
        wait_marker('INPUT_TEST second document loaded')
        offset = len(healthy())
        click(180, 24, False)
        fresh('TABS_TEST external script loaded', offset)
        offset = len(healthy())
        click(220, 24, False)
        fresh('INPUT_TEST second document loaded', offset)
        offset = len(healthy())
        click(180, 24, False)
        fresh('TABS_TEST external script loaded', offset)
        click(100, 352)
        wait_marker('TABS: created id=3 count=2')
        wait_marker('INPUT_TEST third document loaded')
        key('shift', 'esc')
        wait_marker('MONITOR: opened tabs=2')
        expected = 0 if 'RENDERER: AP process launched' in healthy() else min(4, int(os.environ.get('QEMU_TEST_CPUS', '1')) - 1)
        wait_marker('MONITOR: workers=' + str(expected))
        snapshot('tabs-monitor')
        key('esc')
        wait_marker('MONITOR: closed tabs=2')
        key('ctrl', 'w')
        wait_marker('TABS: closed id=3 remaining=1')
        key('ctrl', 't')
        key('ctrl', 't')
        key('ctrl', 't')
        key('ctrl', 't')
        wait_marker('TABS: creation refused by tab or tracked-heap limit')
        key('ctrl', '1')
        click(100, 215)
        wait_marker('TABS_TEST count=1 text=')
        key('ctrl', '9')
        for _ in range(4): key('ctrl', 'w')
        wait_marker('TABS: created id=7 count=1')
        click(245, 64, False)
        wait_marker('TABS: created id=8 count=2')
        click(1250, 64, False)
        snapshot('tabs-monitor-blank')
        key('esc')
        key('ctrl', 't')
        type_address('http://10.0.2.2:8765/slow')
        key('ret')
        offset = len(healthy())
        key('ctrl', 'w')
        fresh('TABS: closed id=9 remaining=2', offset)
        time.sleep(3)
        healthy()
        print('PASS: tabs preserve DOM and form state, isolate history, retain resource loading after close, open target blank, enforce limits and show actual process workers', flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'settings-reset':
        key('ctrl', 'comma')
        wait_marker('UI: toolbar settings started')
        click(420, 52)
        for character in 'reset': key(character)
        snapshot('settings-reset-confirm')
        key('tab')
        key('spc')
        key('tab')
        key('ret')
        wait_marker('SETTINGS: save status=0 theme=0 font=16 javascript=1 images=1 startup=0')
        snapshot('settings-reset')
        click(100, 306)
        snapshot('settings-about-final')
        click(180, 24, False)
        snapshot('settings-back-final')
        click(220, 24, False)
        snapshot('settings-forward-final')
        print('PASS: keyboard settings shortcut, search, confirmed reset and internal history',flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'settings-persist':
        wait_marker('SETTINGS: loaded theme=1 font=20 javascript=0 images=0 startup=1')
        snapshot('settings-persisted')
        key('ctrl', 'l')
        type_address('http://10.0.2.2:8765/settings.html')
        key('ret')
        wait_marker('SETTINGS: apply internal=0 font=20 javascript=0 images=0')
        wait_marker('url=http://10.0.2.2:8765/settings.html')
        disabled = snapshot('settings-disabled')
        width, _, data = disabled
        def rgb(x,y): return data[(y*width+x)*3:(y*width+x)*3+3]
        assert rgb(100,100) == bytes([224,0,0]), rgb(100,100)
        assert rgb(400,100) == bytes([255,255,255]), rgb(400,100)
        assert 'SETTINGS_FIXTURE javascript enabled' not in healthy()
        offset = len(healthy())
        click(200, 115)
        time.sleep(1)
        assert 'SETTINGS: save' not in healthy()[offset:]
        wait_marker('SETTINGS: rejected untrusted navigation')
        click(1250, 24, False)
        snapshot('settings-reopened')
        click(1090, 510)
        click(1090, 595)
        key('ctrl', 'l')
        type_address('http://10.0.2.2:8765/settings.html')
        key('ret')
        wait_marker('SETTINGS_FIXTURE javascript enabled font=20px')
        enabled = snapshot('settings-enabled')
        width, _, data = enabled
        assert rgb(100,100) == bytes([0,192,0]), rgb(100,100)
        assert rgb(400,100) == bytes([0,192,0]), rgb(400,100)
        click(1250, 24, False)
        click(100, 306)
        snapshot('settings-about')
        print('PASS: firmware restart persistence; actual JavaScript, image and font behavior; external settings blocked',flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'settings':
        click(1250, 24, False)
        light = snapshot('settings-light')
        click(1160, 272)
        wait_marker('SETTINGS: save status=0 theme=1')
        dark = snapshot('settings-dark')
        assert pixel(light, 600, 160) > 600 and pixel(dark, 600, 160) < 160
        click(1160, 358)
        wait_marker('theme=1 font=20')
        click(1160, 510)
        wait_marker('theme=1 font=20 javascript=0')
        click(1160, 595)
        wait_marker('theme=1 font=20 javascript=0 images=0')
        click(420, 52)
        for character in 'startup': key(character)
        snapshot('settings-search')
        click(1160, 272)
        wait_marker('theme=1 font=20 javascript=0 images=0 startup=1')
        click(100, 306)
        snapshot('settings-about')
        click(180, 24, False)
        snapshot('settings-back')
        print('PASS: theme pixels, font, JavaScript, images, search, startup, About and back', flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'settings-preview':
        click(1250, 24, False)
        snapshot('settings-light')
        return
    if os.environ.get('INPUT_SCENARIO') == 'heavy-fixture':
        import json
        type_address('http://10.0.2.2:8765/heavy.html')
        key('ret')
        wait_marker('HEAVY ready')
        samples=[]
        def measured(action, marker):
            offset=len(healthy())
            started=time.monotonic()
            action()
            deadline=started+15
            while marker not in healthy()[offset:]:
                if time.monotonic()>deadline: raise TimeoutError(marker+'\n'+healthy())
                time.sleep(.005)
            return (time.monotonic()-started)*1000
        click(130,45)
        for iteration in range(5):
            keyboard=measured(lambda: execute('send-key', {'keys':[{'type':'qcode','data':'x'}]}), 'HEAVY text=')
            move(420,45)
            def press():
                execute('input-send-event', {'events':[{'type':'btn','data':{'button':'left','down':True}}]})
                time.sleep(.03)
                execute('input-send-event', {'events':[{'type':'btn','data':{'button':'left','down':False}}]})
            button=measured(press, 'HEAVY clicked')
            move(700,400)
            def scroll():
                execute('input-send-event', {'events':[{'type':'btn','data':{'button':'wheel-down','down':True}}]})
                execute('input-send-event', {'events':[{'type':'btn','data':{'button':'wheel-down','down':False}}]})
            scrolling=measured(scroll,'HEAVY scroll=')
            samples.append({'keyboard_ms':keyboard,'click_ms':button,'scroll_ms':scrolling})
            assert 'HEAVY finished' not in healthy(), 'Load ended before input measurements'
            click(130,45)
        # Affine exponentiation independently checks the repeated JavaScript LCG.
        def expected_checksum(iterations):
            multiplier, increment = 1664525, 1013904223
            result_multiplier, result_increment = 1, 0
            count = iterations * 500000
            mask = (1 << 32) - 1
            while count:
                if count & 1:
                    result_multiplier, result_increment = (multiplier * result_multiplier) & mask, (multiplier * result_increment + increment) & mask
                multiplier, increment = (multiplier * multiplier) & mask, (multiplier * increment + increment) & mask
                count >>= 1
            return result_increment if result_increment < (1 << 31) else result_increment - (1 << 32)
        checksums = re.findall(r'HEAVY work=(\d+) duration=\d+ checksum=(-?\d+)', healthy())
        assert checksums
        for iteration, checksum in checksums:
            assert int(checksum) == expected_checksum(int(iteration)), (iteration, checksum)
        snapshot('heavy-fixture')
        (build/'heavy-fixture-samples.json').write_text(json.dumps(samples,indent=2))
        print('PASS: real DOM keyboard, click and wheel during repeated CPU and layout work '+json.dumps(samples),flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') in ('heavy-site', 'tabs-site'):
        import json
        type_address('https://zeroday0619.dev')
        key('ret')
        deadline = time.monotonic() + 180
        while 'BLINK: resource 200 type=' not in healthy():
            if time.monotonic() > deadline: raise TimeoutError(healthy())
            time.sleep(.05)
        samples = []
        for index in range(12):
            start = len(healthy())
            loading_at_start = 'BLINK: main document load finished' not in healthy().split('UI: address navigation started')[-1]
            began = time.monotonic()
            # The real page receives these events while resources are arriving.
            move(800, 500)
            wheel('down')
            key('pgdn')
            click(1170, 38)
            key('spc')
            key('ctrl', 'l')
            key('x')
            image = snapshot('heavy-loading-' + str(index))
            key('esc')
            samples.append({'iteration': index, 'loading_at_start': loading_at_start, 'elapsed_ms': (time.monotonic()-began)*1000,
                'loading_finished': 'BLINK: main document load finished' in healthy().split('UI: address navigation started')[-1],
                'scroll_records': re.findall(r'BLINK: scroll offset [^\n]+', healthy()[start:])})
            if samples[-1]['loading_finished']: break
        (build / 'heavy-site-samples.json').write_text(json.dumps(samples, indent=2))
        deadline = time.monotonic() + 180
        while 'BLINK: main document load finished' not in healthy().split('UI: address navigation started')[-1]:
            if time.monotonic() > deadline: raise TimeoutError(healthy())
            time.sleep(.2)
        key('ctrl', 'home')
        original = snapshot('heavy-loaded')
        def changed_theme(previous, name, started):
            deadline = started + 5
            while time.monotonic() < deadline:
                path = build / ('input-' + name + '.ppm')
                execute('screendump', {'filename':str(path)})
                parts = path.read_bytes().split(b'\n',3)
                width,height = map(int,parts[1].split())
                current = (width,height,parts[3])
                if abs(pixel(current,800,120)-pixel(previous,800,120)) > 200:
                    return current,(time.monotonic()-started)*1000
                healthy()
                time.sleep(.02)
            raise AssertionError('Theme did not visibly change within five seconds')
        move(1170,38)
        started=time.monotonic()
        execute('input-send-event', {'events':[{'type':'btn','data':{'button':'left','down':True}}]})
        time.sleep(.03)
        execute('input-send-event', {'events':[{'type':'btn','data':{'button':'left','down':False}}]})
        changed,click_ms = changed_theme(original,'heavy-click',started)
        started=time.monotonic()
        execute('send-key',{'keys':[{'type':'qcode','data':'spc'}]})
        restored,keyboard_ms = changed_theme(changed,'heavy-keyboard',started)
        print('MEASURE: real site visible theme response '+json.dumps({'click_ms':click_ms,'keyboard_ms':keyboard_ms}),flush=True)
        move(800,500)
        start = len(healthy())
        wheel('down',3)
        snapshot('heavy-scroll')
        assert re.search(r'BLINK: scroll offset [\d.-]+ [1-9]', healthy()[start:]), 'Real site did not scroll'
        if os.environ.get('INPUT_SCENARIO') == 'tabs-site':
            key('ctrl', 't')
            wait_marker('TABS: created id=2 count=2')
            key('ctrl', 'comma')
            wait_marker('UI: toolbar settings started')
            snapshot('tabs-site-settings')
            key('ctrl', 'shift', 'tab')
            key('ctrl', 'home')
            previous = snapshot('tabs-site-restored')
            started = time.monotonic()
            click(1170, 38)
            changed_theme(previous, 'tabs-site-interactive', started)
            key('shift', 'esc')
            wait_marker('MONITOR: opened tabs=2')
            snapshot('tabs-site-monitor')
            key('esc')
            key('ctrl', 'w')
            wait_marker('TABS: closed id=1 remaining=1')
            snapshot('tabs-site-survivor')
            print('PASS: real website survives tab switch with interactive JavaScript; settings tab survives website close', flush=True)
        print('PASS: real site loading input exercised; post-load theme click, Space and wheel verified', flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') in ('responsive-loading', 'responsive-navigation', 'responsive-cancel'):
        from pathlib import Path
        origin = os.environ.get('TEST_ORIGIN', 'http://10.0.2.2:8765')
        release = Path(os.environ['NETWORK_RELEASE_FILE'])
        release.unlink(missing_ok=True)
        server_log = Path(os.environ['NETWORK_SERVER_LOG'])
        previous = server_log.read_text().count('HELD_REQUEST_STARTED')
        type_address(origin + '/loading.html')
        key('ret')
        wait_marker('INPUT_TEST first document loaded')
        scenario = os.environ['INPUT_SCENARIO']
        if scenario == 'responsive-loading':
            click(520, 245)
            wait_marker('LOADING started')
        else:
            key('ctrl', 'l')
            type_address(origin + '/held-data')
            key('ret')
        deadline = time.monotonic() + 20
        while server_log.read_text().count('HELD_REQUEST_STARTED') == previous:
            assert time.monotonic() < deadline
            healthy()
            time.sleep(.01)
        if scenario == 'responsive-cancel':
            key('ctrl', 'l')
            type_address('about:blank')
            key('ret')
            deadline = time.monotonic() + 3
            while not re.findall(r'HISTORY: [^\n]+url=(\S+)', healthy())[-1] == 'about:blank':
                assert time.monotonic() < deadline
                time.sleep(.01)
            release.touch()
            time.sleep(2)
            assert re.findall(r'HISTORY: [^\n]+url=(\S+)', healthy())[-1] == 'about:blank'
            print('PASS: newer navigation supersedes a suspended network request without a stale commit', flush=True)
            return
        started = time.monotonic()
        latencies = {}
        def observe(name, marker, send):
            beginning = time.monotonic()
            send()
            while not re.search(marker, healthy()):
                assert time.monotonic() - beginning < .5, name + ' exceeded the 500 ms test bound'
                time.sleep(.005)
            latencies[name] = (time.monotonic() - beginning) * 1000
            assert latencies[name] < 500, latencies
        def button_event(button):
            for down in (True, False):
                execute('input-send-event', {'events': [{'type':'btn','data':{'button':button,'down':down}}]})
                time.sleep(.02)
        click(150, 120)
        observe('keyboard_ms', 'INPUT_TEST text=x', lambda: execute('send-key', {'keys':[{'type':'qcode','data':'x'}], 'hold-time':20}))
        move(520, 120)
        observe('click_ms', 'INPUT_TEST button activated', lambda: button_event('left'))
        move(700, 600)
        observe('scroll_ms', r'INPUT_TEST document scroll=[1-9]', lambda: button_event('wheel-down'))
        import json
        print('INPUT_LATENCY ' + json.dumps(latencies), flush=True)
        assert 'LOADING completed' not in healthy()
        assert 'LOADING failed' not in healthy()
        assert not release.exists()
        snapshot(scenario)
        print('PASS: keyboard, button and scroll reached Blink before the server released the pending response; elapsed=' + str(time.monotonic() - started), flush=True)
        release.touch()
        if scenario == 'responsive-loading': wait_marker('LOADING completed')
        else: wait_marker('url=' + origin + '/held-data')
        return
    if os.environ.get('INPUT_SCENARIO') == 'pointer-network':
        move(200, 200, False)
        from pathlib import Path
        server_log = Path(os.environ['NETWORK_SERVER_LOG'])
        previous_requests = server_log.read_text().count('SLOW_REQUEST_STARTED')
        type_address('http://10.0.2.2:8765/slow')
        key('ret')
        deadline = time.monotonic() + 20
        while server_log.read_text().count('SLOW_REQUEST_STARTED') == previous_requests:
            assert time.monotonic() < deadline, 'Server did not receive the delayed request'
            time.sleep(.01)
        execute('input-send-event', {'events': [{'type':'rel','data':{'axis':'x','value':800}}]})
        time.sleep(.2)
        path = build / 'input-pointer-network.ppm'
        execute('screendump', {'filename': str(path)})
        data = path.read_bytes().split(b'\n', 3)
        width, height = map(int, data[1].split())
        image = (width, height, data[3])
        assert 'url=http://10.0.2.2:8765/slow' not in healthy(), 'Response completed before cursor measurement'
        responsive = pixel(image, 1000, 200) == 0
        print('POINTER_NETWORK responsive_during_wait=' + str(responsive), flush=True)
        if os.environ.get('EXPECT_RESPONSIVE') == '1': assert responsive
        wait_marker('url=http://10.0.2.2:8765/slow')
        return
    if os.environ.get('INPUT_SCENARIO') == 'pointer-performance':
        import json
        move(200, 200, False)
        time.sleep(1)
        measurements = []
        for sweep in range(4):
            direction = 1 if sweep % 2 == 0 else -1
            started = time.monotonic()
            for index in range(160):
                execute('input-send-event', {'events': [{'type': 'rel', 'data': {'axis': 'x', 'value': direction * 5}}]})
                time.sleep(.005)
            target = 1000 if direction == 1 else 200
            settled = False
            for attempt in range(100):
                path = build / 'pointer-performance.ppm'
                execute('screendump', {'filename': str(path)})
                data = path.read_bytes().split(b'\n', 3)
                width, height = map(int, data[1].split())
                pixels = data[3]
                offset = (200 * width + target) * 3
                if pixels[offset:offset + 3] == b'\0\0\0':
                    row = pixels[200 * width * 3:201 * width * 3]
                    black = [x for x in range(width) if row[x * 3:x * 3 + 3] == b'\0\0\0']
                    assert black == [target], 'Cursor left stale pixels: ' + str(black)
                    settled = True
                    break
                time.sleep(.01)
            measurements.append({'sweep_seconds': time.monotonic() - started, 'settled': settled})
            assert settled, measurements
        (build / 'pointer-performance.json').write_text(json.dumps(measurements, indent=2))
        print('POINTER_PERFORMANCE ' + json.dumps(measurements), flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'http-errors':
        for status in (404, 429, 500):
            key('ctrl', 'l')
            type_address('http://10.0.2.2:8765/error' + str(status))
            key('ret')
            wait_marker('HTTP_ERROR_TEST ' + str(status))
            snapshot('http-error-' + str(status))
            assert 'navigation HTTP status=' + str(status) in healthy()
        print('PASS: HTTP 404, 429 and 500 bodies reach Blink and execute their document scripts.', flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'google':
        type_address('https://google.com')
        key('ret')
        for index in range(35):
            time.sleep(1)
            if index % 10 == 0:
                execute('screendump', {'filename': str(build / 'input-google-home.ppm')})
            healthy()
        snapshot('google-home')
        if os.environ.get('QEMU_TEST_GDB_PORT'):
            print('DEBUG: Google loaded; holding input for 60 seconds for GDB inspection.', flush=True)
            time.sleep(60)
        click(500, 352, False)
        wait_marker('BLINK: focus tag=TEXTAREA')
        for character in 'uefi specification':
            key('spc' if character == ' ' else character)
        snapshot('google-query')
        key('ret')
        for index in range(35):
            time.sleep(1)
            if index % 10 == 0:
                execute('screendump', {'filename': str(build / 'input-google-results.ppm')})
            healthy()
        snapshot('google-results')
        text = healthy()
        assert re.search(r'url=https://www.google.com/search\?[^\n]*q=uefi', text), text
        if 'url=https://www.google.com/sorry/' in text:
            assert 'navigation HTTP status=429' in text, text
            print('BLOCKED: Google returned HTTP 429 and its traffic verification page; search results are not verified.', flush=True)
        else:
            print('Search submission captured; visual inspection is required to confirm results.', flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'redirect':
        import json
        type_address('http://10.0.2.2:8765/start')
        key('ret')
        wait_marker('REDIRECT_TEST ')
        snapshot('redirect-final')
        records = re.findall(r'REDIRECT_TEST (\{[^\n]+\})', healthy())
        result = json.loads(records[-1])
        assert result['count'] == 2, result
        assert 0 <= result['start'] <= result['end'] <= result['fetch'], result
        assert result['fetch'] > 0, result
        assert 'url=http://10.0.2.2:8765/final' in healthy()
        print('PASS: 302/307 redirect chain commits final URL with ordered navigation timings', flush=True)
        click(260, 24, False)
        wait_marker('UI: toolbar reload started')
        snapshot('redirect-reload')
        print('PASS: redirected document reload completes', flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'history':
        def history():
            records = re.findall(r'HISTORY: index=(\d+) count=(\d+) back=(\d) forward=(\d) url=(\S+)', healthy())
            assert records, 'No committed history record'
            row = records[-1]
            return tuple(map(int, row[:4])) + (row[4],)
        def button(index):
            click(180 + index * 40, 24, False)
            time.sleep(1)
        def expect_url(suffix):
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if history()[4].endswith(suffix): return history()
                time.sleep(.2)
            raise AssertionError('Unexpected current history entry: ' + str(history()))
        assert history()[2:4] == (0, 0)
        snapshot('history-empty')
        button(0)
        button(1)
        assert history()[2:4] == (0, 0)
        type_address('http://10.0.2.2:8765/index.html')
        key('ret')
        wait_marker('INPUT_TEST first document loaded')
        click(180, 224)
        wait_marker('INPUT_TEST second document loaded')
        current = expect_url('/second.html')
        count = current[1]
        button(0)
        assert expect_url('/index.html')[3] == 1
        button(1)
        assert expect_url('/second.html')[1] == count
        loads = healthy().count('INPUT_TEST second document loaded')
        button(2)
        deadline = time.monotonic() + 30
        while healthy().count('INPUT_TEST second document loaded') <= loads and time.monotonic() < deadline: time.sleep(.2)
        assert healthy().count('INPUT_TEST second document loaded') > loads, 'Reload did not load the document again'
        assert history()[1] == count, 'Reload appended a history entry'
        snapshot('history-reload')
        print('PASS: real toolbar back, forward and reload use committed history without duplicate reload entries', flush=True)
        button(0)
        expect_url('/index.html')
        key('ctrl', 'l')
        type_address('http://10.0.2.2:8765/third.html')
        key('ret')
        wait_marker('INPUT_TEST third document loaded')
        assert expect_url('/third.html')[3] == 0
        previous = history()
        button(1)
        assert history() == previous, 'Disabled forward button changed history'
        button(0)
        expect_url('/index.html')
        click(180, 395)
        expect_url('/index.html#bottom')
        snapshot('history-fragment')
        button(0)
        expect_url('/index.html')
        button(1)
        expect_url('/index.html#bottom')
        snapshot('history-fragment-forward')
        print('PASS: new navigation truncates forward history; fragment back/forward keeps the same document', flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'address':
        assert 'BLINK: resource 200' not in healthy(), 'Blank startup fetched external resources'
        snapshot('blank-start')
        type_address('http://10.0.2.2:8765/index.html')
        key('ret')
        wait_marker('INPUT_TEST first document loaded')
        click(500, 128)
        wait_marker('INPUT_TEST button activated')
        snapshot('address-loaded')
        key('ctrl', 'l')
        type_address('http://10.0.2.2:8765/second.html')
        key('ret')
        wait_marker('INPUT_TEST second document loaded')
        snapshot('address-second')
        key('tab')
        key('ret')
        wait_marker('INPUT_TEST third document loaded')
        snapshot('address-link')
        key('ctrl', 'l')
        type_address('about:blank')
        key('ret')
        snapshot('address-return-blank')
        print('PASS: blank startup, typed URL, page click, Ctrl+L replacement, link navigation and return to blank', flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') in ('wheel-fixture', 'form'):
        if 'url=http://10.0.2.2:8765/index.html' not in healthy():
            type_address('http://10.0.2.2:8765/index.html')
            key('ret')
            wait_marker('INPUT_TEST first document loaded')
    if os.environ.get('INPUT_SCENARIO') == 'wheel-fixture':
        assert 'wheel=1' in healthy(), 'Firmware does not advertise wheel input'
        move(200, 470)
        start = len(healthy())
        wheel('down', 3)
        wait_marker('INPUT_TEST wheel cancelled')
        snapshot('wheel-cancelled')
        assert not re.search(r'INPUT_TEST document scroll=[1-9]', healthy()[start:]), 'Cancelled wheel scrolled the document'
        print('PASS: preventDefault cancels actual device wheel scrolling', flush=True)
        move(950, 200)
        start = len(healthy())
        wheel('down', 3)
        snapshot('wheel-nested')
        positions = re.findall(r'INPUT_TEST nested scroll=([0-9.]+)', healthy()[start:])
        assert positions and max(map(float, positions)) > 0, 'Nested element did not scroll'
        assert not re.search(r'INPUT_TEST document scroll=[1-9]', healthy()[start:]), 'Nested wheel unexpectedly scrolled the document'
        print('PASS: wheel hit testing scrolls the nested element', flush=True)
        move(700, 600)
        start = len(healthy())
        wheel('down', 3)
        snapshot('wheel-document-down')
        positions = re.findall(r'INPUT_TEST document scroll=([0-9.]+)', healthy()[start:])
        assert positions and float(positions[-1]) > 0, 'Wheel down did not scroll document'
        previous = float(positions[-1])
        start = len(healthy())
        wheel('up', 2)
        snapshot('wheel-document-up')
        positions = re.findall(r'INPUT_TEST document scroll=([0-9.]+)', healthy()[start:])
        assert positions and float(positions[-1]) < previous, 'Wheel up did not reverse scrolling'
        print('PASS: physical wheel scrolls document in both directions', flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'form':
        click(150, 120)
        for character in 'abcq': key(character)
        key('backspace')
        wait_marker('INPUT_TEST text=abc')
        key('ctrl', 'a')
        key('shift', 'a')
        wait_marker('INPUT_TEST text=A')
        key('minus')
        key('shift', '1')
        wait_marker('INPUT_TEST text=A-!')
        key('esc')
        wait_marker('INPUT_TEST key=Escape')
        key('tab')
        key('spc')
        wait_marker('INPUT_TEST button activated')
        snapshot('text-keyboard')
        print('PASS: typed text, Backspace, Ctrl+A, Shift+A, Escape, Tab and Space reached the real DOM', flush=True)
        click(180, 224)
        wait_marker('INPUT_TEST second document loaded')
        snapshot('second-document')
        print('PASS: clicked link fetched and committed a new document', flush=True)
        key('tab')
        key('ret')
        wait_marker('INPUT_TEST third document loaded')
        snapshot('new-window-document')
        print('PASS: Tab and Enter activated a target=_blank link in a new tab', flush=True)
        return
    assert 'relative=1' in healthy(), 'No relative UEFI pointer was discovered'
    original = snapshot('initial')
    click(1170, 38)
    dark = snapshot('dark-click')
    assert pixel(dark, 800, 120) + 200 < pixel(original, 800, 120), 'Mouse click did not activate dark theme'
    print('PASS: real mouse click activates the site dark-theme button', flush=True)
    key('spc')
    light = snapshot('light-keyboard')
    assert pixel(light, 800, 120) > pixel(dark, 800, 120) + 200, 'Space did not activate focused button'
    print('PASS: keyboard Space activates the focused site button', flush=True)
    click(1100, 38)
    english = snapshot('english-click')
    assert english[2] != light[2], 'Language button did not change the framebuffer'
    print('CHECK: language-button screenshot captured for visual inspection', flush=True)
    if os.environ.get('INPUT_SCENARIO') == 'site-no-wheel':
        print('SKIP: wheel is blocked by the installed firmware driver', flush=True)
        key('pgdn')
        snapshot('keyboard-page-down')
        positions = re.findall(r'BLINK: scroll offset [\d.-]+ ([\d.-]+)', healthy())
        assert positions and float(positions[-1]) > 0, 'PageDown did not scroll the document'
        print('PASS: PageDown scrolls the real document', flush=True)
    else:
        move(800, 500)
        start = len(healthy())
        for _ in range(5):
            for down in (True, False):
                execute('input-send-event', {'events':[{'type':'btn','data':{'button':'wheel-down','down':down}}]})
                time.sleep(.15)
        snapshot('wheel-scroll')
        positions = re.findall(r'BLINK: scroll offset [\d.-]+ ([\d.-]+)', healthy()[start:])
        assert positions and max(map(float, positions)) > 0, 'Wheel input did not scroll the document'
        print('PASS: physical wheel reports scroll the Blink document', flush=True)
    key('ctrl', 'home')
    snapshot('keyboard-home')
    click(200, 444)
    time.sleep(3)
    snapshot('project-link')
    positions = re.findall(r'BLINK: scroll offset [\d.-]+ ([\d.-]+)', healthy())
    assert positions and float(positions[-1]) > 400, 'Projects anchor did not move to the target section'
    print('PASS: site Projects anchor changes the actual scroll offset', flush=True)
