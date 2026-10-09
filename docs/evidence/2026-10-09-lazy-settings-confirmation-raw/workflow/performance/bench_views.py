#!/usr/bin/env python3
"""Measure empty Settings, History and Activity view retention on physical Cinnamon.

This opt-in diagnostic opens and closes only its own resident's windows. It does
not acquire screenshots, clipboard content, credentials, or cloud objects, and
does not establish cold idle or the approximately 40 MiB acceptance target.
"""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from observe_idle import (IDLE_HARNESS, check_physical_session, environment_metadata,
                          executable, operation_count, owned_windows, sha256,
                          stop_owned_process_group, write_json)


def fixture_config(root):
    """Accept a recorded successful idle fixture, then copy only its empty config."""
    result_path = root / 'results.json'
    result = json.loads(result_path.read_text())
    if (result.get('session') != 'x11'
            or 'Cinnamon' not in result.get('desktop', '')
            or result.get('elapsedSeconds', 0) < 300
            or result.get('budgetPassed') is not True
            or operation_count(root) != 0):
        raise ValueError('fixture_must_be_a_successful_empty_physical_idle_run')
    candidates = list((root / 'config' / 'putgd').rglob('config.json'))
    if len(candidates) != 1:
        raise ValueError('fixture_requires_one_putgd_config')
    config = candidates[0]
    if config.is_symlink() or not config.resolve().is_relative_to(root):
        raise ValueError('fixture_config_must_stay_inside_fixture')
    document = json.loads(config.read_text())
    actions = document.get('actions', {})
    if (document.get('profiles') != []
            or set(actions) != {'region', 'window', 'desktop', 'clipboard'}
            or any(action.get('destination') != 'local'
                   or action.get('profileId') is not None
                   or action.get('shortcut') != '' for action in actions.values())
            or document.get('preferences') != {'notifications': False, 'launchAtLogin': False}):
        raise ValueError('fixture_requires_empty_profiles_local_actions_and_disabled_shortcuts')
    return config.relative_to(root / 'config'), document, {
        'fixtureName': root.name, 'resultsSha256': sha256(result_path),
        'configSha256': sha256(config), 'idleAppSha256': result['appSha256'],
        'idlePeakSampledRssBytes': result['peakSampledRssBytes']}


def run(args):
    check_physical_session()
    app, cli = executable(args.app), executable(args.cli)
    fixture = Path(args.fixture).resolve(strict=True)
    relative_config, document, fixture_metadata = fixture_config(fixture)
    root = Path(args.output).resolve()
    root.mkdir(mode=0o700, exist_ok=False)
    environment = os.environ.copy()
    for key, name in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'),
                      ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')]:
        directory = root / name
        directory.mkdir(mode=0o700)
        environment[key] = str(directory)
    config = root / 'config' / relative_config
    config.parent.mkdir(parents=True, mode=0o700)
    # All payload destinations stay private even if a future fixture is reused.
    document['archive']['root'] = str(root / 'archive')
    write_json(config, document)
    environment['QT_QPA_PLATFORM'] = 'xcb'
    specification = importlib.util.spec_from_file_location('frozen_idle', IDLE_HARNESS)
    benchmark = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(benchmark)
    result = {'schemaVersion': 2,
              'scope': 'physical Cinnamon X11 empty-view lifecycle diagnostic; no pixels, payloads, clipboard or cloud calls; no idle budget claim',
              'appSha256': sha256(app), 'cliSha256': sha256(cli),
              'workloadSha256': sha256(__file__), 'observerSha256': sha256(Path(__file__).with_name('observe_idle.py')),
              'frozenIdleHarnessSha256': sha256(IDLE_HARNESS),
              'environment': environment_metadata(), 'fixture': fixture_metadata,
              'command': [sys.executable, str(Path(__file__).resolve()),
                          '--app', str(app), '--cli', str(cli), '--fixture', str(fixture),
                          '--output', str(root), '--cycles', str(args.cycles),
                          '--idle-seconds', str(args.idle_seconds)],
              'parameters': {'cycles': args.cycles, 'startupWarmupSeconds': 5,
                             'visibleSettlingSeconds': 2, 'hiddenIdleSeconds': args.idle_seconds,
                             'hiddenWindowPollingSeconds': .1, 'readinessTimeoutSeconds': 20,
                             'hiddenWindowTimeoutSeconds': 5},
              'limitations': ['View retention is separate from untouched cold idle and the approximately 40 MiB target.',
                              'Sparse lifecycle samples do not prove absence of leaks or a permanent plateau.',
                              'Footprint maps export basenames only; no screenshots or unrelated titles are recorded.',
                              'This harness does not enable or reproduce temporary allocator instrumentation.'],
              'samples': [], 'activations': [], 'passed': False, 'status': 'starting'}
    write_json(root / 'progress.json', result)

    def request(method, params=None):
        output = subprocess.check_output([str(cli), method, json.dumps(params or {})],
                                         env=environment, stderr=subprocess.DEVNULL, timeout=10)
        reply = json.loads(output)
        if not reply.get('ok'):
            raise RuntimeError('resident_request_failed')
        return reply['result']

    with (root / 'resident.log').open('wb') as log:
        os.chmod(root / 'resident.log', 0o600)
        resident = subprocess.Popen([str(app), '--background'], env=environment,
                                    stdout=log, stderr=log, start_new_session=True)
        started = time.monotonic()

        def sample(stage):
            if resident.poll() is not None:
                raise RuntimeError('resident_exited')
            groups = benchmark.footprint(resident.pid)
            status = Path(f'/proc/{resident.pid}/status').read_text()
            row = {'stage': stage, 'elapsedSeconds': time.monotonic() - started,
                   'rssBytes': sum(group['rssBytes'] for group in groups),
                   'pssBytes': sum(group['pssBytes'] for group in groups),
                   'privateBytes': sum(group['privateBytes'] for group in groups),
                   'threads': int(re.search(r'Threads:\s+(\d+)', status)[1]), 'footprint': groups}
            result['samples'].append(row)
            write_json(root / 'progress.json', result)
            print(json.dumps({key: value for key, value in row.items() if key != 'footprint'}), flush=True)

        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if resident.poll() is not None:
                    raise RuntimeError('resident_exited_before_readiness')
                try:
                    if request('diagnostics')['journalReady']:
                        break
                except (subprocess.CalledProcessError, RuntimeError):
                    pass
                time.sleep(.2)
            else:
                raise RuntimeError('resident_readiness_timeout')
            result['status'] = 'running'
            time.sleep(5)
            if owned_windows(resident.pid) or operation_count(root):
                raise RuntimeError('cold_view_fixture_not_empty')
            sample('cold-idle')
            for cycle in range(args.cycles):
                for view, expected_count in [('settings', 1), ('history', 2), ('activity', 3)]:
                    request('ui.show', {'view': view})
                    time.sleep(2)
                    windows = owned_windows(resident.pid)
                    if len(windows) != expected_count:
                        raise RuntimeError('unexpected_mapped_owned_window_count')
                    result['activations'].append({'cycle': cycle, 'view': view,
                                                  'mappedOwnedWindows': windows})
                    sample(f'cycle-{cycle + 1}-{view}-visible')
                for window in owned_windows(resident.pid):
                    # Recheck ownership after sampling: native IDs need not stay
                    # stable when Qt hides and reopens the retained widgets.
                    if window in owned_windows(resident.pid):
                        subprocess.run(['wmctrl', '-ic', window], check=True,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
                deadline = time.monotonic() + 5
                while owned_windows(resident.pid):
                    if time.monotonic() >= deadline:
                        raise RuntimeError('owned_windows_did_not_hide')
                    time.sleep(.1)
                time.sleep(args.idle_seconds)
                sample(f'cycle-{cycle + 1}-hidden-idle')
            result['operationCount'] = operation_count(root)
            if result['operationCount']:
                raise RuntimeError('unexpected_operation_in_view_workload')
            result.update(passed=True, status='complete', allThreeViewsReopened=True,
                          finalHiddenRssBytes=result['samples'][-1]['rssBytes'],
                          peakSampledRssBytes=max(row['rssBytes'] for row in result['samples']))
            write_json(root / 'results.json', result)
            return 0
        except BaseException as error:
            result['status'] = 'failed-or-interrupted'
            result['errorType'] = type(error).__name__
            write_json(root / 'results.json', result)
            raise
        finally:
            stop_owned_process_group(resident)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--fixture', required=True, help='Successful >=300-second private empty idle run directory')
    parser.add_argument('--output', required=True, help='New private diagnostic directory')
    parser.add_argument('--cycles', type=int, default=3)
    parser.add_argument('--idle-seconds', type=int, default=30)
    args = parser.parse_args()
    if args.cycles < 2 or args.idle_seconds < 5:
        parser.error('--cycles must be at least 2 and --idle-seconds at least 5')
    try:
        return run(args)
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError):
        print('View diagnostic failed; inspect its private progress/results metadata.', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
