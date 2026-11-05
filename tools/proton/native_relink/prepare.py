#!/usr/bin/env python3
"""Stage only checksum-verified public inputs for the offline Bun 1.4.4 proof.

Run on a builder. No Proton account, keychain, session or application profile is
read. Compiler installation is confined to the disposable Docker image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tomllib


HERE = Path(__file__).resolve().parent
PROTON = HERE.parent


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare(assets, bootstrap, output):
    catalog = json.loads((PROTON / 'notices/SOURCES.json').read_text())
    runtime = json.loads((PROTON / 'runtime-lock.json').read_text())
    names = ('putgd-proton-bun-sources-1.4.4.tar',
             'putgd-proton-helper-rebuild.tar.gz',
             'putgd-proton-native-rebuild-supplement.tar.gz')
    expected = {item['filename']: item['sha256'] for item in catalog['artifacts']}
    for name in names:
        if digest(assets / name) != expected[name]:
            raise ValueError('Source packet checksum mismatch: ' + name)
    if digest(bootstrap) != runtime['bunSha256']:
        raise ValueError('Bootstrap must be the checksum-pinned official Bun 1.4.4')
    # Check every input before creating anything; never reuse an earlier attempt.
    output.mkdir(mode=0o700)
    stage = output / 'stage'
    stage.mkdir(mode=0o700)
    with tarfile.open(assets / names[0]) as archive:
        archive.extractall(stage, filter='data')
    (stage / 'bun-source-materials').rename(stage / 'bundle')
    with tarfile.open(assets / names[2]) as archive:
        archive.extractall(output, filter='data')
    supplement = output / 'p3-rebuild-supplement'
    shutil.copyfile(assets / names[1], stage / 'helper-rebuild.tar.gz')
    shutil.copyfile(PROTON / 'notices/SOURCES.json', stage / 'SOURCES.json')
    shutil.copy2(bootstrap, stage / 'bootstrap-bun')
    for name in ('apply_proof.py', 'run_build.py', 'run_helper_proof.py', 'resume_build.py', 'proof_command.py'):
        shutil.copyfile(HERE / name, stage / name)
    shutil.copyfile(HERE / 'preflight.py', stage / 'rebuild_native.py')
    shutil.copyfile(supplement / 'install_std_vendor.py', stage / 'install_std_vendor.py')
    for name in ('helper-proof-inputs', 'argon-proof'):
        shutil.copytree(supplement / name, stage / name)
    # The historical supplement's smoke test asserts Bun 1.4.2. Use the current
    # production test, including its current runtime identity assertions.
    shutil.copyfile(PROTON / 'smoke_test.py', stage / 'helper-proof-inputs/smoke_test.py')
    channel = stage / 'bundle/rust-nightly-2026-09-15.toml'
    shutil.copyfile(channel, stage / channel.name)
    image = output / 'image'
    image.mkdir(mode=0o700)
    shutil.copyfile(HERE / 'Dockerfile', image / 'Dockerfile')
    for name in ('llvm.asc', 'install_rust.py'):
        shutil.copyfile(supplement / 'image' / name, image / name)
    pin = tomllib.loads(channel.read_text())
    components = {}
    for name in ('rustc', 'cargo', 'rust-std', 'rust-src'):
        target = pin['pkg'][name]['target']['*' if name == 'rust-src' else 'x86_64-unknown-linux-gnu']
        components[name] = {'url': target['xz_url'], 'sha256': target['xz_hash']}
    (image / 'rust-components.json').write_text(json.dumps(components, indent=2) + '\n')
    receipt = {'sourcePackets': {name: expected[name] for name in names},
               'bootstrapSha256': runtime['bunSha256'], 'rustComponents': components}
    (output / 'inputs.json').write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', required=True, type=Path)
    parser.add_argument('--bootstrap', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='New private directory')
    args = parser.parse_args()
    prepare(args.assets.resolve(), args.bootstrap.resolve(), args.output.resolve())
