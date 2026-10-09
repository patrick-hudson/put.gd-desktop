#!/usr/bin/env python3
"""Unscored stdlib ownership-metadata stress; never launches putgd or a desktop."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cohorts', type=int, default=20)
    args = parser.parse_args()
    if not 1 <= args.cohorts <= 20:
        parser.error('cohorts must be 1..20')
    output = args.output.absolute()
    if (output.exists() or any(p.is_symlink() for p in [output, *output.parents])
            or not output.parent.is_dir() or output.parent.stat().st_uid != os.getuid()
            or output.parent.stat().st_mode & 0o777 != 0o700):
        parser.error('output must be new in an owned private nonsymlink directory')
    performance = args.repo.resolve() / 'tests/performance'
    sys.path.insert(0, str(performance))
    import observe_cold_family as observer
    rows = []
    for cohort in range(args.cohorts):
        process = subprocess.Popen([sys.executable, '-c',
            'import subprocess; [subprocess.run(["/usr/bin/true"]) for _ in range(400)]'])
        owned = None
        calls = 0
        failure = cleanup_failure = None
        try:
            owned = observer.OwnedSubtree(process)
            deadline = time.monotonic() + 20
            while process.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError('synthetic_cohort_deadline')
                owned.discover()
                owned.live()
                calls += 1
        except BaseException as error:
            failure = {'errorType': type(error).__name__}
            diagnostic = getattr(observer, 'failure_diagnostics', None)
            if diagnostic:
                failure['diagnostics'] = diagnostic(error)
        finally:
            try:
                if owned:
                    owned.stop()
                elif process.poll() is None:
                    process.terminate()  # Still-unreaped direct child, never a PID search.
                process.wait(timeout=5)
            except BaseException as error:
                cleanup_failure = {'errorType': type(error).__name__}
        remaining = None
        try:
            remaining = len(owned.live(record=False)) if owned else None
        except BaseException as error:
            cleanup_failure = cleanup_failure or {'errorType': type(error).__name__}
        row = {'cohort': cohort, 'calls': calls, 'failure': failure,
               'cleanupFailure': cleanup_failure, 'exitCode': process.returncode,
               'knownGenerations': len(owned.known) if owned else 0,
               'ownedLiveAfterCleanup': remaining}
        rows.append(row)
        if failure or cleanup_failure:
            break
    passed = all(r['failure'] is None and r['cleanupFailure'] is None
                 and r['exitCode'] == 0 and r['ownedLiveAfterCleanup'] == 0 for r in rows)
    value = {'scope': 'Synthetic metadata churn, not physical memory or desktop acceptance',
             'cohortsRequested': args.cohorts, 'syntheticChildLaunches':400 * len(rows),
             'observerSha256': hashlib.sha256((performance / 'observe_cold_family.py').read_bytes()).hexdigest(),
             'recipeSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
             'rows': rows, 'passed': passed}
    descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
    print(json.dumps({'passed': passed, 'cohorts': len(rows), 'receipt': str(output)}))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
