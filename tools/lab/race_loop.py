#!/usr/bin/env python3
"""Loop tools/lab/action_fuzz.py over seeds under ThreadSanitizer or `rr record --chaos`, to hunt data races and order bugs.

  tsan  --build build/tsan-instrumented-qt --tsan-prefix work/sanitizer-diagnosis   the instrumented Qt build of docs/testing/thread-sanitizer.md
  rr    --build build/rrb                                              a RelWithDebInfo build, recorded with rr's chaos mode

Each seed runs `action_fuzz.py run` in its own run folder inside a private session bus. A clean seed keeps only its small records
(steps.jsonl, logs/, san/, receiver.log): the payloads and the resident's roots (fixtures/, saves/, archive/, r/) are deleted file by file, and its rr traces are removed
with `rr rm`, only for folders that are complete rr traces (a `version` file, no `incomplete` marker, not the `latest-trace` link). A
cleanup that fails is reported. A finding keeps the run folder whole (finding.json, steps.jsonl, the TSan reports in san/) and, for
rr, the traces under <out>/traces. --stress N starts N busy loops (this script's own processes, stopped by PID) to shake the schedules. The loop stops
at --hours or --max-findings.

  python3 -I tools/lab/race_loop.py tsan --build build/tsan-instrumented-qt --tsan-prefix work/sanitizer-diagnosis --out /tmp/races-tsan-1 \\
      --first-seed 1 --steps 150 --hours 3 --stress 2

On a build server, with --out on disk (it is made absolute first); rr needs real performance counters, a kernel.perf_event_paranoid it
accepts (rr 5.9+ on Linux 6.10+ works at 2; see docs/testing/race-hunting.md) and a filesystem whose ioctls it knows: rr 5.10 aborts on
FS_IOC_GETFSLABEL, which btrfs and ext4 both answer, so put --out and TMPDIR on tmpfs for rr.
"""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

HOG = 'import signal\nsignal.signal(signal.SIGTERM, signal.SIG_DFL)\nwhile True:\n    pass\n'
PAYLOADS = ('fixtures', 'saves', 'archive', 'r')  # what a clean seed made and does not need to keep (r: the resident's roots)


def prune_clean_run(run):
    """Delete a clean seed's payload files one by one (never a whole tree); return (bytes kept, errors)."""
    errors = []
    for name in PAYLOADS:
        top = run / name
        if not top.is_dir() or top.is_symlink():
            continue
        for folder, subfolders, files in os.walk(top, topdown=False):
            files = files + [d for d in subfolders if os.path.islink(os.path.join(folder, d))]  # a link to a folder is removed, never followed
            for file in files:
                try:
                    os.chmod(folder, 0o700)
                    os.unlink(os.path.join(folder, file))
                except OSError as error:
                    errors.append('%s: %s' % (os.path.join(folder, file), error))
            try:
                os.rmdir(folder)
            except OSError as error:
                errors.append('%s: %s' % (folder, error))
    kept = 0
    for folder, _, files in os.walk(run):
        for file in files:
            try:
                kept += os.lstat(os.path.join(folder, file)).st_size
            except OSError:
                pass
    return kept, errors


def remove_traces(root, rr):
    """`rr rm` each complete trace folder under root; return the errors."""
    errors = []
    for trace in sorted(root.glob('*')):
        if trace.is_symlink() or not trace.is_dir():
            continue  # latest-trace is a link to a trace, not one
        if not (trace / 'version').exists() or (trace / 'incomplete').exists():
            errors.append('%s: not a complete rr trace, left in place' % trace)
            continue
        done = subprocess.run([rr, 'rm', str(trace)], capture_output=True, text=True)
        if done.returncode != 0:
            errors.append('%s: rr rm failed (%d) %s' % (trace, done.returncode, done.stderr.strip()[-200:]))
    return errors


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
    parser.add_argument('--rr', default='rr', help='the rr binary (a pinned build such as ~/opt/rr-5.10.0/bin/rr)')
    parser.add_argument('--keep-limit', type=int, default=4, help='MiB a clean seed may keep after pruning; more is reported')
    options = parser.parse_args()
    # Absolute before anything uses them: rr, the fuzzer and this script may resolve a relative path from different folders.
    options.out = options.out.absolute()
    options.build = os.path.abspath(options.build)
    if options.tsan_prefix:
        options.tsan_prefix = os.path.abspath(options.tsan_prefix)
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
                command += ['--wrap', '%s record --chaos' % options.rr, '--env', '_RR_TRACE_DIR=%s' % trace_root]
            command += [options.mode]
            started = time.monotonic()
            with open(options.out / ('s%d.log' % seed), 'w') as log:
                code = subprocess.run(['dbus-run-session', '--'] + command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT).returncode
            note = 'seed %d: %s in %.0f s' % (seed, 'clean' if code == 0 else 'FINDING (exit %d)' % code, time.monotonic() - started)
            if code == 0:
                clean += 1
                problems = remove_traces(traces / ('s%d' % seed), options.rr) if options.kind == 'rr' else []
                kept, errors = prune_clean_run(run)
                problems += errors
                if kept > options.keep_limit << 20:
                    problems.append('%s keeps %.1f MiB after pruning (limit %d MiB)' % (run, kept / 1048576, options.keep_limit))
                for problem in problems:
                    print('cleanup: %s' % problem, flush=True)
                if not problems:
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
