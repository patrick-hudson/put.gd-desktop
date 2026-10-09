#!/usr/bin/env python3
"""Verify archived hashes and failed capture scores; never run a desktop."""
from pathlib import Path
import hashlib
import json
import math
import statistics

root = Path(__file__).resolve().parent
packet = json.loads((root.parent / '2026-10-09-capture-parity-debug.json').read_text())
for name, entry in packet['manifest'].items():
    data = (root / name).read_bytes()
    assert len(data) == entry['bytes'], name
    assert hashlib.sha256(data).hexdigest() == entry['sha256'], name
for run in packet['runs']:
    directory = root / 'runs' / run['name']
    result = json.loads((directory / 'results.json').read_text())
    samples = result['samples']
    assert len(samples) == run['sampleCount'], run['name']
    scores = sorted(sample['paintMs'] for sample in samples)
    p95 = scores[math.ceil(.95 * len(scores)) - 1] if len(scores) == 30 else -1
    assert p95 == result['p95PaintMs'] == run['p95PaintMs'], run['name']
    expected = not result['error'] and p95 >= 0 and p95 <= 150
    assert expected == result['budgetPassed'] == run['budgetPassed'], run['name']
    if scores:
        assert statistics.median(scores) == run['medianPaintMs'], run['name']
    counts = {}
    for operation in json.loads((directory / 'operations.json').read_text()):
        counts[operation['state']] = counts.get(operation['state'], 0) + 1
    assert counts == run['operationStates'], run['name']
for test in packet['twoStageHelperPreparation']['testReceipts']:
    summary = json.loads((root / test['summary']).read_text())
    assert int(summary['suite']['tests']) == test['tests']
    assert int(summary['suite']['failures']) == test['failures']
    assert len(summary['testcases']) == test['tests']
    directory = root / 'tests' / test['name']
    receipt = directory / ('test/result.json' if test['name'] == 'buffered-frame-negative' else 'result.json')
    exit_code = json.loads(receipt.read_text())['exitCode']
    assert exit_code == (8 if test['name'] == 'buffered-frame-negative' else 0)
for name in ['early-outline-A', 'early-phases']:
    preflight = json.loads((root / 'runs' / name / 'socket-preflight.json').read_text())
    assert preflight['passed'] and preflight['limitBytesExcludingNul'] == 107
    for path in preflight['paths']:
        assert len(path['path'].encode('utf-8')) == path['utf8Bytes'] <= 107
        assert path['passed']
originals = json.loads((root / 'phases/early-phases/originals.json').read_text())
restored = json.loads((root / 'phases/early-phases/restored.json').read_text())
assert originals == restored
negative = json.loads((root / 'tests/buffered-frame-negative/variant.json').read_text())
restored_negative = json.loads((root / 'tests/buffered-frame-negative/restored.json').read_text())
assert restored_negative['restored']
assert negative['originalSha256'] == restored_negative['sha256']
for test in packet['cursorCacheInvestigation']['tests']:
    summary = json.loads((root / test['summary']).read_text())
    assert len(summary['testcases']) == int(summary['suite']['tests']) == test['tests']
    assert int(summary['suite']['failures']) == test['failures']
    exit_code = json.loads((root / 'cursor-cache' / test['name'] / 'result.json').read_text())['exitCode']
    assert exit_code == test['exitCode'] == (8 if test['name'] in ['proof', 'diagnostic'] else 0)
diag = json.loads((root / 'cursor-cache/diagnostic/junit-summary.json').read_text())
assert diag['numericProbeObservations'] == [
    {'probe': 'grab', 'first': 0, 'second': 0},
    {'probe': 'image', 'first': 0, 'second': True},
    {'probe': 'grab', 'first': 0, 'second': 0},
    {'probe': 'image', 'first': 10, 'second': False}]
input_summary = json.loads((root / 'phases/input-screen-phases/input-summary.json').read_text())
for label in ['invoke', 'input-escape', 'selection-cancel', 'prewarm-screen-other', 'parent-screen-different']:
    assert input_summary['fixedLabelCounts'][label] == 35
cache_runs = {run['name']: run for run in packet['runs'] if run['name'] in ['cache-A', 'cache-B']}
assert len(cache_runs) == 2
assert cache_runs['cache-A']['binarySha256'] == cache_runs['cache-B']['binarySha256']
assert cache_runs['cache-A']['harnessSha256'] == cache_runs['cache-B']['harnessSha256']
for run in cache_runs.values():
    assert run['sampleCount'] == 30 and run['operationStates'] == {'cancelled': 35}
    assert not run['error'] and not run['budgetPassed'] and not run['instrumented']
    preflight = json.loads((root / 'runs' / run['name'] / 'socket-preflight.json').read_text())
    assert preflight['passed']
    for item in preflight['paths']:
        assert len(item['path'].encode('utf-8')) == item['utf8Bytes'] <= 107
        assert item['passed']
for test in packet['cursorCacheInvestigation']['latestTestChronology']:
    summary = json.loads((root / test['summary']).read_text())
    assert len(summary['testcases']) == int(summary['suite']['tests']) == test['tests']
    assert int(summary['suite']['failures']) == test['failures']
    result = json.loads((root / 'cursor-cache' / test['name'] / 'result.json').read_text())
    assert result['exitCode'] == test['exitCode'] == (8 if test['name'] in ['sdk-proof', 'final-dev'] else 0)
review = json.loads((root / 'review/cursor-cache/review.json').read_text())
peer = json.loads((root / 'review/cursor-cache/adversarial-claude.json').read_text())
assert review['status'] == 'complete' and review['verdict'] == 'Not ready'
assert not review['findings'] and len(review['historical_resolutions']) == 3
assert all(item['status'] == 'addressed_static' for item in review['historical_resolutions'])
assert peer['independence_verified'] and peer['receipt_supported']
assert peer['model_requested'] == peer['model_actual']
assert peer['effort_actual'] == 'unverified'
assert json.loads((root / 'phases/cache-phases/originals.json').read_text()) == json.loads((root / 'phases/cache-phases/restored.json').read_text())
summary = json.loads((root / 'phases/cache-sync/summary.json').read_text())
assert summary['unscored'] and summary['durationsMs']['fdatasync']['over30Ms'] == 20
assert summary['durationsMs']['fdatasync']['max'] == 74.22
assert summary['durationsMs']['fcntl']['over30Ms'] == 0
host_before = json.loads((root / 'host-visibility/before.json').read_text())
host_after = json.loads((root / 'host-visibility/after.json').read_text())
assert {row['pid'] for row in host_before['rows']} == {2205651, 2215650, 2204498}
assert {row['pid'] for row in host_after['rows']} == {2205651, 2215650, 2204498}
assert all(row['verifiedOnHost'] and row['ownedOverlay'] for row in host_before['rows'])
assert all(not row['sameProcessPresent'] and row['overlayPreserved'] for row in host_after['rows'])
for row in host_before['rows']:
    result = json.loads((root / 'host-visibility' / ('stop-' + row['run']) / 'result.json').read_text())
    assert result['exitCode'] == 0
for test in packet['journalVariantFollowup']['tests']:
    summary = json.loads((root / test['summary']).read_text())
    assert len(summary['testcases']) == int(summary['suite']['tests']) == test['tests']
    assert int(summary['suite']['failures']) == test['failures'] == 0
    result = json.loads((root / 'journal-variants/tests' / test['name'] / 'result.json').read_text())
    assert result['exitCode'] == 0
restoration = json.loads((root / 'journal-variants/original-policy-restored.json').read_text())
assert all(item['matchesHEAD'] and item['sourceSha256'] == item['headSha256'] for item in restoration['paths'])
control = next(run for run in packet['runs'] if run['name'] == 'control45-repeat')
provenance = json.loads((root / 'controls/source45-provenance.json').read_text())
assert control['binarySha256']['app'] == provenance['binaries']['putgd']
assert control['binarySha256']['cli'] == provenance['binaries']['putgd-cli']
assert control['sourceCommitKnown'] == provenance['head']
assert control['dirtySource'] is None and provenance['build']['dirty'] is None
namespace = {'__name__': 'archive_verifier_module'}
analyzer_path = root / 'analyze_history_threads.py'
exec(compile(analyzer_path.read_text(), str(analyzer_path), 'exec'), namespace)
for name in ['cache-phases', 'truncate-phases']:
    corrected = namespace['analyze'](root / 'phases' / name / 'phases.log')
    saved = (root / 'phases' / name / 'summary-thread-paired.json').read_text()
    assert json.dumps(corrected, indent=2) + '\n' == saved
    assert corrected['groups'] == 35 and corrected['warmupGroups'] == 5 and corrected['measuredGroups'] == 30
    assert not corrected['unmatchedEvents'] and not corrected['nestedBegins']
    for group in corrected['invocations']:
        for worker in group['workers'].values():
            for phase in worker['history'].values():
                assert phase is not None and phase['durationMs'] >= 0
                assert phase['pid'] == worker['pid'] and phase['tid'] == worker['tid']
late = json.loads((root / 'phases/late-finalizer-phases/summary.json').read_text())
assert late['unscored'] and len(late['groups']) == 35
assert late['unpairedBegins'] == 0 and late['incompleteSyscalls'] == 0
worst = late['groups'][19]
commit = next(span for span in worst['spans'] if span['phase'] == 'history-commit')
assert commit['durationMs'] == 270.594567 and not worst['priorOverlaps']
assert commit['syscalls']['fdatasync']['totalMs'] == 268.275
assert commit['syscalls']['fdatasync']['maxMs'] == 171.402
raw_scores = [run for run in packet['runs'] if run['budgetPassed']]
assert len(raw_scores) == 1 and raw_scores[0]['name'] == 'truncate-phases' and raw_scores[0]['instrumented']
assert not any(run['acceptance'] for run in packet['runs'])
print(json.dumps({'schemaVersion': 1, 'archiveVerified': True,
                  'files': len(packet['manifest']), 'runs': len(packet['runs']),
                  'passingRawCaptureScores': sum(run['budgetPassed'] for run in packet['runs']),
                  'passingAcceptedCaptureGates': sum(run['budgetPassed'] and run['acceptance'] for run in packet['runs']),
                  'acceptance': False}, indent=2))
