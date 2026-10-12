#!/usr/bin/env python3
"""Export one completed numeric run; never contacts a guest or cloud service."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tarfile
import xml.etree.ElementTree as ET

MAX_MEMBER = 16 * 1024 * 1024
MAX_TOTAL = 32 * 1024 * 1024
MAX_MEMBERS = 512
os.umask(0o077)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def regular(path):
    s = path.lstat()
    assert stat.S_ISREG(s.st_mode) and s.st_uid == os.getuid(), 'owned_regular_input_required'
    assert s.st_size <= MAX_MEMBER, 'member_size_bound'
    return path.read_bytes()


def protected_needles(path):
    # Read only into this process, never display values or persist the input.
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        s = os.fstat(fd)
        assert stat.S_ISREG(s.st_mode) and s.st_uid == os.getuid() and not s.st_mode & 0o077
        assert s.st_size <= 256 * 1024
        with os.fdopen(fd, 'r', closefd=False) as source:
            fields = {k.strip(): v.strip() for line in source.read().splitlines()
                      if ':' in line for k, v in [line.split(':', 1)]}
        needles = [fields[k].encode() for k in ('Access Key ID', 'Secret Access Key', 'Session Token')
                   if fields.get(k)]
        assert fields.get('Access Key ID') and fields.get('Secret Access Key')
        fields.clear()
        return needles
    finally:
        os.close(fd)


def scan(data, needles):
    assert not any(v in data for v in needles), 'protected_value_scan_failed'
    assert not re.search(rb'(?:AKIA|ASIA)[0-9A-Z]{16}', data), 'access_key_pattern_scan_failed'
    assert not re.search(rb'(?i)x-amz-security-token|aws4-hmac-sha256|authorization:|secretaccesskey', data)


def transform(value, replacements):
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            # Configuration snapshots are unnecessary for numeric replay.
            if key in ('settings', 'credentials', 'historyResponse'):
                continue
            if key in ('text', 'body', 'payload', 'content'):
                data = item.encode() if isinstance(item, str) else json.dumps(item, sort_keys=True).encode()
                result[key + 'Excluded'] = {'bytes': len(data), 'sha256': sha(data)}
                continue
            result[key] = transform(item, replacements)
        return result
    if isinstance(value, list):
        return [transform(v, replacements) for v in value]
    if isinstance(value, str):
        for old, new in replacements:
            value = value.replace(old, new)
        return value
    return value


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--live', default='live')
    ap.add_argument('--name', required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--credentials', type=Path, required=True)
    ap.add_argument('--functional', required=True)
    ap.add_argument('--final', required=True)
    ap.add_argument('--evaluator-sha256', required=True)
    ap.add_argument('--observer-sha256', required=True)
    ap.add_argument('--extras', type=Path, required=True, help='Explicit base-relative metadata whitelist')
    a = ap.parse_args()
    assert re.fullmatch(r'[a-z0-9-]+', a.name)
    base = a.base.resolve(); live = (base / a.live).resolve(); output = a.output.resolve()
    assert live.is_relative_to(base) and live != base
    output.mkdir(mode=0o700, parents=True, exist_ok=True)
    archive = output / (a.name + '.tar.gz')
    manifest_path = output / (a.name + '.tar.gz.manifest.json')
    assert not archive.exists() and not manifest_path.exists(), 'never_overwrite_export'
    evaluator = regular(base / 'evaluate.py'); observer = regular(base / 'observe_guest.py')
    assert sha(evaluator) == a.evaluator_sha256 and sha(observer) == a.observer_sha256
    needles = protected_needles(a.credentials)
    baseline = json.loads(regular(live / 'baseline102.json'))
    saved = json.loads(regular(live / 'evaluated-result.json'))
    assert saved['sourceCommit'] == baseline['build']['gitCommit'] and not saved['reproduction_gate']
    assert len(baseline['operationIds']) == 30 and len(baseline['historyIds']) == 31
    assert json.loads(regular(live / 'completed.json'))['residentIdentityUnchanged']
    assert json.loads(regular(base / a.functional))['functionalPassed']
    final = json.loads(regular(base / a.final))
    assert final['overallPassed'] and final['runtimePinReleased'] and final['sourceCleanAtRelease']
    replacements = [(str(base), '${RUN}'),
                    ('/home/patrick/.codex/worktrees/live-s3-memory/put.gd', '${SOURCE}'),
                    ('/home/patrick/work/put.gd', '${PROJECT}'),
                    ('/home/lab/putgd-lab', '${GUEST_PROFILE}')]
    files = {}
    required = ['baseline102.json', 'baseline-dev.xml', 'parameters.json', 'samples.jsonl',
                'completed.json', 'evaluated-result.json', 'public-region-pixel-oracle.json',
                'cleanup-completed.json', 'cleanup-prepared.json', 'cleanup-verified.json',
                'cleanup-history.json', 'cleanup-local.json', 'cleanup-cache.json',
                'quiet-start.json', 'observer-command.json', 'normal-drain.json',
                'idle-resources.json', 'diagnostic-events.jsonl']
    for name in required:
        files[name] = live / name
    for pattern in ('batch-*/receipt.json', 'batch-*/operation-*.json', 'batch-*/verification-*.json',
                    'final-operations/*.json', 'cleanup-confirm-*/receipt.json'):
        for p in live.glob(pattern):
            files[p.relative_to(live).as_posix()] = p
    extras = json.loads(regular(a.extras))
    assert isinstance(extras, list) and a.functional in extras and a.final in extras
    for name in extras:
        path = PurePosixPath(name)
        assert not path.is_absolute() and '..' not in path.parts
        assert path.suffix in ('.json', '.jsonl'), 'metadata_only_extra'
        files['acceptance/' + name] = base / name
    assert len(files) <= MAX_MEMBERS
    buffers = []; members = []; origins = []
    for name, source in sorted(files.items()):
        assert source.resolve().is_relative_to(base) and source.suffix in ('.json', '.jsonl', '.xml')
        raw = regular(source); scan(raw, needles)
        if name == 'baseline102.json':
            data = json.dumps({k: baseline[k] for k in ('prefix', 'profileId', 'resident', 'build',
                              'operationIds', 'historyIds', 'quietStage')}, indent=2).encode() + b'\n'
        elif source.suffix == '.jsonl':
            data = b''.join(json.dumps(transform(json.loads(line), replacements), separators=(',', ':')).encode() + b'\n'
                            for line in raw.splitlines())
        elif source.suffix == '.json':
            data = json.dumps(transform(json.loads(raw), replacements), indent=2).encode() + b'\n'
        else:
            ET.fromstring(raw)
            text = raw.decode()
            for old, new in replacements:
                text = text.replace(old, new)
            data = text.encode(); ET.fromstring(data)
        scan(data, needles)
        assert len(data) <= MAX_MEMBER
        members.append({'path': name, 'bytes': len(data), 'sha256': sha(data)})
        origins.append({'member': name, 'sourceRelativePath': source.relative_to(base).as_posix(),
                        'originalSha256': sha(raw), 'exportedSha256': sha(data), 'transformed': raw != data})
        buffers.append((name, data))
    provenance = {'sourceCommit': saved['sourceCommit'], 'version': saved['version'],
                  'originalInputs': origins, 'metadataOnly': True,
                  'transform': 'Remove settings/credential/history snapshots; replace text/body/payload/content values with length/hash only; normalize private paths. Numerical samples and operation/receipt IDs, hashes, outcome/status/guard fields retain their values.',
                  'privateImagesExcluded': True, 'guiWitnessesNotReplayed': True,
                  'qualificationScope': 'Earlier native failures and the separately qualified controlled numeric XFixes witness remain historical failures/metadata; replay does not relabel or rerun them.'}
    body = json.dumps(provenance, indent=2).encode() + b'\n'; scan(body, needles)
    buffers.append(('export-provenance.json', body)); members.append({'path': 'export-provenance.json', 'bytes': len(body), 'sha256': sha(body)})
    assert len(members) <= MAX_MEMBERS and sum(m['bytes'] for m in members) <= MAX_TOTAL
    for name, body in [(a.name + '.evaluate.py', evaluator), (a.name + '.observe_guest.py', observer)]:
        scan(body, needles); target = output / name
        assert not target.exists(); target.write_bytes(body)
    with archive.open('xb') as target:
        with gzip.GzipFile(filename='', mode='wb', fileobj=target, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w') as tar:
                for name, body in buffers:
                    info = tarfile.TarInfo(name); info.size = len(body)
                    info.mode = 0o600; info.uid = info.gid = info.mtime = 0
                    tar.addfile(info, io.BytesIO(body))
    entry = {'schemaVersion': 1, 'file': archive.name, 'sha256': sha(archive.read_bytes()),
             'bytes': archive.stat().st_size, 'members': members, 'memberCount': len(members),
             'uncompressedBytes': sum(m['bytes'] for m in members), 'evaluator': a.name + '.evaluate.py',
             'evaluatorSha256': sha(evaluator), 'observer': a.name + '.observe_guest.py',
             'observerSha256': sha(observer), 'protectedCredentialValuesScanned': len(needles),
             'actualCredentialValuesWritten': False, 'payloadImagesRawConfigurationExcluded': True,
             'sourceCommit': saved['sourceCommit'], 'version': saved['version'],
             'savedResultSha256': sha(regular(live / 'evaluated-result.json')),
             'scope': 'Saved numeric evidence only; actual native/GUI acceptance is referenced, never fabricated by replay.'}
    body = json.dumps(entry, indent=2).encode() + b'\n'; scan(body, needles)
    manifest_path.write_bytes(body); needles.clear()
    print(json.dumps({'archive': str(archive), 'sha256': entry['sha256'], 'bytes': entry['bytes'],
                      'memberCount': entry['memberCount'], 'uncompressedBytes': entry['uncompressedBytes']}))


if __name__ == '__main__':
    main()
