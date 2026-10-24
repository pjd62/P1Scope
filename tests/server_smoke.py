#!/usr/bin/env python3
"""Exercise the real headless executable and its HTTP API in simulation mode."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='p1scope-http-test-') as tmp:
    groups = Path(tmp) / 'groups.json'
    groups.write_text(json.dumps({'groups': [{'number': 1, 'name': '<Kitchen>', 'phase': 1}]}))
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    env = dict(os.environ)
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    process = subprocess.Popen([str(binary), '--simulation', '--port', str(port), '--config', str(groups)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    base = f'http://127.0.0.1:{port}'
    def request(path, method='GET', headers=None):
        req = urllib.request.Request(base + path, method=method, headers=headers or {})
        try:
            with urllib.request.urlopen(req, timeout=4) as response:
                return response.status, response.headers, response.read()
        except urllib.error.HTTPError as error:
            return error.code, error.headers, error.read()
    try:
        deadline = time.monotonic() + 10
        while True:
            try:
                status, headers, body = request('/api/state')
                state = json.loads(body)
                if state['measurement']['valid'] and state['hue']['sensors']:
                    break
            except (OSError, ValueError):
                pass
            if process.poll() is not None or time.monotonic() > deadline:
                raise AssertionError('Server failed to become ready')
            time.sleep(.1)
        assert state['hue']['lightsAvailable'] is True
        assert len(state['hue']['lights']) == 3
        assert len({light['roomId'] for light in state['hue']['lights']}) == 3
        assert all(light['room'] for light in state['hue']['lights'])
        assert any(light['on'] for light in state['hue']['lights'])
        assert any(not light['on'] for light in state['hue']['lights'])
        assert all(0 <= light['brightness'] <= 100 for light in state['hue']['lights'])
        assert status == 200 and state['simulation'] is True
        light = state['hue']['lights'][0]
        target = not light['on']
        route = '/api/hue/lights/' + light['id'] + ('/on' if target else '/off')
        assert request(route, 'POST')[0] == 403
        assert request('/api/hue/lights/bad%2Fid/on', 'POST', {'X-P1Scope-Action': 'light'})[0] == 400
        rejected = json.loads(request('/api/hue/lights/unknown/on', 'POST', {'X-P1Scope-Action': 'light'})[2])
        assert rejected['ok'] is False
        changed = json.loads(request(route, 'POST', {'X-P1Scope-Action': 'light'})[2])
        assert changed['ok'] is True
        deadline = time.monotonic() + 5
        while True:
            updated = json.loads(request('/api/state')[2])['hue']
            if not updated['pendingLight']:
                break
            assert time.monotonic() < deadline
            time.sleep(.05)
        assert next(item for item in updated['lights'] if item['id'] == light['id'])['on'] == target
        assert updated['lightActionStatus'] == 'Simulated light changed'
        brightness_route = '/api/hue/lights/' + light['id'] + '/brightness-35'
        assert request(brightness_route, 'POST')[0] == 403
        for invalid in ['brightness-101', 'brightness--1', 'brightness-nan', 'brightness-', 'brightness-35.5']:
            assert request('/api/hue/lights/' + light['id'] + '/' + invalid, 'POST', {'X-P1Scope-Action': 'light'})[0] == 400
        assert json.loads(request(brightness_route, 'POST', {'X-P1Scope-Action': 'light'})[2])['ok']
        deadline = time.monotonic() + 5
        while True:
            updated = json.loads(request('/api/state')[2])['hue']
            if not updated['pendingLight']:
                break
            assert time.monotonic() < deadline
            time.sleep(.05)
        changed_light = next(item for item in updated['lights'] if item['id'] == light['id'])
        assert changed_light['brightness'] == 35
        assert changed_light['on'] == target




        assert state['historySeconds'] == 3600 and state['samples']
        assert len(state['measurement']['phases']) == 3
        assert len(state['hue']['sensors']) == 3
        assert state['hue']['sensors'][1]['history'][-1]['value'] >= 18
        assert state['groups'][0]['name'] == '<Kitchen>'
        assert b'application_key' not in body and b'bridge_ip' not in body
        assert 'no-store' in headers['Cache-Control']
        for asset in ['/', '/dashboard.js', '/dashboard.css', '/vendor/d3.min.js']:
            status, _, data = request(asset)
            assert status == 200 and len(data) > 100, asset
        for secret in ['/.hue-credentials.json', '/groups.json', '/../.hue-credentials.json', '/%2e%2e/main.cpp']:
            assert request(secret)[0] == 404
        assert request('/api/groups/reload', 'POST')[0] == 403
        groups.write_text('{invalid')
        result = json.loads(request('/api/groups/reload', 'POST', {'X-P1Scope-Action': 'reload'})[2])
        assert result['ok'] is False
        assert json.loads(request('/api/state')[2])['groups'][0]['name'] == '<Kitchen>'
        groups.write_text(json.dumps({'groups': [{'number': 2, 'name': 'Updated', 'phase': None}]}))
        assert json.loads(request('/api/groups/reload', 'POST', {'X-P1Scope-Action': 'reload'})[2])['ok']
        assert json.loads(request('/api/state')[2])['groups'][0]['phase'] is None
        first = state['samples'][-1]['t']
        time.sleep(1.1)
        assert json.loads(request('/api/state')[2])['samples'][-1]['t'] > first
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=8) == 0
        print('HTTP smoke checks passed: headless startup, readings, sensors, assets, reload, secret isolation, shutdown.')
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        diagnostics = process.stderr.read().decode()
        if process.returncode == 0:
            assert f'Web dashboard URL: http://127.0.0.1:{port}/' in diagnostics
        if process.returncode:
            print(diagnostics, file=sys.stderr)
