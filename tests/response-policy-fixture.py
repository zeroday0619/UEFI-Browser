"""Serve cross-origin resources with deterministic ORB, CORP and COEP policies."""
import json
import struct
import time
import zlib
from urllib.parse import parse_qs, urlsplit


def handle(handler):
    parsed = urlsplit(handler.path)
    if not parsed.path.startswith('/response-policy/'):
        return False
    parameters = parse_qs(parsed.query)
    value = lambda name, default='': parameters.get(name, [default])[0]
    kind = value('kind', 'script')
    content_type = 'text/javascript'
    status = 200
    body = b'window.policyScriptCount=(window.policyScriptCount||0)+1;'
    extra_headers = []
    if parsed.path.endswith('/delayed'):
        content_type = 'text/plain'
        body = b'complete delayed response'
        if value('stage') == 'headers':
            time.sleep(12 if value('expire') else 6)
    elif parsed.path.endswith('/document'):
        content_type = 'text/html'
        extra_headers.append(('Cross-Origin-Embedder-Policy', value('coep')))
        remote = 'https://10.0.2.2:8768/response-policy/resource'
        target = remote + '?corp=' + value('corp')
        # Responses report the actual Cookie header so credentialless is observable.
        target += '&kind=cookie'
        body = ('<!doctype html><script>const script=document.createElement("script");'
                'script.onload=()=>parent.postMessage({policy:true,loaded:true,cookie:window.policyCookie},"*");'
                'script.onerror=()=>parent.postMessage({policy:true,loaded:false},"*");'
                + ('script.crossOrigin="anonymous";' if value('cors') else '')
                + 'script.src=' + json.dumps(target) + ';document.head.append(script);</script>').encode()
        if value('worker'):
            body = b'''<!doctype html><script>
              const worker=new Worker('/response-policy/worker.js');
              worker.onmessage=event=>{parent.postMessage({policy:true,worker:event.data},'*');worker.terminate()};
              worker.onerror=event=>parent.postMessage({policy:true,error:event.message},'*');
              worker.postMessage('test');</script>'''
    elif parsed.path.endswith('/worker.js'):
        body = b'''onmessage=async()=>{try{
          const remote='https://10.0.2.2:8768/response-policy/resource';
          let denied=false;
          try{await fetch(remote+'?corp=same-origin',{mode:'no-cors'})}catch{denied=true}
          let allowed,blocked=false;
          try{allowed=await fetch(remote,{mode:'no-cors'})}catch{blocked=true}
          postMessage({denied,blocked,opaque:allowed?.type==='opaque',body:allowed?await allowed.text():null});
        }catch(error){postMessage({error:String(error)})}};'''
    elif parsed.path.endswith('/redirect'):
        status = 302
        extra_headers.append(('Location', value('to')))
        body = b''
    elif kind == 'css':
        content_type = 'text/css'
        body = b'#policy-target { color: rgb(12, 34, 56); }'
    elif kind == 'css-duplicate':
        content_type = 'application/octet-stream'
        extra_headers.append(('Content-Type', 'text/css'))
        body = b'#policy-target { background-color: rgb(56, 34, 12); }'
    elif kind == 'script-duplicate':
        content_type = 'application/json'
        extra_headers.append(('Content-Type', 'text/javascript'))
    elif kind == 'image':
        content_type = 'image/png'
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        body = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(b'\0\xff\0\0')) + chunk(b'IEND', b''))
    elif kind == 'html':
        body = b'<!doctype html><html><body>private response</body></html>'
    elif kind == 'json':
        body = b'{"private":"response"}'
    elif kind == 'xml':
        body = b'<?xml version="1.0"?><private>response</private>'
    elif kind == 'nosniff':
        content_type = 'text/plain'
        extra_headers.append(('X-Content-Type-Options', 'nosniff'))
    elif kind == 'never':
        content_type = 'application/pdf'
    elif kind == 'range':
        content_type = 'application/octet-stream'
        status = 206
        body = b'window.policyRangeExecuted=true;'
        extra_headers.append(('Content-Range', f'bytes 100-{99+len(body)}/500'))
    elif kind == 'cookie':
        body = ('window.policyCookie=' + json.dumps(handler.headers.get('Cookie', '')) + ';').encode()
    handler.send_response(status)
    handler.send_header('Content-Type', content_type)
    handler.send_header('Content-Length', str(len(body)))
    handler.send_header('Cache-Control', 'no-store')
    handler.send_header('Access-Control-Allow-Origin', '*')
    if value('corp'):
        handler.send_header('Cross-Origin-Resource-Policy', value('corp'))
    for name, header in extra_headers:
        handler.send_header(name, header)
    handler.end_headers()
    if parsed.path.endswith('/delayed') and value('stage') == 'body':
        handler.wfile.flush()
        time.sleep(6)
    handler.wfile.write(body)
    return True
