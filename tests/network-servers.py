#!/usr/bin/env python3
"""Serve two HTTP and two HTTPS origins with one isolated test trust store."""
import argparse
from http.server import ThreadingHTTPServer
from pathlib import Path
import runpy
import signal
import ssl
import struct
import subprocess
import threading
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--directory', type=Path, default=Path('build/network-https'))
options = parser.parse_args()
directory = options.directory.resolve()
directory.mkdir(mode=0o700, parents=True, exist_ok=True)
certificate = directory / 'cert.pem'
key = directory / 'key.pem'
with (directory / 'openssl.log').open('w') as output:
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
                    '-subj', '/CN=10.0.2.2', '-addext', 'subjectAltName=IP:10.0.2.2',
                    '-addext', 'basicConstraints=critical,CA:TRUE', '-keyout', str(key),
                    '-out', str(certificate)], stdout=output, stderr=subprocess.STDOUT, check=True)
der = ssl.PEM_cert_to_DER_cert(certificate.read_text())
identifier = uuid.UUID('a5c059a1-94e4-4aa7-87b5-ab155c2bf072').bytes_le
(directory / 'ca.esl').write_bytes(identifier + struct.pack('<III', 44 + len(der), 0, 16 + len(der)) + uuid.uuid4().bytes_le + der)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(certificate, key)
servers = []
try:
    for port in (8765, 8766, 8767, 8768):
        # Each origin retains its own request counters for negative assertions.
        handler = runpy.run_path(str(Path(__file__).with_name('redirect-server.py')))['Handler']
        server = ThreadingHTTPServer(('127.0.0.1', port), handler)
        if port >= 8767:
            server.socket = context.wrap_socket(server.socket, server_side=True)
        servers.append(server)
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    stopped = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopped.set())
    signal.signal(signal.SIGINT, lambda *_: stopped.set())
    print('READY: HTTP 8765/8766 and HTTPS 8767/8768; isolated CA at ' + str(directory / 'ca.esl'), flush=True)
    stopped.wait()
finally:
    for server in servers:
        server.server_close()
