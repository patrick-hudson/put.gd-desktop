"""#342 dry run: does the PR's signer build the same production apt/dnf output as the deployed one?

Both versions build the live nightly workspace for the same verified release (v0.1.1-nightly.847,
downloaded and verified from GitHub by each) into their own temporary state, with a throwaway GPG
key and rpm_sign off, and nothing is uploaded. The trees are compared file by file after dropping
what changes with the clock (Release/InRelease Date, repomd timestamps, the feed's generatedAt).
Then every key of the deployed tree must get the same upload phase, cache headers and type.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

OLD, NEW, TAG = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]


def load(source, name):
    sys.path.insert(0, str(source / 'tools/releases'))
    spec = importlib.util.spec_from_file_location(name, source / 'tools/releases/pkgrepo.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    sys.path.pop(0)
    return module


def normalized(path):
    data = path.read_bytes()
    name = path.name
    if name in ('Release', 'InRelease'):
        data = re.sub(rb'(?m)^Date: .*\n', b'', data)
        data = re.sub(rb'-----BEGIN PGP SIGNATURE-----.*?-----END PGP SIGNATURE-----\n?', b'', data, flags=re.S)
    elif name == 'Release.gpg':
        return b'(detached signature)'
    elif name == 'repomd.xml':
        # Timestamps, and the hashes and sizes that follow from primary's file time (below).
        data = re.sub(rb'<(timestamp|revision|checksum|open-checksum|size|open-size)[^>]*>[^<]*</\1>', b'', data)
        data = re.sub(rb'<location href="[^"]*"/>', b'', data)
    elif name.endswith('-primary.xml.zst'):
        # The RPM's mtime (time file=) is when this run downloaded it; drop it, compare the rest.
        data = subprocess.run(['zstd', '-dc', str(path)], check=True, capture_output=True).stdout
        data = re.sub(rb'<time file="[0-9]+"', b'<time', data)
    elif name.endswith('.json') and '/update/' in str(path):
        document = json.loads(data)
        document.pop('generatedAt', None)
        data = json.dumps(document, sort_keys=True).encode()
    return data


def files(tree):
    result = {}
    for path in sorted(tree.rglob('*')):
        if path.is_file():
            relative = path.relative_to(tree).as_posix()
            # Hash-named repodata differs when its timestamps do; compare it by kind.
            relative = re.sub(r'^(.*/repodata/)[0-9a-f]{64}-', r'\1<hash>-', relative)
            result[relative] = hashlib.sha256(normalized(path)).hexdigest()
    return result


with tempfile.TemporaryDirectory(prefix='dry342-') as directory:
    root = Path(directory)
    gnupg = root / 'gnupg'
    gnupg.mkdir(mode=0o700)
    subprocess.run(['gpg', '--homedir', str(gnupg), '--batch', '--passphrase', '', '--quick-gen-key', 'putgd dry run 342', 'rsa2048', 'sign', '1d'],
                   check=True, capture_output=True)
    fingerprint = re.search(r'fpr:+([0-9A-F]{40}):', subprocess.run(['gpg', '--homedir', str(gnupg), '--with-colons', '--list-keys'],
                                                                     check=True, capture_output=True, text=True).stdout).group(1)
    trees, modules = {}, {}
    for label, source in (('deployed', OLD), ('pr', NEW)):
        module = modules[label] = load(source, 'pkgrepo_' + label)
        config = module.Config(state=root / label / 'state', gnupghome=gnupg, minisign_key=root / 'none', keys=NEW / 'packaging/linux/repo/keys',
                               nightly_fingerprint=fingerprint, stable_fingerprint=fingerprint, rpm_sign=False)
        github = module.GitHub(module.REPOSITORY)
        release = next(r for r in github.releases() if r['tag_name'] == TAG)
        inputs = module.verify_release(release, github, root / label / 'downloads')
        signer = module.Signer(config, github=github, clients=(None, None))
        workspace = signer.build_next('live', 'nightly', inputs, module.new_receipt(inputs, 'live'))
        trees[label] = workspace.tree
        print(label, source, 'appimage input:', bool(inputs.get('appimage')))
    old, new = files(trees['deployed']), files(trees['pr'])
    changed = sorted(key for key in old if key in new and old[key] != new[key])
    only_old = sorted(set(old) - set(new))
    only_new = sorted(set(new) - set(old))
    headers = [key for key in old if (modules['deployed'].phase(key), modules['deployed'].cache_control(key), modules['deployed'].content_type(key))
               != (modules['pr'].phase(key), modules['pr'].cache_control(key), modules['pr'].content_type(key))]
    print(json.dumps({'tag': TAG, 'deployedFiles': len(old), 'prFiles': len(new), 'changed': changed, 'onlyDeployed': only_old,
                      'onlyPr': only_new, 'phaseOrHeaderChanges': headers}, indent=1))
