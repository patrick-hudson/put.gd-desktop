#!/usr/bin/env python3
"""Observe PID-owned X11 windows beside the unchanged physical idle benchmark.

Only this opt-in command runs the benchmark. Importing it or asking for help
does not start a resident, query the display, or acquire user content.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time


REPO_ROOT = Path(__file__).resolve().parents[2]
IDLE_HARNESS = REPO_ROOT / 'tests/performance/bench_idle.py'
WINDOW_POLL_SECONDS = 1


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    """Atomically write private measurement metadata, without a shell."""
    path = Path(path)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent,
                                     prefix=path.name + '.', delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(value, stream, indent=2)
            stream.write('\n')
            stream.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def executable(value):
    path = Path(value).resolve(strict=True)
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError('app_and_cli_must_be_executable_files')
    return path


def check_physical_session():
    if (os.environ.get('XDG_SESSION_TYPE') != 'x11'
            or 'Cinnamon' not in os.environ.get('XDG_CURRENT_DESKTOP', '')):
        raise RuntimeError('physical_cinnamon_x11_required')
    result = subprocess.run(['systemd-detect-virt'], capture_output=True,
                            text=True, timeout=5)
    if result.stdout.strip() != 'none':
        raise RuntimeError('physical_machine_required')


def environment_metadata():
    """Export selected system identity only; never dump the environment."""
    packages = ['cinnamon', 'muffin', 'libqt6core6', 'libqt6core6t64',
                'libgtk-3-0', 'libgtk-3-0t64', 'libcairo2', 'libc6']
    result = subprocess.run(['dpkg-query', '-W', '-f=${Package} ${Version}\n',
                             *packages], capture_output=True, text=True, timeout=5)
    versions = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] in packages:
            versions[fields[0]] = fields[1]
    cpu = next((line.split(':', 1)[1].strip()
                for line in Path('/proc/cpuinfo').read_text().splitlines()
                if line.startswith('model name')), 'unavailable')
    return {'kernel': os.uname().release, 'machine': os.uname().machine,
            'cpu': cpu, 'desktop': os.environ.get('XDG_CURRENT_DESKTOP', ''),
            'session': os.environ.get('XDG_SESSION_TYPE', ''),
            'python': sys.version.split()[0], 'packages': versions}


def owned_windows(pid):
    """Use EWMH IDs and PID ownership; never enumerate/export window titles."""
    result = subprocess.run(['xprop', '-root', '_NET_CLIENT_LIST'], text=True,
                            capture_output=True, timeout=5)
    if result.returncode or '_NET_CLIENT_LIST(WINDOW)' not in result.stdout:
        raise RuntimeError('ewmh_client_list_unavailable')
    windows = []
    for window in re.findall(r'0x[0-9a-fA-F]+', result.stdout):
        prop = subprocess.run(['xprop', '-id', window, '_NET_WM_PID'], text=True,
                              capture_output=True, timeout=5)
        if prop.returncode or not re.search(r'=\s*' + str(pid) + r'\s*$', prop.stdout):
            continue
        # A withdrawn transient can disappear between enumeration and lookup.
        info = subprocess.run(['xwininfo', '-id', window], text=True,
                              capture_output=True, timeout=5)
        if info.returncode == 0 and 'Map State: IsViewable' in info.stdout:
            windows.append(window.lower())
    return sorted(set(windows))


def operation_count(root):
    """Read only counts from the harness's isolated journal."""
    databases = sorted(set((root / 'data').rglob('*.sqlite'))
                       | set((root / 'data').rglob('*.db')))
    counts = []
    for database in databases:
        with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as connection:
            tables = {row[0] for row in connection.execute(
                'SELECT name FROM sqlite_master WHERE type="table"')}
            if 'operations' in tables:
                counts.append(connection.execute('SELECT COUNT(*) FROM operations').fetchone()[0])
    if not counts:
        raise RuntimeError('isolated_journal_unavailable')
    return sum(counts)


def resident_children(process, app):
    children = Path(f'/proc/{process.pid}/task/{process.pid}/children')
    try:
        identifiers = children.read_text().split()
    except FileNotFoundError:
        return []
    candidates = []
    for identifier in identifiers:
        try:
            if (Path('/proc') / identifier / 'exe').resolve(strict=True) == app:
                candidates.append(int(identifier))
        except FileNotFoundError:
            continue
    return candidates


def stop_owned_process_group(process):
    """Children were launched in this command's new process session."""
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)


def run(args):
    check_physical_session()
    app, cli = executable(args.app), executable(args.cli)
    root = Path(args.output).resolve()
    sidecar = root.with_name(root.name + '-windows.json')
    if root.exists() or sidecar.exists() or not root.parent.is_dir():
        raise ValueError('output_and_sidecar_must_be_new_with_existing_parent')
    command = [sys.executable, str(IDLE_HARNESS), '--app', str(app), '--cli', str(cli),
               '--output', str(root), '--seconds', str(args.seconds)]
    result = {'schemaVersion': 2,
              'scope': 'PID-owned EWMH window observations beside the frozen physical idle workload; no pixels, payloads, clipboard or unrelated titles exported',
              'observerSha256': sha256(__file__), 'harnessSha256': sha256(IDLE_HARNESS),
              'appSha256': sha256(app), 'cliSha256': sha256(cli),
              'benchmarkCommand': command, 'workingDirectory': str(REPO_ROOT),
              'environment': environment_metadata(),
              'parameters': {'seconds': args.seconds,
                             'ownerWindowPollingSeconds': WINDOW_POLL_SECONDS},
              'limitations': ['Window visibility is sampled, so a window can appear and disappear between polls.',
                              'The observer adds host work without changing the frozen resident workload.',
                              'An uninterrupted observation and zero operations do not establish the approximately 40 MiB target.'],
              'observations': [], 'pollCount': 0, 'status': 'running'}
    write_json(sidecar, result)
    process = subprocess.Popen(command, cwd=REPO_ROOT, start_new_session=True)
    started = time.monotonic()
    previous = {}
    try:
        while process.poll() is None:
            for pid in resident_children(process, app):
                native = owned_windows(pid)
                result['pollCount'] += 1
                if previous.get(pid) != native:
                    result['observations'].append({'elapsedSeconds': time.monotonic() - started,
                                                   'residentPid': pid,
                                                   'mappedOwnedWindows': native})
                    previous[pid] = native
                    write_json(sidecar, result)
            time.sleep(WINDOW_POLL_SECONDS)
        result['benchmarkExitCode'] = process.wait(timeout=5)
        result['operationCount'] = operation_count(root)
        result['noOwnedActivationObserved'] = (
            result['pollCount'] > 0
            and not any(row['mappedOwnedWindows'] for row in result['observations'])
            and result['operationCount'] == 0)
        result['coldIdleUninterrupted'] = (result['benchmarkExitCode'] == 0
                                          and result['noOwnedActivationObserved'])
        result['status'] = 'complete' if result['coldIdleUninterrupted'] else 'invalid-idle-observation'
        write_json(sidecar, result)
        print(json.dumps({key: result[key] for key in
                          ['status', 'benchmarkExitCode', 'operationCount',
                           'noOwnedActivationObserved', 'coldIdleUninterrupted']}), flush=True)
        return 0 if result['coldIdleUninterrupted'] else 1
    except BaseException as error:
        result['status'] = 'observer-failed-or-interrupted'
        result['errorType'] = type(error).__name__
        write_json(sidecar, result)
        raise
    finally:
        stop_owned_process_group(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--output', required=True, help='New private benchmark directory; its sibling stores window observations')
    parser.add_argument('--seconds', type=int, default=300)
    args = parser.parse_args()
    if args.seconds < 1:
        parser.error('--seconds must be positive; the idle budget requires at least 300')
    try:
        return run(args)
    except (OSError, ValueError, RuntimeError, sqlite3.Error, subprocess.SubprocessError):
        print('Idle observation failed; inspect the private sidecar and benchmark metadata.', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
