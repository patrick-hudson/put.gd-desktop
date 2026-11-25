#!/usr/bin/env python3
"""Report flake ledgers: the job summary, and one tracker issue per flaky test.

A ledger is the JSON that tools/ci/ctest_retry.py (one CI job) or
tools/ci/stress_suite.py (many runs on a builder) writes. Both list every test
with its attempts; a test with a failed attempt and a passed one is flaky.

  flake_ledger.py summary LEDGER... [--title TEXT] [--output PATH]
      Markdown table of the flaky and failed tests (appends to PATH, such as
      $GITHUB_STEP_SUMMARY).
  flake_ledger.py issues LEDGER [--repo OWNER/NAME]
      For each test that passed only on its retry (a CI ledger), or failed at
      least one run (a stress ledger), add an occurrence comment to its
      `flaky-test` issue, reopening or creating it as needed. A closed issue
      is reopened only when the run's commit contains the fix: the commit that
      closed it, or else the default branch as it stood at the close. A run on
      an older base only gets the comment. Needs
      GITHUB_TOKEN (or GH_TOKEN) with issues: write; without one it prints a
      warning.
  flake_ledger.py failures LEDGER --title TITLE --job NAME [--output PATH] [--repo OWNER/NAME]
      For a run that must not fail silently (nightly.yml's sanitizer run of every
      test): when a test failed, CTest failed without one, the ledger is missing,
      or a test passed only on its retry after a sanitizer report, add one comment
      to the open issue with exactly TITLE, or open it. Prints a workflow error and
      exits 1 for any of those; appends what it found to PATH.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

LABEL = 'flaky-test'
MARKER = '<!-- flake-ledger: {} -->'
FAILURE_LABEL = 'area: tests'
# A sanitizer's own report lines, as ctest_retry.py keeps them in a failed attempt's details.
SANITIZER_REPORT = ('Sanitizer', 'runtime error:')


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

    def lacks_fix(self, issue, sha, git=None):
        """True when commit `sha` does not contain the fix for closed `issue`: the commit that
        closed it, or else the default branch's head when it closed, read from the first-parent
        history of the job's full checkout. Any doubt is False."""
        git = git or (lambda *args: subprocess.run(['git', *args], capture_output=True, text=True, timeout=60))
        try:
            fix = None
            for page in range(1, 11):
                events = self.call('GET', f"/repos/{self.repo}/issues/{issue['number']}/timeline?per_page=100&page={page}")
                for event in events:
                    if event.get('event') == 'closed':
                        fix = event.get('commit_id') or None
                if len(events) < 100:
                    break
            if not fix:
                closed = issue.get('closed_at')
                if not closed:
                    return False
                branch = self.call('GET', f'/repos/{self.repo}')['default_branch']
                head = git('rev-list', '--first-parent', '-1', f'--before={closed}', f'refs/remotes/origin/{branch}')
                fix = head.stdout.strip() if head.returncode == 0 else ''
                if not fix:
                    return False
            # 0: sha contains fix; 1: it does not; anything else (an unknown commit) is doubt.
            return git('merge-base', '--is-ancestor', fix, sha).returncode == 1
        except (urllib.error.URLError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError):
            return False  # a missed real flake costs more than a false reopen

    def ensure_label(self):
        try:
            self.call('POST', f'/repos/{self.repo}/labels',
                      {'name': LABEL, 'color': 'd93f0b',
                       'description': 'A test that failed and then passed its CI retry (tools/ci/ctest_retry.py)'})
        except urllib.error.HTTPError as error:
            if error.code != 422:  # already exists
                raise


def run_commit(source):
    """The commit a CI run tested (for a pull request, its merge with the base)."""
    return source.get('GITHUB_SHA', '')


def occurrence(name, test, source):
    first = test['attempts'][0]
    retry = test['attempts'][1] if len(test['attempts']) > 1 else {}
    details = '\n'.join(first.get('details') or [first.get('message', '')])
    sha = (source.get('PUTGD_CI_HEAD_SHA') or source.get('GITHUB_SHA', ''))[:12]
    return (f"**Passed on retry** in {describe_source(source)}, commit `{sha}` on "
            f"`{source.get('GITHUB_REF_NAME', '?')}` ({source.get('GITHUB_EVENT_NAME', '?')}), "
            f"time scale {source.get('PUTGD_TEST_TIME_SCALE', '1')}.\n\n"
            f"First attempt {first.get('seconds', 0):.1f} s, retry {retry.get('seconds', 0):.1f} s.\n\n"
            f"```\n{details[:3000]}\n```\n")


def stress_occurrence(name, test, source):
    runs, failed = counts(test)
    every = ' (every run: likely broken, not only flaky)' if test['outcome'] == 'failed' else ''
    return (f"**Failed {failed} of {runs} runs**{every} in the builder {describe_source(source)}, commit "
            f"`{source.get('commit', '')[:12]}`" + (f", {source['label']}" if source.get('label') else '') + ".\n\n"
            f"```\n{first_detail(test)[:3000]}\n```\n")


def tracked(ledger):
    """The tests that get a tracker comment: passed on retry in CI, or failed any stress run."""
    outcomes = ('flaky', 'failed') if ledger['source'].get('kind') == 'stress' else ('passed_on_retry',)
    return {name: test for name, test in ledger['tests'].items() if test['outcome'] in outcomes}


def issues(ledger, repo, token):
    flaky = tracked(ledger)
    stress = ledger['source'].get('kind') == 'stress'
    if not flaky:
        print('flake_ledger: no test failed a stress run; no tracker update' if stress else
              'flake_ledger: no test passed on retry; no tracker update')
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
            comment = (stress_occurrence if stress else occurrence)(name, test, source)
            if issue is None:
                found = ("failed in the builder stress lane (`tools/ci/stress_suite.py`, CPU contention). "
                         if stress else "failed in CI and then passed its one retry. ")
                body = (f"{MARKER.format(name)}\n`{name}` ({', '.join(test.get('labels', []))}) {found}"
                        "`tools/ci/ctest_retry.py` retries a failed integration test once and records every pass "
                        "on a retry here, and the weekly stress lane records every failure, one comment per "
                        "occurrence.\n\n"
                        "Done when the cause is fixed and the builder stress lane "
                        "(`tools/ci/stress_suite.py`) shows the test stable under contention.\n")
                issue = github.call('POST', f'/repos/{repo}/issues',
                                    {'title': f'Flaky test: {name}', 'body': body, 'labels': [LABEL]})
                print(f"flake_ledger: opened {issue['html_url']}")
            elif issue['state'] == 'closed' and source.get('kind') != 'stress' and run_commit(source) and \
                    github.lacks_fix(issue, run_commit(source)):
                comment = (f"Not reopened: commit `{run_commit(source)[:12]}` does not contain the fix this issue was "
                           "closed with, so this run tested the code from before it.\n\n") + comment
                print(f"flake_ledger: {issue['html_url']} stays closed; the run predates its fix")
            elif issue['state'] == 'closed':
                github.call('PATCH', f"/repos/{repo}/issues/{issue['number']}", {'state': 'open'})
                comment = ('Reopened: the test failed under stress again.\n\n' if stress else
                           'Reopened: the test passed only on its retry again.\n\n') + comment
                print(f"flake_ledger: reopened {issue['html_url']}")
            github.call('POST', f"/repos/{repo}/issues/{issue['number']}/comments", {'body': comment})
            print(f"flake_ledger: recorded {name} in {issue['html_url']}")
    except (urllib.error.URLError, OSError) as error:
        print(f'::warning title=Flaky-test tracker not updated::{error}; the flaky tests are listed in the job '
              'summary.')
    return 0


def sanitizer_report(attempt):
    # ctest_retry.py keeps the report lines in `sanitizer`; older ledgers only in `details`.
    return attempt.get('sanitizer') or [line for line in attempt.get('details') or []
                                         if any(mark in line for mark in SANITIZER_REPORT)]


def run_failures(ledger):
    """[(test or None, why, detail lines)] for everything that makes this run a failure."""
    if ledger is None:
        return [(None, 'no test ledger: the run stopped before its tests finished', [])]
    found = []
    for name, test in sorted(ledger['tests'].items()):
        first = test['attempts'][0] if test['attempts'] else {}
        if test['outcome'] == 'failed':
            details = first.get('details') or [first.get('message', '')]
            # The sanitizer's own lines first: the comment shows only the first 24.
            report = sanitizer_report(first)
            found.append((name, 'failed', report + [line for line in details if line not in report]))
        elif test['outcome'] == 'passed_on_retry' and sanitizer_report(first):
            found.append((name, 'sanitizer report, then passed its retry', sanitizer_report(first)))
    if ledger.get('summary', {}).get('unexplainedCtestFailure'):
        found.append((None, 'CTest failed without a failed test to show for it', []))
    if not ledger['tests']:
        found.append((None, 'no test ran: the preset selected nothing', []))
    return found


def failure_comment(found, ledger, job):
    source = (ledger or {}).get('source') or {key: os.environ[key] for key in
                                              ('GITHUB_REPOSITORY', 'GITHUB_SERVER_URL', 'GITHUB_RUN_ID',
                                               'GITHUB_RUN_ATTEMPT', 'GITHUB_SHA', 'GITHUB_REF_NAME',
                                               'GITHUB_EVENT_NAME') if os.environ.get(key)}
    link = run_link(source)
    sha = (source.get('PUTGD_CI_HEAD_SHA') or source.get('GITHUB_SHA', ''))[:12]
    where = f"[{job}]({link})" if link else job
    lines = [f"**{len(found)} failure(s)** in {where}, commit `{sha}` on `{source.get('GITHUB_REF_NAME', '?')}` "
             f"({source.get('GITHUB_EVENT_NAME', '?')}).", '']
    for name, why, details in found:
        lines.append(f"- `{name}`: {why}" if name else f"- {why}")
        if details:
            lines += ['', '  ```', *('  ' + line for line in details[:24]), '  ```']
    lines += ['', 'Full output: the `ctest-clang-asan-all` artifact of the run (`LastTest.log`).']
    return '\n'.join(lines)[:60000] + '\n'


def failures(ledger, title, job, repo, token, output=None):
    found = run_failures(ledger)
    if output:
        with Path(output).open('a') as stream:
            stream.write(f'### {title}\n\n' + (failure_comment(found, ledger, job) if found else
                                                 'Every test passed under ASan/UBSan.\n') + '\n')
    if not found:
        print('flake_ledger: no failure; no tracking issue update')
        return 0
    print(f'::error title={title}::{len(found)} failure(s) under ASan/UBSan: '
          + ', '.join(name or why for name, why, _ in found), flush=True)
    if not token or not repo:
        print('::warning title=Sanitizer tracking issue not updated::No GITHUB_TOKEN/GITHUB_REPOSITORY; '
              'the failures are listed in the job summary only.')
        return 1
    github = GitHub(repo, token)
    try:
        # By exact title among open issues, whatever their labels.
        query = urllib.parse.quote(f'repo:{repo} is:issue is:open in:title "{title}"')
        found_items = github.call('GET', f'/search/issues?q={query}&per_page=100').get('items', [])
        issue = next((item for item in found_items if item.get('title') == title), None)
        if issue is None:
            body = (f"The nightly workflow's daily run of every test under clang ASan/UBSan (#2065) failed. "
                    "Each failed run adds a comment here with the failed tests and their first report lines.\n\n"
                    "Fix the code, not the sanitizer: suppress only a report from inside a third-party library "
                    "(Qt, GStreamer, the AWS SDK), with the reason beside the suppression. "
                    "Close this issue when the next run passes; a later failure opens a new one.\n")
            issue = github.call('POST', f'/repos/{repo}/issues', {'title': title, 'body': body, 'labels': [FAILURE_LABEL]})
            print(f"flake_ledger: opened {issue['html_url']}")
        github.call('POST', f"/repos/{repo}/issues/{issue['number']}/comments",
                    {'body': failure_comment(found, ledger, job)})
        print(f"flake_ledger: recorded {len(found)} failure(s) in {issue['html_url']}")
    except (urllib.error.URLError, OSError) as error:
        print(f'::warning title=Sanitizer tracking issue not updated::{error}; the failures are listed in the job '
              'summary.')
    return 1


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
    failed = sub.add_parser('failures')
    failed.add_argument('ledger', type=Path)
    failed.add_argument('--title', required=True)
    failed.add_argument('--job', required=True)
    failed.add_argument('--output', type=Path)
    failed.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', ''))
    args = parser.parse_args(argv)
    token = os.environ.get('GITHUB_TOKEN') or os.environ.get('GH_TOKEN', '')
    if args.command == 'failures':
        return failures(load(args.ledger) if args.ledger.is_file() else None, args.title, args.job, args.repo, token,
                        args.output)
    if args.command == 'summary':
        text = summary([load(path) for path in args.ledgers if path.is_file()], args.title)
        if args.output:
            with args.output.open('a') as stream:
                stream.write(text)
        print(text)
        return 0
    return issues(load(args.ledger), args.repo, token)


if __name__ == '__main__':
    sys.exit(main())
