#!/usr/bin/python3
"""Synthetic stdio peer for testing adapter failure disposition; no SDK or account."""
import json
import os
import sys
import time
if sys.argv[1:] == ['--session']:
    # One process answers every request line until its input closes.
    served = 0
    for line in sys.stdin.buffer:
        request, served = json.loads(line), served + 1
        answer = {'status': 'absent', 'objectReference': {'pid': os.getpid(), 'served': served}, 'concurrencyToken': '', 'error': {}}
        print(json.dumps({'ok': True, 'result': answer}), flush=True)
    sys.exit(0)
request = json.loads(sys.stdin.buffer.readline())
case = request['destination']['settings'].get('fixture', 'auth')
if case == 'bad-progress-negative':
    print(json.dumps({'event': 'progress', 'phase': 'uploading', 'bytesTransferred': -1}), flush=True)
elif case == 'flood':
    sys.stdout.write((json.dumps({'event': 'progress', 'phase': 'uploading', 'bytesTransferred': 1}) + '\n') * 40000)
    sys.stdout.flush()
if case == 'timeout':
    time.sleep(3)
elif case == 'malformed':
    print('malformed', flush=True)
else:
    print(json.dumps({'ok': False, 'error': {'code': 'authentication_required',
        'message': 'Synthetic account is disconnected.', 'details': {}}}), flush=True)
    if case == 'crash':
        sys.exit(1)
