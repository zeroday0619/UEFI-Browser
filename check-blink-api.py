#!/usr/bin/env python3
"""Compile only the adapter object using GN flags and real generated headers."""
import argparse
import configparser
import hashlib
import json
import pathlib
import re
import shlex
import subprocess

project = pathlib.Path(__file__).resolve().parent
root = project.parents[1] / 'work/chromium-build/chromium'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--resolve-inputs', action='store_true')
parser.add_argument('--source', choices=['navigation', 'message_pump', 'scheduler', 'frame_loop', 'resource_loader', 'browser_frame', 'browser_entry'], default='navigation')
parser.add_argument('--firmware', action='store_true')
options = parser.parse_args()
build = root / ('out/uefi-blink-native' if options.firmware else 'out/uefi-blink')
label = 'native-' if options.firmware else ''
evidence_path = project / ('validation/blink-' + label + options.source + '-result.json')
evidence_path.unlink(missing_ok=True)
preparation = ['python3', str(project / 'prepare-blink.py')]
if options.firmware:
    preparation.append('--firmware')
if options.resolve_inputs:
    preparation.append('--resolve-inputs')
subprocess.run(preparation, check=True)
modules = configparser.ConfigParser()
modules.read(root / '.gitmodules')
urls = {modules[section]['path']: modules[section]['url'] for section in modules.sections()}
links = {}
for entry in subprocess.check_output(['git', '-C', str(root), 'ls-tree', '-rz', 'HEAD']).split(b'\0'):
    if entry.startswith(b'160000 '):
        metadata, name = entry.split(b'\t')
        links[name.decode()] = metadata.split()[2].decode()
commands = json.loads(subprocess.check_output(['ninja', '-C', str(build), '-t', 'compdb', 'cxx']))
command = next(item for item in commands if item['file'].endswith('/uefi/src/blink/' + options.source + '.cc') and (not options.firmware or '-DUEFI_RUNTIME' in item['command']))
arguments = shlex.split(command['command'])
if options.firmware:
    for required in ('-DUEFI_RUNTIME', '-nostdinc', '-mno-red-zone', '-fno-stack-protector'):
        if required not in arguments:
            raise SystemExit('Missing firmware compiler option: ' + required)
    if '-fstack-protector' in arguments or '-fstack-protector-strong' in arguments:
        raise SystemExit('Firmware objects must not require a host stack protector')

syntax_arguments = []
index = 0
while index < len(arguments):
    argument = arguments[index]
    if argument in ('-o', '-MF'):
        index += 2
        continue
    if argument not in ('-c', '-MD', '-MMD'):
        syntax_arguments.append(argument)
    index += 1
syntax_arguments += ['-fsyntax-only', '-fno-color-diagnostics']
targets = subprocess.check_output(['ninja', '-C', str(build), '-t', 'targets', 'all'], text=True)
outputs = {line.split(': ', 1)[0] for line in targets.splitlines()}
include_directories = []
index = 0
while index < len(arguments):
    argument = arguments[index]
    if argument in ('-I', '-isystem', '-iquote'):
        include_directories.append((build / arguments[index + 1]).resolve())
        index += 1
    elif argument.startswith('-I'):
        include_directories.append((build / argument[2:]).resolve())
    elif argument.startswith('-isystem'):
        include_directories.append((build / argument[len('-isystem'):]).resolve())
    index += 1
attempted = set()
with (project / ('validation/blink-' + label + options.source + '-build.log')).open('w') as log:
    for attempt in range(512):
        result = subprocess.run(syntax_arguments, cwd=build, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log.write(result.stdout)
        log.flush()
        if not result.returncode:
            output = build / arguments[arguments.index('-o') + 1]
            output.parent.mkdir(parents=True, exist_ok=True)
            result = subprocess.run(arguments, cwd=build, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise SystemExit(result.returncode)
            message = 'PASS: Blink adapter ' + ('musl firmware' if options.firmware else 'host') + ' object compiled; Blink libraries and firmware linkage are unverified.'
            evidence = {
                'source': options.source,
                'compiler_arguments': arguments,
                'host_object_compiled': not options.firmware,
                'firmware_object_compiled': options.firmware,
                'object_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                'object_bytes': output.stat().st_size,
                'source_sha256': hashlib.sha256(
                    (project / ('src/blink/' + options.source + '.cc')).read_bytes()).hexdigest(),
                'blink_firmware_linked': False,
                'website_rendered': False,
            }
            (project / ('validation/blink-' + label + options.source + '-result.json')).write_text(
                json.dumps(evidence, indent=2) + '\n')
            log.write(message + '\n')
            print(message)
            break
        missing = re.search(r"fatal error: '([^']+)' file not found", result.stdout)
        if not missing:
            print(result.stdout)
            raise SystemExit(result.returncode)
        header = missing[1]
        dependency = next((name for name in links if header.startswith(name + '/')), None)
        if options.resolve_inputs and dependency and dependency not in attempted:
            directory = root / dependency
            if not directory.exists() or not any(directory.iterdir()):
                directory.mkdir(parents=True, exist_ok=True)
                attempted.add(dependency)
                for operation in (['init', '-q'], ['remote', 'add', 'origin', urls[dependency]],
                                  ['fetch', '--depth=1', 'origin', links[dependency]],
                                  ['checkout', '--detach', 'FETCH_HEAD']):
                    subprocess.run(['git', '-C', str(directory), *operation], check=True)
                continue
        candidates = []
        for directory in include_directories:
            path = (directory / header).resolve()
            if path.is_relative_to(build):
                relative = path.relative_to(build).as_posix()
                if relative in outputs:
                    candidates.append(relative)
        candidates = list(dict.fromkeys(candidates))
        if len(candidates) != 1 or candidates[0] in attempted:
            print(result.stdout)
            raise SystemExit('Cannot resolve a unique generated header target: ' + header)
        target = candidates[0]
        attempted.add(target)
        print('Generating ' + target, flush=True)
        log.write('Generating ' + target + '\n')
        result = subprocess.run(['ninja', '-C', str(build), '-j12', target], stdout=log, stderr=subprocess.STDOUT)
        log.flush()
        if result.returncode:
            raise SystemExit(result.returncode)
    else:
        raise SystemExit('Generated header resolution limit reached')
