"""Exercise affine paint, clip and input coordinates through QEMU during blocked JavaScript."""
import json
import re
import time


def run(execute, build, healthy, move, click, key, wait_marker, type_address, horizontal=False):
    key('ctrl', 'l'); type_address('http://10.0.2.2:8765/scroll-affine.html'); key('ret')
    wait_marker('AFFINE ready'); time.sleep(3)

    def capture(name):
        destination = build / ('input-affine-' + name + '.ppm')
        execute('screendump', {'filename': str(destination)})
        parts = destination.read_bytes().split(b'\n', 3)
        return int(parts[1].split()[0]), parts[3]

    def crop(image, rectangle):
        width, pixels = image
        left, top, right, bottom = rectangle
        return b''.join(pixels[(y * width + left) * 3:(y * width + right) * 3] for y in range(top, bottom))

    def point(x, y):
        return round(400 + 1.2 * x - .2 * y), round(180 + .3 * x + .8 * y)

    def color(image, x, y):
        screen_x, screen_y = point(x, y)
        return crop(image, (screen_x, screen_y + 80, screen_x + 1, screen_y + 81))

    def wheel():
        for down in (True, False):
            execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-down', 'down': down}}]})

    def press(down):
        execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
        time.sleep(.15)

    if horizontal:
        click(100, 40); wait_marker('AFFINE begin')
        move(*point(100, 290)); time.sleep(.15)
        hovered = capture('horizontal-hover')
        assert color(hovered, 94, 290) == bytes((221, 51, 119)), 'Horizontal hover was not applied'
        press(True)
        pressed = capture('horizontal-pressed')
        assert color(pressed, 94, 290) == bytes((119, 51, 204)), 'Horizontal pressed state was not applied'
        move(*point(220, 290)); time.sleep(.15); press(False)

        def last_position():
            positions = re.findall(r'COMPOSITOR: async (?:drag|repeat) sequence=\d+ position=\d+ element=\d+ x=(\d+)', healthy())
            assert positions, 'No horizontal scroll position was published'
            return int(positions[-1])

        dragged = last_position()
        assert dragged > 200, ('Horizontal drag', dragged)
        click(*point(370, 290)); arrow = last_position()
        assert arrow > dragged, ('Forward arrow did not advance', dragged, arrow)
        click(*point(50, 290)); track = last_position()
        assert track < arrow, ('Backward track did not page', arrow, track)
        move(950, 600); busy = capture('horizontal-busy')
        assert 'AFFINE end' not in healthy(), 'Horizontal controls ran after JavaScript resumed'
        wait_marker('AFFINE end'); wait_marker('AFFINE nested ' + str(track) + ' 0')
        time.sleep(3); reconciled = capture('horizontal-reconciled')
        left = crop(busy, (300, 240, 930, 650)); right = crop(reconciled, (300, 240, 930, 650))
        mismatch = sum(abs(a - b) > 2 for a, b in zip(left, right)) / len(left)
        assert mismatch < .02, ('Horizontal reconciliation differs', mismatch)
        result = {'javascript_block_ms': 20000, 'drag_x': dragged, 'arrow_x': arrow,
                  'track_x': track, 'hover_pressed': True, 'reconciliation_difference_fraction': mismatch}
        (build / 'scroll-affine-controls-results.json').write_text(json.dumps(result, indent=2) + '\n')
        print('PASS: affine horizontal scrollbar controls', json.dumps(result))
        return

    initial = capture('initial')
    assert color(initial, 250, 90) == bytes((221, 102, 51))
    click(100, 40); wait_marker('AFFINE begin')
    # The wheel listener occupies a transformed local rectangle, not its screen bounding box.
    move(*point(60, 120))
    start = len(healthy()); wheel(); time.sleep(.2)
    assert 'COMPOSITOR: async scroll' not in healthy()[start:], 'Blocking listener was bypassed'
    move(*point(300, 90))
    before = capture('before-wheel'); started = time.monotonic(); wheel()
    deadline = started + 1
    while time.monotonic() < deadline:
        first = capture('first-wheel')
        if color(first, 250, 90) == bytes((51, 170, 102)):
            break
        time.sleep(.005)
    else:
        raise AssertionError('Transformed nested scroll did not repaint within one second')
    latency = (time.monotonic() - started) * 1000
    assert color(first, 300, 20) == bytes((136, 68, 204)), 'Transformed sticky header moved'
    assert 'AFFINE end' not in healthy()
    # This point lies inside the screen bounding box but outside the transformed rounded clip.
    move(350, 210); start = len(healthy()); wheel(); time.sleep(.2)
    assert 'COMPOSITOR: async scroll' in healthy()[start:], 'Root outside the transformed clip did not scroll'
    outside = capture('outside-clip')
    assert color(outside, 250, 90) == color(first, 250, 90), 'Bounding-box hit scrolled the nested element'
    # The vertical thumb begins after two 20-pixel buttons and moves with its logical offset.
    move(*point(390, 57)); time.sleep(.15)
    hover = capture('thumb-hover')
    assert color(hover, 384, 57) == bytes((221, 51, 119)), ('Thumb hover', color(hover, 384, 57))
    press(True)
    pressed = capture('thumb-pressed')
    assert color(pressed, 384, 57) == bytes((119, 51, 204)), ('Thumb pressed', color(pressed, 384, 57))
    move(*point(390, 150)); time.sleep(.15); press(False)
    move(950, 600)
    busy = capture('dragged')
    assert 'AFFINE end' not in healthy(), 'Checks ran after JavaScript resumed'
    assert 'AFFINE nested ' not in healthy(), 'DOM ran inside the long task'
    wait_marker('AFFINE end'); wait_marker('AFFINE blocked wheel true'); wait_marker('AFFINE root 80'); wait_marker('AFFINE nested 0 ')
    time.sleep(3)
    reconciled = capture('reconciled')
    left = crop(busy, (300, 240, 930, 650)); right = crop(reconciled, (300, 240, 930, 650))
    mismatch = sum(abs(a - b) > 2 for a, b in zip(left, right)) / len(left)
    assert mismatch < .02, ('Affine reconciliation differs', mismatch)
    positions = re.findall(r'AFFINE nested (\d+) (\d+)', healthy())
    assert positions and int(positions[-1][1]) > 500, positions
    compositor_positions = re.findall(r'COMPOSITOR: async drag sequence=\d+ position=(\d+)', healthy())
    assert compositor_positions and int(positions[-1][1]) == int(compositor_positions[-1]), 'Queued wheel changed the reconciled offset'
    frame_left = crop(busy, (20, 450, 250, 600)); frame_right = crop(reconciled, (20, 450, 250, 600))
    frame_mismatch = sum(abs(a - b) > 2 for a, b in zip(frame_left, frame_right)) / len(frame_left)
    assert frame_mismatch < .02, ('Transformed iframe reconciliation differs', frame_mismatch)
    result = {'javascript_block_ms': 20000, 'first_scroll_capture_ms': round(latency, 2),
              'nested_position': list(map(int, positions[-1])), 'blocking_listener_preserved': True,
              'transformed_iframe_difference_fraction': frame_mismatch, 'rounded_clip_hit': True, 'sticky': True, 'hover_pressed_drag': True,
              'reconciliation_difference_fraction': mismatch}
    (build / 'scroll-affine-results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: affine scroll compositor', json.dumps(result))
