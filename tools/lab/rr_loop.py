#!/usr/bin/env python3
"""Run CTest tests under `rr record --chaos` in a loop and keep the trace of every failure.

rr serializes threads and, with --chaos, randomizes their scheduling, so a race that needs an unlucky interleaving shows up
within a few runs and a failing run replays exactly (`rr replay`, reverse-continue, watchpoints). One run is recorded per
iteration; a passing trace is removed with `rr rm`, a failing trace is kept and printed.

  python3 -I tools/lab/rr_loop.py --build build/rrb --test http_uploader_integration --iterations 50 \\
      --run-dir ~/work/races-1/rr-http -- -testcase lostReplyWaitsForAnExplicitRetryThatMayDuplicate

The test command, environment and working directory come from `ctest --show-only=json-v1`, so the run is the CTest one.
Arguments after `--` are appended to the test's command line (a QtTest function name, for one case). Extra
--env NAME=VALUE entries go into the test's environment. Use a RelWithDebInfo or Debug build and run on a build server:
rr needs `kernel.perf_event_paranoid <= 1` and real performance counters (a VM often has none).
Only processes this script starts are waited for; nothing is killed by pattern.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def test_definition(build, name):
    listing = subprocess.run(['ctest', '--test-dir', str(build), '--show-only=json-v1', '-R', '^%s$' % name],
                             capture_output=True, text=True, check=True)
    tests = json.loads(listing.stdout)['tests']
    if len(tests) != 1:
        raise SystemExit('%s: %d tests match' % (name, len(tests)))
    test = tests[0]
    env, cwd = {}, str(build)
    for prop in test.get('properties', []):
        if prop['name'] == 'ENVIRONMENT':
            for item in prop['value']:
                key, _, value = item.partition('=')
                env[key] = value
        elif prop['name'] == 'WORKING_DIRECTORY':
            cwd = prop['value']
    return test['command'], env, cwd


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--test', required=True, help='the CTest name')
    parser.add_argument('--iterations', type=int, default=20)
    parser.add_argument('--run-dir', type=Path, required=True, help='a new folder for traces and logs')
    parser.add_argument('--timeout', type=float, default=900, help='seconds one recorded run may take before it counts as a hang')
    parser.add_argument('--time-scale', default='3', help='PUTGD_TEST_TIME_SCALE (rr is several times slower than a plain run)')
    parser.add_argument('--env', action='append', default=[])
    parser.add_argument('--no-chaos', action='store_true')
    parser.add_argument('--stop-after', type=int, default=1, help='stop after this many failing runs')
    parser.add_argument('extra', nargs=argparse.REMAINDER)
    options = parser.parse_args()
    extra = options.extra[1:] if options.extra[:1] == ['--'] else options.extra
    options.run_dir.mkdir(parents=True)
    command, env, cwd = test_definition(options.build, options.test)
    environment = dict(os.environ)
    environment.update(env)
    if options.time_scale:
        environment['PUTGD_TEST_TIME_SCALE'] = options.time_scale
    for assignment in options.env:
        key, _, value = assignment.partition('=')
        environment[key] = value
    failures = 0
    for index in range(1, options.iterations + 1):
        trace = options.run_dir / ('trace-%03d' % index)
        log = options.run_dir / ('run-%03d.log' % index)
        argv = ['rr', 'record', '-o', str(trace)] + ([] if options.no_chaos else ['--chaos']) + command + extra
        started = time.monotonic()
        with open(log, 'w') as out:
            process = subprocess.Popen(argv, cwd=cwd, env=environment, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                code = process.wait(options.timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGTERM)  # rr records the tree; its own process group is ours alone
                try:
                    process.wait(30)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                code = 'timeout'
        seconds = time.monotonic() - started
        if code == 0:
            subprocess.run(['rr', 'rm', str(trace)], capture_output=True)
            print('run %d: passed in %.1f s' % (index, seconds), flush=True)
            continue
        failures += 1
        print('run %d: FAILED (%s) in %.1f s: log %s, trace %s (rr replay %s)' % (index, code, seconds, log, trace, trace), flush=True)
        if failures >= options.stop_after:
            break
    print('%d failing run(s) in %d' % (failures, index), flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
