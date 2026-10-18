#!/usr/bin/env python3
"""Check CA embedding preserves certificate DER and rejects malformed PEM."""
import base64
from pathlib import Path
import re
import subprocess
import sys
import tempfile

project = Path(__file__).resolve().parents[1]
pattern = rb'-----BEGIN CERTIFICATE-----\s+([A-Za-z0-9+/=\s]+?)-----END CERTIFICATE-----'
def certificates(contents):
    return [base64.b64decode(b''.join(block.split()), validate=True)
            for block in re.findall(pattern, contents)]

with tempfile.TemporaryDirectory() as directory:
    temporary = Path(directory)
    original = (project / 'trust/mozilla-ca.pem').read_bytes()
    source = temporary / 'roots.pem'
    output = temporary / 'roots.h'
    source.write_bytes(original)
    command = [sys.executable, str(project / 'tools/embed-ca.py'), str(source), str(output)]
    subprocess.run(command, check=True)
    assert certificates(original) == certificates(output.read_bytes())
    timestamp = output.stat().st_mtime_ns
    subprocess.run(command, check=True)
    assert output.stat().st_mtime_ns == timestamp
    subprocess.run(['openssl', 'crl2pkcs7', '-nocrl', '-certfile', str(source),
                    '-outform', 'DER', '-out', str(temporary / 'roots.p7b')], check=True)
    first = re.search(rb'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', original, re.S)[0]
    source.write_bytes('주석의 인코딩은 인증서에 영향을 주지 않습니다.\n'.encode() + first + b'\n')
    subprocess.run(command, check=True)
    assert certificates(output.read_bytes()) == certificates(first)
    for invalid in (b'no certificate', first + b'\n-----BEGIN CERTIFICATE-----\nbroken',
                    b'-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----'):
        source.write_bytes(invalid)
        previous = output.read_bytes()
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert result.returncode != 0
        assert output.read_bytes() == previous
print('PASS: bundled certificates parse, DER bytes are preserved, Unicode comments are accepted, malformed blocks fail, unchanged input does not rewrite output.')
