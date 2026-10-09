#!/usr/bin/env python3
"""Serial, unchanged physical harness; preserve each result before the next run."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path('/home/patrick/work/put.gd')
WORK = ROOT / 'work/view-startup-latency'
LOG = ROOT / '.context/compound-engineering/ce-optimize/view-startup-latency/experiment-log.yaml'
HARNESS = ROOT / 'tests/performance/bench_ui_process.py'
HASH = 'a28c94b075036ea65f8eca8e068d480cff46ee959d4680593f5c4d55cce3fc40'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def save(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)
    assert json.loads(path.read_text()) == value

def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--gate', required=True)
parser.add_argument('--output', required=True)
args = parser.parse_args()
gate_path = Path(args.gate).resolve(strict=True)
gate = json.loads(gate_path.read_text())
assert gate['functionalWorkersQuiet'] and gate['buildsQuiet']
assert gate['cleanCandidate'] and len(gate['nativeReceipts']) == 3
assert sha(HARNESS) == HASH
for receipt in gate['nativeReceipts']:
    assert sha(Path(receipt['path'])) == receipt['sha256']
output = Path(args.output).resolve()
assert output.parent == WORK and not output.exists()
output.mkdir(mode=0o700)
reference = json.loads((WORK / 'reference45/provenance.json').read_text())
assert reference['head'] == '9d142919e5e6365227a4424212380e92f85285fe'
execution = {'startedUtc': utc(), 'status': 'running', 'gate': {'path': str(gate_path), 'sha256': sha(gate_path)}, 'runnerSha256': sha(Path(__file__)), 'runs': []}
save(output / 'execution.json', execution)
for label in ('reference-1', 'candidate-1', 'reference-2', 'candidate-2'):
    candidate = label.startswith('candidate')
    build = Path(gate['candidateBuild']) if candidate else WORK / 'reference45/source/build/release'
    commit = gate['sourceCommit'] if candidate else reference['head']
    archive = Path(gate['sourceArchive']) if candidate else WORK / 'reference45/source.tar'
    argv = [sys.executable, str(HARNESS), '--mode', 'split', '--app', str(build/'putgd'), '--cli', str(build/'putgd-cli'), '--helper', str(build/'putgd-ui'), '--source-revision', commit, '--source-archive', str(archive), '--fixture', '/tmp/putgd-startup-reclaim-exp001', '--output', str(output/label), '--views', 'settings', 'history', 'activity', '--cycles', '3', '--warmup-seconds', '5', '--visible-seconds', '2', '--quiet-seconds', '30', '--interval', '5']
    if candidate:
        for receipt in gate['nativeReceipts']:
            argv += ['--acceptance-receipt', receipt['path']]
    started = time.monotonic()
    with (output/(label+'.stdout')).open('x') as stdout, (output/(label+'.stderr')).open('x') as stderr:
        run = subprocess.run(argv, cwd=ROOT, stdout=stdout, stderr=stderr)
    result_path = output/label/'results.json'
    row = {'label': label, 'utc': utc(), 'command': argv, 'exitCode': run.returncode, 'elapsedSeconds': time.monotonic()-started, 'rawResult': str(result_path)}
    if result_path.exists():
        row['rawResultSha256'] = sha(result_path)
        row['rawMetrics'] = json.loads(result_path.read_text())
    execution['runs'].append(row)
    if run.returncode:
        execution['status'] = 'failed-preserved-no-automatic-retry'
    save(output/'execution.json', execution)
    has_candidate = any(x['label'].startswith('candidate') for x in execution['runs'])
    marker = {'iteration': 1, 'hypothesis': 'Lazy advanced Settings pages', 'outcome': ('measured' if has_candidate else 'reference-only-pending-candidate') if not run.returncode else 'error', 'runs': execution['runs'], 'cleanCandidateCommit': gate['sourceCommit'], 'limits': ['No keep decision until paired aggregation and actual decide.mjs evaluation; original source41 baseline remains separate.']}
    marker_directory = WORK/'exp001/work/lazy-confirmation'
    marker_directory.mkdir(parents=True, exist_ok=True)
    save(marker_directory/'result.yaml', marker)
    ledger = json.loads(LOG.read_text())
    entry = next((x for x in ledger['experiments'] if x['iteration']==1), None)
    if entry is None and has_candidate:
        entry = {'iteration':1, 'description':'Construct advanced Settings pages on demand', 'category':'architecture', 'kind':'standalone', 'outcome':'measured', 'measurements':[]}
        ledger['experiments'].append(entry)
    rows = [{'label':x['label'], 'path':x['rawResult'], 'sha256':x.get('rawResultSha256'), 'exitCode':x['exitCode']} for x in execution['runs']]
    ledger['matched_reference_measurements'] = [x for x in rows if x['label'].startswith('reference')]
    if entry is not None:
        entry['measurements'] = rows
        entry['outcome'] = marker['outcome']
        entry['sourceCommit'] = gate['sourceCommit']
        entry['nativeGate'] = execution['gate']
        entry['resultMarker'] = str(marker_directory/'result.yaml')
    save(LOG, ledger)
    print(json.dumps({'label':label,'exitCode':run.returncode,'cp3Verified':True}), flush=True)
    if run.returncode:
        sys.exit(run.returncode)
execution['status']='complete'
execution['completedUtc']=utc()
save(output/'execution.json', execution)
