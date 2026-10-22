#!/usr/bin/env python3
"""Pair with a local Hue bridge and read its temperature sensors."""
import argparse
import json
import os
from pathlib import Path
import ssl
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', default='192.168.0.145')
    parser.add_argument('--pair', action='store_true', help='Wait up to three minutes for a link-button press')
    args = parser.parse_args()
    credentials = Path(__file__).with_name('.hue-credentials.json')
    # Local Hue bridges use their own certificates. This context is scoped to this bridge.
    context = ssl._create_unverified_context()
    base = 'https://' + args.bridge

    def request(path, key=None, data=None):
        headers = {'Content-Type': 'application/json'}
        if key:
            headers['hue-application-key'] = key
        req = urllib.request.Request(base + path, headers=headers,
                                     data=json.dumps(data).encode() if data is not None else None)
        with urllib.request.urlopen(req, context=context, timeout=5) as response:
            return json.load(response)

    if args.pair:
        print('Pairing active for 3 minutes. Press the large link button on top of the bridge now.', flush=True)
        deadline = time.monotonic() + 180
        key = None
        while time.monotonic() < deadline:
            result = request('/api', data={'devicetype': 'p1scope#desktop'})
            key = next((item['success']['username'] for item in result
                        if 'username' in item.get('success', {})), None)
            if key:
                break
            if not any(item.get('error', {}).get('type') == 101 for item in result):
                raise RuntimeError('Pairing failed: ' + json.dumps(result))
            time.sleep(2)
        if not key:
            raise RuntimeError('Pairing timed out: bridge did not report a link-button press.')
        fd = os.open(credentials, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as file:
            os.fchmod(file.fileno(), 0o600)
            json.dump({'bridge_ip': args.bridge, 'application_key': key}, file)
        print('Paired. Credentials saved locally with owner-only permissions.', flush=True)
    else:
        if not credentials.exists():
            parser.error('No credentials yet. Run with --pair first.')
        saved = json.loads(credentials.read_text())
        base = 'https://' + saved['bridge_ip']
        key = saved['application_key']

    def resources(kind):
        result = request('/clip/v2/resource/' + kind, key=key)
        if result.get('errors'):
            raise RuntimeError(json.dumps(result['errors']))
        return result.get('data', [])

    devices = {device['id']: device.get('metadata', {}).get('name', device['id'])
               for device in resources('device')}
    sensors = resources('temperature')
    if not sensors:
        print('No temperature sensors reported by the bridge.')
    for sensor in sensors:
        name = devices.get(sensor.get('owner', {}).get('rid'), sensor['id'])
        temperature = sensor.get('temperature', {})
        value = temperature.get('temperature')
        valid = temperature.get('temperature_valid', False)
        print(f'{name}: {value:.2f} °C' if valid and isinstance(value, (float, int))
              else f'{name}: temperature unavailable')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        raise SystemExit(str(error)) from None
