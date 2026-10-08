"""Aggregate executed physical/view and native desktop receipts; never fabricate a run."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics

parser = argparse.ArgumentParser()
parser.add_argument('--physical', nargs=2, required=True)
parser.add_argument('--wave', required=True)
args = parser.parse_args()
physical = [json.loads(Path(p).read_text()) for p in args.physical]
for sample in physical:
    assert sample['passed'] and sample['allThreeViewsReopened']
    assert sample['parameters']['cycles'] == 3
    assert sample['parameters']['hiddenIdleSeconds'] == 30
    assert sample['operationCount'] == 0
assert physical[0]['appSha256'] == physical[1]['appSha256']
assert physical[0]['workloadSha256'] == physical[1]['workloadSha256']
wave_path = Path(args.wave)
wave = json.loads(wave_path.read_text())
metrics = {'finalHiddenRssBytes': statistics.median(p['finalHiddenRssBytes'] for p in physical),
           'coldSnapshotRssBytes': statistics.median(p['samples'][0]['rssBytes'] for p in physical),
           'allProfilesPassed': 1, 'physicalViewsReopened': 1, 'operationCount': 0}
for name, run in wave['runs'].items():
    assert run['measurement']['identityUnchanged'] and run['measurement']['exitCode'] == 0
    cases = []
    for test_id in run['functionalTestIds']:
        result = json.loads((Path(run['run']) / 'tests' / test_id / 'results.json').read_text())
        assert result['ok'] and result['runningApp'] == run['runningApp']
        cases.append((result['scenario'], result['clipboardClient']))
    assert len(cases) == len(set(cases)) == 12
    path = wave_path.parent / name / 'memory-sample.json'
    assert hashlib.sha256(path.read_bytes()).hexdigest() == run['measurement']['resultSha256']
    memory = json.loads(path.read_text())
    assert memory['processIdentityUnchanged']
    assert memory['pid'] == run['runningApp']['pid']
    assert memory['startTicks'] == run['runningApp']['startTicks']
    assert memory['appSha256'] == run['runningApp']['sha256']
    assert memory['parameters'] == {'durationSeconds': 120, 'intervalSeconds': 5, 'warmupSeconds': 5}
    if name in ('gnome', 'kde'):
        metrics[name + 'PostFunctionalRssBytes'] = memory['finalRssBytes']
        metrics[name + 'PssBytes'] = memory['finalPssBytes']
assert set(wave['runs']) == {'cinnamon', 'gnome', 'kde'}
print(json.dumps(metrics))
