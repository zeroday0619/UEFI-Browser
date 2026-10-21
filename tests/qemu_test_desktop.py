#!/usr/bin/env python3
"""Keep automated SDL QEMU windows on an inactive KDE virtual desktop."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

DESKTOP_NAME = 'UEFI QEMU Tests'
WINDOW_CLASS = 'uefi-browser-test'
RULE_GROUP = 'uefi-browser-automated-tests'
MANAGER = 'org.kde.KWin.VirtualDesktopManager'


def run(*arguments):
    return subprocess.check_output(arguments, text=True, timeout=10).strip()


def desktops():
    value = run('qdbus6', '--literal', 'org.kde.KWin', '/VirtualDesktopManager',
                'org.freedesktop.DBus.Properties.Get', MANAGER, 'desktops')
    return [(int(position), identifier, name) for position, identifier, name in
            re.findall(r'\[Argument: \(uss\) (\d+), "([0-9a-f-]+)", "([^"\\]*)"\]', value)]


def current_desktop():
    return run('qdbus6', 'org.kde.KWin', '/VirtualDesktopManager',
               'org.freedesktop.DBus.Properties.Get', MANAGER, 'current')


def prepare():
    """Install a narrow pre-map rule and refuse to use the active desktop."""
    for executable in ('qdbus6', 'kwriteconfig6', 'kreadconfig6'):
        if not shutil.which(executable):
            raise RuntimeError('Isolated SDL tests require KDE and ' + executable)
    before = current_desktop()
    available = desktops()
    matches = [entry for entry in available if entry[2] == DESKTOP_NAME]
    if not matches:
        run('qdbus6', 'org.kde.KWin', '/VirtualDesktopManager',
            MANAGER + '.createDesktop', str(len(available)), DESKTOP_NAME)
        matches = [entry for entry in desktops() if entry[2] == DESKTOP_NAME]
    if len(matches) != 1 or matches[0][1] == before:
        raise RuntimeError('The QEMU test desktop must be unique and inactive.')
    position, identifier, name = matches[0]
    config = Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config')) / 'kwinrulesrc'
    if config.exists():
        backup = config.with_name('kwinrulesrc.before-uefi-tests')
        if not backup.exists():
            shutil.copy2(config, backup)
    # KWin applies forced rules while managing the window, before activation.
    settings = {
        'Description': 'Isolate automated UEFI QEMU tests',
        'Enabled': 'true',
        'wmclass': WINDOW_CLASS, 'wmclassmatch': '1', 'wmclasscomplete': 'false',
        'desktops': identifier, 'desktopsrule': '2',
        'acceptfocus': 'false', 'acceptfocusrule': '2',
        'fsplevel': '4', 'fsplevelrule': '2',
        'skiptaskbar': 'true', 'skiptaskbarrule': '2',
        'skipswitcher': 'true', 'skipswitcherrule': '2',
    }
    changed = False
    for key, value in settings.items():
        previous = run('kreadconfig6', '--file', str(config), '--group', RULE_GROUP, '--key', key)
        if previous != value:
            run('kwriteconfig6', '--file', str(config), '--group', RULE_GROUP, '--key', key, value)
            changed = True
    rules = run('kreadconfig6', '--file', str(config), '--group', 'General', '--key', 'rules')
    if RULE_GROUP not in rules.split(','):
        # Existing user rules retain their relative order and contents.
        value = RULE_GROUP + (',' + rules if rules else '')
        run('kwriteconfig6', '--file', str(config), '--group', 'General', '--key', 'rules', value)
        changed = True
    if changed:
        run('qdbus6', 'org.kde.KWin', '/KWin', 'org.kde.KWin.reconfigure')
        time.sleep(.3)
    if current_desktop() != before:
        raise RuntimeError('The active desktop changed while preparing QEMU isolation.')
    return {'name': name, 'id': identifier, 'index': position,
            'original_desktop': before, 'window_class': WINDOW_CLASS}


if __name__ == '__main__':
    print(json.dumps(prepare(), indent=2))
