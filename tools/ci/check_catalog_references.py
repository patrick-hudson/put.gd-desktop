#!/usr/bin/env python3
"""Render the UI catalog for one colour scheme and compare it with its references.

The ui_catalog_* CTest tests run this (CMake option PUTGD_UI_CATALOG_REFERENCES,
enabled on the Ubuntu 26.04 CI leg only). It renders putgd-ui-catalog offscreen
into OUTPUT, keeps the catalog's own layout checks, then compares every state's
PNG with tests/gui/catalog-references/<scheme>/ using tools/ci/image_diff.py.

It fails when a state differs beyond the tolerance, when a reference has no
rendered state, and when a rendered state has no reference. Failing states are
copied to OUTPUT/failures/ as <state>.expected.png, .actual.png and .diff.png,
and OUTPUT/catalog-check.json lists every result. --layout-only renders (for
the 150% text runs) without comparing images.

tools/ci/update_catalog_references.py rewrites the references from a render.
"""
import argparse
import dataclasses
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import image_diff  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
REFERENCES = ROOT / 'tests/gui/catalog-references'
SCHEMES = ('dark', 'light')
REPORT = 'catalog-check.json'
# A stopped wall clock and a fixed zone for the render: History shows times
# and dates taken from the clock (tests/gui/ui_catalog.cpp seeds it from
# "now"), down to milliseconds. Monotonic time, which Qt's timers use, runs.
CLOCK = '2026-06-15 12:00:00'
# The catalog's data folders show in Settings, so their paths are fixed too.
DATA_ROOT = Path('/tmp')
DATA_MARKER = '.putgd-catalog-data'
FULL_SCOPE = {'review': True, 'recordOnly': False, 'editorOnly': False, 'handshake': False}
# The desktop's colour scheme must not decide the render (CMakeLists.txt, #251).
UNSET = ('DBUS_SESSION_BUS_ADDRESS', 'XDG_CURRENT_DESKTOP', 'QT_QPA_PLATFORMTHEME', 'WAYLAND_DISPLAY', 'DISPLAY')
TOLERANCE = image_diff.Tolerance()


def faketime_library():
    for candidate in ('/usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1',
                      '/usr/lib/aarch64-linux-gnu/faketime/libfaketime.so.1',
                      '/usr/lib/faketime/libfaketime.so.1', '/usr/lib64/faketime/libfaketime.so.1'):
        if Path(candidate).is_file():
            return candidate
    return None


def prepare_output(output):
    """Empty OUTPUT, but only when it is new, empty or an earlier run of this script."""
    if output.exists():
        if any(output.iterdir()) and not (output / REPORT).is_file():
            raise SystemExit('refusing to replace %s: not an earlier catalog check (no %s)' % (output, REPORT))
        shutil.rmtree(output)
    output.mkdir(parents=True)


def data_folder(scheme, text_scale):
    """The fixed, emptied data folder of one render variant, and its lock.

    Two renders of one variant (two build trees on one host) take turns.
    """
    folder = DATA_ROOT / ('putgd-catalog-%s%s' % (scheme, '-text-%s' % text_scale if text_scale else ''))
    lock = open(str(folder) + '.lock', 'a')
    fcntl.flock(lock, fcntl.LOCK_EX)
    if folder.exists():
        if not (folder / DATA_MARKER).is_file():
            raise SystemExit('refusing to reuse %s: not a catalog data folder (no %s)' % (folder, DATA_MARKER))
        shutil.rmtree(folder)
    folder.mkdir(mode=0o700)
    (folder / DATA_MARKER).write_text('')
    return folder, lock


def render(binary, scheme, output, text_scale=None, log=sys.stderr):
    """Run the catalog offscreen at the fixed clock; returns (exit status, seconds)."""
    library = faketime_library()
    if library is None:
        raise SystemExit('libfaketime is required for a reproducible catalog clock (apt install faketime)')
    env = {k: v for k, v in os.environ.items() if k not in UNSET and not k.startswith('PUTGD_CATALOG_')}
    env.update({'QT_QPA_PLATFORM': 'offscreen', 'TZ': 'UTC', 'LC_ALL': 'C.UTF-8', 'LANG': 'C.UTF-8',
                'LD_PRELOAD': library, 'FAKETIME': CLOCK, 'FAKETIME_DONT_FAKE_MONOTONIC': '1',
                'PUTGD_CATALOG_THEME': scheme})
    env.pop('PUTGD_TEXT_SCALE', None)
    if text_scale:
        env['PUTGD_TEXT_SCALE'] = str(text_scale)
    data, lock = data_folder(scheme, text_scale)
    try:
        with tempfile.TemporaryDirectory(prefix='pgcat-') as runtime:
            os.chmod(runtime, 0o700)
            env.update({'XDG_RUNTIME_DIR': runtime, 'PUTGD_CATALOG_DATA_DIR': str(data)})
            started = time.monotonic()
            status = subprocess.run([str(binary), str(output)], env=env, stdout=log, stderr=log).returncode
        return status, time.monotonic() - started
    finally:
        shutil.rmtree(data, ignore_errors=True)
        lock.close()


def render_problem(directory):
    """Why DIRECTORY is not a complete, clean catalog render, or '' when it is.

    putgd-ui-catalog writes manifest.json last, with its scope and every image it saved.
    """
    try:
        manifest = json.loads((Path(directory) / 'manifest.json').read_text())
    except (OSError, ValueError):
        return 'no readable manifest.json; the catalog did not finish'
    if manifest.get('kind') != 'synthetic-ui-catalog' or manifest.get('sourceReads') != 0:
        return 'manifest.json is not a clean synthetic catalog run'
    # Every state needs a full offscreen run of a build with the review states.
    if manifest.get('platform') != 'offscreen' or manifest.get('scope') != FULL_SCOPE:
        return 'not a full offscreen catalog run (platform %r, scope %r)' % (manifest.get('platform'),
                                                                             manifest.get('scope'))
    images = manifest.get('images') or []
    captures = [c.get('file') for c in manifest.get('captures') or []]
    if not captures or not images:
        return 'manifest.json lists no images'
    missing = [name for name in images + captures if not name or not (Path(directory) / name).is_file()]
    if missing:
        return 'images listed in manifest.json are missing: ' + ', '.join(map(str, missing[:10]))
    return ''


def states(directory):
    return {p.stem: p for p in Path(directory).glob('*.png')} if Path(directory).is_dir() else {}


def compare(rendered, references, failures, tolerance=TOLERANCE):
    """Compare a rendered catalog directory with a reference directory."""
    actual, expected = states(rendered), states(references)
    failures.mkdir(exist_ok=True)
    results = {}
    for name in sorted(expected.keys() | actual.keys()):
        if name not in actual:
            results[name] = {'status': 'missing', 'reason': 'the catalog no longer renders this state'}
        elif name not in expected:
            results[name] = {'status': 'unreferenced', 'reason': 'no reference image'}
            shutil.copyfile(actual[name], failures / (name + '.actual.png'))
        else:
            result = image_diff.compare_files(expected[name], actual[name], tolerance, failures / (name + '.diff.png'))
            results[name] = {'status': 'match' if result.matches else 'differs', 'reason': result.reason,
                             'comparison': result.to_json()}
            if not result.matches:
                shutil.copyfile(expected[name], failures / (name + '.expected.png'))
                shutil.copyfile(actual[name], failures / (name + '.actual.png'))
    if not any(failures.iterdir()):
        failures.rmdir()
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--catalog', type=Path, required=True, help='the putgd-ui-catalog executable')
    parser.add_argument('--scheme', choices=SCHEMES, required=True)
    parser.add_argument('--output', type=Path, required=True, help='render directory (replaced)')
    parser.add_argument('--references', type=Path, help='default: tests/gui/catalog-references/SCHEME')
    parser.add_argument('--text-scale', type=float, help='PUTGD_TEXT_SCALE for the render')
    parser.add_argument('--layout-only', action='store_true', help="keep the catalog's own checks; compare no images")
    args = parser.parse_args(argv)
    references = args.references or REFERENCES / args.scheme
    output = args.output.resolve()
    prepare_output(output)
    report = {'scheme': args.scheme, 'textScale': args.text_scale, 'layoutOnly': args.layout_only,
              'clock': CLOCK + ' UTC', 'tolerance': dataclasses.asdict(TOLERANCE)}

    def finish(ok, problem=''):
        report.update({'ok': ok, 'problem': problem})
        (output / REPORT).write_text(json.dumps(report, indent=2) + '\n')
        if problem:
            print('ui catalog (%s): %s' % (args.scheme, problem), file=sys.stderr)
        return 0 if ok else 1

    status, seconds = render(args.catalog, args.scheme, output, args.text_scale)
    report['renderSeconds'] = round(seconds, 1)
    print('ui catalog (%s%s): rendered in %.1f s' % (args.scheme, ', text x%s' % args.text_scale
                                                      if args.text_scale else '', seconds))
    if status != 0:
        return finish(False, 'putgd-ui-catalog failed its own checks (exit %d)' % status)
    problem = render_problem(output)
    if problem:
        return finish(False, problem)
    report['qtVersion'] = json.loads((output / 'manifest.json').read_text()).get('qtVersion')
    if args.layout_only:
        return finish(True)
    if not references.is_dir():
        return finish(False, 'no reference directory %s' % references)
    results = compare(output, references, output / 'failures')
    report['states'] = results
    counts = {}
    for result in results.values():
        counts[result['status']] = counts.get(result['status'], 0) + 1
    report['counts'] = counts
    bad = {name: r for name, r in results.items() if r['status'] != 'match'}
    for name, result in bad.items():
        print('  %-12s %s: %s' % (result['status'], name, result['reason']), file=sys.stderr)
    print('ui catalog (%s): %s' % (args.scheme, ', '.join('%d %s' % (n, s) for s, n in sorted(counts.items()))))
    if bad:
        return finish(False, '%d of %d states do not match their references; expected, actual and diff images are '
                             'in %s. If the change is intended, run tools/ci/update_catalog_references.py.'
                      % (len(bad), len(results), output / 'failures'))
    return finish(True)


if __name__ == '__main__':
    sys.exit(main())
