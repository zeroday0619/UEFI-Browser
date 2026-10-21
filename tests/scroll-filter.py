"""Verify filtered cached pixels and clip ordering while the renderer runs JavaScript."""
import json
import time


def run(execute, build, healthy, move, click, key, wait_marker, type_address):
    key('ctrl', 'l'); type_address('http://10.0.2.2:8765/scroll-filter.html'); key('ret')
    wait_marker('FILTER ready'); time.sleep(3)

    def capture(name):
        destination = build / ('input-filter-' + name + '.ppm')
        execute('screendump', {'filename': str(destination)})
        parts = destination.read_bytes().split(b'\n', 3)
        return int(parts[1].split()[0]), parts[3]

    def crop(image, rectangle):
        width, pixels = image
        left, top, right, bottom = rectangle
        return b''.join(pixels[(y * width + left) * 3:(y * width + right) * 3] for y in range(top, bottom))

    def wheel(ticks=1):
        for _ in range(ticks):
            for down in (True, False):
                execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-down', 'down': down}}]})
                time.sleep(.015)

    def difference(left, right):
        return sum(abs(a - b) > 2 for a, b in zip(left, right)) / len(left)

    initial = capture('initial')
    click(100, 70); wait_marker('FILTER begin'); move(300, 310)
    started = time.monotonic(); wheel()
    deadline = started + 1
    while time.monotonic() < deadline:
        first = capture('first-wheel')
        if crop(first, (400, 350, 450, 390)) != crop(initial, (400, 350, 450, 390)):
            break
        time.sleep(.005)
    else:
        raise AssertionError('Filtered nested scroll did not update within one second')
    latency = (time.monotonic() - started) * 1000
    assert 'COMPOSITOR: async scroll' in healthy()
    # Fixed filtered regions must keep the same pixels when an unrelated scroller moves.
    regions = {'palette': (640, 190, 1210, 510), 'shadow': (720, 570, 960, 720),
               'affine': (980, 570, 1220, 740), 'outside_viewport_halo': (740, 80, 1200, 110)}
    initial_differences = {name: difference(crop(initial, rectangle), crop(first, rectangle)) for name, rectangle in regions.items()}
    assert all(value < .02 for value in initial_differences.values()), initial_differences
    wheel(39)
    wait_marker('COMPOSITOR: async scroll sequence=40 position=3200')
    nested = capture('nested-busy')
    move(600, 600); wheel(48)
    wait_marker('COMPOSITOR: async scroll sequence=88 position=3840')
    busy = capture('root-busy')
    assert 'FILTER end' not in healthy(), 'Checks ran after JavaScript resumed'
    assert 'FILTER nested ' not in healthy(), 'DOM ran during the long task'
    wait_marker('FILTER end'); wait_marker('FILTER nested 3200'); wait_marker('FILTER root 3840')
    time.sleep(3); reconciled = capture('reconciled')
    # The busy renderer cannot update the native button's pressed appearance until the task ends.
    # Compare all marker pixels around that independent control.
    comparisons = dict(regions, nested=(50, 230, 570, 610),
                       marker_top=(10, 80, 580, 129), marker_bottom=(10, 174, 580, 370),
                       marker_left=(10, 129, 19, 174), marker_right=(211, 129, 580, 174),
                       nested_effect=(630, 710, 980, 790))
    differences = {name: difference(crop(busy, rectangle), crop(reconciled, rectangle)) for name, rectangle in comparisons.items()}
    result = {'javascript_block_ms': 20000, 'nested_scroll_px': 3200, 'root_scroll_px': 3840,
              'first_scroll_capture_ms': round(latency, 2), 'initial_filter_difference': initial_differences,
              'reconciliation_difference': differences}
    (build / 'scroll-filter-results.json').write_text(json.dumps(result, indent=2) + '\n')
    assert all(value < .02 for value in differences.values()), differences
    print('PASS: filtered asynchronous scrolling', json.dumps(result))
