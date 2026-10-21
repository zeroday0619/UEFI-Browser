#!/usr/bin/env python3
"""Embed the pinned Chromium DevTools distribution from its GN resource manifest."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import zlib


def generate(source, output):
    source = source.resolve()
    names = json.loads((source / 'expected_grd_files.json').read_text())
    if not isinstance(names, list) or not names or len(names) != len(set(names)):
        raise ValueError('The DevTools manifest must contain unique resource paths.')
    records = []
    archive = bytearray()
    total = 0
    for name in sorted(names):
        path = PurePosixPath(name)
        if (not isinstance(name, str) or path.is_absolute() or '..' in path.parts or
                '\\' in name or str(path) != name or not name.startswith('front_end/')):
            raise ValueError('Invalid DevTools resource path: ' + repr(name))
        target = (source / name).resolve()
        if not target.is_relative_to(source):
            raise ValueError('DevTools resource escapes the generated directory.')
        data = target.read_bytes()
        total += len(data)
        if len(data) > 8 * 1024 * 1024 or total > 64 * 1024 * 1024:
            raise ValueError('DevTools resource size exceeds firmware limits.')
        compressed = zlib.compress(data, 9)
        records.append({'path': '/' + name, 'offset': len(archive),
                        'compressed': len(compressed), 'length': len(data),
                        'sha256': hashlib.sha256(data).hexdigest()})
        archive.extend(compressed)
    output.mkdir(parents=True, exist_ok=True)
    (output / 'devtools.bin').write_bytes(archive)
    (output / 'devtools-manifest.json').write_text(json.dumps(records, indent=2) + '\n')
    with (output / 'devtools-index.c').open('w') as result:
        result.write('/* Generated from the pinned Chromium DevTools resource manifest. */\n'
                     '#include <stddef.h>\n#include <string.h>\n'
                     'extern const unsigned char firmware_devtools_start[];\n'
                     'struct Entry { const char *path; size_t offset, compressed, length; };\n'
                     'static const struct Entry entries[] = {\n')
        for entry in records:
            result.write('{%s,%d,%d,%d},\n' % (
                json.dumps(entry['path']), entry['offset'], entry['compressed'], entry['length']))
        result.write('''};
const unsigned char *firmware_devtools_resource(const char *path, size_t *compressed, size_t *length) {
  size_t first = 0, last = sizeof(entries) / sizeof(entries[0]);
  *compressed = *length = 0;
  while (first < last) {
    size_t middle = first + (last - first) / 2;
    int order = strcmp(path, entries[middle].path);
    if (order < 0) last = middle;
    else if (order > 0) first = middle + 1;
    else {
      *compressed = entries[middle].compressed;
      *length = entries[middle].length;
      return firmware_devtools_start + entries[middle].offset;
    }
  }
  return NULL;
}
''')
    (output / 'devtools-data.S').write_text(
        '.section .rodata,"a",@progbits\n.balign 16\n'
        '.globl firmware_devtools_start\nfirmware_devtools_start:\n.incbin ' +
        json.dumps(str((output / 'devtools.bin').resolve())) +
        '\n.section .note.GNU-stack,"",@progbits\n')
    print(f'Embedded {len(records)} DevTools resources: {total} bytes, {len(archive)} compressed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    options = parser.parse_args()
    generate(options.source, options.output)
