"""Compare scrollable query paints before and after a blocked renderer resumes."""
import json
import os
import time


def run(execute, build, healthy, move, click, key, wait_marker, type_address):
    stuck = os.environ.get('INPUT_SCENARIO') == 'scroll-stuck'
    page = os.environ['INPUT_SCENARIO'] + '.html'
    key('ctrl', 'l'); type_address('http://10.0.2.2:8765/' + page); key('ret')
    wait_marker('STATE ready'); time.sleep(5)

    def capture(name):
        destination = build / ('input-state-' + name + '.ppm')
        execute('screendump', {'filename': str(destination)})
        parts = destination.read_bytes().split(b'\n', 3)
        return int(parts[1].split()[0]), parts[3]

    def color(image, x, y):
        width, pixels = image
        offset = (y * width + x) * 3
        return tuple(pixels[offset:offset + 3])

    def wheel(direction, count=1):
        for _ in range(count):
            for down in (True, False):
                execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'wheel-' + direction, 'down': down}}]})
            time.sleep(.12)
        time.sleep(.3)

    before = capture('before')
    assert color(before, 100, 215) == (238, 34, 68), color(before, 100, 215)
    click(130, 42); wait_marker('STATE begin')
    publications = healthy().count('COMPOSITOR: scene pictures=')
    move(300, 250)
    wheel('down')
    middle = capture('middle-busy')
    assert color(middle, 100, 215) == (34, 204, 102), color(middle, 100, 215)
    wheel('up')
    restored = capture('top-busy')
    assert color(restored, 100, 215) == (238, 34, 68), color(restored, 100, 215)
    if stuck:
        wheel('down')
        assert color(capture('expanded-busy'),100,265) == (34,204,102), 'Stuck query did not expand descendant layout'
        wheel('down',35)
        wheel('up')
    else:
        wheel('down', 35)
        bottom = capture('bottom-busy')
        assert color(bottom, 260, 215) == (238, 187, 34), color(bottom, 260, 215)
        wheel('up')
    final_busy = capture('final-busy')
    assert color(final_busy, 260, 215) == (34, 85, 204), color(final_busy, 260, 215)
    assert 'STATE end' not in healthy()
    assert healthy().count('COMPOSITOR: scene pictures=') == publications
    wait_marker('STATE end'); time.sleep(4)
    reconciled = capture('reconciled')
    def crop(image):
        width, pixels = image
        return b''.join(pixels[(y * width + 60) * 3:(y * width + 560) * 3] for y in range(200, 740))
    left, right = crop(final_busy), crop(reconciled)
    difference = sum(abs(a-b)>2 for a,b in zip(left,right))/len(left)
    assert difference < .02, difference
    result = {'javascript_block_ms': 25000, 'renderer_publications_during_input': 0,
              'top_middle_bottom_states': not stuck, 'stuck_layout': stuck, 'reconciliation_difference_fraction': difference}
    (build / 'scroll-state-results.json').write_text(json.dumps(result, indent=2)+'\n')
    print('PASS: scrollable query states', json.dumps(result))
