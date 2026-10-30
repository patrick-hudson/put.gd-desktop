"""Live-destination admission: who may produce evidence with real links.

DriverContext enforces this at construction for every non-local destination,
run_recipes refuses a live run whose context was not admitted, and run.main
checks it again before touching a target. A refusal names fixed blockers only.

    python3 -m tools.lab.surface_driver.gate check-catalog CATALOG
    python3 -m tools.lab.surface_driver.gate write-pin CATALOG

check-catalog compares a rendered ui-catalog's secret-control states with the
committed pin (CI runs it on every catalog); write-pin regenerates the pin
after an intended UI change.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import sys

PIN = Path(__file__).with_name('secret-states.json')
COMMIT = re.compile(r'[0-9a-f]{40}')
PREFIX = re.compile(r'[0-9a-f]{12,40}')
NOT_SECRET_CLASSES = ('QPushButton', 'QToolButton', 'QCheckBox', 'QRadioButton')


from .api import RecipeBlocked


class LiveEvidenceRefused(RecipeBlocked):
    """Raised before any live evidence exists; the message lists blockers only."""


def build_matches(expected, commit):
    """--build names one exact commit: 12-40 hex, a prefix of a full commit.

    Version strings and short prefixes are never accepted, for the catalog and
    for the resident's own reported build.
    """
    expected = expected.lower() if isinstance(expected, str) else ''
    commit = commit.lower() if isinstance(commit, str) else ''
    return bool(PREFIX.fullmatch(expected) and COMMIT.fullmatch(commit) and commit.startswith(expected))


def catalog_states(directory):
    """{capture: sorted secret-control names} from the manifest-declared inventories.

    Raises ValueError for an incomplete catalog: a missing/foreign manifest,
    or a declared controls file that is absent or unreadable.
    """
    from .redact import SECRET_FIELD
    directory = Path(directory)
    manifest = json.loads((directory/'manifest.json').read_text())
    if manifest.get('kind') != 'synthetic-ui-catalog' or not manifest.get('captures'):
        raise ValueError('catalog-empty')
    states = {}
    for capture in manifest['captures']:
        name = capture.get('controls')
        if not name:
            continue  # native-only captures declare no inventory
        path = directory/name
        if Path(name).name != name or not path.is_file():
            raise ValueError('catalog-controls-missing')
        controls = json.loads(path.read_text()).get('controls', [])
        secret = sorted({c['name'] for c in controls if isinstance(c, dict) and isinstance(c.get('name'), str)
                         and (SECRET_FIELD.search(c['name']) or c.get('sensitive')) and c.get('class') not in NOT_SECRET_CLASSES})
        if secret:
            states[name[:-len('.controls.json')] if name.endswith('.controls.json') else name] = secret
    return manifest, states


def expected_secret_states():
    return json.loads(PIN.read_text())['states']


def catalog_blockers(catalog, build):
    """A complete ui-catalog of this exact clean build whose secret-control
    states equal the committed pin (every History and destination/credential
    state, each with its secret controls)."""
    if not catalog or not build:
        return ['catalog-missing']
    try:
        manifest, states = catalog_states(catalog)
    except (OSError, ValueError) as error:
        reason = str(error)
        return [reason if reason in ('catalog-empty', 'catalog-controls-missing') else 'catalog-missing']
    blockers = []
    identity = manifest.get('build') if isinstance(manifest.get('build'), dict) else {}
    if not build_matches(build, identity.get('gitCommit')) or identity.get('dirty') is not False:
        blockers.append('catalog-build-mismatch')
    try:
        expected = expected_secret_states()
    except (OSError, ValueError, KeyError):
        return blockers+['catalog-pin-unavailable']
    if states != expected:
        blockers.append('catalog-secret-states-differ-from-pin')
    return blockers


def secret_safe_evidence_blockers(placement=None, catalog=None, build=None, physical=None):
    """Check the loaded persistence path, not a global live-upload switch.

    Live destinations also need a disposable placement (boat sandbox or owned
    guest), an exact-build pinned catalog and an OCR that reads a fresh random
    token at small text sizes. Physical desktops are refused: clipboard
    managers and browser history keep working links outside the run (#828).
    """
    from tools.lab.surface_driver import redact, receipts, context, run
    blockers = []
    if (receipts.Redactor is not redact.Redactor or getattr(receipts.Redactor, 'evidence_revision', None) != 2
            or not all(callable(getattr(receipts.Redactor, name, None)) for name in ('remember', 'text', 'safe', 'add_link_hosts'))):
        blockers.append('redaction-not-loaded')
    if (run.Evidence is not receipts.Evidence or context.Evidence is not run.Evidence or run.DriverContext is not context.DriverContext
            or receipts.save_frame is not redact.save_frame or not callable(receipts.save_frame)
            or getattr(receipts.save_frame, 'evidence_revision', None) != 2
            or getattr(getattr(run.Evidence, 'frame', None), 'prewrite_gate', None) is not receipts.save_frame
            or getattr(context.DriverContext.frame, 'live_policy', None) is not context.DriverContext._frame_regions
            or getattr(context.DriverContext, 'live_gate', None) is not secret_safe_evidence_blockers
            or not callable(redact.ocr_text)):
        blockers.append('prewrite-frame-gate-not-loaded')
    if (run.scan_run is not redact.scan_run or receipts.scan_run is not run.scan_run or not callable(run.scan_run)
            or getattr(run.scan_run, 'evidence_revision', None) != 2
            or getattr(getattr(run.Evidence, 'scan', None), 'run_audit', None) is not run.scan_run
            or not callable(run.finish_evidence)
            or getattr(run.run_recipes, 'final_audit', None) is not run.finish_evidence):
        blockers.append('end-run-scan-not-wired')
    if not (ocr := shutil.which('tesseract')):
        blockers.append('ocr-unavailable-install-tesseract-in-disposable-target')
    else:
        try:
            if not redact.ocr_self_test(ocr):
                blockers.append('ocr-self-test-failed')
        except Exception:
            blockers.append('ocr-self-test-failed')
    if placement not in ('boat', 'guest'):
        blockers.append('placement-not-disposable')
    elif physical and placement != 'boat':
        blockers.append('physical-desktop-refused')
    blockers += catalog_blockers(catalog, build)
    return blockers


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('check-catalog', 'write-pin'))
    parser.add_argument('catalog', type=Path)
    args = parser.parse_args(argv)
    try:
        manifest, states = catalog_states(args.catalog)
    except (OSError, ValueError) as error:
        print('incomplete catalog: '+str(error), file=sys.stderr)
        return 2
    if args.command == 'write-pin':
        PIN.write_text(json.dumps({'schemaVersion': 1, 'source': 'putgd-ui-catalog offscreen render; CI (linux 26.04) checks every rendered catalog against this pin',
                                   'states': states}, indent=2, sort_keys=True)+'\n')
        print('pinned %d secret-control states' % len(states))
        return 0
    expected = expected_secret_states()
    if states == expected:
        print('secret-control states match the pin (%d states)' % len(states))
        return 0
    for name in sorted(set(states) | set(expected)):
        if states.get(name) != expected.get(name):
            print('%s: catalog %s, pin %s' % (name, states.get(name), expected.get(name)), file=sys.stderr)
    print('the catalog\'s secret-control states differ from %s; if the UI change is intended, run '
          '`python3 -m tools.lab.surface_driver.gate write-pin CATALOG` and commit it' % PIN.relative_to(PIN.parents[3]),
          file=sys.stderr)
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
