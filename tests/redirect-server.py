#!/usr/bin/env python3
"""Serve deterministic redirects for firmware navigation regression tests."""
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import time
import os

class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *arguments, **keywords):
        super().__init__(*arguments, directory=str(Path(__file__).parent / 'input-pages'), **keywords)

    def do_GET(self):
        if self.path == '/held-data':
            release = Path(os.environ['NETWORK_RELEASE_FILE'])
            print('HELD_REQUEST_STARTED', flush=True)
            deadline = time.monotonic() + 12
            while not release.exists() and time.monotonic() < deadline:
                time.sleep(.01)
            body = b'completed'
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            print('HELD_REQUEST_FINISHED', flush=True)
            return
        if self.path == '/slow':
            print('SLOW_REQUEST_STARTED', flush=True)
            time.sleep(2)
            body = b'<h1>Delayed response completed</h1>'
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path in ('/error404', '/error429', '/error500'):
            status = int(self.path[-3:])
            body = (f'<!doctype html><title>HTTP {status}</title><h1>HTTP {status} body delivered</h1>'
                    f'<script>console.log("HTTP_ERROR_TEST {status}")</script>').encode()
            self.send_response(status)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        redirects = {'/start': (302, '/middle'), '/middle': (307, '/final')}
        if self.path in redirects:
            status, target = redirects[self.path]
            self.send_response(status)
            self.send_header('Location', target)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if self.path != '/final':
            return super().do_GET()
        body = b'''<!doctype html><title>Redirect regression</title><h1>Redirect completed</h1>
<script>addEventListener('load',()=>{const n=performance.getEntriesByType('navigation')[0];
console.log('REDIRECT_TEST '+JSON.stringify({count:n.redirectCount,start:n.redirectStart,end:n.redirectEnd,fetch:n.fetchStart,type:n.type}));});</script>'''
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

if __name__ == '__main__':
    ThreadingHTTPServer(('127.0.0.1', 8765), Handler).serve_forever()
