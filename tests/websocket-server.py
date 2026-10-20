#!/usr/bin/env python3
"""Serve bounded RFC 6455 fixtures without an additional Python dependency."""
import base64
import hashlib
from pathlib import Path
import signal
import socketserver
import ssl
import struct
import subprocess
import threading

class Handler(socketserver.StreamRequestHandler):
    def exact(self, length):
        data = self.rfile.read(length)
        if len(data) != length: raise EOFError()
        return data

    def frame(self, opcode, payload, final=True, masked=False):
        size = len(payload)
        header = bytes([(128 if final else 0) | opcode])
        marker = 128 if masked else 0
        header += bytes([marker | size]) if size < 126 else bytes([marker | (126 if size < 65536 else 127)]) + struct.pack('!H' if size < 65536 else '!Q', size)
        if masked:
            header += b'1234'
            payload = bytes(value ^ b'1234'[index % 4] for index, value in enumerate(payload))
        self.wfile.write(header + payload)
        self.wfile.flush()

    def handle(self):
        self.connection.settimeout(20)
        try:
            line = self.rfile.readline(8192).decode('ascii')
            if not line: return
            method, path, version = line.strip().split(' ')
            headers = {}
            for _ in range(100):
                line = self.rfile.readline(8192).decode('ascii')
                if line == '\r\n': break
                name, value = line.split(':', 1)
                headers[name.lower()] = value.strip()
            assert method == 'GET' and headers['upgrade'].lower() == 'websocket'
            assert headers['sec-websocket-version'] == '13'
            assert len(base64.b64decode(headers['sec-websocket-key'])) == 16
            assert headers.get('origin') == 'http://10.0.2.2:8765'
            assert 'UEFI' in headers.get('user-agent', '')
            accept = base64.b64encode(hashlib.sha1((headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
            if path == '/bad-accept': accept = 'invalid'
            protocol = 'unrequested' if path == '/bad-protocol' else 'chat'
            self.wfile.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\nSec-WebSocket-Protocol: ' + protocol + '\r\n\r\n').encode())
            self.wfile.flush()
            print('WS_HANDSHAKE', self.server.server_address[1], path, headers['origin'], headers['user-agent'], flush=True)
            if path in ('/bad-accept', '/bad-protocol'): return
            if path == '/masked': self.frame(1, b'invalid', masked=True); return
            if path == '/bad-utf8': self.frame(1, b'\xff'); return
            if path == '/bad-continuation': self.frame(0, b'invalid'); return
            if path == '/fragment':
                data = 'fragment 한글'.encode()
                self.frame(1, data[:11], final=False)
                self.frame(9, b'ping-token')
                self.frame(0, data[11:])
            while True:
                first, second = self.exact(2)
                assert first & 128, 'Client fixture expects final frames'
                assert second & 128, 'Client frame must be masked'
                length = second & 127
                if length == 126: length = struct.unpack('!H', self.exact(2))[0]
                elif length == 127: length = struct.unpack('!Q', self.exact(8))[0]
                assert length <= 8 * 1024 * 1024
                mask = self.exact(4)
                data = self.exact(length)
                data = bytes(value ^ mask[index % 4] for index, value in enumerate(data))
                opcode = first & 15
                print('WS_FRAME', path, opcode, length, hashlib.sha256(data).hexdigest(), flush=True)
                if opcode == 8:
                    self.frame(8, data)
                    return
                if opcode == 10:
                    assert data == b'ping-token'
                    self.frame(1, b'pong-verified')
                elif opcode in (1, 2):
                    self.frame(opcode, data)
                else: raise AssertionError(opcode)
        except (EOFError, ConnectionError, TimeoutError, ssl.SSLError):
            pass
        except Exception as error:
            print('WS_FIXTURE_FAIL', repr(error), flush=True)

class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

project = Path(__file__).resolve().parents[1]
directory = project / 'build/network-https'
untrusted = directory / 'untrusted.pem'
untrusted_key = directory / 'untrusted-key.pem'
with (directory / 'untrusted-openssl.log').open('w') as output:
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2', '-subj', '/CN=10.0.2.2',
                    '-addext', 'subjectAltName=IP:10.0.2.2', '-keyout', str(untrusted_key), '-out', str(untrusted)],
                   stdout=output, stderr=subprocess.STDOUT, check=True)
servers = []
try:
    for port in (8870, 8871, 8872):
        server = Server(('127.0.0.1', port), Handler)
        if port != 8870:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(directory / 'cert.pem' if port == 8871 else untrusted,
                                    directory / 'key.pem' if port == 8871 else untrusted_key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
        servers.append(server)
        threading.Thread(target=server.serve_forever, daemon=True).start()
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    print('READY: WebSocket 8870; trusted TLS 8871; untrusted TLS 8872', flush=True)
    stop.wait()
finally:
    for server in servers: server.server_close()
