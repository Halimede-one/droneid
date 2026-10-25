#!/usr/bin/env python3
"""Serve the local DroneID bench dashboard at http://127.0.0.1:8766."""

import argparse
from importlib.util import find_spec
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
from urllib.parse import parse_qs, urlparse

from dashboard import Manager, ROOT
import bench
import guard_bench
import guard_fixture


def handler_for(manager):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, format, *args):
            pass

        def allowed(self, mutation=False):
            port = self.server.server_port
            hosts = {f'127.0.0.1:{port}', f'localhost:{port}'}
            host = self.headers.get('Host')
            origin = self.headers.get('Origin')
            if host not in hosts or (origin and origin not in {'http://' + h for h in hosts}):
                self.send_json(403, {'error':'This dashboard accepts local requests only.'})
                return False
            if mutation and not secrets.compare_digest(self.headers.get('X-DroneID-Token', ''), manager.token):
                self.send_json(403, {'error':'Refresh this dashboard before making changes.'})
                return False
            return True

        def response_headers(self, status, content_type, length=None):
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'")
            if length is not None:
                self.send_header('Content-Length', str(length))

        def send_json(self, status, value):
            data = json.dumps(value, allow_nan=False).encode()
            self.response_headers(status, 'application/json', len(data))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if not self.allowed(): return
            url = urlparse(self.path)
            try:
                if url.path == '/api/state':
                    self.send_json(200, manager.snapshot() | {'token':manager.token, 'firmware':manager.firmware.info(),
                        'guards':guard_bench.catalog(), 'guard_fixture':guard_fixture.info()})
                elif url.path == '/api/events':
                    self.response_headers(200, 'text/event-stream')
                    self.send_header('Connection', 'keep-alive')
                    self.end_headers()
                    self.connection.settimeout(5)
                    while not manager.stop.is_set():
                        data = json.dumps(manager.snapshot(), allow_nan=False)
                        self.wfile.write(('event: state\ndata: ' + data + '\n\n').encode())
                        self.wfile.flush()
                        manager.stop.wait(1)
                elif url.path == '/README.md':
                    data=(ROOT/'README.md').read_bytes()
                    self.response_headers(200,'text/plain; charset=utf-8',len(data))
                    self.end_headers(); self.wfile.write(data)
                elif url.path == '/guard/README.md':
                    data=(ROOT/'guard/README.md').read_bytes()
                    self.response_headers(200,'text/plain; charset=utf-8',len(data))
                    self.end_headers(); self.wfile.write(data)
                elif url.path.startswith('/api/devices/'):
                    parts = url.path.strip('/').split('/')
                    if len(parts) != 4 or parts[3] not in ('logs','download'):
                        raise KeyError('Unknown endpoint.')
                    device = manager.device(parts[2])
                    with device.lock:
                        logs = list(device.logs)
                        cursor = device.log_cursor
                    if parts[3] == 'logs':
                        after = max(0, int(parse_qs(url.query).get('after',['0'])[0]))
                        self.send_json(200, {'logs':[x for x in logs if x['cursor'] > after], 'cursor':cursor,
                            'truncated':bool(logs and after and logs[0]['cursor'] > after + 1)})
                    else:
                        data = ''.join(json.dumps(x['record'])+'\n' for x in logs).encode()
                        self.response_headers(200,'application/x-ndjson',len(data))
                        self.send_header('Content-Disposition', 'attachment; filename="droneid-'+parts[2]+'.jsonl"')
                        self.end_headers()
                        self.wfile.write(data)
                else:
                    files = {'/':'index.html','/app.js':'app.js','/style.css':'style.css','/favicon.svg':'favicon.svg'}
                    if url.path not in files: raise KeyError('Not found.')
                    name = files[url.path]
                    data = (ROOT/'web'/name).read_bytes()
                    mime = {'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8', '.css':'text/css; charset=utf-8','.svg':'image/svg+xml'}[Path(name).suffix]
                    self.response_headers(200,mime,len(data)); self.end_headers(); self.wfile.write(data)
            except KeyError as exc:
                self.send_json(404, {'error':str(exc)})
            except (ValueError, OSError) as exc:
                if url.path != '/api/events': self.send_json(400, {'error':str(exc)})

        def do_POST(self):
            if not self.allowed(mutation=True): return
            try:
                length = int(self.headers.get('Content-Length','0'))
                if not 0 < length <= 4096 or self.headers.get('Content-Type','').split(';')[0] != 'application/json':
                    raise ValueError('Expected a small JSON request.')
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict): raise ValueError('Expected a JSON object.')
                parts = urlparse(self.path).path.strip('/').split('/')
                if len(parts) != 4 or parts[:2] != ['api','devices']: raise KeyError('Unknown endpoint.')
                device = manager.device(parts[2])
                if parts[3] == 'configure':
                    job = device.configure(body)
                elif parts[3] == 'uart':
                    job = device.configure_uart(body)
                elif parts[3] == 'flash':
                    job = device.flash(manager.firmware, body.get('fingerprint'))
                elif parts[3] == 'provision':
                    job = device.provision(manager.firmware, body)
                elif parts[3] == 'guard-flash':
                    job = device.flash_guard(body.get('board'), body.get('fingerprint'))
                elif parts[3] == 'guard-check':
                    job = device.check_guard(body.get('board'))
                elif parts[3] == 'guard-uart-test':
                    job = device.test_guard_uart(body)
                elif parts[3] == 'identify':
                    status = device.identify()
                    self.send_json(200, {'ok':True, 'status':status}); return
                elif parts[3] == 'monitor':
                    if type(body.get('enabled')) is not bool: raise ValueError('enabled must be boolean.')
                    device.set_monitoring(body['enabled'])
                    self.send_json(200, {'ok':True}); return
                else: raise KeyError('Unknown action.')
                self.send_json(202, {'job_id':job})
            except KeyError as exc:
                self.send_json(404, {'error':str(exc)})
            except (ValueError, TypeError, OSError) as exc:
                self.send_json(400, {'error':str(exc)})
            except RuntimeError as exc:
                self.send_json(409, {'error':str(exc)})

        def do_OPTIONS(self):
            self.send_json(403, {'error':'Cross-origin access is disabled.'})
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8766)
    args = parser.parse_args()
    bench.serial_modules()
    if find_spec('esptool') is None:
        parser.exit(1, 'Install dependencies: python -m pip install -r tools/requirements.txt\n')
    manager = Manager()
    try:
        server = ThreadingHTTPServer(('127.0.0.1',args.port),handler_for(manager))
    except OSError as exc:
        parser.exit(1, f'Cannot start on port {args.port}: {exc}. Try --port with another port.\n')
    manager.thread.start()
    print(f'DroneID bench: http://127.0.0.1:{server.server_port}',flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        manager.close()
        server.server_close()


if __name__ == '__main__':
    main()
