#!/usr/bin/env python3
"""Embed the selected CA PEM bundle without adding any other trust anchors."""
import argparse
import base64
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('input', type=Path)
parser.add_argument('output', type=Path)
options = parser.parse_args()
contents = options.input.read_bytes()
blocks = re.findall(rb'-----BEGIN CERTIFICATE-----\s+([A-Za-z0-9+/=\s]+?)-----END CERTIFICATE-----', contents)
if not blocks:
    raise SystemExit('CA input contains no certificates')
if contents.count(b'-----BEGIN CERTIFICATE-----') != len(blocks) or contents.count(b'-----END CERTIFICATE-----') != len(blocks):
    raise SystemExit('CA input contains a malformed PEM certificate block')
certificates = []
for block in blocks:
    encoded = b''.join(block.split())
    base64.b64decode(encoded, validate=True)
    lines = [encoded[index:index + 64] for index in range(0, len(encoded), 64)]
    certificates.append(b'-----BEGIN CERTIFICATE-----\n' + b'\n'.join(lines)
                        + b'\n-----END CERTIFICATE-----\n')
text = b''.join(certificates).decode('ascii')
output = ('// Generated from the selected CA bundle.\n'
          '#ifndef UEFI_MOZILLA_ROOT_CERTIFICATES_H_\n'
          '#define UEFI_MOZILLA_ROOT_CERTIFICATES_H_\n'
          'inline constexpr char kUefiMozillaRootCertificates[] = R"UEFI_CA('
          + text + ')UEFI_CA";\n'
          'inline constexpr unsigned kUefiMozillaRootCertificateCount = '
          + str(len(blocks)) + ';\n#endif\n')
options.output.parent.mkdir(parents=True, exist_ok=True)
if not options.output.exists() or options.output.read_text() != output:
    options.output.write_text(output)
print('Embedded ' + str(len(blocks)) + ' CA certificates')
