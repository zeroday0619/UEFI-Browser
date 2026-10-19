"""Verify real text reflow and scrollbar thickness while the renderer executes JavaScript."""
import json
import re
import os
import time


def run(execute, build, healthy, move, click, key, wait_marker, type_address):
    horizontal = os.environ.get('INPUT_SCENARIO') == 'scroll-gutter-horizontal'
    root = os.environ.get('INPUT_SCENARIO') in ('scroll-gutter-root', 'scroll-gutter-parent-frame')
    page = 'scroll-gutter-root.html' if root else 'scroll-gutter.html'
    if os.environ.get('INPUT_SCENARIO') == 'scroll-gutter-container': page = 'scroll-gutter-container.html'
    if os.environ.get('INPUT_SCENARIO') in ('scroll-gutter-iframe', 'scroll-gutter-srcdoc', 'scroll-gutter-nested-frame'): page = os.environ['INPUT_SCENARIO'] + '.html'
    if os.environ.get('INPUT_SCENARIO') == 'scroll-gutter-parent-frame': page = 'scroll-gutter-parent-frame.html'
    if horizontal: page = 'scroll-gutter-horizontal.html'
    normal_size, hover_size, pressed_size = (16, 48, 80) if horizontal else (24, 80, 120)
    left_edge, right_edge = (0, 1280) if root else (60, 560)
    pointer_y = 50 if root else 160
    pointer_x, outside_x = (1268, 900) if root else (548, 600)
    outside_y = pointer_y
    if horizontal: pointer_x, pointer_y, outside_x, outside_y = 120, 654, 120, 700
    key('ctrl', 'l'); type_address('http://10.0.2.2:8765/' + page); key('ret')
    wait_marker('GUTTER ready'); time.sleep(5)
    checks = []

    def capture(name):
        destination = build / ('input-gutter-' + name + '.ppm')
        execute('screendump', {'filename': str(destination)})
        parts = destination.read_bytes().split(b'\n', 3)
        return int(parts[1].split()[0]), parts[3]

    def geometry(name, thickness, busy=True):
        width, pixels = capture(name)
        points = [(x, y) for y in range(205, 735) for x in range(left_edge, right_edge)
                  if pixels[(y * width + x) * 3:(y * width + x) * 3 + 3] == bytes((255, 0, 136))]
        assert points, (name, 'missing text end marker')
        bounds = (min(x for x, _ in points), min(y for _, y in points),
                  max(x for x, _ in points) + 1, max(y for _, y in points) + 1)
        expected_width = (540 - thickness) // 2 if horizontal else right_edge - left_edge - thickness
        assert bounds[2] - bounds[0] == expected_width, (name, bounds, thickness)
        if busy:
            assert 'GUTTER end' not in healthy(), name + ' ran after JavaScript ended'
        checks.append({'state': name, 'thickness': thickness, 'text_end_y': bounds[1], 'marker_width': bounds[2] - bounds[0]})
        return bounds, (width, pixels)

    def press(down):
        execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
        time.sleep(.2)

    idle = healthy().count('COMPOSITOR: scene pictures=')
    observer_count = healthy().count('GUTTER size=')
    time.sleep(1)
    idle_publications = healthy().count('COMPOSITOR: scene pictures=') - idle
    assert idle_publications <= 1, 'Speculation scheduled continuous repaint'
    assert healthy().count('GUTTER size=') == observer_count, 'Speculation delivered ResizeObserver callbacks'
    normal, _ = geometry('normal-reference', normal_size, False)
    move(pointer_x, pointer_y); time.sleep(2)
    hovered, reference = geometry('hover-reference', hover_size, False)
    assert hovered[1] >= normal[1], (normal, hovered)
    move(outside_x, outside_y); time.sleep(2)
    click(130, 42); wait_marker('GUTTER begin')
    normal_busy, _ = geometry('normal-busy', normal_size)
    assert normal_busy == normal
    publications = healthy().count('COMPOSITOR: scene pictures=')
    move(pointer_x, pointer_y)
    hover_busy, _ = geometry('hover-busy', hover_size)
    assert hover_busy == hovered
    press(True)
    pressed, _ = geometry('pressed-busy', pressed_size)
    assert pressed[1] >= hovered[1], (hovered, pressed)
    assert pressed[1] > normal[1], (normal, hovered, pressed)
    move(outside_x, outside_y)
    outside, _ = geometry('pressed-outside-busy', pressed_size)
    assert outside == pressed
    press(False)
    restored, _ = geometry('restored-busy', normal_size)
    assert restored == normal
    if os.environ.get('INPUT_SCENARIO') in ('scroll-gutter-iframe', 'scroll-gutter-srcdoc', 'scroll-gutter-nested-frame', 'scroll-gutter-parent-frame'):
        move(180, 300)
        before_wheel, _ = geometry('before-wheel-busy', normal_size)
        for button in ('wheel-down', 'wheel-up'):
            for down in (True, False):
                execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': button, 'down': down}}]})
            time.sleep(.2)
            after_wheel, _ = geometry(button + '-busy', normal_size)
            if button == 'wheel-down': assert after_wheel[1] < before_wheel[1], 'Child scroll did not move asynchronously'
            else: assert after_wheel == before_wheel, 'Child scroll did not restore its offset'
    move(pointer_x, pointer_y)
    _, final_busy = geometry('final-hover-busy', hover_size)
    assert healthy().count('COMPOSITOR: scene pictures=') == publications, 'Renderer produced a frame during the blocking script'
    wait_marker('GUTTER end'); time.sleep(5)
    final, reconciled = geometry('reconciled', hover_size, False)
    assert final == hovered
    def crop(image):
        width, pixels = image
        return b''.join(pixels[(y * width + left_edge) * 3:(y * width + right_edge) * 3] for y in range(200, 740))
    left, right = crop(final_busy), crop(reconciled)
    difference = sum(abs(a-b) > 2 for a,b in zip(left,right)) / len(left)
    assert difference < .02, ('Reconciliation mismatch', difference)
    extent = 540 if horizontal else right_edge - left_edge
    assert 'GUTTER size=' + str(extent - hover_size) in healthy(), 'Final layout did not reach Blink'
    sizes = [(int(a), int(b), int(c)) for a,b,c in re.findall(r'GUTTER size=(\d+),(\d+) range=(\d+)', healthy())]
    normal_range = next(c for a,b,c in sizes if a == extent - normal_size)
    hover_range = next(c for a,b,c in sizes if a == extent - hover_size)
    assert horizontal or hover_range > normal_range, ('Reflow did not update scroll range', sizes)
    result = {'javascript_block_ms': 25000, 'root_scrollbar': root, 'horizontal_scrollbar': horizontal, 'idle_scene_publications': idle_publications, 'checks': checks, 'renderer_publications_during_input': 0,
              'reconciliation_difference_fraction': difference, 'normal_scroll_height': normal_range, 'hover_scroll_height': hover_range}
    (build / 'scroll-gutter-results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: asynchronous document reflow', json.dumps(result))
