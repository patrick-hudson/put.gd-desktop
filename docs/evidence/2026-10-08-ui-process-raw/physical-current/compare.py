#!/usr/bin/env python3
"""Compare the four completed matched U11 view runs; never launch a workload."""
import hashlib
import json
from pathlib import Path
import statistics

root = Path(__file__).resolve().parent
protocol = json.loads((root / 'protocol.json').read_text())
rows = {}
for mode in ('retained', 'split'):
    runs = []
    for repeat in (1, 2):
        path = root / f'{mode}-{repeat}' / 'results.json'
        result = json.loads(path.read_text())
        assert result['passed'] and result['status'] == 'complete'
        assert result['sourceRevision'] == protocol['baselineCommit' if mode == 'retained' else 'candidateCommit']
        assert result['completeViewCycles'] == protocol['cycles']
        assert result['allViews'] == protocol['views']
        assert result['operationCount'] == 0
        for parameter, expected in [('warmup_seconds', 'warmupSeconds'), ('quiet_seconds', 'quietSeconds'), ('visible_seconds', 'visibleSeconds'), ('interval', 'sampleIntervalSeconds')]:
            assert result['parameters'][parameter] == protocol[expected]
        closed = [sample for sample in result['samples'] if '-closed-' in sample['label']]
        assert closed and all(sample['processCount'] == 1 and sample['uiProcessCount'] == 0 and sample['escapedProcessCount'] == 0 for sample in closed)
        assert result['harnessSha256'] == protocol['harnessSha256']
        latency = [event for event in result['events'] if event['kind'] == 'view-shown']
        runs.append({'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'summary': result['summary'], 'mappedReadyMilliseconds': [event['mappedReadyMilliseconds'] for event in latency]})
    rows[mode] = {
        'runs': runs,
        'medianFinalClosedTotals': {metric: statistics.median(run['summary']['finalClosedTotals'][metric] for run in runs) for metric in ('rssBytes', 'pssBytes', 'privateBytes')},
        'maximumSampledOpenTotals': {metric: max(run['summary']['peakOpenTotals'][metric] for run in runs) for metric in ('rssBytes', 'pssBytes', 'privateBytes')},
        'medianMappedReadyMilliseconds': statistics.median(value for run in runs for value in run['mappedReadyMilliseconds']),
        'maximumMappedReadyMilliseconds': max(value for run in runs for value in run['mappedReadyMilliseconds']),
    }
output = {'schemaVersion': 1, 'protocolSha256': hashlib.sha256((root / 'protocol.json').read_bytes()).hexdigest(), 'scope': 'Matched empty three-view physical Cinnamon workload, two runs per build, all observed processes counted', 'builds': rows, 'closedSavingBytes': {metric: rows['retained']['medianFinalClosedTotals'][metric] - rows['split']['medianFinalClosedTotals'][metric] for metric in ('rssBytes', 'pssBytes', 'privateBytes')}, 'limits': protocol['limits'], 'cold40MiBTargetEstablished': False}
(root / 'comparison.json').write_text(json.dumps(output, indent=2) + '\n')
print(json.dumps({key: value for key, value in output.items() if key != 'builds'}, indent=2))
