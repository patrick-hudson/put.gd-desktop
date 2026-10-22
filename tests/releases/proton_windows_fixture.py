"""A provisioned Windows Proton runtime directory with a synthetic helper (#408).

The redistribution files are the repository's own; the catalog and Bun scope are
rewritten for the synthetic helper and Bun executable, as provisioning would see them
after updating the catalog. No network, Bun or Proton account is involved.
"""
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
NOTICES = ROOT / 'tools/proton/notices'
HELPER = b'MZ synthetic Windows Proton helper; no network or credential access'
BUN = 'b' * 64


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_runtime(directory, files, helper=HELPER):
    """Write OUTPUT/ of provision_runtime.py --target windows-x64; returns its manifest."""
    directory = Path(directory)
    redistribution = directory / 'redistribution'
    redistribution.mkdir(parents=True)
    lock = json.loads((ROOT / 'tools/proton/runtime-lock.json').read_text())
    (directory / 'putgd-proton-runtime.exe').write_bytes(helper)
    helper_sha = digest(directory / 'putgd-proton-runtime.exe')
    shutil.copyfile(NOTICES / 'THIRD-PARTY-NOTICES.txt', directory / 'THIRD-PARTY-NOTICES.txt')
    for name in files:
        shutil.copyfile(NOTICES / name, redistribution / name)
    catalog = json.loads((redistribution / 'SOURCES-WINDOWS.json').read_text())
    runtime = dict(catalog['bunTargetRuntime'], executableSha256=BUN)
    catalog.update(helperSha256=helper_sha, bunTargetRuntime=runtime)
    (redistribution / 'SOURCES-WINDOWS.json').write_text(json.dumps(catalog, indent=2) + '\n')
    scope = json.loads((redistribution / 'BUN-WINDOWS-SCOPE.json').read_text())
    scope['bunExecutable']['executableSha256'] = BUN
    (redistribution / 'BUN-WINDOWS-SCOPE.json').write_text(json.dumps(scope, indent=2) + '\n')
    manifest = {'schemaVersion': 1, 'platform': 'windows-x64', 'artifact': 'putgd-proton-runtime.exe',
                'sha256': helper_sha, 'sourceReference': lock['sourceReference'], 'protocolVersion': 1,
                'cryptoVersion': lock['cryptoVersion'], 'cryptoSourceReference': lock['cryptoSourceReference'],
                'bunTargetRuntime': runtime,
                'redistributionFilesSha256': {name: digest(redistribution / name) for name in files},
                'dependencyNotices': 'THIRD-PARTY-NOTICES.txt',
                'dependencyNoticesSha256': digest(directory / 'THIRD-PARTY-NOTICES.txt'),
                'redistributionComplete': True}
    save_manifest(directory, manifest)
    return manifest


def save_manifest(directory, manifest):
    (Path(directory) / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
