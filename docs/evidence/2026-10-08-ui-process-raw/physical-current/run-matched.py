#!/usr/bin/env python3
"""Replay four serial physical view runs after the caller's native quiet gate."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--gate', required=True, help='Root-inspected native functional/quiet receipt')
args = parser.parse_args()
gate_path = Path(args.gate).resolve(strict=True)
gate = json.loads(gate_path.read_text())
protocol = json.loads((root / 'protocol.json').read_text())
assert gate['sourceCommit'] == protocol['candidateCommit']
assert gate['functionalWorkersQuiet'] is True and gate['buildsQuiet'] is True
assert len(gate['nativeReceipts']) == 3
for receipt in gate['nativeReceipts']:
    path = Path(receipt['path']).resolve(strict=True)
    assert hashlib.sha256(path.read_bytes()).hexdigest() == receipt['sha256']
commands = json.loads((root / 'view-commands.json').read_text())
receipt_path = root / 'matched-execution.json'
assert not receipt_path.exists()
execution = {'schemaVersion': 1, 'startedUtc': datetime.now(timezone.utc).isoformat(), 'status': 'running', 'gate': {'path': str(gate_path), 'sha256': hashlib.sha256(gate_path.read_bytes()).hexdigest()}, 'protocolSha256': hashlib.sha256((root / 'protocol.json').read_bytes()).hexdigest(), 'runnerSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'runs': []}
def persist():
    receipt_path.write_text(json.dumps(execution, indent=2) + '\n')
def host_conditions():
    wanted = {'MemTotal', 'MemAvailable', 'SwapTotal', 'SwapFree'}
    memory = {key: int(value.split()[0]) * 1024 for line in Path('/proc/meminfo').read_text().splitlines() for key, value in [line.split(':', 1)] if key in wanted}
    return {'memoryBytes': memory, 'loadAverage': Path('/proc/loadavg').read_text().split()[:3]}
persist()
for label in commands['runOrder']:
    argv = commands['commands'][label].copy()
    if label.startswith('split-'):
        for receipt in gate['nativeReceipts']:
            argv += ['--acceptance-receipt', receipt['path']]
    before = host_conditions()
    started = time.monotonic()
    log_path = root / (label + '.log')
    with log_path.open('x') as log:
        result = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT)
    row = {'label': label, 'command': argv, 'exitCode': result.returncode, 'elapsedSeconds': time.monotonic() - started, 'log': str(log_path), 'logSha256': hashlib.sha256(log_path.read_bytes()).hexdigest(), 'hostBefore': before, 'hostAfter': host_conditions()}
    execution['runs'].append(row)
    persist()
    print(json.dumps(row), flush=True)
    if result.returncode:
        execution['status'] = 'failed-preserved-no-automatic-retry'
        persist()
        sys.exit(result.returncode)
execution['status'] = 'complete'
execution['completedUtc'] = datetime.now(timezone.utc).isoformat()
persist()
subprocess.run([sys.executable, str(root / 'compare.py')], check=True)
