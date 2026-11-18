#!/usr/bin/env python3
"""Loop tools/lab/action_fuzz.py over seeds under ThreadSanitizer or `rr record --chaos`, to hunt data races and order bugs.

  tsan  --build build/tsan-qt --tsan-prefix work/sanitizer-diagnosis   the instrumented Qt build of docs/testing/thread-sanitizer.md
  rr    --build build/rrb                                              a RelWithDebInfo build, recorded with rr's chaos mode

Each seed runs `action_fuzz.py run` in its own run folder inside a private session bus. A clean seed is removed (for rr, its traces are
removed with `rr rm`); a finding keeps the run folder (finding.json, steps.jsonl, the TSan reports in san/) and, for rr, the traces
under <out>/traces. --stress N starts N busy loops (this script's own processes, stopped by PID) to shake the schedules. The loop stops
at --hours or --max-findings.

  python3 -I tools/lab/race_loop.py tsan --build build/tsan-qt --tsan-prefix work/sanitizer-diagnosis --out ~/work/races-1/tsan \\
      --first-seed 1 --steps 150 --hours 3 --stress 2

On a build server, with --out on disk; rr needs `kernel.perf_event_paranoid <= 1`, real performance counters and a filesystem whose ioctls
it knows (rr 5.10 aborts on btrfs's subvolume-info ioctl: put --out on tmpfs or ext4).
"""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

HOG = 'import signal\nsignal.signal(signal.SIGTERM, signal.SIG_DFL)\nwhile True:\n    pass\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument('kind', choices=('tsan', 'rr'))
    parser.add_argument('--build', required=True)
    parser.add_argument('--tsan-prefix')
    parser.add_argument('--out', type=Path, required=True, help='a new folder for the runs')
    parser.add_argument('--first-seed', type=int, default=1)
    parser.add_argument('--steps', type=int, default=150)
    parser.add_argument('--hours', type=float, default=3)
    parser.add_argument('--max-findings', type=int, default=5)
    parser.add_argument('--stress', type=int, default=0)
    parser.add_argument('--destination', choices=('http', 's3'), default='http')
    parser.add_argument('--mode', choices=('run', 'crash'), default='run')
    options = parser.parse_args()
    if options.kind == 'tsan' and not options.tsan_prefix:
        raise SystemExit('tsan needs --tsan-prefix')
    options.out.mkdir(parents=True)
    fuzz = Path(__file__).resolve().parent / 'action_fuzz.py'
    traces = options.out / 'traces'
    traces.mkdir()
    hogs = [subprocess.Popen([sys.executable, '-c', HOG], stdin=subprocess.DEVNULL) for _ in range(options.stress)]
    deadline = time.monotonic() + options.hours * 3600
    seed, findings, clean = options.first_seed, 0, 0
    try:
        while time.monotonic() < deadline and findings < options.max_findings:
            run = options.out / ('s%d' % seed)
            command = [sys.executable, '-I', str(fuzz), '--build', options.build, '--seed', str(seed), '--steps', str(options.steps),
                       '--run-dir', str(run), '--destination', options.destination]
            if options.kind == 'tsan':
                command += ['--tsan-prefix', options.tsan_prefix]
            else:
                trace_root = traces / ('s%d' % seed)
                trace_root.mkdir()
                command += ['--wrap', 'rr record --chaos', '--env', '_RR_TRACE_DIR=%s' % trace_root]
            command += [options.mode]
            started = time.monotonic()
            with open(options.out / ('s%d.log' % seed), 'w') as log:
                code = subprocess.run(['dbus-run-session', '--'] + command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT).returncode
            note = 'seed %d: %s in %.0f s' % (seed, 'clean' if code == 0 else 'FINDING (exit %d)' % code, time.monotonic() - started)
            if code == 0:
                clean += 1
                if options.kind == 'rr':
                    for trace in sorted((traces / ('s%d' % seed)).glob('*-*')):
                        subprocess.run(['rr', 'rm', str(trace)], capture_output=True)
                (options.out / ('s%d.log' % seed)).unlink()
            else:
                findings += 1
                note += ' (kept %s)' % run
            print(note, flush=True)
            seed += 1
    finally:
        for hog in hogs:
            hog.terminate()
        for hog in hogs:
            hog.wait()
    print('%d clean, %d findings, seeds %d-%d' % (clean, findings, options.first_seed, seed - 1), flush=True)
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
