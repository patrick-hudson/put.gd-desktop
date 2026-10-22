#!/usr/bin/env python3
"""Run CTest once, retry failed integration tests once, and write a flake ledger.

Usage: ctest_retry.py --ledger PATH --junit PATH -- CTEST_ARGS...

The CTEST_ARGS (a preset, --parallel, --resource-spec-file, ...) are used for
both passes. Only tests labelled `integration` (cmake/TestPolicy.cmake) are
retried, once, after the whole first pass. The result:

- every test passed the first time: exit 0;
- some integration tests failed and each passed its retry: exit 0, and each is
  recorded as `passed_on_retry` in the ledger and printed as a workflow warning;
- a `unit` test failed, a retry failed too, or CTest itself failed without a
  failed test to show for it: exit 1.

A pass on the retry is never silent. tools/ci/flake_ledger.py turns the ledger
into the job summary and the flaky-test tracker issues.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ElementTree

RETRY_LABEL = 'integration'
FAILURE_LINE = re.compile(r'^(FAIL!|XPASS|QFATAL|   Actual|   Expected|   Loc:|putgd wait:|.*Sanitizer).*$', re.M)


def registered(ctest_args, cwd=None):
    """{test name: [labels]} for the tests these arguments select."""
    result = subprocess.run(['ctest', *ctest_args, '--show-only=json-v1'], capture_output=True, text=True, cwd=cwd)
    if result.returncode:
        raise SystemExit('ctest --show-only failed:\n' + result.stderr)
    tests = {}
    for test in json.loads(result.stdout)['tests']:
        labels = []
        for prop in test.get('properties', []):
            if prop['name'] == 'LABELS':
                labels = list(prop['value'])
        tests[test['name']] = labels
    return tests


def junit_results(path):
    """{test name: {status, seconds, details}} from a CTest JUnit report."""
    results = {}
    if not Path(path).is_file():
        return results
    for case in ElementTree.parse(path).getroot().iter('testcase'):
        name = case.get('name')
        status = case.get('status', 'run')
        failure = case.find('failure')
        if failure is not None or status == 'fail':
            outcome = 'failed'
        elif status in ('notrun', 'disabled') or case.find('skipped') is not None:
            outcome = 'skipped'
        else:
            outcome = 'passed'
        output = case.findtext('system-out') or ''
        details = [match.group(0).rstrip() for match in FAILURE_LINE.finditer(output)][:24] if outcome == 'failed' else []
        message = failure.get('message', '') if failure is not None else ''
        results[name] = {'outcome': outcome, 'seconds': float(case.get('time') or 0),
                         'message': message, 'details': details}
    return results


def run_ctest(ctest_args, junit, selection=None):
    command = ['ctest', *ctest_args, '--output-junit', str(junit)]
    if selection:
        command += ['-R', '^(' + '|'.join(re.escape(name) for name in selection) + ')$']
    print('$ ' + ' '.join(command), flush=True)
    started = time.monotonic()
    code = subprocess.run(command).returncode
    return code, round(time.monotonic() - started, 2), command


def environment_identity():
    keys = ('GITHUB_REPOSITORY', 'GITHUB_SERVER_URL', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_SHA',
            'GITHUB_REF_NAME', 'GITHUB_EVENT_NAME', 'GITHUB_JOB', 'PUTGD_TEST_TIME_SCALE', 'PUTGD_CI_JOB',
            'PUTGD_CI_HEAD_SHA')
    return {key: os.environ[key] for key in keys if os.environ.get(key)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--ledger', required=True, type=Path)
    parser.add_argument('--junit', required=True, type=Path, help='first-pass JUnit; the retry writes NAME-retry.xml')
    parser.add_argument('--no-retry', action='store_true', help='record failures without retrying any')
    parser.add_argument('ctest_args', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    ctest_args = args.ctest_args[1:] if args.ctest_args[:1] == ['--'] else args.ctest_args

    labels = registered(ctest_args)
    first_code, first_seconds, first_command = run_ctest(ctest_args, args.junit)
    first = junit_results(args.junit)
    failed = sorted(name for name, result in first.items() if result['outcome'] == 'failed')
    retryable = [] if args.no_retry else [name for name in failed if RETRY_LABEL in labels.get(name, [])]
    blocking = [name for name in failed if name not in retryable]

    passes = [{'kind': 'first', 'command': first_command, 'exitCode': first_code, 'seconds': first_seconds}]
    retry = {}
    if retryable:
        retry_junit = args.junit.with_name(args.junit.stem + '-retry' + args.junit.suffix)
        print(f'Retrying {len(retryable)} failed integration test(s) once: {", ".join(retryable)}', flush=True)
        code, seconds, command = run_ctest(ctest_args, retry_junit, retryable)
        passes.append({'kind': 'retry', 'command': command, 'exitCode': code, 'seconds': seconds,
                       'junit': retry_junit.name})
        retry = junit_results(retry_junit)

    tests = {}
    for name, result in first.items():
        attempts = [dict(result)]
        if name in retry:
            attempts.append(dict(retry[name]))
        if result['outcome'] != 'failed':
            outcome = result['outcome']
        elif name in retry and retry[name]['outcome'] == 'passed':
            outcome = 'passed_on_retry'
        else:
            outcome = 'failed'
        tests[name] = {'labels': labels.get(name, []), 'outcome': outcome, 'attempts': attempts}
    missing = [name for name in retryable if name not in retry]
    for name in missing:
        tests[name]['outcome'] = 'failed'

    flaky = sorted(name for name, test in tests.items() if test['outcome'] == 'passed_on_retry')
    still_failing = sorted(name for name, test in tests.items() if test['outcome'] == 'failed')
    # A CTest failure with no failed test in the report (a crash, a missing
    # executable, a resource-spec error) is never retried or forgiven.
    unexplained = first_code != 0 and not failed
    ok = not still_failing and not unexplained
    ledger = {
        'schemaVersion': 1,
        'source': {'kind': 'ci', **environment_identity()},
        'retryLabel': RETRY_LABEL,
        'passes': passes,
        'summary': {'tests': len(tests), 'failedFirst': failed, 'retried': retryable, 'passedOnRetry': flaky,
                    'failed': still_failing, 'blocking': blocking, 'unexplainedCtestFailure': unexplained,
                    'ok': ok},
        'tests': tests,
    }
    args.ledger.write_text(json.dumps(ledger, indent=1, sort_keys=True) + '\n')

    for name in flaky:
        first_detail = (tests[name]['attempts'][0]['details'] or [tests[name]['attempts'][0]['message']])[0]
        print(f'::warning title=Flaky test {name}::{name} failed, then passed its one retry. '
              f'It is listed in the job summary and the flaky-test tracker. First failure: {first_detail}',
              flush=True)
    if blocking:
        print('Not retried (unit tests never retry): ' + ', '.join(blocking), file=sys.stderr)
    if still_failing:
        print('Failed: ' + ', '.join(still_failing), file=sys.stderr)
    if unexplained:
        print(f'CTest exited {first_code} without a failed test in {args.junit}', file=sys.stderr)
    print(f'ctest_retry: {len(tests)} tests, {len(failed)} failed first, {len(flaky)} passed on retry, '
          f'{len(still_failing)} failed', flush=True)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
