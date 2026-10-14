#!/usr/bin/python3
"""Synthetic stdio peer for testing adapter failure disposition; no SDK or account."""
import json
import sys
import time
request = json.loads(sys.stdin.buffer.readline())
case = request['destination']['settings'].get('fixture', 'auth')
if case == 'budget-delay':
    time.sleep(1.2)
    print(json.dumps({'ok': False, 'error': {'code': 'authentication_required', 'message': 'Synthetic authentication failure', 'details': {'attemptTimeoutMs': request.get('requestTimeoutMs')}}}), flush=True)
    sys.exit(0)
if case == 'progress':
    for event in [{'event': 'progress', 'phase': 'preparing'},
                  {'event': 'progress', 'phase': 'uploading', 'bytesTransferred': 1048576},
                  {'event': 'progress', 'phase': 'uploading', 'bytesTransferred': 524288},
                  {'event': 'progress', 'phase': 'finalizing', 'bytesTransferred': 2097152}]:
        print(json.dumps(event), flush=True)
if case == 'cancel-progress':
    print(json.dumps({'event': 'progress', 'phase': 'uploading', 'bytesTransferred': 1}), flush=True)
    time.sleep(3)
elif case.startswith('bad-progress-'):
    fields = {'negative': {'bytesTransferred': -1}, 'fraction': {'bytesTransferred': 1.5},
              'too-large': {'bytesTransferred': 9007199254740992},
              'extra': {'bytesTransferred': 1, 'url': 'https://example.invalid'},
              'total': {'bytesTransferred': 2, 'bytesTotal': 1},
              'phase': {'phase': 'completed'}}[case.removeprefix('bad-progress-')]
    print(json.dumps({'event': 'progress', 'phase': 'uploading', **fields}), flush=True)
elif case == 'flood':
    sys.stdout.write((json.dumps({'event': 'progress', 'phase': 'uploading', 'bytesTransferred': 1}) + '\n') * 40000)
    sys.stdout.flush()
elif case == 'after-result':
    print(json.dumps({'ok': False, 'error': {'code': 'authentication_required', 'message': 'Synthetic', 'details': {}}}), flush=True)
    print(json.dumps({'event': 'progress', 'phase': 'uploading', 'bytesTransferred': 1}), flush=True)
if case == 'timeout':
    time.sleep(3)
elif case == 'malformed':
    print('malformed', flush=True)
else:
    print(json.dumps({'ok': False, 'error': {'code': 'authentication_required',
        'message': 'Synthetic account is disconnected.', 'details': {}}}), flush=True)
    if case == 'crash':
        sys.exit(1)
