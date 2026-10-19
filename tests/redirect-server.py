#!/usr/bin/env python3
"""Serve deterministic redirects for firmware navigation regression tests."""
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import time
import os

class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *arguments, **keywords):
        super().__init__(*arguments, directory=str(Path(__file__).parent / 'input-pages'), **keywords)

    def do_GET(self):
        if self.path in ('/chrome-identity', '/chrome-identity.js'):
            identity = {name: self.headers.get_all(name, []) for name in
                        ('User-Agent', 'Sec-CH-UA', 'Sec-CH-UA-Platform', 'Sec-CH-UA-Mobile')}
            chrome = self.headers.get('Host', '').split(':')[0] == 'google.com'
            print('CHROME_IDENTITY ' + self.headers.get('Host', '') + ' ' + self.path + ' ' + json.dumps(identity), flush=True)
            if self.path == '/chrome-identity':
                body = ('<!doctype html><title>Browser identity</title><h1>Browser identity</h1><script>'
                        'window.expectedChrome=' + json.dumps(chrome) + ';window.documentIdentity=' + json.dumps(identity) +
                        ';</script><script src="/chrome-identity.js"></script>').encode()
                content_type = 'text/html'
            else:
                body = ('window.resourceIdentity=' + json.dumps(identity) + ';' +
                        (Path(__file__).parent / 'input-pages/chrome-identity.js').read_text()).encode()
                content_type = 'text/javascript'
            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path in ('/parallel-slow', '/parallel-fast'):
            print('PARALLEL_START ' + self.path + ' ' + str(time.monotonic()), flush=True)
            if self.path == '/parallel-slow': time.sleep(3)
            body = b'completed'
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            print('PARALLEL_DONE ' + self.path + ' ' + str(time.monotonic()), flush=True)
            return
        if self.path in ('/hints', '/hints-policy', '/hints.js'):
            hints = {name.lower(): self.headers.get_all(name, []) for name in
                     ('Sec-CH-UA', 'Sec-CH-UA-Mobile', 'Sec-CH-UA-Platform')}
            print('CLIENT_HINTS ' + self.path + ' ' + json.dumps(hints), flush=True)
            if self.path != '/hints.js':
                body = ('<!doctype html><title>Client Hints</title><h1>Client Hints</h1>'
                        '<script>window.hintsPolicy=' + json.dumps(self.path == '/hints-policy') + ';window.documentHints=' + json.dumps(hints) +
                        ';</script><script src="/hints.js"></script>').encode()
                content_type = 'text/html'
            else:
                body = ('window.resourceHints=' + json.dumps(hints) + ';' +
                        (Path(__file__).parent / 'input-pages/client-hints.js').read_text()).encode()
                content_type = 'text/javascript'
            self.send_response(200)
            if self.path == '/hints-policy':
                self.send_header('Permissions-Policy', 'ch-ua=(), ch-ua-mobile=(), ch-ua-platform=()')
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path in ('/identity', '/identity.js'):
            agents = self.headers.get_all('User-Agent', [])
            print('USER_AGENT ' + self.path + ' ' + json.dumps(agents), flush=True)
            identity = json.dumps(agents)
            if self.path == '/identity':
                body = ('<!doctype html><title>User-Agent test</title>'
                        '<h1>User-Agent consistency</h1><script>window.documentAgents=' + identity +
                        ';</script><script src="/identity.js"></script>').encode()
                content_type = 'text/html; charset=utf-8'
            else:
                body = ('const agents=' + identity + ';'
                        'const valid=documentAgents.length===1 && agents.length===1 && '
                        'documentAgents[0]===navigator.userAgent && agents[0]===navigator.userAgent && '
                        '/^Mozilla\\/5\\.0 \\(UEFI; x86_64\\).* Chrome\\/[0-9.]+ Safari\\/537\\.36 UEFIBrowser\\/0\\.3$/.test(navigator.userAgent);'
                        'console.log("USER_AGENT_TEST "+(valid?"PASS ":"FAIL ")+navigator.userAgent);').encode()
                content_type = 'text/javascript'
            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
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
