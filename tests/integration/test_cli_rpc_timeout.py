#!/usr/bin/env python3
"""Exercise CLI receive timeouts over real isolated HTTP sockets."""
import json
import pathlib
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

binary = str(pathlib.Path(sys.argv[1]).resolve())


def run_case(seconds, respond):
    requests = []
    errors = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                requests.append(request)
                if request['method'] == 'blockchain.getblockcount':
                    result = 7  # The CLI's initial connection probe.
                else:
                    assert request['method'] == 'timeout_fixture' and request['params'] == []
                    if not respond:
                        # No response: only the configured client timeout ends
                        # this call. An ignored option would exceed the test bound.
                        assert self.connection.recv(1) == b''
                        return
                    time.sleep(2)
                    result = {'delayed_reply_received': True}
                body = json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'result': result, 'error': None}).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception as error:
                errors.append(str(error))

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        completed = subprocess.run([binary, '-rpchost=127.0.0.1',
            '-rpcport=' + str(server.server_port), '-rpcuser=fixture', '-rpcpassword=fixture',
            '-rpcclienttimeout=' + str(seconds), 'timeout_fixture'],
            capture_output=True, text=True, timeout=5 if not respond else 15)
        assert len(requests) == 2, requests
        if respond:
            assert completed.returncode == 0, completed.stderr
            assert json.loads(completed.stdout) == {'delayed_reply_received': True}
        else:
            assert completed.returncode != 0
        assert not errors, errors
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


run_case(1, False)
run_case(10, True)
for value in ['', '0', '-1', '7201', 'one', '30junk', '999999999999']:
    completed = subprocess.run([binary, '-rpcclienttimeout=' + value, 'help'],
                               capture_output=True, text=True, timeout=5)
    assert completed.returncode != 0 and '-rpcclienttimeout' in completed.stderr, value
accepted = subprocess.run([binary, '-rpcclienttimeout=7200', 'help'],
                          capture_output=True, text=True, timeout=5)
assert accepted.returncode == 0
print('PASS configured short timeout, delayed success, strict bounds and option isolation')
