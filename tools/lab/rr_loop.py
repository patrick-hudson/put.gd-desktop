#!/usr/bin/env python3
"""Run CTest tests under `rr record --chaos` in a loop and keep the trace of every failure.

rr serializes threads and, with --chaos, randomizes their scheduling, so a race that needs an unlucky interleaving shows up
within a few runs and a failing run replays exactly (`rr replay`, reverse-continue, watchpoints). One run is recorded per
iteration; a passing trace is removed with `rr rm`, a failing trace is kept and printed.

  python3 -I tools/lab/rr_loop.py --build build/rrb --test http_uploader_integration --iterations 50 \\
      --run-dir /tmp/rr-http -- lostReplyWaitsForAnExplicitRetryThatMayDuplicate

The test command, environment and working directory come from `ctest --show-only=json-v1`, so the run is the CTest one.
Arguments after `--` are appended to the test's command line (a QtTest function name, for one case). Extra
--env NAME=VALUE entries go into the test's environment. --build and --run-dir are made absolute first. Use a RelWithDebInfo or
Debug build and run on a build server: rr needs real performance counters (a VM often has none) and a kernel.perf_event_paranoid
it accepts (rr 5.9+ on Linux 6.10+ works at 2). Keep --run-dir and TMPDIR on tmpfs: rr 5.10 aborts on FS_IOC_GETFSLABEL, which
btrfs and ext4 answer. --timeout is how long one recorded run may take before it is stopped and counted as
a possible slowdown or hang; --time-scale (PUTGD_TEST_TIME_SCALE) stretches the test's own waits for rr's slower schedule.
A run that is stopped gets SIGTERM on rr itself, so rr finishes the trace; a trace is reported replayable only when rr completed it
(no `incomplete` marker and a `version` file). Each run lives in a systemd user scope of its own (tools/lab/run_scope.py); when the run
ends, however it ends, the scope is stopped, which ends every process rr started. Nothing is killed by pattern.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))  # python -I does not put the script's folder on the path
import desktop_lock  # noqa: E402
import run_scope  # noqa: E402


def pick_test(listing, name):
    """The one test in a `ctest --show-only=json-v1` listing whose name is exactly `name` (CTest's -R is a regular expression, so
    `unit.v2` would also select `unit_v2`)."""
    tests = [test for test in json.loads(listing)['tests'] if test.get('name') == name]
    if len(tests) != 1:
        raise SystemExit('%s: %d tests have that exact name' % (name, len(tests)))
    return tests[0]


def test_definition(build, name):
    listing = subprocess.run(['ctest', '--test-dir', str(build), '--show-only=json-v1'], capture_output=True, text=True, check=True)
    test = pick_test(listing.stdout, name)
    env, cwd = {}, str(build)
    for prop in test.get('properties', []):
        if prop['name'] == 'ENVIRONMENT':
            for item in prop['value']:
                key, _, value = item.partition('=')
                env[key] = value
        elif prop['name'] == 'WORKING_DIRECTORY':
            cwd = prop['value']
    return test['command'], env, cwd


class Interrupted(BaseException):
    def __init__(self, signum):
        super().__init__(signum)
        self.signum = signum


def interrupted(signum, frame):
    raise Interrupted(signum)


def stop_recording(process, handle, why):
    """SIGTERM to rr alone: it stops its tracees and finishes the trace. Only if it does not, its process group (ours alone, and pinned
    while rr is unreaped) gets SIGKILL, and that trace stays incomplete. Reaps rr."""
    previous = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        signal.pidfd_send_signal(handle, signal.SIGTERM)
        try:
            process.wait(120)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    return why


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
    parser.add_argument('--rr', default='rr', help='the rr binary (a pinned build such as ~/opt/rr-5.10.0/bin/rr)')
    parser.add_argument('extra', nargs=argparse.REMAINDER)
    options = parser.parse_args()
    desktop_lock.refuse_while_quiet('rr_loop')
    if not run_scope.available():
        raise SystemExit('rr_loop needs a systemd user manager (systemd-run --user --scope): each recorded run lives in a scope that cleanup stops')
    # Ctrl+C or SIGTERM stops the recording this script started (rr runs in a session of its own and would not see the signal).
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    options.build, options.run_dir = options.build.absolute(), options.run_dir.absolute()
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
        argv = [options.rr, 'record', '-o', str(trace)] + ([] if options.no_chaos else ['--chaos']) + command + extra
        started = time.monotonic()
        with open(log, 'w') as out:
            process, handle, stopped_by, scope, cleanup_failed = None, None, None, run_scope.Scope('rr%03d' % index), None
            try:
                process = scope.launch(argv, cwd=cwd, env=environment, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                       start_new_session=True)
                handle = os.pidfd_open(process.pid)  # rr itself, not reaped until wait() below, so this names it for good
                code = process.wait(options.timeout)
            except subprocess.TimeoutExpired:
                code = stop_recording(process, handle, 'stopped after %.0f s: a possible slowdown or hang' % options.timeout)
            except Interrupted as stop:
                stopped_by = stop.signum
                if process is None:
                    return 128 + stop.signum
                if handle is None:
                    handle = os.pidfd_open(process.pid)  # still unreaped, so the PID is still rr's
                code = stop_recording(process, handle, 'interrupted by %s' % signal.Signals(stop.signum).name)
            finally:
                # rr has finished or been stopped: nothing it recorded may outlive the run. Signals are held during the stop, and a stop
                # that fails is reported (never called a stop) and ends the loop.
                held = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
                try:
                    if handle is not None:
                        os.close(handle)
                    try:
                        left = scope.stop()
                    except run_scope.ScopeError as error:
                        cleanup_failed = error
                        left = 0
                    if left:
                        print('run %d: %d processes were left in %s and were stopped with it' % (index, left, scope.unit), flush=True)
                    if process is not None and process.returncode is None and cleanup_failed is None:
                        process.wait()
                finally:
                    signal.pthread_sigmask(signal.SIG_SETMASK, held)
        if cleanup_failed is not None:
            print('run %d: cleanup failed: %s; stop it with systemctl --user stop %s' % (index, cleanup_failed, run_scope.pattern()), flush=True)
            return 2
        if stopped_by is not None:
            replayable = (trace / 'version').is_file() and not os.path.lexists(trace / 'incomplete')
            print('run %d: %s; trace %s (%s)' % (index, code, trace, 'rr replay %s' % trace if replayable else 'INCOMPLETE, not replayable'), flush=True)
            return 128 + stopped_by
        seconds = time.monotonic() - started
        if code == 0:
            removed = subprocess.run([options.rr, 'rm', str(trace)], capture_output=True, text=True)
            note = '' if removed.returncode == 0 else ' (rr rm failed: %s)' % removed.stderr.strip()[-200:]
            print('run %d: passed in %.1f s%s' % (index, seconds, note), flush=True)
            continue
        failures += 1
        replayable = (trace / 'version').is_file() and not os.path.lexists(trace / 'incomplete')
        print('run %d: FAILED (%s) in %.1f s: log %s, trace %s (%s)' % (
            index, code, seconds, log, trace, 'rr replay %s' % trace if replayable else 'INCOMPLETE: rr did not finish it, not replayable'), flush=True)
        if failures >= options.stop_after:
            break
    print('%d failing run(s) in %d' % (failures, index), flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
