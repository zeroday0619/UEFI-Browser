#!/usr/bin/env python3
"""Validate the native resource transport against a real HTTP server."""
import http.server
import os
import pathlib
import subprocess
import threading
import ssl
import tempfile

project = pathlib.Path(__file__).resolve().parents[1]
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    unsafe_requests_received = 0
    def do_GET(self):
        if self.path == '/must-not-send':
            Handler.unsafe_requests_received += 1
        if self.path == '/headers':
            valid = (self.headers.get_all('Accept') == ['text/css'] and
                     self.headers.get('X-UEFI-Probe') == 'header-forwarding' and
                     self.headers.get_all('Accept-Encoding') == ['identity'] and
                     self.headers.get_all('Host') == ['10.0.2.2:18765'])
            body = b'headers' if valid else b'invalid headers'
            self.send_response(200 if valid else 400)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path in ('/truncated', '/bad-chunk', '/oversized', '/encoded'):
            headers, body = {
                '/truncated': (b'Content-Length: 20\r\n', b'short'),
                '/bad-chunk': (b'Transfer-Encoding: chunked\r\n', b'z\r\nbad\r\n0\r\n\r\n'),
                '/oversized': (b'Content-Length: 8388609\r\n', b''),
                '/encoded': (b'Content-Encoding: gzip\r\nContent-Length: 0\r\n', b''),
            }[self.path]
            self.wfile.write(b'HTTP/1.1 200 OK\r\n' + headers + b'Connection: close\r\n\r\n' + body)
        elif self.path == '/chunked':
            self.send_response(200)
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            self.wfile.write(b'9\r\nchunk-one\r\na\r\n\x00chunk-two\r\n0\r\n\r\n')
        else:
            status, body = {
                '/document': (200, b'<!doctype html><title>UEFI network</title><p>Live HTTP</p>'),
                '/missing': (404, b'missing'),
                '/redirect': (302, b''),
                '/empty': (204, b''),
                '/choices': (300, b'choices'),
            }.get(self.path, (404, b'missing'))
            self.send_response(status)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            if status == 302:
                self.send_header('Location', '/document')
            self.end_headers()
            self.wfile.write(body)
        self.close_connection = True
    def log_message(self, *arguments):
        pass

class UntrustedHandler(Handler):
    requests_received = 0
    def do_GET(self):
        type(self).requests_received += 1
        super().do_GET()

with tempfile.TemporaryDirectory(prefix='uefi-network-') as directory:
    certificate = pathlib.Path(directory) / 'certificate.pem'
    key = pathlib.Path(directory) / 'key.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-keyout', str(key), '-out', str(certificate), '-days', '1',
                    '-subj', '/CN=10.0.2.2', '-addext', 'subjectAltName=IP:10.0.2.2'],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    with http.server.ThreadingHTTPServer(('127.0.0.1', 18765), Handler) as server, \
         http.server.ThreadingHTTPServer(('127.0.0.1', 18766), UntrustedHandler) as tls_server:
        tls_server.socket = context.wrap_socket(tls_server.socket, server_side=True)
        for listener in (server, tls_server):
            threading.Thread(target=listener.serve_forever, daemon=True).start()
        environment = dict(os.environ,
            BUILD_DIRECTORY=str(project / 'build/network'),
            REQUIRED_MARKER='PASS: untrusted TLS endpoint rejected')
        try:
            subprocess.run(['python3', str(project / 'test-qemu.py')], env=environment, check=True)
            assert Handler.unsafe_requests_received == 0, 'Unsafe headers reached transport'
            assert UntrustedHandler.requests_received == 0, 'HTTP reached the untrusted server'
            print('PASS: rejected TLS connection sent no HTTP request')
        finally:
            server.shutdown()
            tls_server.shutdown()
