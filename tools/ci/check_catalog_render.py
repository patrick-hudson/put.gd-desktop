#!/usr/bin/env python3
"""Render a complete offscreen UI catalog without comparing pixels.

The ui_catalog_render CTest runs this on the Ubuntu 26.04 CI leg. The
catalog's own checks must pass, and its manifest must describe a full,
synthetic render with no skipped captures and every listed image present.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

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
# The desktop's colour scheme must not decide the render (cmake/TestsGui.cmake, #251).
UNSET = ('DBUS_SESSION_BUS_ADDRESS', 'XDG_CURRENT_DESKTOP', 'QT_QPA_PLATFORMTHEME', 'WAYLAND_DISPLAY', 'DISPLAY')


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

    Skipped captures never constitute a complete render.

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
    skipped = [s.get('state') for s in manifest.get('skippedCaptures') or []]
    if skipped:
        return 'the catalog skipped captures: ' + ', '.join(map(str, skipped))
    images = manifest.get('images') or []
    captures = [c.get('file') for c in manifest.get('captures') or []]
    if not captures or not images:
        return 'manifest.json lists no images'
    missing = [name for name in images + captures if not name or not (Path(directory) / name).is_file()]
    if missing:
        return 'images listed in manifest.json are missing: ' + ', '.join(map(str, missing[:10]))
    if '46a-capture-countdown.png' in images:
        try:
            pill = json.loads((Path(directory) / '46a-capture-countdown.controls.json').read_text())
        except (OSError, ValueError):
            return 'countdown cancel inventory is missing or unreadable'
        bounds = [0, 0, pill.get('width'), pill.get('height')]
        if not any(c.get('name') == 'captureCountdown' and c.get('kind') == 'button'
                   and c.get('enabled') is True and c.get('rect') == bounds
                   for c in pill.get('controls', [])):
            return 'countdown cancel control does not cover the full pill'
    return ''


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--catalog', type=Path, required=True, help='the putgd-ui-catalog executable')
    parser.add_argument('--scheme', choices=SCHEMES, required=True)
    parser.add_argument('--output', type=Path, required=True, help='render directory (replaced)')
    parser.add_argument('--text-scale', type=float, help='PUTGD_TEXT_SCALE for the render')
    args = parser.parse_args(argv)
    output = args.output.resolve()
    prepare_output(output)
    report = {'scheme': args.scheme, 'textScale': args.text_scale,
              'clock': CLOCK + ' UTC'}

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
    return finish(True)


if __name__ == '__main__':
    sys.exit(main())
