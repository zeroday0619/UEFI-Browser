"""Verify asynchronous scrollbar geometry through QEMU input and framebuffer pixels."""
import json
import re
import time


def run(execute, build, healthy, move, click, key, wait_marker, type_address):
    key('ctrl', 'l'); type_address('http://10.0.2.2:8765/scroll-layout.html'); key('ret')
    wait_marker('LAYOUT ready'); time.sleep(3)
    idle_start = healthy().count('COMPOSITOR: scene pictures=')
    time.sleep(1)
    idle_publications = healthy().count('COMPOSITOR: scene pictures=') - idle_start
    assert idle_publications <= 1, ('Snapshot capture scheduled repeated idle frames', idle_publications)
    checks = []

    def capture(name):
        destination = build / ('input-layout-' + name + '.ppm')
        execute('screendump', {'filename': str(destination)})
        parts = destination.read_bytes().split(b'\n', 3)
        return int(parts[1].split()[0]), parts[3]

    def color_bounds(name, rectangle, color, length, horizontal=False):
        width, pixels = capture(name)
        left, top, right, bottom = rectangle
        points = [(x, y) for y in range(top, bottom) for x in range(left, right)
                  if pixels[(y * width + x) * 3:(y * width + x) * 3 + 3] == bytes(color)]
        assert points, (name, 'missing color', color)
        bounds = (min(x for x, _ in points), min(y for _, y in points),
                  max(x for x, _ in points) + 1, max(y for _, y in points) + 1)
        actual = bounds[2] - bounds[0] if horizontal else bounds[3] - bounds[1]
        assert actual == length, (name, bounds, length)
        assert 'LAYOUT end' not in healthy(), name + ' ran after JavaScript ended'
        checks.append({'state': name, 'bounds': bounds, 'length': actual})
        return bounds

    def press(down):
        execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
        time.sleep(.15)

    vertical = (537, 230, 542, 586)
    horizontal = (60, 587, 536, 592)
    capture('initial'); click(130, 42); wait_marker('LAYOUT begin')
    color_bounds('vertical-normal', vertical, (238, 85, 34), 40)
    move(548, 215)
    hover = color_bounds('vertical-hover', vertical, (204, 51, 204), 80)
    assert hover[1] == 290, hover
    press(True)
    active = color_bounds('vertical-pressed', vertical, (34, 204, 204), 140)
    assert active[1] == 302, active
    move(600, 215)
    color_bounds('vertical-pressed-outside', vertical, (34, 187, 102), 120)
    press(False)
    color_bounds('vertical-restored', vertical, (238, 85, 34), 40)

    move(548, 162)
    color_bounds('button-hover', vertical, (238, 187, 34), 42)
    move(548, 182)
    color_bounds('expanded-button-hit', vertical, (238, 187, 34), 42)
    command_start = len(healthy())
    press(True)
    assert 'COMPOSITOR: async' not in healthy()[command_start:], 'Expanded backward button hit the forward button'
    color_bounds('button-pressed', vertical, (136, 68, 204), 60)
    press(False)
    color_bounds('button-released', vertical, (238, 187, 34), 42)
    move(600, 400)
    move(548, 494)
    color_bounds('end-button-hover', vertical, (238, 187, 34), 42)
    press(True)
    width, pixels = capture('end-button-hidden')
    assert pixels[(580 * width + 538) * 3:(580 * width + 538) * 3 + 3] == bytes((68, 136, 204)), 'Remaining end button did not occupy the released space'
    assert all(pixels[(y * width + 538) * 3:(y * width + 538) * 3 + 3] != bytes((136, 68, 204)) for y in range(230, 586)), 'Hidden active button remained painted'
    assert 'LAYOUT end' not in healthy()
    press(False); move(600, 400)
    move(548, 470)
    press(True)
    oversized = color_bounds('oversized-buttons-hidden', vertical, (238, 85, 34), 40)
    assert oversized[1] == 230, ('Buttons did not release the full track', oversized)
    width, pixels = capture('oversized-button-track')
    assert all(pixels[(y * width + 538) * 3:(y * width + 538) * 3 + 3] not in
               (bytes((68, 136, 204)), bytes((136, 68, 204))) for y in range(230, 586)), 'Oversized buttons remained painted'
    press(False); move(600, 400)

    move(145, 518)
    color_bounds('horizontal-hover', horizontal, (204, 51, 204), 100, True)
    press(True)
    before = color_bounds('horizontal-pressed', horizontal, (34, 204, 204), 160, True)
    move(225, 518)
    after = color_bounds('horizontal-drag', horizontal, (34, 204, 204), 160, True)
    assert abs(after[0] - before[0] - 80) <= 1, (before, after)
    press(False)
    color_bounds('horizontal-released', horizontal, (204, 51, 204), 100, True)
    move(600, 400)

    move(548, 420)
    image = capture('track-hover')
    width, pixels = image
    assert pixels[(280 * width + 538) * 3:(280 * width + 538) * 3 + 3] == bytes((221, 221, 221)), 'Hover track margin missing'
    press(True)
    image = capture('track-pressed'); width, pixels = image
    assert pixels[(294 * width + 538) * 3:(294 * width + 538) * 3 + 3] == bytes((221, 221, 221)), 'Pressed track margin missing'
    press(False); move(600, 400)
    move(548, 162); move(548, 182)
    color_bounds('final-button-hover', vertical, (238, 187, 34), 42)
    final_busy = capture('final-busy')
    assert 'LAYOUT end' not in healthy()
    wait_marker('LAYOUT end'); time.sleep(3)
    reconciled = capture('reconciled')
    rectangle = (60, 230, 560, 610)

    def crop(image):
        width, pixels = image; left, top, right, bottom = rectangle
        return b''.join(pixels[(y * width + left) * 3:(y * width + right) * 3] for y in range(top, bottom))

    left, right = crop(final_busy), crop(reconciled)
    difference = sum(abs(a - b) > 2 for a, b in zip(left, right)) / len(left)
    assert difference < .02, ('Reconciliation mismatch', difference)
    assert re.search(r'LAYOUT custom=[1-9][0-9]*,[1-9][0-9]*', healthy()), 'Both scroll offsets did not reach Blink'
    result = {'javascript_block_ms': 25000, 'idle_scene_publications': idle_publications, 'checks': checks,
              'track_margins': True, 'expanded_button_hit': True, 'hidden_button_reflow': True, 'oversized_buttons_hidden': True, 'drag_delta_pixels': after[0] - before[0],
              'reconciliation_difference_fraction': difference}
    (build / 'scroll-layout-results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: asynchronous scrollbar layout', json.dumps(result))
