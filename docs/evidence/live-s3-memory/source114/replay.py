#!/usr/bin/env python3
"""Verify an explicit metadata archive and replay its exact frozen evaluator."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import tempfile

os.umask(0o077)
MAX_TOTAL = 32 * 1024 * 1024
MAX_MEMBER = 16 * 1024 * 1024


def digest(data):
    return hashlib.sha256(data).hexdigest()


def replay_entry(root, entry, require_target):
    members = {m['path']: m for m in entry['members']}
    assert len(members) == len(entry['members']) == entry['memberCount'] <= 512
    assert sum(m['bytes'] for m in members.values()) == entry['uncompressedBytes'] <= MAX_TOTAL
    assert members['evaluated-result.json']['sha256'] == entry['savedResultSha256']
    for kind in ('evaluator', 'observer'):
        name = entry[kind]; assert Path(name).name == name
        assert digest((root / name).read_bytes()) == entry[kind + 'Sha256']
    assert Path(entry['file']).name == entry['file']
    archive = root / entry['file']
    assert archive.stat().st_size == entry['bytes'] and digest(archive.read_bytes()) == entry['sha256']
    with tempfile.TemporaryDirectory(prefix='pgr114-') as temporary:
        destination = Path(temporary); seen = set()
        with tarfile.open(archive, 'r:gz') as tar:
            for member in tar:
                path = PurePosixPath(member.name)
                assert not path.is_absolute() and '..' not in path.parts
                assert member.isfile() and member.name in members and member.name not in seen
                expected = members[member.name]
                assert member.size == expected['bytes'] <= MAX_MEMBER
                body = tar.extractfile(member).read(member.size + 1)
                assert len(body) == member.size and digest(body) == expected['sha256']
                target = destination / member.name
                target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                with target.open('xb') as output:
                    output.write(body)
                target.chmod(0o600); seen.add(member.name)
        assert seen == set(members)
        saved = json.loads((destination / 'evaluated-result.json').read_text())
        result = json.loads(subprocess.run([sys.executable, str(root / entry['evaluator']),
                            '--bundle', str(destination)], capture_output=True, text=True,
                            check=True, timeout=60).stdout)
        assert result == saved, 'replay_result_must_match_exactly'
        assert result['sourceCommit'] == entry['sourceCommit'] and result['version'] == entry['version']
        if require_target:
            assert result['post_s3_idle_rss_plus_swap_mib'] <= 40
        return {'scope': 'Saved-evidence replay; no fresh desktop or live S3 acceptance',
                'archive': entry['file'], 'archiveSha256': entry['sha256'],
                'memberHashesAndBoundsPassed': True, 'exactSavedResultMatched': True,
                'result': result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--bundle', action='append', help='Archive stem; default: all entries')
    parser.add_argument('--require-target', action='store_true')
    args = parser.parse_args()
    root = args.manifest.resolve().parent
    manifest = json.loads(args.manifest.read_text())
    entries = manifest.get('archives', [manifest])
    available = {entry['file'].removesuffix('.tar.gz'): entry for entry in entries}
    assert len(available) == len(entries) and 0 < len(entries) <= 16
    selected = args.bundle or list(available)
    assert len(set(selected)) == len(selected) and all(name in available for name in selected)
    results = [replay_entry(root, available[name], args.require_target) for name in selected]
    value = results[0] if 'archives' not in manifest else {
        'scope': 'Saved-evidence replay; no fresh desktop or live S3 acceptance', 'runs': results}
    print(json.dumps(value, indent=2))


if __name__ == '__main__':
    main()
