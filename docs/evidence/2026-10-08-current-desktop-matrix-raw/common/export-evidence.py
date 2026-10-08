"""One-time explicit export of the 2026-10-08 current desktop wave."""
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path('/home/patrick/work/put.gd')
BASE = ROOT / 'work/cross-desktop-current'
OUT = ROOT / 'docs/evidence/2026-10-08-current-desktop-matrix-raw'
REVIEW = Path('/tmp/compound-engineering-1000/ce-code-review/current-lab-identity')
TEXT_SUFFIXES = {'.json', '.jsonl', '.xml', '.log', '.txt', '.py', '.sh', '.md'}
selected = {}
omitted = {}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def select(source, target):
    source = source.resolve(strict=True)
    target = Path(target)
    assert not target.is_absolute() and '..' not in target.parts
    assert source.is_file() and source.name != 'credentials.txt'
    if target in selected:
        assert selected[target] == source
    selected[target] = source


for name in ('source-identity.json', 'cinnamon-source.json', 'cinnamon-hostkey-endpoint.json',
             'memory-wave.json', 'guest-sample-resident.py', 'retired-vms.json',
             'retired-vms-final.json', 'lab-contracts.json', 'lab-contracts.log',
             'lab-contracts-final.json', 'lab-contracts-final.log', 'export-evidence.py'):
    select(BASE / name, Path('common') / name)
for name in ('lab-contracts.json', 'lab-contracts.log', 'body.py'):
    select(BASE / 'ci-step-check-final' / name, Path('common/ci-step-local') / name)
for name in ('review.json', 'metadata.json', 'caller-followup.json', 'adversarial-claude.json',
             'scope.json', 'local-review.json', 'stages.jsonl',
             'adversarial-review-constraints.md', 'adversarial-review-brief.md'):
    select(REVIEW / name, Path('review') / name)

# Check original independent manifests before selecting their bounded records.
for item in json.loads((BASE / 'gnome/manifest.json').read_text())['files']:
    source = ROOT / item['path']
    assert source.resolve().is_relative_to(ROOT / 'work')
    assert digest(source) == item['sha256'], source
    assert source.stat().st_size == item['bytes'], source
    if source.is_relative_to(BASE / 'gnome') and (source.suffix in TEXT_SUFFIXES | {'.png'} or source.name == 'status'):
        select(source, source.relative_to(BASE))
for name, item in json.loads((BASE / 'kde/evidence-manifest.json').read_text())['files'].items():
    source = BASE / 'kde' / name
    assert source.resolve().is_relative_to(BASE / 'kde')
    assert digest(source) == item['sha256'], source
    assert source.stat().st_size == item['bytes'], source
    select(source, Path('kde') / name)

# Only owned wave summaries/scripts/logs/screenshots, never whole VM directories.
for profile in ('cinnamon', 'gnome', 'kde'):
    for source in (BASE / profile).iterdir():
        if source.is_file() and source.suffix in TEXT_SUFFIXES | {'.png'}:
            select(source, source.relative_to(BASE))
    run = ROOT / 'work/lab-runs' / f'current-{profile}-a3d70dc'
    select(run / 'test-events.jsonl', Path(profile) / 'test-events.jsonl')
    for case in (run / 'tests').iterdir():
        if not case.is_dir():
            continue
        assert re.fullmatch(r'[0-9a-f]{32}', case.name)
        for name in ('parameters.json', 'results.json', 'results.xml', 'ready.json',
                     'paste.json', 'manifest.json', 'diagnostics.json', 'versions.json', 'package.json'):
            source = case / name
            if source.exists():
                select(source, Path(profile) / 'tests' / case.name / name)
        for source in case.iterdir():
            if source.is_file() and (source.suffix == '.ppm' or re.fullmatch(r'artifact-\d+', source.name)):
                omitted[str(source.relative_to(ROOT))] = {
                    'sha256': digest(source), 'bytes': source.stat().st_size,
                    'reason': 'Synthetic raw witness/payload retained locally; result verification and hashes exported'
                }

assert not OUT.exists(), 'Refuse to overwrite previous evidence'
for target, source in selected.items():
    data = source.read_bytes()
    if source.suffix in TEXT_SUFFIXES:
        content = data.decode('utf-8')
        assert not re.search(r'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b|-----BEGIN [A-Z ]*PRIVATE KEY-----', content), source
        if source.suffix == '.json':
            json.loads(content)
        if source.suffix == '.jsonl':
            for line in content.splitlines():
                if line.strip():
                    json.loads(line)

OUT.mkdir()
files = []
for target, source in sorted(selected.items()):
    destination = OUT / target
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    files.append({'path': str(target), 'originalPath': str(source),
                  'sha256': digest(destination), 'bytes': destination.stat().st_size})
(OUT / 'manifest.json').write_text(json.dumps({
    'schemaVersion': 1,
    'scope': 'Exact selected records, commands and synthetic PNG witnesses; original paths inside records are historical, use this map to resolve committed copies',
    'files': files,
    'localOnlySyntheticArtifacts': omitted,
    'excluded': ['VM disks, SSH keys and configuration', 'credentials.txt and credential values',
                 'Package/archive binaries (separate hashes retained)', 'Unrelated working changes'],
    'reviewStatus': 'Completed independent review, one actionable finding fixed and regression tested; external review was not rerun after the fix'
}, indent=2) + '\n')
print(json.dumps({'files': len(files), 'bytes': sum(x['bytes'] for x in files),
                  'localOnlyArtifacts': len(omitted), 'manifestSha256': digest(OUT / 'manifest.json')}))
