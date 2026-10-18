#!/usr/bin/env python3
"""Run one group of hosted Python contract suites and retain a structured report.

Every suite in a group runs even after an earlier failure. The report records
each command and exit code; the process exits with the first failing code.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]

GROUPS = {
    'release': {
        'globs': ('tests/ci/test_*.py', 'tests/releases/test_*.py', 'tests/unit/test_*.py'),
        'scope': 'Release, packaging and CI tool contracts; no package build or publication',
    },
    'proton': {
        'globs': ('tools/proton/test_crypto_source.py', 'tools/proton/test_redistribution.py'),
        'scope': 'Proton helper source and redistribution contracts; the runtime is provisioned by build jobs',
    },
    'lab': {
        'globs': ('tests/desktop/test_*.py', 'tests/maintenance/test_*.py',
                  'tests/performance/test_bench_ui_process.py'),
        'scope': 'Host protocol and storage safety contracts only; no graphical desktop acceptance',
    },
}


def suites(group):
    found = sorted({path.relative_to(ROOT).as_posix() for pattern in GROUPS[group]['globs']
                    for path in ROOT.glob(pattern) if path.is_file()})
    if not found:
        raise ValueError('Contract group has no suites: ' + group)
    return found


def run_group(group, output):
    commands = [[sys.executable, suite] for suite in suites(group)]
    results, log = [], []
    for command in commands:
        started = time.monotonic()
        result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
        results.append({'command': command[1:], 'exitCode': result.returncode,
                        'seconds': round(time.monotonic() - started, 2)})
        log.append('$ python3 ' + command[1] + '\n' + result.stdout)
        print(f"{'ok  ' if result.returncode == 0 else 'FAIL'} {command[1]} "
              f"({results[-1]['seconds']}s)", flush=True)
    (output / (group + '-contracts.log')).write_text('\n'.join(log))
    (output / (group + '-contracts.json')).write_text(json.dumps(
        {'suites': results, 'scope': GROUPS[group]['scope']}, indent=2) + '\n')
    failures = [result for result in results if result['exitCode']]
    for failure in failures:
        print('Failed: ' + ' '.join(failure['command']), file=sys.stderr)
    return failures[0]['exitCode'] if failures else 0


def run_observer(output):
    """Run the cold-family observer suite inside an owned, deadline-bounded subtree."""
    sys.path.insert(0, str(ROOT / 'tests/performance'))
    from observe_cold_family import OwnedSubtree, require_pidfd_support
    command = [sys.executable, 'tests/performance/test_observe_cold_family.py']
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
                       PUTGD_OBSERVER_TEST_RECEIPTS=str((output / 'cold-family-unit').resolve()))
    process = owned = None
    code, failure, cleanup = 1, None, None
    log = output / 'cold-family-contracts.log'
    try:
        require_pidfd_support()
        with log.open('x') as stream:
            process = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=stream,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            owned = OwnedSubtree(process)
            deadline = time.monotonic() + 45
            while process.poll() is None:
                owned.discover()
                if time.monotonic() >= deadline:
                    raise TimeoutError('observer_contract_deadline')
                time.sleep(.1)
            code = process.wait()
    except Exception as error:
        failure = type(error).__name__
        code = 124 if isinstance(error, TimeoutError) else 1
    finally:
        try:
            if owned and (process.poll() is None or owned.live(record=False)):
                owned.stop()
        except Exception as error:
            cleanup = type(error).__name__
            code = 1
        (output / 'cold-family-contracts.json').write_text(json.dumps({
            'command': command[1:], 'exitCode': code, 'timeoutSeconds': 45,
            'failureType': failure, 'cleanupErrorType': cleanup,
            'observedOwnedGenerations': len(owned.known) if owned else 0,
            'knownOwnedCleanupComplete': owned is not None and not owned.live(record=False),
            'scope': 'Synthetic Linux process/observer contracts only; no application or physical memory acceptance'
        }, indent=2) + '\n')
    if log.is_file():
        print(log.read_text(), end='')
    return code


def run_evidence(output):
    """Replay the saved source114 live-S3 numeric evidence; not a fresh cloud run."""
    command = [sys.executable, 'docs/evidence/live-s3-memory/source114/replay.py',
               '--manifest', 'docs/evidence/live-s3-memory/source114/manifest.json', '--require-target']
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    (output / 'live-s3-evidence-replay.log').write_text(result.stdout + result.stderr)
    (output / 'live-s3-evidence-replay.json').write_text(json.dumps({
        'command': command[1:], 'exitCode': result.returncode,
        'scope': 'Saved numeric evidence replay only; no fresh desktop or cloud acceptance',
        'result': json.loads(result.stdout) if result.returncode == 0 else None
    }, indent=2) + '\n')
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('group', choices=(*GROUPS, 'observer', 'evidence'))
    parser.add_argument('--output', type=Path, default=ROOT / 'build/contracts')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.group == 'observer':
        raise SystemExit(run_observer(args.output))
    if args.group == 'evidence':
        raise SystemExit(run_evidence(args.output))
    raise SystemExit(run_group(args.group, args.output))


if __name__ == '__main__':
    main()
