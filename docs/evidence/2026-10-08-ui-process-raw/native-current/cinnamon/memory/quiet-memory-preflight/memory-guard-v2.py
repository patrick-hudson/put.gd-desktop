#!/usr/bin/env python3
"""Run an existing-process sample only when the root dispatches this script.

No product writes, UI input, clipboard access or process termination. A new
operation invalidates this sample; it never changes an earlier functional receipt.
Only operation identifiers/counts/states are inspected, never payload contents.
"""
import datetime
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def main():
    plan = json.loads(Path(sys.argv[1]).read_text())
    argv = plan['argv']
    peer = plan['expectedSocketPeer']
    endpoint = argv[argv.index('--socket') + 1]
    cli = argv[argv.index('--cli') + 1]
    target = Path(argv[argv.index('--output') + 1])
    output = target.with_name(target.name + '-guard')
    output.mkdir(mode=0o700, exist_ok=False)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(3)
        connection.connect(endpoint)
        actual_pid, actual_uid, _ = struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    assert actual_pid == peer['pid'] and actual_uid == peer['uid']

    def inspect():
        checked_at = utc()
        result = subprocess.run([cli, '--socket', endpoint, 'operation.recent'], capture_output=True, text=True, timeout=10)
        envelope = json.loads(result.stdout)
        if result.returncode or not envelope.get('ok'):
            raise RuntimeError('Operation-count inspection failed')
        operations = envelope['result']['operations']
        states = sorted([{'id': operation['id'], 'state': operation['state'], 'copyStatus': operation.get('copyStatus')} for operation in operations], key=lambda row: row['id'])
        ids = [row['id'] for row in states]
        ui_result = subprocess.run([cli, '--socket', endpoint, 'ui.snapshot'], capture_output=True, text=True, timeout=10)
        ui_envelope = json.loads(ui_result.stdout)
        assert ui_result.returncode == 0 and ui_envelope.get('ok')
        snapshot = ui_envelope['result']; diagnostics = snapshot['diagnostics']
        assert snapshot['build']['gitCommit'] == plan['sourceCommit']
        resident_proc = Path('/proc') / str(peer['pid'])
        actual_start = int((resident_proc / 'stat').read_text().rsplit(')', 1)[1].split()[19])
        assert actual_start == plan['resident']['startTicks']
        assert hashlib.sha256((resident_proc / 'exe').read_bytes()).hexdigest() == plan['resident']['sha256']
        terminal = all(operation['state'] in ('completed', 'failed', 'cancelled', 'partial', 'interrupted')
                       and operation.get('copyStatus') in ('copied', 'failed', 'suppressed', 'no_result')
                       for operation in operations)
        return {'timestampUtc': checked_at, 'monotonicSeconds': time.monotonic(),
                'operationCount': len(ids), 'operationIds': ids, 'operations': states, 'allTerminal': terminal,
                'ui': diagnostics['ui'], 'trayAvailable': diagnostics['trayAvailable'],
                'operationIdsSha256': hashlib.sha256(json.dumps(ids, separators=(',', ':')).encode()).hexdigest()}

    baseline = inspect()
    if not baseline['allTerminal'] or baseline['ui']['pid'] != 0 or baseline['ui']['state'] != 'stopped' or not baseline['trayAvailable']:
        raise RuntimeError('An operation was still active before quiet sampling')
    observations = [baseline]
    failures = []
    started = time.monotonic()
    with (output / 'sampler.stdout').open('w') as stdout, (output / 'sampler.stderr').open('w') as stderr:
        process = subprocess.Popen(argv, stdout=stdout, stderr=stderr)
        while process.poll() is None:
            time.sleep(5)
            try:
                observed = inspect()
                observed['matchesBaseline'] = observed['operations'] == baseline['operations'] and observed['allTerminal'] and observed['ui'] == baseline['ui'] and observed['ui']['pid'] == 0 and observed['trayAvailable']
                observations.append(observed)
                if not observed['matchesBaseline']:
                    failures.append({'timestampUtc': observed['timestampUtc'], 'reason': 'operation inventory or active state changed'})
                with (output / 'operation-counts.jsonl').open('a') as stream:
                    stream.write(json.dumps(observed) + '\n')
            except Exception as error:
                failures.append({'timestampUtc': utc(), 'reason': str(error)})
            if time.monotonic() - started > 150:
                failures.append({'timestampUtc': utc(), 'reason': 'Sampler exceeded bounded expected duration; app untouched'})
                break
        result_code = process.poll()
    final = inspect()
    if final['operations'] != baseline['operations'] or not final['allTerminal'] or final['ui'] != baseline['ui'] or final['ui']['pid'] != 0 or not final['trayAvailable']:
        failures.append({'timestampUtc': final['timestampUtc'], 'reason': 'final operation inventory or active state changed'})
    receipt = {'schemaVersion': 2, 'guardScriptSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'planSha256': hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest(), 'startedUtc': baseline['timestampUtc'], 'finishedUtc': utc(),
               'workloadLabel': plan['label'], 'expectedResident': plan['resident'],
               'validatedPeer': {'pid': actual_pid, 'uid': actual_uid}, 'samplerCommand': argv,
               'baseline': baseline, 'final': final, 'observations': observations,
               'pollIntervalSeconds': 5, 'samplerExitCode': result_code,
               'sampleValid': result_code == 0 and not failures,
               'invalidations': failures,
               'scope': 'VM post-use sample including the user-confirmed manual region capture; no physical cold-idle or matched empty-view claim.',
               'limits': ['Operation inventory is sampled every five seconds. IDs are checked as well as count, so replacement at a bounded recent-list limit cannot silently pass.',
                          'Read-only observer CLI calls add bounded observation work; no payloads are read or exported.']}
    (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({'sampleValid': receipt['sampleValid'], 'samplerExitCode': result_code,
                      'baselineOperationCount': baseline['operationCount'], 'finalOperationCount': final['operationCount'],
                      'guardReceipt': str(output / 'receipt.json')}))
    return 0 if receipt['sampleValid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
