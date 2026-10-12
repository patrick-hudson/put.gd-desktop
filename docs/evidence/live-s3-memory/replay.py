#!/usr/bin/env python3
"""Verify and replay saved numeric evidence; no desktop or cloud access."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', action='append', help='Archive stem; default: all')
    parser.add_argument('--require-target', action='store_true', help='Require each selected run <=40 MiB')
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    manifest = json.loads((source / 'manifest.json').read_text())
    evaluator = source / 'evaluate.py'
    assert digest(evaluator.read_bytes()) == manifest['evaluatorSha256']
    available = {a['file'].removesuffix('.tar.gz'): a for a in manifest['archives']}
    selected = args.bundle or list(available)
    assert len(set(selected)) == len(selected) and all(s in available for s in selected)
    results = []
    for name in selected:
        entry = available[name]
        archive = source / entry['file']
        assert archive.parent == source and digest(archive.read_bytes()) == entry['sha256']
        members = {m['path']: m for m in entry['members']}
        assert len(members) == len(entry['members'])
        assert sum(m['bytes'] for m in members.values()) <= 32 * 1024 * 1024
        with tempfile.TemporaryDirectory(prefix='pgr-') as temporary:
            destination = Path(temporary)
            seen = set()
            with tarfile.open(archive, 'r:gz') as tar:
                for member in tar:
                    path = PurePosixPath(member.name)
                    assert not path.is_absolute() and '..' not in path.parts
                    assert member.isfile() and member.name in members and member.name not in seen
                    expected = members[member.name]
                    assert member.size == expected['bytes'] and member.size <= 32 * 1024 * 1024
                    body = tar.extractfile(member).read(member.size + 1)
                    assert len(body) == member.size and digest(body) == expected['sha256']
                    target = destination / member.name
                    target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                    with target.open('xb') as output:
                        output.write(body)
                    target.chmod(0o600)
                    seen.add(member.name)
            assert seen == set(members)
            saved = json.loads((destination / 'evaluated-result.json').read_text())
            evaluated = subprocess.run(
                [sys.executable, str(evaluator), '--bundle', str(destination)],
                capture_output=True, text=True, check=True, timeout=60)
            result = json.loads(evaluated.stdout)
            assert result == saved
            results.append({'bundle': name, 'sourceCommit': result['sourceCommit'],
                            'version': result['version'],
                            'idleRssPlusSwapMiB': result['post_s3_idle_rss_plus_swap_mib'],
                            'quietSeconds': result['quiet_seconds'],
                            'objectsVerifiedAndRemoved': result['confirmed_remote_objects'],
                            'savedEvidenceChecksPassed': True})
    print(json.dumps({'scope': 'Saved-evidence replay; no fresh desktop acceptance',
                      'runs': results}, indent=2))
    if args.require_target and any(r['idleRssPlusSwapMiB'] > 40 for r in results):
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
