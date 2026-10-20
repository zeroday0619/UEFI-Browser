"""Deterministic HTTP methods, CORS and redirect responses for firmware tests."""
from urllib.parse import urlsplit, parse_qs
from collections import Counter
import json

requests = Counter()


def handle(handler):
    parsed = urlsplit(handler.path)
    if not parsed.path.startswith('/network/'):
        return False
    parameters = parse_qs(parsed.query)
    length = int(handler.headers.get('Content-Length', '0'))
    if not 0 <= length <= 8 * 1024 * 1024:
        handler.send_error(413)
        return True
    body = handler.rfile.read(length) if length else b''
    requests[(parsed.path, handler.command)] += 1
    origin = handler.headers.get('Origin', '')
    record = {'method': handler.command, 'body': body.decode('utf-8', errors='replace'),
              'hex': body.hex(), 'headers': dict(handler.headers), 'path': parsed.path}
    print('NETWORK_REQUEST ' + json.dumps(record), flush=True)
    status = 200
    content_type = 'application/json'
    if parsed.path == '/network/count':
        record = {'count': requests[(parameters['path'][0], parameters['method'][0])]}
    payload = json.dumps(record).encode()
    if parsed.path == '/network/form':
        content_type = 'text/html'
        payload = ('<!doctype html><title>Form result</title><h1>Form submitted</h1><pre>' +
                   body.decode('ascii') + '</pre><script>console.log("NETWORK FORM " + ' +
                   json.dumps(json.dumps(record)) + ')</script>').encode()
    handler.send_response(status if parsed.path != '/network/redirect' else int(parameters.get('code', ['302'])[0]))
    if parsed.path == '/network/redirect':
        handler.send_header('Location', parameters['to'][0])
        payload = b''
    if origin and parsed.path != '/network/no-cors':
        handler.send_header('Access-Control-Allow-Origin', '*' if parsed.path == '/network/wildcard' else origin)
        if parsed.path == '/network/duplicate':
            handler.send_header('Access-Control-Allow-Origin', origin)
        handler.send_header('Access-Control-Allow-Credentials', 'true')
    if handler.command == 'OPTIONS':
        methods = 'GET' if parsed.path == '/network/denied-method' else 'GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS'
        headers = 'content-type' if parsed.path == '/network/denied-header' else 'content-type, x-probe, authorization'
        handler.send_header('Access-Control-Allow-Methods', methods)
        handler.send_header('Access-Control-Allow-Headers', headers)
        handler.send_header('Access-Control-Max-Age', '0')
    handler.send_header('Access-Control-Expose-Headers', 'X-Visible')
    handler.send_header('X-Visible', 'visible')
    handler.send_header('X-Hidden', 'hidden')
    handler.send_header('Content-Type', content_type)
    handler.send_header('Content-Length', str(len(payload)))
    handler.send_header('Cache-Control', 'no-store')
    handler.end_headers()
    if handler.command != 'HEAD':
        handler.wfile.write(payload)
    return True
