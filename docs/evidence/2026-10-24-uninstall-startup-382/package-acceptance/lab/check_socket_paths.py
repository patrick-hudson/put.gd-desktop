#!/usr/bin/env python3
"""Check Linux test socket paths before launching an isolated workload."""

import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--socket', nargs='+', help='Complete socket paths')
    inputs.add_argument('--ui-output', nargs='+',
                        help='Per-run bench_ui_process.py output directories')
    args = parser.parse_args()
    paths = [Path(value).resolve() for value in (args.socket or args.ui_output)]
    if args.ui_output:
        paths = [path / 'runtime' / 'putgd' / 'session.sock' for path in paths]
    checks = [{'path': str(path), 'utf8Bytes': len(str(path).encode('utf-8'))}
              for path in paths]
    for check in checks:
        check['passed'] = check['utf8Bytes'] <= 107
    passed = all(check['passed'] for check in checks)
    print(json.dumps({'schemaVersion': 1, 'passed': passed,
                      'limitBytesExcludingNul': 107, 'paths': checks}))
    return 0 if passed else 2


if __name__ == '__main__':
    raise SystemExit(main())
