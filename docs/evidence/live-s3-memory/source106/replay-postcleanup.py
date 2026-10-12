#!/usr/bin/env python3
"""Verify saved postcleanup numeric evidence; no desktop or cloud access."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent/'postcleanup106')
    args = parser.parse_args()
    root = args.bundle.resolve()
    manifest = json.loads((root/'manifest.json').read_text())
    allowed = {'parameters.json', 'samples.jsonl', 'completed.json',
               'evaluated-confirmation.json', 'start.json', 'command.json'}
    files = {}
    for entry in manifest['members']:
        name = entry['file']
        assert Path(name).name == name and name.endswith('.gz')
        plain_name = name.removesuffix('.gz')
        assert plain_name in allowed and plain_name not in files
        path = root/name
        assert path.is_file() and not path.is_symlink()
        packed = path.read_bytes()
        assert len(packed) == entry['bytes'] and len(packed) <= 32*1024*1024
        assert hashlib.sha256(packed).hexdigest() == entry['sha256']
        assert 0 <= entry['uncompressedBytes'] <= 32*1024*1024
        # Bound decompression rather than trusting a compressed-stream header.
        import io
        with gzip.GzipFile(fileobj=io.BytesIO(packed)) as source:
            body = source.read(entry['uncompressedBytes']+1)
        assert len(body) == entry['uncompressedBytes']
        assert hashlib.sha256(body).hexdigest() == entry['uncompressedSha256']
        files[plain_name] = body
    assert set(files) == allowed
    params = json.loads(files['parameters.json'])
    saved = json.loads(files['evaluated-confirmation.json'])
    completed = json.loads(files['completed.json'])
    start = json.loads(files['start.json'])
    rows = [json.loads(line) for line in files['samples.jsonl'].splitlines()]
    assert completed['residentIdentityUnchanged'] and start['noNewUploads']
    assert start['buildDrain']['bothGuestBuildsDrained'] and start['buildDrain']['hostCompilersAbsent']
    assert params['build'] == start['initialDiagnostics']['build'] and not params['build']['dirty']
    assert params['build']['gitCommit'] == manifest['sourceCommit'] == saved['sourceCommit']
    assert params['build']['version'] == saved['version']
    chosen = []
    for row in rows:
        if row['elapsedSeconds'] < rows[0]['elapsedSeconds']+5:
            continue
        chosen.append(row)
        if row['elapsedSeconds']-chosen[0]['elapsedSeconds'] >= 300:
            break
    duration = chosen[-1]['elapsedSeconds']-chosen[0]['elapsedSeconds']
    assert duration >= 300
    assert max(y['elapsedSeconds']-x['elapsedSeconds'] for x,y in zip(chosen,chosen[1:])) <= 2.5
    for row in chosen:
        assert len(row['family']) == 1
        resident = row['family'][0]
        assert resident['pid'] == params['pid'] and resident['startTicks'] == params['startTicks']
        assert resident['memoryBytes'].get('Swap',0) == 0
    diagnostics = [row['diagnostics'] for row in chosen if 'diagnostics' in row]
    assert len(diagnostics) >= 29
    assert all(d['activeOperations'] == d['ui']['pid'] == 0 for d in diagnostics)
    peak = max(sum(p['memoryBytes']['Rss']+p['memoryBytes'].get('Swap',0)
                   for p in row['family']) for row in chosen)/1048576
    cpu = (chosen[-1]['family'][0]['cpuTicks']-chosen[0]['family'][0]['cpuTicks'])/params['clockTicksPerSecond']/duration*100
    assert peak == saved['idleRssPlusSwapMiB'] and cpu == saved['idleCpuPercentOneCore']
    assert duration == saved['quietSeconds'] and len(chosen) == saved['quietSamples']
    assert len(diagnostics) == saved['diagnosticSamples']
    assert peak <= 40 and cpu < .5 and saved['targetPassed'] and saved['noNewUploads']
    print(json.dumps({'scope': 'Saved zero-upload postcleanup confirmation; no matched-workload comparison or fresh desktop proof',
                      'sourceCommit': saved['sourceCommit'], 'version': saved['version'],
                      'idleRssPlusSwapMiB': peak, 'quietSeconds': duration,
                      'savedEvidenceChecksPassed': True}, indent=2))


if __name__ == '__main__':
    main()
