"""Verify that unsupported filtered reflows wait for the live renderer."""
import json
import time


def run(execute, build, healthy, move, click, key, wait_marker, type_address):
    key('ctrl', 'l'); type_address('http://10.0.2.2:8765/scroll-gutter-fallback.html'); key('ret')
    wait_marker('GUTTER ready'); time.sleep(4)

    def width(name):
        destination = build / ('input-gutter-fallback-' + name + '.ppm')
        execute('screendump', {'filename': str(destination)})
        parts = destination.read_bytes().split(b'\n', 3)
        stride = int(parts[1].split()[0]); pixels = parts[3]
        columns = [x for y in range(205, 735) for x in range(60, 560)
                   if pixels[(y * stride + x) * 3:(y * stride + x) * 3 + 3] == bytes((255, 0, 136))]
        assert columns, name
        return max(columns) - min(columns) + 1

    assert width('before') == 476
    assert 'COMPOSITOR: gutter snapshots=0' in healthy(), 'Unsupported filtered reflow was not rejected'
    click(130, 42); wait_marker('GUTTER begin')
    publication_count = healthy().count('COMPOSITOR: scene pictures=')
    move(548, 160); time.sleep(.3)
    assert width('busy') == 476, 'Unsupported layout changed before renderer dispatch'
    assert 'GUTTER end' not in healthy()
    assert 'COMPOSITOR: async gutter' not in healthy()
    assert healthy().count('COMPOSITOR: scene pictures=') == publication_count
    wait_marker('GUTTER end'); wait_marker('GUTTER size=420'); time.sleep(3)
    assert width('resumed') == 420
    assert 'GUTTER end width=476 extra=40' in healthy(), 'Speculation changed the live nested scroll offset'
    result = {'unsupported_paint_fallback': True, 'javascript_block_ms': 25000,
              'busy_content_width': 476, 'resumed_content_width': 420}
    (build / 'scroll-gutter-fallback-results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: scrollbar topology renderer fallback', json.dumps(result))
