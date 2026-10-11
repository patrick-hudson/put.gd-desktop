#!/usr/bin/env python3
"""Export an allowlisted native correctness cohort; never a performance receipt."""
from pathlib import Path
import datetime
import hashlib
import json
import re
import shutil

from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'work/ui-refinement/native-replay/lazy-exp001-native'
OUT = Path(__file__).resolve().parent
SUMMARY = OUT.with_name('2026-10-09-lazy-settings-native.json')
GUESTS = ('cinnamon', 'gnome', 'kde')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy(path, relative):
    assert path.is_file() and not path.is_symlink()
    assert path.name not in {'credentials.txt', 'password'}
    target = OUT / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        assert target.read_bytes() == path.read_bytes(), str(target)
    else:
        shutil.copyfile(path, target)


def export_frame(path, relative):
    target = OUT / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(path) as image:
        image.save(target, format='PNG', compress_level=6)
    return {'originalPath': str(path.relative_to(ROOT)),
            'originalSha256': digest(path),
            'exportPath': str(target.relative_to(ROOT)),
            'sha256': digest(target),
            'conversion': 'Lossless PNG, original dimensions; no resize or crop'}


def main():
    reviews = {}
    for guest in GUESTS:
        path = SOURCE / guest / 'visual-review.json'
        if not path.exists():
            raise SystemExit('Not ready: ' + guest + ' visual-review.json')
        reviews[guest] = json.loads(path.read_text())
    assert not SUMMARY.exists(), 'Never overwrite a finalized export'
    source = json.loads((SOURCE / 'source.json').read_text())
    assert source['commitCount'] == 45 and source['candidate']['dirty'] is True
    assert digest(SOURCE / 'candidate.patch') == source['candidate']['patchSha256']
    frames = []
    for name in ('source.json', 'base-source.json', 'metadata-erratum.json', 'candidate.patch', 'prepared.json'):
        copy(SOURCE / name, Path('common') / name)
    for name in ('replay.py', 'README.md'):
        copy(SOURCE.parent / name, Path('common') / ('original-preparation-' + name if name == 'README.md' else name))
    frozen_runner = ROOT / 'work/view-startup-latency/exp001/tools/lab/lab.py'
    assert digest(frozen_runner) == source['files']['tools/lab/lab.py']['sha256']
    copy(frozen_runner, Path('common/frozen-native-replay.py'))
    copy(SOURCE / 'host/build.json', Path('common/host-release-build.json'))
    for attempt in sorted((SOURCE / 'attempts').glob('*-host-build')):
        for path in sorted(attempt.iterdir()):
            if path.suffix in {'.stdout', '.stderr', '.stdin'} or path.name.endswith(('.command.json', '.result.json')) or path.name in {'request.json', 'complete.json'}:
                copy(path, Path('common/host-release-build-attempt') / path.name)
    validations = ROOT / 'work/view-startup-latency/exp001/work'
    for name in ('full-dev-build.log', 'full-dev-tests.log', 'full-dev.xml', 'full-dev-commands.json'):
        path = validations / 'lazy-validation' / name
        if path.exists():
            copy(path, Path('common/host-base44-full-validation') / name)
    for path in sorted((validations / 'base45-root-validation').iterdir()):
        if path.is_file():
            copy(path, Path('common/host-base45-affected-validation') / path.name)
    matrix_summary = {}
    for guest in GUESTS:
        folder = SOURCE / guest
        for name in ('visual-review.json', 'matrix.json', 'installed.json', 'install.json', 'build.json', 'build-info.json', 'start.json', 'dev.xml', 'watcher-registration.json', 'final-accepted.json', 'full-matrix-followup-review.json', 'root-settled-add-review.json', 'console-unlock-receipt.json'):
            if (folder / name).exists():
                copy(folder / name, Path(guest) / name)
        matrix = json.loads((folder / 'matrix.json').read_text())
        assert len(matrix['cases']) == 12
        matrix_summary[guest] = {'passed': sum(bool(case['result']['ok']) for case in matrix['cases']), 'total': 12}
        for number, case in enumerate(matrix['cases'], 1):
            control = Path(case['controlDirectory'])
            assert control.is_dir() and control.resolve().is_relative_to(ROOT / 'work/lab-runs')
            relative = Path(guest) / 'matrix' / f"{number:02d}-{case['client']}-{case['scenario']}"
            for name in ('parameters.json', 'results.json', 'results.xml', 'versions.json', 'test-events.jsonl'):
                if (control / name).exists():
                    copy(control / name, relative / name)
            witness = Path(case['result']['witness']['path'])
            assert digest(witness) == case['result']['witness']['sha256']
            frames.append(export_frame(witness, relative / 'final.png'))
        # Only known build/test outputs are exported. Arbitrary resident/helper
        # stdout, stderr, profile data, payloads and password-input attempts are excluded.
        for attempt in sorted((folder / 'attempts').iterdir()):
            for failure in attempt.glob('failed.json'):
                copy(failure, Path(guest) / 'failures' / attempt.name / failure.name)
            if attempt.name.endswith('-guest-build'):
                for path in sorted(attempt.iterdir()):
                    if path.suffix in {'.stdout', '.stderr', '.stdin'} or path.name.endswith(('.command.json', '.result.json')) or path.name in {'request.json', 'complete.json'}:
                        copy(path, Path(guest) / 'build-attempts' / attempt.name / path.name)
            elif attempt.name.endswith(('-install', '-start')):
                for path in sorted(attempt.iterdir()):
                    if path.name.endswith(('.command.json', '.result.json')) or path.name in {'request.json', 'complete.json'}:
                        copy(path, Path(guest) / 'identity-attempts' / attempt.name / path.name)
        for path in sorted(folder.glob('*contact*.png')):
            copy(path, Path(guest) / path.name)
    labels = {
        'cinnamon': ['empty-wide', 'empty-compact', 'empty-compact-add', 'configured-wide-simple', 'configured-wide-advanced-first', 'configured-wide-capture-tab', 'configured-wide-shortcuts-tab', 'configured-wide-preferences-tab', 'configured-postcancel-simple-proof', 'activity-reopen-normal-closed', 'full-matrix-fixture-close', 'matrix-failure-current', 'diagnostic-selector-drag'],
        'gnome': ['settings-empty-wide-1200', 'settings-empty-compact', 'settings-empty-compact-add-draft', 'settings-configured-simple-compact', 'settings-configured-advanced-compact', 'settings-capture-tab-first', 'settings-shortcuts-tab-first', 'settings-preferences-tab-first', 'settings-preferences-wide-retained', 'settings-configured-normal-close', 'activity-final-normal-close'],
        'kde': ['settings-empty-wide-settled', 'settings-empty-wide-missing-add-settled', 'settings-empty-wide-recovery-collapsed', 'settings-empty-compact-collapsed', 'settings-empty-compact-add-draft', 'settings-configured-wide-simple-settled', 'settings-configured-compact-advanced', 'settings-compact-capture-tab', 'settings-compact-capture-shift-tab', 'settings-compact-shortcuts', 'settings-configured-cancel', 'activity-final-normal-close'],
    }
    for guest, names in labels.items():
        for label in names:
            path = SOURCE / guest / 'views' / (label + '.png')
            if path.exists():
                copy(path, Path(guest) / 'views' / path.name)
                if path.with_suffix('.json').exists():
                    copy(path.with_suffix('.json'), Path(guest) / 'views' / (label + '.json'))
    # Preserve the original Cinnamon region failure as an independent attempt;
    # the later full matrix must not erase or rewrite it as a first-pass success.
    failure_control = ROOT / 'work/lab-runs/ui43-cinnamon-20261009T042304792474-1'
    for name in ('parameters.json', 'results.json', 'results.xml', 'versions.json', 'test-events.jsonl'):
        if (failure_control / name).exists():
            copy(failure_control / name, Path('cinnamon/original-region-timeout') / name)
    diagnostic = SOURCE / 'cinnamon/attempts/20261009T043155260643-diagnostic-region'
    for name in ('request.json', 'selector.json', 'drag.json', 'result.json'):
        copy(diagnostic / name, Path('cinnamon/controlled-region-diagnostic') / name)
    copy(SOURCE / 'cinnamon/diagnostic-region.py', Path('cinnamon/controlled-region-diagnostic/replay.py'))
    readme = OUT / 'README.md'
    readme.write_text('''# Lazy Settings native correctness evidence\n\nThis cohort tested `0.1.1-dev.45+g9d142919e5e6.dirty`: committed base45 plus the exact four-path Settings overlay in `common/candidate.patch`. It is not clean46 and must not be scored as a frozen performance comparison. The complete source-file hash map and original metadata are retained; `metadata-erratum.json` corrects only the original Base44 prose.\n\nThe three existing disposable overlays remained visible through VNC5938/5936/5934 and the display wall. No golden disk was booted or modified. Settings tests used synthetic local-only configuration. Each matrix ran twelve named actions against independent Qt/GTK fixtures; per-case parameters retain timing, dimensions, hotkeys, package/binary hashes, versions, result IDs and structured event logs. Final frames are lossless PNG conversions with original PPM hashes recorded in the summary. These are bounded single-head correctness tests, not full desktop-profile certification.\n\n`common/replay.py` is the orchestration snapshot. `common/frozen-native-replay.py` is the exact production lab runner used by the matrix. `common/original-preparation-README.md` is preserved as preparation guidance; its historical statement that no actions had yet run describes its preparer, not this executed cohort. Use the actual attempt receipts for executed commands. Replaying requires new attempt/output directories, exact installed identities, existing visible unlocked test sessions, and synthetic local-only fixtures. Native GUI actions are serialized within each guest.\n\nThe original Cinnamon region timeout, controlled successful diagnostic and later complete twelve-case repeat are separate evidence. The timeout's cause remains unconfirmed. GNOME's locked-start tray behavior and KDE's resize/Add-destination and exact-geometry gates must be read from their actual receipts; earlier source43 gates are not silently replaced by this candidate. KDE's root-settled-add-review preserves the root's correction after independently inspecting the actual alleged-missing frames.\n\nGNOME's reviewer statement that no credentials were read describes that reviewer agent. The root agent used protected original VM bootstrap logins in memory for masked typing through the visible consoles, as the user requested. Those logins were never printed, logged or placed in command arguments. No credentials.txt was read. Only unlock metadata is retained here; no password-field frame or keystroke sequence is exported.\n\nOnly allowlisted source metadata, synthetic-case results, known build/CTest logs, identity commands, and selected synthetic graphical witnesses are exported. No profile, payload file, credential input, password-input attempt, arbitrary application stdout/stderr, package, disk, or whole source archive is included. No startup-latency, quiet memory, 40MiB target, S3, or complete-plan acceptance claim is made.\n''')
    files = []
    for path in sorted(OUT.rglob('*')):
        if path.is_file():
            if path.suffix != '.png':
                data = path.read_bytes()
                assert re.search(rb'AKIA[A-Z0-9]{16}', data) is None, 'Secret-shaped access key rejected'
            files.append({'path': str(path.relative_to(ROOT)), 'sha256': digest(path), 'bytes': path.stat().st_size})
    summary = {
        'schemaVersion': 1, 'recordedUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'sourceCommit': source['commit'], 'sourceTree': source['tree'], 'commitCount': 45,
        'version': '0.1.1-dev.45+g9d142919e5e6.dirty', 'dirty': True,
        'candidatePatchSha256': source['candidate']['patchSha256'],
        'archiveSha256': source['archiveSha256'], 'bundleSha256': source['candidate']['bundleSha256'],
        'nativeMatrix': matrix_summary,
        'boundedVisualOutcomes': {g: {'overall': r.get('overall', r.get('overallOutcome', 'See individual case statuses in actual review receipt')), 'caseStatuses': {k: v.get('status') for k, v in r.get('cases', {}).items()} if isinstance(r.get('cases'), dict) else {}, 'blockedGates': r.get('blockedGates', []), 'notEstablished': r.get('notEstablished', [])} for g, r in reviews.items()},
        'scope': 'Bounded current45dirty native correctness only; exact installed identities and twelve synthetic cases per guest. No clean46, performance, remote-storage or full-profile claim.',
        'priorFailure': 'Original Cinnamon region timeout retained, cause unconfirmed. Controlled diagnostic and later root-reviewed complete12/12 repeat are separate receipts.',
        'source43Gates': 'Earlier GNOME locked-start tray and KDE Add-destination resize/exact700px gates remain in source43 evidence; this cohort records its own actual outcomes.',
        'convertedFrames': frames, 'files': files,
    }
    SUMMARY.write_text(json.dumps(summary, indent=2) + '\n')
    assert json.loads(SUMMARY.read_text()) == summary
    for record in files:
        assert digest(ROOT / record['path']) == record['sha256']
    print(json.dumps({'files': len(files), 'bytes': sum(f['bytes'] for f in files), 'summary': str(SUMMARY), 'hashesValidated': True}))


if __name__ == '__main__':
    main()
