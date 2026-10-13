#!/usr/bin/python3
"""Synthetic stdio peer for testing adapter failure disposition; no SDK or account."""
import json
import sys
import time
request = json.loads(sys.stdin.buffer.readline())
case = request['destination']['settings'].get('fixture', 'auth')
if case == 'timeout':
    time.sleep(3)
elif case == 'malformed':
    print('malformed', flush=True)
else:
    print(json.dumps({'ok': False, 'error': {'code': 'authentication_required',
        'message': 'Synthetic account is disconnected.', 'details': {}}}), flush=True)
    if case == 'crash':
        sys.exit(1)
