#!/usr/bin/env python3
"""Name, check and build the CI images of tools/ci/images/profiles.json.

  ci_images.py plan --registry ghcr.io/OWNER/putgd-ci [--check]
  ci_images.py build PROFILE --registry ghcr.io/OWNER/putgd-ci [--push]

A profile's tag is <profile>-<ISO week>-<hash>. The hash covers the base image,
the build target and arguments, and the bytes of every input file, and the build
context holds exactly those files, so an image can't contain anything its tag
does not name. The week makes every image at most a week old. `plan` writes the
`images` (profile -> image) and `missing` (the build matrix) step outputs; with
--check, an image already in the registry is not missing.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PROFILES = ROOT / 'tools/ci/images/profiles.json'
DOCKERFILE = 'tools/ci/images/Dockerfile'


def load(path=PROFILES):
    return json.loads(path.read_text())


def week(today=None):
    year, number, _ = (today or datetime.datetime.now(datetime.timezone.utc).date()).isocalendar()
    return f'{year}w{number:02d}'


def qt_version(root=ROOT):
    lock = json.loads((root / 'packaging/appimage/tools.lock.json').read_text())
    return lock['buildInputs']['qt']


def build_args(name, profile, root=ROOT):
    args = {'BASE': profile['base'], 'PROFILE': name}
    if profile.get('qt'):
        args['PUTGD_QT_VERSION'] = qt_version(root)
    return args


def inputs(config, name):
    return sorted(set(config['common']) | set(config['profiles'][name].get('inputs', ())))


def tag(config, name, period, root=ROOT):
    profile = config['profiles'][name]
    files = {path: hashlib.sha256((root / path).read_bytes()).hexdigest() for path in inputs(config, name)}
    identity = {'target': profile.get('target', 'profile'), 'args': build_args(name, profile, root), 'files': files}
    digest = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    return f'{name}-{period}-{digest[:16]}'


def exists(image):
    """True when the registry has the image (docker must be logged in for a private package). Any
    other answer counts as missing: rebuilding costs minutes, a wrong "present" fails every job."""
    result = subprocess.run(['docker', 'manifest', 'inspect', image], stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    if result.returncode != 0 and 'manifest unknown' not in result.stderr:
        print(f'::warning::Treating {image} as missing: {result.stderr.strip()[-300:]}', flush=True)
    return result.returncode == 0


def plan(registry, period, check, root=ROOT):
    config = load(root / 'tools/ci/images/profiles.json')
    tags = {name: f'{registry}:{tag(config, name, period, root)}' for name in config['profiles']}
    missing = [name for name, image in tags.items() if not (check and exists(image))]
    return tags, missing


def context(config, name, directory, root=ROOT):
    for path in inputs(config, name):
        target = directory / path
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / path, target)


def build(name, registry, period, push, root=ROOT):
    config = load(root / 'tools/ci/images/profiles.json')
    profile = config['profiles'][name]
    image = f'{registry}:{tag(config, name, period, root)}'
    if push and exists(image):
        print(f'{image} is already in the registry')
        return image
    with tempfile.TemporaryDirectory(prefix='putgd-ci-image-') as temporary:
        directory = Path(temporary)
        context(config, name, directory, root)
        command = ['docker', 'build', '--pull', '--file', str(directory / DOCKERFILE),
                   '--target', profile.get('target', 'profile'), '--tag', image,
                   '--label', 'org.opencontainers.image.source=https://github.com/'
                   + os.environ.get('GITHUB_REPOSITORY', 'patrick-hudson/put.gd'),
                   '--label', f'org.opencontainers.image.description=put.gd CI image {name}']
        for key, value in build_args(name, profile, root).items():
            command += ['--build-arg', f'{key}={value}']
        subprocess.run([*command, str(directory)], check=True)
    if push:
        subprocess.run(['docker', 'push', image], check=True)
    return image


def output(name, value):
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
            stream.write(f'{name}={value}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    planning = sub.add_parser('plan')
    planning.add_argument('--registry', required=True)
    planning.add_argument('--check', action='store_true')
    building = sub.add_parser('build')
    building.add_argument('profile')
    building.add_argument('--registry', required=True)
    building.add_argument('--push', action='store_true')
    for each in (planning, building):
        each.add_argument('--week', default=os.environ.get('PUTGD_CI_IMAGE_WEEK') or week())
    args = parser.parse_args()
    registry = args.registry.lower()
    if args.command == 'plan':
        tags, missing = plan(registry, args.week, args.check)
        output('images', json.dumps(tags, separators=(',', ':')))
        output('missing', json.dumps(missing, separators=(',', ':')))
        output('week', args.week)
        if os.environ.get('GITHUB_STEP_SUMMARY'):
            with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as stream:
                stream.write('| CI image | State |\n| --- | --- |\n')
                for name, image in tags.items():
                    stream.write(f"| `{image}` | {'built in this run' if name in missing else 'in the registry'} |\n")
        print(json.dumps({'tags': tags, 'missing': missing}, indent=2))
    else:
        print(build(args.profile, registry, args.week, args.push))


if __name__ == '__main__':
    sys.exit(main())
