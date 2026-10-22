#!/usr/bin/env python3
"""Report flake ledgers: the job summary, and one tracker issue per flaky test.

A ledger is the JSON that tools/ci/ctest_retry.py (one CI job) or
tools/ci/stress_suite.py (many runs on a builder) writes. Both list every test
with its attempts; a test with a failed attempt and a passed one is flaky.

  flake_ledger.py summary LEDGER... [--title TEXT] [--output PATH]
      Markdown table of the flaky and failed tests (appends to PATH, such as
      $GITHUB_STEP_SUMMARY).
  flake_ledger.py issues LEDGER [--repo OWNER/NAME]
      For each test that passed only on its retry, add an occurrence comment
      to its open `flaky-test` issue, reopening or creating it as needed.
      Needs GITHUB_TOKEN with issues: write; without one it prints a warning.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import urllib.error
import urllib.request

LABEL = 'flaky-test'
MARKER = '<!-- flake-ledger: {} -->'


def load(path):
    ledger = json.loads(Path(path).read_text())
    if ledger.get('schemaVersion') != 1:
        raise ValueError(f'{path}: unsupported ledger schema')
    return ledger


def counts(test):
    attempts = test['attempts']
    failed = sum(1 for attempt in attempts if attempt['outcome'] == 'failed')
    passed = sum(1 for attempt in attempts if attempt['outcome'] == 'passed')
    return passed + failed, failed


def run_link(source):
    if source.get('GITHUB_RUN_ID'):
        server = source.get('GITHUB_SERVER_URL', 'https://github.com')
        attempt = source.get('GITHUB_RUN_ATTEMPT', '1')
        return f"{server}/{source['GITHUB_REPOSITORY']}/actions/runs/{source['GITHUB_RUN_ID']}/attempts/{attempt}"
    return ''


def describe_source(source):
    if source.get('kind') == 'stress':
        return (f"stress on {source.get('host', '?')}: {source.get('iterations', '?')} runs, "
                f"{source.get('contention', 'no contention')}, scale {source.get('PUTGD_TEST_TIME_SCALE', '1')}")
    link = run_link(source)
    job = source.get('PUTGD_CI_JOB') or source.get('GITHUB_JOB', 'ci')
    return f"[{job}]({link})" if link else job


def first_detail(test):
    for attempt in test['attempts']:
        if attempt['outcome'] == 'failed':
            lines = attempt.get('details') or [attempt.get('message', '')]
            return lines[0] if lines else ''
    return ''


def summary(ledgers, title):
    rows, total = [], 0
    for ledger in ledgers:
        source = ledger['source']
        total += len(ledger['tests'])
        for name, test in sorted(ledger['tests'].items()):
            runs, failed = counts(test)
            if not failed:
                continue
            rows.append((name, ','.join(test.get('labels', [])), runs, failed, test['outcome'],
                         describe_source(source), first_detail(test)))
    lines = [f'### {title}', '']
    if not rows:
        lines.append(f'No test failed, flaky or otherwise ({total} test results).')
        return '\n'.join(lines) + '\n'
    lines.append('| Test | Label | Runs | Failed | Outcome | Where | First failure |')
    lines.append('| --- | --- | ---: | ---: | --- | --- | --- |')
    for name, label, runs, failed, outcome, where, detail in rows:
        shown = {'passed_on_retry': '**passed on retry**', 'failed': '**failed**'}.get(outcome, outcome)
        detail = detail.replace('|', '\\|').replace('`', "'")[:160]
        lines.append(f'| `{name}` | {label} | {runs} | {failed} | {shown} | {where} | `{detail}` |')
    flaky = sum(1 for row in rows if row[4] in ('passed_on_retry', 'flaky'))
    tracker = f'`{LABEL}` issue'
    source = ledgers[0]['source'] if ledgers else {}
    if source.get('GITHUB_REPOSITORY'):
        server = source.get('GITHUB_SERVER_URL', 'https://github.com')
        tracker = f"[`{LABEL}`]({server}/{source['GITHUB_REPOSITORY']}/issues?q=label%3A{LABEL}) issue"
    lines += ['', f'{flaky} flaky, {sum(1 for row in rows if row[4] == "failed")} failed, {total} test results.'
              + (f' A test that passed only on its retry is tracked in a {tracker}.'
                 if any(ledger['source'].get('kind') == 'ci' for ledger in ledgers) else '')]
    return '\n'.join(lines) + '\n'


class GitHub:
    def __init__(self, repo, token):
        self.repo, self.token = repo, token
        self.api = os.environ.get('GITHUB_API_URL', 'https://api.github.com')

    def call(self, method, path, body=None):
        request = urllib.request.Request(self.api + path, method=method,
                                         data=json.dumps(body).encode() if body is not None else None)
        request.add_header('Authorization', 'Bearer ' + self.token)
        request.add_header('Accept', 'application/vnd.github+json')
        request.add_header('X-GitHub-Api-Version', '2022-11-28')
        with urllib.request.urlopen(request, timeout=30) as response:
            data = response.read()
        return json.loads(data) if data else None

    def find_issue(self, name):
        marker = MARKER.format(name)
        for page in range(1, 11):
            issues = self.call('GET', f'/repos/{self.repo}/issues?labels={LABEL}&state=all&per_page=100&page={page}')
            for issue in issues:
                if 'pull_request' not in issue and marker in (issue.get('body') or ''):
                    return issue
            if len(issues) < 100:
                return None
        return None

    def ensure_label(self):
        try:
            self.call('POST', f'/repos/{self.repo}/labels',
                      {'name': LABEL, 'color': 'd93f0b',
                       'description': 'A test that failed and then passed its CI retry (tools/ci/ctest_retry.py)'})
        except urllib.error.HTTPError as error:
            if error.code != 422:  # already exists
                raise


def occurrence(name, test, source):
    first = test['attempts'][0]
    retry = test['attempts'][1] if len(test['attempts']) > 1 else {}
    details = '\n'.join(first.get('details') or [first.get('message', '')])
    sha = source.get('GITHUB_SHA', '')[:12]
    return (f"**Passed on retry** in {describe_source(source)}, commit `{sha}` on "
            f"`{source.get('GITHUB_REF_NAME', '?')}` ({source.get('GITHUB_EVENT_NAME', '?')}), "
            f"time scale {source.get('PUTGD_TEST_TIME_SCALE', '1')}.\n\n"
            f"First attempt {first.get('seconds', 0):.1f} s, retry {retry.get('seconds', 0):.1f} s.\n\n"
            f"```\n{details[:3000]}\n```\n")


def issues(ledger, repo, token):
    flaky = {name: test for name, test in ledger['tests'].items() if test['outcome'] == 'passed_on_retry'}
    if not flaky:
        print('flake_ledger: no test passed on retry; no tracker update')
        return 0
    if not token or not repo:
        print('::warning title=Flaky-test tracker not updated::No GITHUB_TOKEN/GITHUB_REPOSITORY; '
              'the flaky tests are listed in the job summary only.')
        return 0
    github = GitHub(repo, token)
    source = ledger['source']
    try:
        github.ensure_label()
        for name, test in sorted(flaky.items()):
            issue = github.find_issue(name)
            comment = occurrence(name, test, source)
            if issue is None:
                body = (f"{MARKER.format(name)}\n`{name}` ({', '.join(test.get('labels', []))}) failed in CI and "
                        "then passed its one retry. `tools/ci/ctest_retry.py` retries a failed integration test "
                        "once and records every pass on a retry here, one comment per occurrence.\n\n"
                        "Done when the cause is fixed and the builder stress lane "
                        "(`tools/ci/stress_suite.py`) shows the test stable under contention.\n")
                issue = github.call('POST', f'/repos/{repo}/issues',
                                    {'title': f'Flaky test: {name}', 'body': body, 'labels': [LABEL]})
                print(f"flake_ledger: opened {issue['html_url']}")
            elif issue['state'] == 'closed':
                github.call('PATCH', f"/repos/{repo}/issues/{issue['number']}", {'state': 'open'})
                comment = 'Reopened: the test passed only on its retry again.\n\n' + comment
                print(f"flake_ledger: reopened {issue['html_url']}")
            github.call('POST', f"/repos/{repo}/issues/{issue['number']}/comments", {'body': comment})
            print(f"flake_ledger: recorded {name} in {issue['html_url']}")
    except (urllib.error.URLError, OSError) as error:
        print(f'::warning title=Flaky-test tracker not updated::{error}; the flaky tests are listed in the job '
              'summary.')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    report = sub.add_parser('summary')
    report.add_argument('ledgers', nargs='+', type=Path)
    report.add_argument('--title', default='Flaky and failed tests')
    report.add_argument('--output', type=Path)
    tracker = sub.add_parser('issues')
    tracker.add_argument('ledger', type=Path)
    tracker.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', ''))
    args = parser.parse_args(argv)
    if args.command == 'summary':
        text = summary([load(path) for path in args.ledgers if path.is_file()], args.title)
        if args.output:
            with args.output.open('a') as stream:
                stream.write(text)
        print(text)
        return 0
    return issues(load(args.ledger), args.repo, os.environ.get('GITHUB_TOKEN', ''))


if __name__ == '__main__':
    sys.exit(main())
