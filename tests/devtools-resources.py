#!/usr/bin/env python3
"""Validate the compiled DevTools resource lookup against every upstream asset."""
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import zlib

project = Path(__file__).resolve().parents[1]
source = project.parents[1] / 'work/chromium-build/chromium/out/uefi-blink-native/gen/third_party/devtools-frontend/src'
specification = importlib.util.spec_from_file_location('embed_devtools', project / 'tools/embed-devtools.py')
module = importlib.util.module_from_spec(specification)
specification.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix='uefi-devtools-') as directory:
    output = Path(directory)
    module.generate(source, output)
    subprocess.run(['clang', '-shared', '-fPIC', str(output / 'devtools-index.c'),
                    str(output / 'devtools-data.S'), '-o', str(output / 'lookup.so')], check=True)
    library = ctypes.CDLL(str(output / 'lookup.so'))
    lookup = library.firmware_devtools_resource
    lookup.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    lookup.restype = ctypes.c_void_p
    manifest = json.loads((output / 'devtools-manifest.json').read_text())
    for entry in manifest:
        compressed, length = ctypes.c_size_t(), ctypes.c_size_t()
        pointer = lookup(entry['path'].encode(), ctypes.byref(compressed), ctypes.byref(length))
        assert pointer and length.value == entry['length']
        decoded = zlib.decompress(ctypes.string_at(pointer, compressed.value))
        assert decoded == (source / entry['path'][1:]).read_bytes()
        assert hashlib.sha256(decoded).hexdigest() == entry['sha256']
    for path in ('', '/', '/not-found.js', '/front_end/../package.json', '/front_end/inspector.html?x'):
        compressed, length = ctypes.c_size_t(100), ctypes.c_size_t(100)
        assert not lookup(path.encode(), ctypes.byref(compressed), ctypes.byref(length))
        assert compressed.value == length.value == 0
    malformed = output / 'malformed'
    malformed.mkdir()
    for names in ([], ['front_end/a', 'front_end/a'], ['../escape'], ['/absolute'],
                  ['front_end/../escape'], ['front_end//a']):
        (malformed / 'expected_grd_files.json').write_text(json.dumps(names))
        try:
            module.generate(malformed, output / 'rejected')
        except ValueError:
            pass
        else:
            raise AssertionError(names)
    print(f'PASS: {len(manifest)} compiled resource lookups, decompression, missing paths and manifest boundaries')
