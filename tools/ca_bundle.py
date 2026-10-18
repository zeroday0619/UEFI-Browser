#!/usr/bin/env python3
"""Convert a PEM CA bundle to an EFI signature list without changing host trust."""
import pathlib
import re
import ssl
import struct
import sys
import uuid

def convert(source, target):
    certificates = re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', source.read_text(), re.S)
    if not certificates:
        raise ValueError('No PEM certificates found')
    signature_type = uuid.UUID('a5c059a1-94e4-4aa7-87b5-ab155c2bf072').bytes_le
    owner = uuid.UUID('61dcfb1f-737a-4248-af29-9a50cd18fc4d').bytes_le
    result = bytearray()
    for certificate in certificates:
        der = ssl.PEM_cert_to_DER_cert(certificate)
        result += signature_type + struct.pack('<III', 44 + len(der), 0, 16 + len(der)) + owner + der
    target.write_bytes(result)
    print(f'Exported {len(certificates)} CA certificates to {target}')

if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('Usage: ca_bundle.py INPUT.pem OUTPUT.esl')
    convert(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
