#!/usr/bin/python3
"""Synthetic stdio peer for testing adapter failure disposition; no SDK or account."""
import json
import sys
import time
request = json.loads(sys.stdin.buffer.readline())
case = request['destination']['settings'].get('fixture', 'auth')
if case == 'inspect-account':
    if request['method'] != 'inspectAccount':
        print(json.dumps({'ok': False, 'error': {'code': 'invalid_request',
            'message': 'Connection tests may only inspect the account.', 'details': {}}}), flush=True)
    else:
        print(json.dumps({'ok': True, 'result': {'accountRootUid': 'synthetic-root',
            'folders': [{'uid': 'synthetic-folder', 'name': 'Uploads'}]}}), flush=True)
    sys.exit(0)
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
if case == 'plan-limit':
    # The helper's mapping of Proton error 200003 (MAX_FILE_SIZE_FOR_FREE_USER), #147.
    print(json.dumps({'ok': False, 'error': {'code': 'destination_file_too_large_for_plan',
        'message': 'This file is larger than your Proton plan allows for a single file.', 'details': {}}}), flush=True)
    sys.exit(0)
if case == 'plan-limit-settled':
    # The helper proved the name still free after 200003: nothing was created (#201).
    print(json.dumps({'ok': True, 'result': {'status': 'failed', 'error': {'code': 'destination_file_too_large_for_plan',
        'message': 'This file is larger than your Proton plan allows for a single file.', 'details': {}}}}), flush=True)
    sys.exit(0)
if case.startswith('share-expiry'):
    # A confirmed share result with the service's link expiry (#253).
    receipt = dict(request['receipt'], directUrl='https://example.invalid/share#synthetic')
    expires = {'share-expiry': '2026-10-27T17:48:40.000Z', 'share-expiry-invalid': 'next week'}[case]
    print(json.dumps({'ok': True, 'result': {'status': 'shared', 'receipt': receipt, 'linkAvailable': True,
        'expiresAt': expires, 'progress': {'schemaVersion': 1}, 'error': {}}}), flush=True)
    sys.exit(0)
if case == 'timeout':
    # The cancellation tests must know the request was dispatched, rather than
    # guessing how long account-lock persistence and helper startup will take.
    received = request['destination']['settings'].get('receivedFile')
    if received:
        with open(received, 'x') as marker:
            marker.write(request['method'])
    time.sleep(3)
elif case == 'malformed':
    print('malformed', flush=True)
else:
    print(json.dumps({'ok': False, 'error': {'code': 'authentication_required',
        'message': 'Synthetic account is disconnected.', 'details': {}}}), flush=True)
    if case == 'crash':
        sys.exit(1)
