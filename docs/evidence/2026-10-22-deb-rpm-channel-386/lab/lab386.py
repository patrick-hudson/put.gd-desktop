#!/usr/bin/env python3
"""#386 lab channels on pkg.put.gd/lab, through the PR branch's pkgrepo.py, on d69.

Lab nightlies go through the signer's own lab path (publish_nightly, the nightly
key, as in #293, #343 and #373). The lab stable side uses pkgrepo.py's real
stable staging and publication (stage_stable, publish_stable), but signs with a
lab-only GPG and minisign key made here; the real stable key never leaves the
workstation and isn't used. Only the labels under test are published:
ubuntu24.04 (m74) and fedora44 (f44).

The live signer's config is read by path (its AWS and key files only through
the environment it names); STATE is this run's own directory, so the live
signer's state, checkout and timer are never touched. Every key is under lab/.

  lab386.py keys KEYS                                    lab stable GPG + minisign key (no passphrase)
  lab386.py SRC CONFIG STATE KEYS check
  lab386.py SRC CONFIG STATE KEYS nightly TAG ASSETS [--dry-run]
  lab386.py SRC CONFIG STATE KEYS stable TAG ASSETS [--dry-run]
  lab386.py SRC CONFIG STATE KEYS clean                  delete every tracked lab object and receipt, invalidate /lab/*
"""
import dataclasses, fcntl, json, os, shutil, subprocess, sys, urllib.error, urllib.request
from pathlib import Path

LABELS = ('ubuntu24.04', 'fedora44')
UID = 'put.gd lab stable (#386, lab only, never released)'


def run(*command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def make_keys(keys):
    keys.mkdir(mode=0o700, parents=True, exist_ok=False)
    home = keys / 'gnupg'
    home.mkdir(mode=0o700)
    env = dict(os.environ, GNUPGHOME=str(home))
    run('gpg', '--batch', '--passphrase', '', '--quick-gen-key', UID, 'rsa3072', 'sign', '7d', env=env, capture_output=True)
    listing = run('gpg', '--batch', '--with-colons', '--list-secret-keys', env=env, capture_output=True, text=True).stdout
    fingerprint = next(line.split(':')[9] for line in listing.splitlines() if line.startswith('fpr:'))
    public = keys / 'pub'
    public.mkdir()
    (public / 'putgd-stable.asc').write_bytes(run('gpg', '--batch', '--armor', '--export', fingerprint, env=env, capture_output=True).stdout)
    (public / 'putgd-stable.gpg').write_bytes(run('gpg', '--batch', '--export', fingerprint, env=env, capture_output=True).stdout)
    run('minisign', '-G', '-W', '-p', str(public / 'putgd-stable.minisign.pub'), '-s', str(keys / 'stable.minisign.key'), capture_output=True)
    (keys / 'fingerprint').write_text(fingerprint + '\n')
    print(json.dumps({'fingerprint': fingerprint, 'minisign': (public / 'putgd-stable.minisign.pub').read_text().splitlines()[1]}))


if sys.argv[1] == 'keys':
    make_keys(Path(sys.argv[2]))
    sys.exit(0)

SRC, CONFIG, STATE, KEYS, COMMAND = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]), sys.argv[5]
sys.path.insert(0, str(SRC / 'tools/releases'))
import pkgrepo  # noqa: E402
from release_identity import parse_tag  # noqa: E402

pkgrepo.DEB_LABELS = tuple(label for label in LABELS if label != 'fedora44')
pkgrepo.LABELS = LABELS
# The lab key directory: the real nightly public keys, the lab stable ones.
public = KEYS / 'pub'
for name in ('putgd-nightly.asc', 'putgd-nightly.gpg', 'putgd-nightly.minisign.pub'):
    if not (public / name).exists():
        shutil.copy2(SRC / 'packaging/linux/repo/keys' / name, public / name)
LAB_FINGERPRINT = (KEYS / 'fingerprint').read_text().strip()
config = pkgrepo.Config.load(CONFIG, state=str(STATE), keys=str(public), stable_fingerprint=LAB_FINGERPRINT)
assert config.state == STATE and not str(STATE).startswith(str(Path.home() / 'putgd-signer'))
assert config.stable_fingerprint == LAB_FINGERPRINT != pkgrepo.Config.load(CONFIG).stable_fingerprint
STATE.mkdir(parents=True, exist_ok=True)
LAB_GPG = dict(os.environ, GNUPGHOME=str(KEYS / 'gnupg'))


def status(url):
    try:
        with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=30) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


def stable_inputs(tag, assets):
    """pkgrepo.local_inputs takes nightly tags only; the same checks for a lab stable tag."""
    identity = parse_tag(tag)
    assert identity.channel == 'stable', tag
    original = pkgrepo.parse_tag
    pkgrepo.parse_tag = lambda value: dataclasses.replace(original(value), channel='nightly') if value == tag else original(value)
    try:
        inputs = pkgrepo.local_inputs(tag, assets, base=config.base_url + '/' + pkgrepo.SCOPES['lab'])
    finally:
        pkgrepo.parse_tag = original
    inputs['channel'] = 'stable'
    return inputs


def sign_stage(stage):
    """What sign_stable.py does on the workstation, with the lab key on d69."""
    manifest = json.loads((stage / 'manifest.json').read_text())
    assert manifest['scope'] == 'lab' and manifest['stableFingerprint'] == LAB_FINGERPRINT
    tree, signatures = stage / 'workspace' / 'tree', stage / 'signatures'
    for relative in manifest['sign']['clearsign']:
        name = relative.replace('/', '__')
        run('gpg', '--batch', '--yes', '--local-user', LAB_FINGERPRINT, '--clearsign', '--digest-algo', 'SHA512',
            '--output', str(signatures / (name + '.InRelease')), str(tree / relative), env=LAB_GPG)
        run('gpg', '--batch', '--yes', '--local-user', LAB_FINGERPRINT, '--armor', '--detach-sign', '--digest-algo', 'SHA512',
            '--output', str(signatures / (name + '.gpg')), str(tree / relative), env=LAB_GPG)
    for relative in manifest['sign']['minisign']:
        name = relative.replace('/', '__')
        run('minisign', '-S', '-q', '-s', str(KEYS / 'stable.minisign.key'), '-m', str(tree / relative),
            '-x', str(signatures / (name + '.minisig')), '-t', f'put.gd {manifest["tag"]} stable', stdin=subprocess.DEVNULL)
    if manifest['rpm']:
        signed = signatures / manifest['rpm']['file']
        shutil.copy2(stage / 'unsigned.rpm', signed)
        run('rpmsign', '--define', '_gpg_name ' + LAB_FINGERPRINT, '--addsign', str(signed), env=LAB_GPG, capture_output=True)


def lab_keys(signer, channel, built=None):
    tree = (built or signer.workspace('lab', channel)).tree
    keys = sorted('lab/' + str(path.relative_to(tree)) for path in tree.rglob('*') if path.is_file())
    assert all(key.startswith('lab/') for key in keys)
    return keys


with open(STATE / 'lock', 'w') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    signer = pkgrepo.Signer(config)
    dry = '--dry-run' in sys.argv
    if COMMAND == 'check':
        for path in ('lab/update/v1/nightly.json', 'lab/update/v1/stable.json', 'lab/apt/ubuntu24.04/dists/stable/InRelease',
                     'lab/rpm/fedora/44/x86_64/stable/repodata/repomd.xml', 'update/v1/nightly.json'):
            print(path, status(f'{config.base_url}/{path}'))
    elif COMMAND == 'nightly':
        tag, assets = sys.argv[6], Path(sys.argv[7])
        inputs = pkgrepo.local_inputs(tag, assets, base=config.base_url + '/' + pkgrepo.SCOPES['lab'])
        published = signer.workspace('lab', 'nightly').published()
        if published and pkgrepo.version_key(tag[1:]) <= pkgrepo.version_key(published['version']):
            raise SystemExit('The lab repository already carries ' + published['version'])
        if dry:
            receipt = pkgrepo.new_receipt(inputs, 'lab')
            workspace = signer.build_next('lab', 'nightly', inputs, receipt)
            signer.sign_nightly(workspace, inputs, receipt)
            keys = lab_keys(signer, 'nightly', workspace)
            print('\n'.join(keys)); print(len(keys), 'keys, all under lab/')
            shutil.rmtree(workspace.root)
        else:
            signer.publish_nightly('lab', inputs)
            print(json.dumps(signer.workspace('lab', 'nightly').published()))
    elif COMMAND == 'stable':
        tag, assets = sys.argv[6], Path(sys.argv[7])
        inputs = stable_inputs(tag, assets)
        published = signer.workspace('lab', 'stable').published()
        if published and pkgrepo.version_key(tag[1:]) <= pkgrepo.version_key(published['version']):
            raise SystemExit('The lab repository already carries ' + published['version'])
        stage = signer.stage_stable('lab', inputs)
        sign_stage(stage)
        assert signer.stable_signed(stage)
        if dry:
            keys = sorted('lab/' + str(p.relative_to(stage / 'workspace' / 'tree')) for p in (stage / 'workspace' / 'tree').rglob('*') if p.is_file())
            print('\n'.join(keys)); print(len(keys), 'staged keys, all under lab/:', all(k.startswith('lab/') for k in keys))
            shutil.rmtree(stage)
        else:
            signer.publish_stable('lab', stage)
            print(json.dumps(signer.workspace('lab', 'stable').published()))
    elif COMMAND == 'clean':
        s3, cloudfront = signer.clients()
        keys = set()
        for channel in ('nightly', 'stable'):
            path = config.state / 'published' / f'lab-{channel}.json'
            if path.exists():
                state = json.loads(path.read_text())
                keys |= set(state['objects']) | set(state['pending'])
        receipts = config.state / 'receipts' / 'lab'
        keys |= {f'lab/receipts/{p.stem}.json' for p in receipts.glob('*.json') if not p.stem.endswith('.failed')} if receipts.exists() else set()
        for key in sorted(keys):
            assert key.startswith('lab/'), key
            s3.delete_object(Bucket=config.bucket, Key=key)
            print('deleted', key)
        print('deleted', len(keys), 'lab objects')
        answer = cloudfront.create_invalidation(DistributionId=config.distribution, InvalidationBatch={
            'Paths': {'Quantity': 1, 'Items': ['/lab/*']}, 'CallerReference': 'putgd-386-lab-clean-' + str(len(keys))})
        print('invalidation', answer['Invalidation']['Id'])
    else:
        raise SystemExit(__doc__)
