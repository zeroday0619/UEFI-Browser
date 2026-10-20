#!/usr/bin/env python3
"""Namespace static SwiftShader Vulkan exports using the pinned driver entry points."""
from pathlib import Path
import re
import sys
root = Path(sys.argv[1])
source = root / 'third_party/swiftshader/src/Vulkan'
# The proc lookup stringifies the original token, preserving the Vulkan API names.
names = set()
for path in source.glob('*.cpp'):
    names.update(re.findall(r'\b(vk[A-Z]\w+)\b', path.read_text()))
header = '// Generated from the pinned SwiftShader sources.\n#pragma once\n'
header += ''.join(f'#define {name} uefi_swiftshader_{name}\n' for name in sorted(names))
destination = source / 'UEFIVulkanSymbols.hpp'
if not destination.exists() or destination.read_text() != header:
    destination.write_text(header)
