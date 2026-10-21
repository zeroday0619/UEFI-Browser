"""Verify real Mozc input and candidate selection through QEMU keyboard and mouse events."""
import json
import re


def run(key, click, type_address, wait_marker, snapshot, healthy):
    key('ctrl', 'l')
    type_address('http://10.0.2.2:8765/ime.html')
    key('ret')
    wait_marker('IME READY')
    click(120, 125)
    key('shift', 'f9')
    wait_marker('IME: mode=Japanese')
    for character in 'nihongo':
        key(character)
    snapshot('ime-japanese-preedit')
    key('spc')
    key('spc')
    key('down')
    key('up')
    snapshot('ime-japanese-candidates')
    # The first candidate row is anchored below the input composition bounds.
    click(80, 166)
    key('ret')
    click(680, 125)
    wait_marker('IME VALUES {"text":"日本語","edit":"","child":""}')
    click(120, 200)
    for character in 'kanji':
        key(character)
    key('spc')
    key('ret')
    click(120, 320)
    for character in 'tokyo':
        key(character)
    key('backspace')
    key('esc')
    for character in 'nihongo':
        key(character)
    key('spc')
    key('ret')
    click(680, 125)
    wait_marker('IME VALUES {"text":"日本語","edit":"感じ","child":"日本語"}')
    events = json.loads(re.findall(r'IME EVENTS (\{[^\r\n]+\})', healthy())[-1])
    assert all(events[name] > 0 for name in ('starts', 'updates', 'ends', 'inputs')), events
    snapshot('ime-japanese-fields')
    key('ctrl', 'l')
    for character in 'nihongo':
        key(character)
    key('spc')
    key('spc')
    snapshot('ime-japanese-address-candidates')
    click(340, 64, page=False)
    snapshot('ime-japanese-address-selected')
    key('shift', 'f9')
    key('ctrl', 'a')
    type_address('http://10.0.2.2:8765/ime.html')
    key('ret')
    wait_marker('IME READY')
    click(120, 125)
    for character in 'abc':
        key(character)
    click(680, 125)
    wait_marker('IME VALUES {"text":"abc","edit":"","child":""}')
    print('PASS: Mozc page and address composition, dictionary conversion, candidate click, cancellation, events, iframe and Latin mode')
