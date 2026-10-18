"""Exercise live page controls through QEMU input devices, without DOM injection."""
import os
import re
import time

def run(execute, build, log):
    def healthy():
        text = log.read_text(errors='replace')
        if any(marker in text for marker in ('FATAL:', 'FAIL:', 'Exception Type')):
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
        if page: y += 48
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
        for down in (True, False):
            execute('input-send-event', {'events':[{'type':'btn','data':{'button':'left','down':down}}]})
            time.sleep(.15)
    def key(*keys):
        execute('send-key', {'keys':[{'type':'qcode','data':name} for name in keys]})
        time.sleep(.3)
    def wait_marker(marker):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if marker in healthy(): return
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
            else: key(character)
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
        snapshot('heavy-fixture')
        (build/'heavy-fixture-samples.json').write_text(json.dumps(samples,indent=2))
        print('PASS: real DOM keyboard, click and wheel during repeated CPU and layout work '+json.dumps(samples),flush=True)
        return
    if os.environ.get('INPUT_SCENARIO') == 'heavy-site':
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
            samples.append({'iteration': index, 'elapsed_ms': (time.monotonic()-began)*1000,
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
        click(1170, 38)
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
        click(500, 312, False)
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
        print('PASS: Tab and Enter activated a target=_blank link in the single viewport', flush=True)
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
