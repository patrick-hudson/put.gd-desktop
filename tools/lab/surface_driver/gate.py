"""Live-destination admission: who may produce evidence with real links.

DriverContext enforces this at construction for every non-local destination,
run_recipes refuses a live run whose context was not admitted, and run.main
checks it again before touching a target. A refusal names fixed blockers only.

    python3 -m tools.lab.surface_driver.gate check-catalog CATALOG
    python3 -m tools.lab.surface_driver.gate write-pin CATALOG

check-catalog compares a rendered ui-catalog's secret-control states with the
checkout's pin (CI renders and checks the catalog whenever the pin or the
classifier changes); write-pin regenerates the pin after an intended UI change.

A live run never trusts the driver checkout's pin or classifier: it reads the
pin, redact.SECRET_FIELD and NOT_SECRET_CLASSES from the build commit itself
(git show, after the build is verified as one exact commit), compares the
catalog with that pin, and refuses when the loaded classifier or the
checkout's pin differ from the build's.
"""
import argparse
import ast
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

PIN = Path(__file__).with_name('secret-states.json')
ROOT = Path(__file__).resolve().parents[3]
PIN_PATH = 'tools/lab/surface_driver/secret-states.json'
REDACT_PATH = 'tools/lab/surface_driver/redact.py'
GATE_PATH = 'tools/lab/surface_driver/gate.py'
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
    """The driver checkout's pin (check-catalog, write-pin); live runs use build_classifier."""
    return json.loads(PIN.read_text())['states']


def checkout_pin():
    return json.loads(PIN.read_text())['states']


def build_source(commit, path):
    """A file as committed at commit, from the driver checkout's git objects."""
    if not COMMIT.fullmatch(commit or ''):
        raise ValueError('build-source-unavailable')
    result = subprocess.run(['git', '-C', str(ROOT), 'show', commit+':'+path], capture_output=True, timeout=20)
    if result.returncode:
        raise ValueError('build-source-unavailable')
    return result.stdout.decode()


def _assigned(source, name):
    for node in ast.parse(source).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return node.value
    raise ValueError('build-source-unavailable')


def build_classifier(commit):
    """(pin states, SECRET_FIELD pattern, its flags, NOT_SECRET_CLASSES) at commit."""
    pin = json.loads(build_source(commit, PIN_PATH))['states']
    call = _assigned(build_source(commit, REDACT_PATH), 'SECRET_FIELD')
    if not (isinstance(call, ast.Call) and call.args and isinstance(call.args[0], ast.Constant)
            and isinstance(call.args[0].value, str)):
        raise ValueError('build-source-unavailable')
    flags = 0
    for argument in call.args[1:]:
        if not (isinstance(argument, ast.Attribute) and argument.attr in ('I', 'IGNORECASE')):
            raise ValueError('build-source-unavailable')
        flags |= re.I
    classes = ast.literal_eval(_assigned(build_source(commit, GATE_PATH), 'NOT_SECRET_CLASSES'))
    return pin, call.args[0].value, re.compile(call.args[0].value, flags).flags, tuple(classes)


def classifier_blockers(commit):
    """The build's pin, or a blocker when the driver would classify differently.

    The loaded SECRET_FIELD/NOT_SECRET_CLASSES and the checkout's pin must be
    the build commit's own; any edit (or runtime patch) is refused.
    """
    from . import redact
    try:
        pin, pattern, flags, classes = build_classifier(commit)
    except (OSError, ValueError, KeyError, TypeError, SyntaxError, subprocess.SubprocessError):
        return None, ['catalog-pin-unavailable']
    blockers = []
    if (redact.SECRET_FIELD.pattern, redact.SECRET_FIELD.flags, NOT_SECRET_CLASSES) != (pattern, flags, classes):
        blockers.append('driver-classifier-differs-from-build')
    try:
        if checkout_pin() != pin:
            blockers.append('driver-pin-differs-from-build')
    except (OSError, ValueError, KeyError):
        blockers.append('driver-pin-differs-from-build')
    return pin, blockers


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
    identity = manifest.get('build') if isinstance(manifest.get('build'), dict) else {}
    if not build_matches(build, identity.get('gitCommit')) or identity.get('dirty') is not False:
        # No exact clean commit: nothing trustworthy to read a pin from.
        return ['catalog-build-mismatch']
    expected, blockers = classifier_blockers(identity['gitCommit'].lower())
    if expected is None:
        return blockers
    if states != expected:
        blockers.append('catalog-secret-states-differ-from-pin')
    return blockers


def secret_safe_evidence_blockers(placement=None, catalog=None, build=None, physical=None, target=None, details=None):
    """Check the loaded persistence path, not a global live-upload switch.

    Live destinations also need a disposable placement (boat sandbox or owned
    guest), an exact-build pinned catalog and an OCR that reads a fresh random
    token at small text sizes. Physical desktops are refused: clipboard
    managers and browser history keep working links outside the run (#828).

    With a target (DriverContext always passes one) the placement label is
    proven: an owned QEMU guest, or a boat sandbox's own head. run.main's
    check before any target exists cannot prove it; construction does.
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
    elif target is not None:
        try:
            run.prove_disposable(target, placement, details)
        except RecipeBlocked:
            blockers.append('placement-unproven')
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
