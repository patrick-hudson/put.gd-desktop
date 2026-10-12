#!/usr/bin/env python3
"""Exact, ABI-separated dependency cache identity and installed-prefix integrity."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fingerprint(root):
    commands = [['cmake', '--version']]
    if platform.system() == 'Windows':
        commands.append(['cl'])
    else:
        commands.append(['c++', '--version'])
    if platform.system() == 'Linux':
        if shutil.which('dpkg-query'):
            commands.append(['dpkg-query', '-W'])
        else:
            commands.append(['rpm', '-qa', '--qf', '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\n'])
    elif platform.system() == 'Darwin':
        commands.append(['sw_vers'])
        if shutil.which('brew'):
            commands.append(['brew', 'list', '--versions'])
    observations = []
    for command in commands:
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        if result.returncode and command != ['cl']:
            raise RuntimeError('Cannot fingerprint build dependencies: ' + command[0])
        observations.append({'command': command, 'output': result.stdout + result.stderr})
    inputs = ['cmake/dependency-lock.json', '.github/workflows/build.yml',
              'tools/ci/sdk_cache.py']
    value = {
        'schemaVersion': 1, 'system': platform.system(), 'machine': platform.machine(),
        'workspace': str(root.resolve()),
        'image': {key: os.environ.get(key, '') for key in
                  ('ImageOS', 'ImageVersion', 'VCToolsVersion', 'WindowsSDKVersion', 'QT_ROOT_DIR')},
        'files': {name: digest(root / name) for name in inputs},
        'observations': observations,
        'scope': 'Release shared SDK/keychain dependency prefix only; no application build products'
    }
    key = 'putgd-sdk-v1-' + hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()
    return key, value


def inventory(prefix):
    if not prefix.is_dir():
        raise ValueError('Installed SDK prefix is missing')
    entries = {}
    for path in sorted(prefix.rglob('*')):
        name = path.relative_to(prefix).as_posix()
        if path.is_symlink():
            path.resolve(strict=True).relative_to(prefix.resolve())
            entries[name] = {'link': os.readlink(path)}
        elif path.is_file():
            entries[name] = {'sha256': digest(path), 'executable': bool(path.stat().st_mode & 0o111)}
    if not entries:
        raise ValueError('Installed SDK prefix is empty')
    return entries


def seal(prefix, manifest, key):
    manifest.write_text(json.dumps({'schemaVersion': 1, 'key': key,
                                  'entries': inventory(prefix)}, indent=2) + '\n')


def verify(prefix, manifest, key):
    saved = json.loads(manifest.read_text())
    if saved.get('schemaVersion') != 1 or saved.get('key') != key:
        raise ValueError('SDK cache provenance differs from this build environment')
    if saved.get('entries') != inventory(prefix):
        raise ValueError('SDK cache installed-prefix integrity failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['key', 'seal', 'verify'])
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--prefix', type=Path, default=Path('build/sdk'))
    parser.add_argument('--manifest', type=Path, default=Path('build/sdk-cache.json'))
    parser.add_argument('--key')
    args = parser.parse_args()
    if args.action == 'key':
        key, value = fingerprint(args.root)
        output = args.root / 'build/ci-sdk-environment.json'
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(dict(value, key=key), indent=2) + '\n')
        if os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
                stream.write('key=' + key + '\n')
        print(key)
    else:
        if not args.key:
            parser.error('--key is required')
        if args.action == 'seal':
            seal(args.prefix, args.manifest, args.key)
        else:
            verify(args.prefix, args.manifest, args.key)
        print('SDK cache ' + args.action + ' passed')


if __name__ == '__main__':
    main()
