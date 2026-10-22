#!/usr/bin/env python3
"""Run CTest repeatedly under CPU contention and record per-test pass/fail counts.

The builder stress lane. Each iteration runs the selected tests once with the
given CTest arguments while busy loops hold some CPUs; the results go into a
flake ledger (the format tools/ci/ctest_retry.py writes, one attempt per
iteration) and a Markdown summary from tools/ci/flake_ledger.py.

Example, on a four-core builder, the integration label 20 times with six busy
loops and the CI time scale:

  python3 tools/ci/stress_suite.py --source ~/work/put.gd-x --out ~/stress/run1 \\
      --iterations 20 --hogs 6 --time-scale 3 -- --preset dev -L integration \\
      --parallel 8 --resource-spec-file tools/ci/ctest-resources.json

Only processes this script starts are stopped, by PID. It uses a private,
short TMPDIR (socket paths stay under the 107-byte limit) and XDG_STATE_HOME,
and removes only that directory. Run one stress suite per host at a time.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ctest_retry  # noqa: E402
import flake_ledger  # noqa: E402

HOG = 'import signal\nsignal.signal(signal.SIGTERM, signal.SIG_DFL)\nwhile True:\n    pass\n'


def start_hogs(count, cpus):
    hogs = []
    for _ in range(count):
        command = [sys.executable, '-c', HOG]
        if cpus:
            command = ['taskset', '-c', cpus] + command
        hogs.append(subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL))
    return hogs


def stop(processes):
    for process in processes:
        if process.poll() is None:
            process.terminate()
    deadline = time.monotonic() + 5
    for process in processes:
        try:
            process.wait(max(0.1, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def git_identity(source):
    result = subprocess.run(['git', '-C', str(source), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ''


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument('--source', type=Path, default=Path.cwd(), help='source tree to run CTest in')
    parser.add_argument('--out', type=Path, required=True, help='new directory for JUnit, ledger and summary')
    parser.add_argument('--iterations', type=int, default=10)
    parser.add_argument('--hogs', type=int, default=0, help='busy-loop processes for the whole run')
    parser.add_argument('--hog-cpus', default='', help='taskset CPU list for the busy loops')
    parser.add_argument('--test-cpus', default='', help='taskset CPU list for CTest and its tests')
    parser.add_argument('--time-scale', default='', help='PUTGD_TEST_TIME_SCALE for the tests (default: unset)')
    parser.add_argument('--commit', default='', help='source identity when the tree is not a git checkout')
    parser.add_argument('--label', default='', help='a name for this run in the ledger')
    parser.add_argument('--iteration-timeout', type=int, default=1800)
    parser.add_argument('ctest_args', nargs=argparse.REMAINDER, help='-- then the CTest arguments')
    args = parser.parse_args(argv)
    ctest_args = args.ctest_args[1:] if args.ctest_args[:1] == ['--'] else args.ctest_args
    source = args.source.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    private = Path(tempfile.mkdtemp(prefix='pgst.', dir='/tmp'))
    (args.out / 'private-tmp.txt').write_text(str(private) + '\n')
    (args.out / 'pid.txt').write_text(f'{os.getpid()}\n')
    environment = dict(os.environ, TMPDIR=str(private), XDG_STATE_HOME=str(private / 'state'))
    if args.time_scale:
        environment['PUTGD_TEST_TIME_SCALE'] = args.time_scale
    else:
        environment.pop('PUTGD_TEST_TIME_SCALE', None)
    (private / 'state').mkdir()

    labels = ctest_retry.registered(ctest_args, source)
    hogs = []
    interrupted = False

    def on_signal(number, _frame):
        raise KeyboardInterrupt(f'signal {number}')
    signal.signal(signal.SIGTERM, on_signal)

    tests, iterations = {}, []
    started = datetime.datetime.now(datetime.timezone.utc)
    try:
        hogs = start_hogs(args.hogs, args.hog_cpus)
        (args.out / 'hog-pids.txt').write_text(''.join(f'{hog.pid}\n' for hog in hogs))
        for index in range(1, args.iterations + 1):
            junit = args.out / f'iteration-{index:03d}.xml'
            command = ['ctest', *ctest_args, '--output-junit', str(junit)]
            if args.test_cpus:
                command = ['taskset', '-c', args.test_cpus] + command
            clock = time.monotonic()
            with (args.out / f'iteration-{index:03d}.log').open('w') as log:
                try:
                    code = subprocess.run(command, cwd=source, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                          timeout=args.iteration_timeout).returncode
                except subprocess.TimeoutExpired:
                    code = 'timeout'
            seconds = round(time.monotonic() - clock, 2)
            load = Path('/proc/loadavg').read_text().split()[:3]
            results = ctest_retry.junit_results(junit)
            if not results:
                raise KeyboardInterrupt(f'iteration {index}: CTest exited {code} without running a test; '
                                        f'see {args.out}/iteration-{index:03d}.log')
            failed = sorted(name for name, result in results.items() if result['outcome'] == 'failed')
            iterations.append({'iteration': index, 'exitCode': code, 'seconds': seconds, 'loadavg': load,
                               'tests': len(results), 'failed': failed})
            for name, result in results.items():
                entry = tests.setdefault(name, {'labels': labels.get(name, []), 'attempts': []})
                entry['attempts'].append({**result, 'iteration': index})
            print(f'iteration {index}/{args.iterations}: {len(results)} tests, {seconds} s, load {" ".join(load)}, '
                  f'failed: {", ".join(failed) or "none"}', flush=True)
    except KeyboardInterrupt as stopped:
        interrupted = True
        print(f'stress suite interrupted: {stopped}', file=sys.stderr)
    finally:
        stop(hogs)
        shutil.rmtree(private, ignore_errors=True)

    for test in tests.values():
        outcomes = {attempt['outcome'] for attempt in test['attempts']} - {'skipped'}
        test['outcome'] = ('failed' if outcomes == {'failed'} else 'flaky' if 'failed' in outcomes
                           else 'passed' if outcomes else 'skipped')
    contention = f'{args.hogs} busy loops' + (f' on CPUs {args.hog_cpus}' if args.hog_cpus else '')
    if args.test_cpus:
        contention += f', tests on CPUs {args.test_cpus}'
    ledger = {
        'schemaVersion': 1,
        'source': {'kind': 'stress', 'label': args.label, 'host': socket.gethostname(), 'cpus': os.cpu_count(),
                   'commit': args.commit or git_identity(source), 'iterations': len(iterations),
                   'requestedIterations': args.iterations, 'interrupted': interrupted, 'contention': contention,
                   'PUTGD_TEST_TIME_SCALE': args.time_scale or '1', 'ctestArgs': ctest_args,
                   'started': started.isoformat(timespec='seconds'),
                   'ended': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')},
        'iterations': iterations,
        'tests': tests,
    }
    (args.out / 'ledger.json').write_text(json.dumps(ledger, indent=1, sort_keys=True) + '\n')
    title = f"Stress: {args.label or source.name} on {ledger['source']['host']}"
    text = flake_ledger.summary([ledger], title)
    counts = sorted(((name, *flake_ledger.counts(test)) for name, test in tests.items()), key=lambda row: -row[2])
    text += '\n| Test | Runs | Failed |\n| --- | ---: | ---: |\n' + ''.join(
        f'| `{name}` | {runs} | {failed} |\n' for name, runs, failed in counts if failed)
    walls = [entry['seconds'] for entry in iterations]
    if walls:
        text += f'\nIterations: {len(walls)}, wall {min(walls)}-{max(walls)} s (median {sorted(walls)[len(walls) // 2]} s).\n'
    (args.out / 'summary.md').write_text(text)
    print(text)
    return 1 if interrupted else 0


if __name__ == '__main__':
    sys.exit(main())
