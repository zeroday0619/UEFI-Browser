"""Verify firmware Korean composition using physical QEMU key events."""
import json
import re

def run(key, click, type_address, wait_marker, snapshot, healthy):
    key('ctrl','l'); type_address('http://10.0.2.2:8765/ime.html'); key('ret')
    wait_marker('IME READY')
    click(120,125); key('f9')
    for character in 'gksrmf': key(character)
    snapshot('ime-korean-preedit')
    key('ret'); click(680,125)
    wait_marker('IME VALUES {"text":"한글","edit":"","child":""}')
    click(120,125); key('ctrl','a')
    for character in 'rhks': key(character)
    key('backspace'); key('ret'); click(680,125)
    wait_marker('IME VALUES {"text":"과","edit":"","child":""}')
    click(120,125); key('end'); key('r'); key('k'); key('esc'); click(680,125)
    # Cancellation must not retain the extra syllable.
    records=re.findall(r'IME VALUES (\{[^\r\n]+\})',healthy())
    assert json.loads(records[-1])['text']=='과',records
    click(120,200)
    for character in 'rkqt': key(character)
    # Clicking another editable field commits the previous preedit.
    click(120,320)
    for character in 'dlfrk': key(character)
    key('ret'); click(680,125)
    wait_marker('IME VALUES {"text":"과","edit":"값","child":"일가"}')
    records=re.findall(r'IME EVENTS (\{[^\r\n]+\})',healthy())
    events=json.loads(records[-1])
    assert all(events[name]>0 for name in ('starts','updates','ends','inputs')),events
    snapshot('ime-korean-fields')
    key('ctrl','l')
    for character in 'gksrmf': key(character)
    snapshot('ime-korean-address')
    key('f9'); key('ctrl','a')
    type_address('http://10.0.2.2:8765/ime.html'); key('ret')
    wait_marker('IME READY')
    click(120,125); key('a'); key('b'); key('c'); click(680,125)
    wait_marker('IME VALUES {"text":"abc","edit":"","child":""}')
    print('PASS: Korean composition, events, deletion, cancellation, contenteditable, iframe, address and Latin mode')
