#!/usr/bin/env python3
"""Measure each CTest test's wall time and CPU time, one test at a time.

  ctest_cpu_profile.py --source TREE --out profile.json -- --preset dev [-L LABEL] [-R REGEX]

Each selected test runs alone (`ctest -R ^NAME$`) on an otherwise idle host;
the CPU time is that of every process the run waited for (the test, its
helpers, dbus-run-session and Xvfb). The ratio CPU/wall is how many cores the
test keeps busy, which cmake/TestPolicy.cmake turns into RESOURCE_GROUPS so
CTest never runs more CPU-bound tests at once than the runner has cores.
Run it on a builder, never on the workstation.
"""
import argparse
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ctest_retry  # noqa: E402


SELECTION = ('-L', '-R', '-LE', '-E', '--label-regex', '--tests-regex', '--label-exclude', '--exclude-regex')


def without_selection(args):
    """CTest arguments without test selection; the profile selects one test at a time."""
    kept, skip = [], False
    for arg in args:
        if skip:
            skip = False
        elif arg in SELECTION:
            skip = True
        else:
            kept.append(arg)
    return kept


def children_cpu():
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--source', type=Path, default=Path.cwd())
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--repeat', type=int, default=1, help='runs per test; the median is reported')
    parser.add_argument('ctest_args', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    ctest_args = args.ctest_args[1:] if args.ctest_args[:1] == ['--'] else args.ctest_args
    source = args.source.resolve()
    tests = ctest_retry.registered(ctest_args, source)
    private = Path(tempfile.mkdtemp(prefix='pgcp.', dir='/tmp'))
    environment = dict(os.environ, TMPDIR=str(private))
    base = without_selection(ctest_args)
    results = {}
    try:
        for name, labels in sorted(tests.items()):
            runs = []
            for _ in range(args.repeat):
                before, clock = children_cpu(), time.monotonic()
                code = subprocess.run(['ctest', *base, '-R', f'^{name}$'], cwd=source, env=environment,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
                wall, cpu = time.monotonic() - clock, children_cpu() - before
                runs.append({'wall': round(wall, 2), 'cpu': round(cpu, 2), 'exitCode': code})
            runs.sort(key=lambda run: run['cpu'])
            median = runs[len(runs) // 2]
            results[name] = {'labels': labels, **median, 'cores': round(median['cpu'] / max(median['wall'], 0.01), 2),
                             'runs': runs}
            print(f"{name:45s} wall {median['wall']:7.2f} s  cpu {median['cpu']:7.2f} s  "
                  f"cores {results[name]['cores']:.2f}  exit {median['exitCode']}", flush=True)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    total_wall = sum(test['wall'] for test in results.values())
    total_cpu = sum(test['cpu'] for test in results.values())
    args.out.write_text(json.dumps({'host': os.uname().nodename, 'cpus': os.cpu_count(), 'ctestArgs': ctest_args,
                                    'totalWall': round(total_wall, 1), 'totalCpu': round(total_cpu, 1),
                                    'tests': results}, indent=1, sort_keys=True) + '\n')
    print(f'{len(results)} tests: {total_wall:.0f} s wall, {total_cpu:.0f} s CPU '
          f'({total_cpu / max(total_wall, 1):.2f} cores on average)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
