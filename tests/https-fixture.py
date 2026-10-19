#!/usr/bin/env python3
"""Run a loopback HTTPS fixture with a separate, temporary firmware trust store."""
import argparse
from http.server import ThreadingHTTPServer
import os
from pathlib import Path
import runpy
import ssl
import struct
import subprocess
import uuid

parser = argparse.ArgumentParser()
parser.add_argument('--directory', type=Path, required=True)
parser.add_argument('--identity-domains', action='store_true', help='Include isolated Google identity fixture names in the temporary certificate.')
parser.add_argument('--port', type=int, default=8766)
arguments = parser.parse_args()
directory = arguments.directory.resolve()
directory.mkdir(mode=0o700, parents=True, exist_ok=True)
certificate = directory / 'cert.pem'
key = directory / 'key.pem'
with (directory / 'openssl.log').open('w') as output:
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
        '-days', '2', '-subj', '/CN=10.0.2.2', '-addext', 'subjectAltName=IP:10.0.2.2' + (',DNS:google.com,DNS:google.com.evil.test' if arguments.identity_domains else ''),
        '-addext', 'basicConstraints=critical,CA:TRUE', '-keyout', str(key),
        '-out', str(certificate)], stdout=output, stderr=subprocess.STDOUT, check=True)
der = ssl.PEM_cert_to_DER_cert(certificate.read_text())
identifier = uuid.UUID('a5c059a1-94e4-4aa7-87b5-ab155c2bf072').bytes_le
(directory / 'ca.esl').write_bytes(identifier + struct.pack('<III', 44 + len(der), 0, 16 + len(der)) + uuid.uuid4().bytes_le + der)
os.environ['NETWORK_RELEASE_FILE'] = str(directory / 'release')
handler = runpy.run_path(str(Path(__file__).with_name('redirect-server.py')))['Handler']
server = ThreadingHTTPServer(('127.0.0.1', arguments.port), handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(certificate, key)
server.socket = context.wrap_socket(server.socket, server_side=True)
print('HTTPS fixture ready; use its ca.esl only through TEST_CA_FILE.', flush=True)
server.serve_forever()
