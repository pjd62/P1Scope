#!/usr/bin/env python3
"""Verify direct meter access without Avahi; accepts a binary or container command."""
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

command = sys.argv[1:]
if not command:
    raise SystemExit('Usage: python3 tests/direct_meter.py COMMAND [ARGS...]')
report = json.loads((Path(__file__).resolve().parent.parent / 'P1.json').read_text())

class Meter(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != '/api/v1/data':
            self.send_error(404)
            return
        body = json.dumps(report).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass

meter = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Meter)
threading.Thread(target=meter.serve_forever, daemon=True).start()
try:
    for cli in [False, True]:
        endpoint = f'127.0.0.1:{meter.server_port}'
        env = dict(os.environ, P1SCOPE_METER_ADDRESS=endpoint if not cli else 'invalid-env-address',
                   DBUS_SYSTEM_BUS_ADDRESS='unix:path=/nonexistent-p1scope-test-bus')
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        arguments = ['--server', '--port', str(port)]
        if cli:
            arguments += ['--meter-address', endpoint]
        with tempfile.TemporaryFile(mode='w+') as logs:
            process = subprocess.Popen(command + arguments, env=env, stdout=subprocess.DEVNULL, stderr=logs)
            try:
                deadline = time.monotonic() + 15
                while True:
                    try:
                        with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/state', timeout=2) as response:
                            state = json.load(response)
                        if state['measurement']['valid']:
                            break
                    except OSError:
                        pass
                    if process.poll() is not None or time.monotonic() > deadline:
                        logs.seek(0)
                        raise AssertionError('Direct meter startup failed: ' + logs.read())
                    time.sleep(.1)
                assert state['simulation'] is False
                assert state['measurement']['grid'] == report['active_power_w']
                assert state['measurement']['wifiSsid'] == report['wifi_ssid']
            finally:
                process.terminate()
                process.wait(timeout=15)
            logs.seek(0)
            output = logs.read()
            assert 'Avahi discovery skipped' in output
            assert 'Could not create Avahi client' not in output
    print('Direct meter checks passed: environment, CLI override, HTTP readings, no D-Bus access, shutdown.')
finally:
    meter.shutdown()
    meter.server_close()
