"""The flaky-test policy: scaled waits only, one reported retry for integration tests.

Pins tools/ci/check_test_waits.py (the wait lint), tools/ci/ctest_retry.py
(retry and ledger), tools/ci/flake_ledger.py (summary and tracker issues),
tools/ci/stress_suite.py (builder stress lane) and cmake/TestPolicy.cmake.
CTest is replaced by a scripted fake whose tests fail or pass per invocation.
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools/ci' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(ROOT / 'tools/ci'))
    spec.loader.exec_module(module)
    return module


waits = load('check_test_waits')
retry = load('ctest_retry')
ledger_tool = load('flake_ledger')

FAKE_CTEST = textwrap.dedent('''\
    #!/usr/bin/env python3
    # Scripted CTest: FAKE_CTEST_SPEC names each test's labels and its result per run.
    import json, os, re, sys
    spec = json.load(open(os.environ['FAKE_CTEST_SPEC']))
    state_path = os.environ['FAKE_CTEST_SPEC'] + '.runs'
    args = sys.argv[1:]
    if '--show-only=json-v1' in args:
        print(json.dumps({'tests': [{'name': name, 'properties': [{'name': 'LABELS', 'value': test['labels']}]}
                                    for name, test in spec['tests'].items()]}))
        sys.exit(0)
    runs = json.load(open(state_path)) if os.path.exists(state_path) else {}
    selected = list(spec['tests'])
    if '-R' in args:
        pattern = re.compile(args[args.index('-R') + 1])
        selected = [name for name in selected if pattern.search(name)]
    cases, failed = [], 0
    for name in selected:
        index = runs.get(name, 0)
        results = spec['tests'][name]['results']
        result = results[min(index, len(results) - 1)]
        runs[name] = index + 1
        if result == 'fail':
            failed += 1
            cases.append(f'<testcase name="{name}" classname="{name}" time="1.5" status="fail">'
                         f'<failure message="Failed"/><system-out>PASS   : T::a()\\n'
                         f'FAIL!  : T::{name}() synthetic failure\\n   Loc: [t.cpp(7)]\\n</system-out></testcase>')
        else:
            cases.append(f'<testcase name="{name}" classname="{name}" time="0.5" status="run">'
                         f'<system-out>PASS   : T::{name}()</system-out></testcase>')
    json.dump(runs, open(state_path, 'w'))
    junit = args[args.index('--output-junit') + 1]
    open(junit, 'w').write('<?xml version="1.0"?><testsuite>' + ''.join(cases) + '</testsuite>')
    sys.exit(spec.get('exitOverride', 8 if failed else 0))
    ''')


class FakeCTest:
    def __init__(self, tests, **extra):
        self.directory = tempfile.TemporaryDirectory()
        root = Path(self.directory.name)
        (root / 'bin').mkdir()
        fake = root / 'bin/ctest'
        fake.write_text(FAKE_CTEST)
        fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
        self.spec = root / 'spec.json'
        self.spec.write_text(json.dumps({'tests': tests, **extra}))
        self.root = root
        self.env = dict(os.environ, PATH=f"{root / 'bin'}:{os.environ['PATH']}", FAKE_CTEST_SPEC=str(self.spec))

    def run(self, script, *args):
        return subprocess.run([sys.executable, str(ROOT / 'tools/ci' / script), *args], cwd=self.root,
                              env=self.env, capture_output=True, text=True)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.directory.cleanup()


class WaitLint(unittest.TestCase):
    def rules(self, text):
        return set(waits.scan_text(text))

    def test_rejects_every_unscaled_primitive(self):
        self.assertEqual(self.rules('QTest::qWait(50);'), {'qwait'})
        self.assertEqual(self.rules('qWait(1);'), {'qwait'})
        self.assertEqual(self.rules('if(!QTest::qWaitFor([&]{return done;},5000))return;'), {'qwaitfor'})
        self.assertEqual(self.rules('QTRY_COMPARE(a,1);QTRY_VERIFY_WITH_TIMEOUT(b,10);'), {'qtry'})
        self.assertEqual(self.rules('QVERIFY(spy.wait(1000));QVERIFY(changedSpy->wait());'), {'spy-wait'})
        self.assertEqual(self.rules('QVERIFY(QTest::qWaitForWindowExposed(&window));'), {'window-wait'})
        self.assertEqual(self.rules('putgd::test::settle(50);QCOMPARE(count,1);'), {'settle-assert'})
        self.assertEqual(self.rules('settle(qMax(1,2));\n  QVERIFY2(done,"x");'), {'settle-assert'})

    def test_accepts_the_helpers_comments_strings_and_other_settles(self):
        for text in ('PUTGD_TRY_COMPARE(a,1);PUTGD_TRY_VERIFY2(b,"m");',
                     'putgd::test::settle(50);PUTGD_TRY_COMPARE(count,1);',
                     'PUTGD_COMPARE_STEADY(count,1,50);PUTGD_VERIFY_STEADY(done,20);',
                     '(void)putgd::test::waitFor([&]{return done;},15000);',
                     'QVERIFY(putgd::test::waitForSignal(spy));',
                     '// QTest::qWait(10) used to be here', 'const char* text="QTRY_VERIFY(x) qWait(1)";',
                     '/* QTest::qWait(5); */', 'appimage::settle(path,image,"1");QVERIFY(ok);',
                     'QVERIFY(putgd::test::waitForWindowExposed(&window));'):
            with self.subTest(text=text):
                self.assertEqual(self.rules(text), set())

    def test_allowlist_counts_are_exact(self):
        found = {'tests/a.cpp': {'qwait': [3, 9]}, 'tests/b.cpp': {'qtry': [1]}}
        self.assertEqual(len(waits.check(found, {('tests/a.cpp', 'qwait'): 2})), 1)  # b.cpp
        self.assertIn('allowlisted', waits.check(found, {('tests/a.cpp', 'qwait'): 1})[0])
        stale = waits.check({}, {('tests/a.cpp', 'qwait'): 1})
        self.assertIn('lower the count', stale[0])

    def test_repository_tests_use_the_scaled_helpers(self):
        problems = waits.check(waits.scan(), waits.load_allowlist())
        self.assertEqual(problems, [], '\n'.join(problems))

    def test_every_allowlist_entry_says_why(self):
        entries = json.loads((ROOT / 'tools/ci/test-waits-allowlist.json').read_text())['entries']
        for entry in entries:
            self.assertGreater(len(entry['reason']), 40, entry)


class TestPolicy(unittest.TestCase):
    def test_unit_tests_name_registered_tests_and_everything_else_is_integration(self):
        policy = (ROOT / 'cmake/TestPolicy.cmake').read_text()
        listed = re.search(r'set\(PUTGD_UNIT_TESTS(.*?)\)', policy, re.S).group(1).split()
        sources = ''.join(path.read_text() for path in [ROOT / 'CMakeLists.txt', *(ROOT / 'cmake').glob('*.cmake')])
        registered = set(re.findall(r'add_test\(NAME (\w+)', sources))
        registered |= {f'{name}_contract' for name in ('payload', 'archive', 'upload_naming', 'history',
                                                        'operations', 'recording_model')}
        self.assertEqual(sorted(set(listed) - registered), [])
        self.assertIn('LABELS unit', policy)
        self.assertIn('LABELS integration', policy)
        # The tests that flaked in CI waiting on threads, processes and the event loop are integration tests.
        for name in ('proton_contract', 'operations_contract', 'gui_tray', 'gui_history_view', 'gui_application',
                     'native_tray_integration', 'b2_integration', 'recording_service_integration'):
            self.assertNotIn(name, listed)
        self.assertEqual((ROOT / 'CMakeLists.txt').read_text().rstrip().splitlines()[-1],
                         'include(cmake/TestPolicy.cmake)')

    def test_wait_helper_scales_budgets_not_settles(self):
        header = (ROOT / 'tests/fixtures/Waits.h').read_text()
        self.assertIn('PUTGD_TEST_TIME_SCALE', header)
        self.assertIn('std::clamp(value, 1.0, 20.0)', header)
        self.assertIn('inline void settle(int ms) { QTest::qWait(ms); }', header)
        # Polls like QTRY (qWait between checks) and never ends before both the
        # scaled budget and the nominal steps have run out.
        self.assertIn('const int step = budget < 350 ? budget / 7 + 1 : 50;', header)
        self.assertIn('if (waited >= budget && deadline.hasExpired()) return predicate();', header)
        policy = (ROOT / 'cmake/TestPolicy.cmake').read_text()
        self.assertIn('math(EXPR putgd_timeout "${putgd_timeout} * ${PUTGD_TEST_TIMEOUT_SCALE}")', policy)
        for fixture in ('b2_server.py', 's3_server.py'):
            self.assertIn("os.environ.get('PUTGD_TEST_TIME_SCALE', '1')", (ROOT / 'tests/fixtures' / fixture).read_text())


class Retry(unittest.TestCase):
    def ledger(self, fake):
        return json.loads((fake.root / 'ledger.json').read_text())

    def run_retry(self, fake, *extra):
        return fake.run('ctest_retry.py', '--ledger', 'ledger.json', '--junit', str(fake.root / 'results.xml'),
                        *extra, '--', '--preset', 'dev', '--parallel', '8')

    def test_all_passing_runs_once(self):
        with FakeCTest({'a_contract': {'labels': ['unit'], 'results': ['pass']},
                        'b_integration': {'labels': ['integration'], 'results': ['pass']}}) as fake:
            result = self.run_retry(fake)
            self.assertEqual(result.returncode, 0, result.stderr)
            ledger = self.ledger(fake)
            self.assertEqual(len(ledger['passes']), 1)
            self.assertTrue(ledger['summary']['ok'])
            self.assertEqual(ledger['summary']['passedOnRetry'], [])
            self.assertFalse((fake.root / 'results-retry.xml').exists())

    def test_integration_test_passing_its_retry_is_reported_not_hidden(self):
        with FakeCTest({'a_contract': {'labels': ['unit'], 'results': ['pass']},
                        'gui_flaky': {'labels': ['integration'], 'results': ['fail', 'pass']}}) as fake:
            result = self.run_retry(fake)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('::warning title=Flaky test gui_flaky::', result.stdout)
            self.assertIn('-R ^(gui_flaky)$', result.stdout)
            ledger = self.ledger(fake)
            self.assertEqual(ledger['summary']['passedOnRetry'], ['gui_flaky'])
            test = ledger['tests']['gui_flaky']
            self.assertEqual(test['outcome'], 'passed_on_retry')
            self.assertEqual([attempt['outcome'] for attempt in test['attempts']], ['failed', 'passed'])
            self.assertIn('FAIL!  : T::gui_flaky() synthetic failure', test['attempts'][0]['details'])
            self.assertEqual(ledger['tests']['a_contract']['attempts'][0]['outcome'], 'passed')
            summary = ledger_tool.summary([ledger], 'linux')
            self.assertIn('| `gui_flaky` | integration | 2 | 1 | **passed on retry** |', summary)

    def test_integration_test_failing_twice_fails_the_job(self):
        with FakeCTest({'gui_broken': {'labels': ['integration'], 'results': ['fail', 'fail']}}) as fake:
            result = self.run_retry(fake)
            self.assertEqual(result.returncode, 1)
            ledger = self.ledger(fake)
            self.assertEqual(ledger['summary']['failed'], ['gui_broken'])
            self.assertEqual(len(ledger['tests']['gui_broken']['attempts']), 2)

    def test_unit_tests_never_retry(self):
        with FakeCTest({'a_contract': {'labels': ['unit'], 'results': ['fail', 'pass']},
                        'gui_flaky': {'labels': ['integration'], 'results': ['fail', 'pass']}}) as fake:
            result = self.run_retry(fake)
            self.assertEqual(result.returncode, 1)
            self.assertIn('Not retried (unit tests never retry): a_contract', result.stderr)
            ledger = self.ledger(fake)
            self.assertEqual(ledger['summary']['retried'], ['gui_flaky'])
            self.assertEqual(ledger['tests']['a_contract']['outcome'], 'failed')
            self.assertEqual(len(ledger['tests']['a_contract']['attempts']), 1)

    def test_unlabelled_test_is_not_retried(self):
        with FakeCTest({'mystery': {'labels': [], 'results': ['fail', 'pass']}}) as fake:
            self.assertEqual(self.run_retry(fake).returncode, 1)

    def test_ctest_error_without_a_failed_test_is_never_forgiven(self):
        with FakeCTest({'a_contract': {'labels': ['unit'], 'results': ['pass']}}, exitOverride=8) as fake:
            result = self.run_retry(fake)
            self.assertEqual(result.returncode, 1)
            self.assertTrue(self.ledger(fake)['summary']['unexplainedCtestFailure'])

    def test_no_retry_switch(self):
        with FakeCTest({'gui_flaky': {'labels': ['integration'], 'results': ['fail', 'pass']}}) as fake:
            self.assertEqual(self.run_retry(fake, '--no-retry').returncode, 1)


class Tracker(unittest.TestCase):
    class FakeGitHub:
        def __init__(self, issues):
            self.issues, self.calls = issues, []

        def call(self, method, path, body=None):
            self.calls.append((method, path, body))
            if method == 'GET':
                return self.issues
            if path.endswith('/issues'):
                return {'number': 99, 'html_url': 'https://github.invalid/issues/99', 'state': 'open'}
            return {}

    def flaky_ledger(self):
        return {'schemaVersion': 1, 'source': {'kind': 'ci', 'GITHUB_REPOSITORY': 'o/r', 'GITHUB_RUN_ID': '5',
                                               'GITHUB_RUN_ATTEMPT': '2', 'GITHUB_SHA': 'abcdef1234567890',
                                               'PUTGD_CI_JOB': 'linux (24.04)'},
                'tests': {'gui_flaky': {'labels': ['integration'], 'outcome': 'passed_on_retry',
                                        'attempts': [{'outcome': 'failed', 'seconds': 9.0, 'details': ['FAIL! x']},
                                                     {'outcome': 'passed', 'seconds': 3.0, 'details': []}]},
                          'steady': {'labels': ['unit'], 'outcome': 'passed',
                                     'attempts': [{'outcome': 'passed', 'seconds': 1.0, 'details': []}]}}}

    def track(self, existing):
        github = self.FakeGitHub(existing)
        original = ledger_tool.GitHub
        ledger_tool.GitHub = lambda repo, token: (setattr(github, 'repo', repo), github)[1]
        github.find_issue = lambda name: original.find_issue(github, name)
        github.ensure_label = lambda: None
        try:
            ledger_tool.issues(self.flaky_ledger(), 'o/r', 'token')
        finally:
            ledger_tool.GitHub = original
        return github.calls

    def test_new_flaky_test_opens_one_issue_with_the_run_link(self):
        calls = self.track([])
        created = [body for method, path, body in calls if method == 'POST' and path == '/repos/o/r/issues']
        self.assertEqual(len(created), 1)
        self.assertEqual(created[0]['title'], 'Flaky test: gui_flaky')
        self.assertEqual(created[0]['labels'], ['flaky-test'])
        self.assertIn('<!-- flake-ledger: gui_flaky -->', created[0]['body'])
        comment = [body for method, path, body in calls if path.endswith('/comments')][0]['body']
        self.assertIn('https://github.com/o/r/actions/runs/5/attempts/2', comment)
        self.assertIn('`abcdef123456`', comment)
        self.assertNotIn('steady', json.dumps(calls))

    def test_known_flaky_test_gets_a_comment_and_a_closed_one_reopens(self):
        issue = {'number': 7, 'state': 'open', 'html_url': 'u', 'body': '<!-- flake-ledger: gui_flaky -->'}
        calls = self.track([issue])
        self.assertEqual([(m, p) for m, p, _ in calls if m != 'GET'], [('POST', '/repos/o/r/issues/7/comments')])
        calls = self.track([dict(issue, state='closed')])
        self.assertIn(('PATCH', '/repos/o/r/issues/7', {'state': 'open'}), calls)

    def test_missing_token_warns_and_succeeds(self):
        self.assertEqual(ledger_tool.issues(self.flaky_ledger(), 'o/r', ''), 0)


class Stress(unittest.TestCase):
    def test_counts_every_iteration_into_one_ledger_and_stops_its_own_loops(self):
        with FakeCTest({'gui_flaky': {'labels': ['integration'], 'results': ['pass', 'fail', 'pass']},
                        'steady': {'labels': ['unit'], 'results': ['pass']}}) as fake:
            out = fake.root / 'stress'
            result = fake.run('stress_suite.py', '--source', str(fake.root), '--out', str(out), '--iterations', '3',
                              '--hogs', '1', '--time-scale', '3', '--commit', 'deadbeef', '--label', 'self-test',
                              '--', '--preset', 'dev')
            self.assertEqual(result.returncode, 0, result.stderr)
            ledger = json.loads((out / 'ledger.json').read_text())
            self.assertEqual(ledger['source']['iterations'], 3)
            self.assertEqual(ledger['source']['PUTGD_TEST_TIME_SCALE'], '3')
            self.assertEqual(ledger_tool.counts(ledger['tests']['gui_flaky']), (3, 1))
            self.assertEqual(ledger['tests']['gui_flaky']['outcome'], 'flaky')
            self.assertEqual(ledger['tests']['steady']['outcome'], 'passed')
            self.assertIn('| `gui_flaky` | 3 | 1 |', (out / 'summary.md').read_text())
            private = Path((out / 'private-tmp.txt').read_text().strip())
            self.assertFalse(private.exists())
            for pid in (out / 'hog-pids.txt').read_text().split():
                self.assertFalse(Path(f'/proc/{pid}').exists() and
                                 'python' in Path(f'/proc/{pid}/cmdline').read_bytes().decode(errors='replace'))


if __name__ == '__main__':
    unittest.main()
