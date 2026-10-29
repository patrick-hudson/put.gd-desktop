"""The weekly builder stress lane: guards, token handling, retention and the systemd units.

Pins tools/ci/stress_weekly.py, tools/lab/putgd-stress-weekly.{service,timer} and
tools/lab/install-stress-weekly.sh. Nothing here fetches, builds or runs CTest.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


weekly = load('stress_weekly', ROOT / 'tools/ci/stress_weekly.py')
desktop_lock = load('desktop_lock', ROOT / 'tools/lab/desktop_lock.py')


def write_lock(directory, purpose, minutes=30, heartbeat_age=0):
    now = desktop_lock.now_utc()
    record = {'owner': 'tester', 'session': 'local_test', 'host': 'k72', 'purpose': purpose,
              'acquired': desktop_lock.stamp(now - desktop_lock.datetime.timedelta(minutes=heartbeat_age)),
              'expires': desktop_lock.stamp(now + desktop_lock.datetime.timedelta(minutes=minutes)),
              'heartbeat': desktop_lock.stamp(now - desktop_lock.datetime.timedelta(minutes=heartbeat_age))}
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'desktop.lock').write_text(desktop_lock.render_lock(record))


class Quiet(unittest.TestCase):
    def check(self, directory):
        lines = []
        weekly.check_desktop_lock(Path(directory) / 'no-tree', Path(directory) / 'locks', 'k72', lines.append)
        return lines

    def test_quiet_av_and_performance_locks_skip_the_week(self):
        for purpose in ('quiet slot for recording #50', 'A/V sync check', 'capture-latency perf', 'memory profile',
                        'audio capture', 'Wayland video smoke', 'benchmark theme switch'):
            with self.subTest(purpose=purpose), tempfile.TemporaryDirectory() as directory:
                write_lock(Path(directory) / 'locks', purpose)
                with self.assertRaisesRegex(weekly.Skip, 'quiet, A/V or performance work'):
                    self.check(directory)

    def test_other_locks_free_and_expired_ones_run(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertIn('free', self.check(directory)[0])
            write_lock(Path(directory) / 'locks', 'region picker acceptance #333')
            self.assertIn('not quiet work, running headless', self.check(directory)[0])
            write_lock(Path(directory) / 'locks', 'quiet recording', minutes=-5)
            self.assertIn('expired lock', self.check(directory)[0])

    def test_a_stale_quiet_lock_still_skips(self):
        with tempfile.TemporaryDirectory() as directory:
            write_lock(Path(directory) / 'locks', 'recording slot', minutes=20, heartbeat_age=30)
            with self.assertRaises(weekly.Skip):
                self.check(directory)


class Suites(unittest.TestCase):
    def test_exact_names_only(self):
        self.assertEqual(weekly.suite_kind('ctest', ['ctest', '--preset', 'dev']), 'ctest')
        self.assertEqual(weekly.suite_kind('python3', ['/usr/bin/python3', '-I', 'tools/ci/stress_suite.py']),
                         'stress_suite.py')
        self.assertIsNone(weekly.suite_kind('ctest-helper', ['ctest-helper']))
        self.assertIsNone(weekly.suite_kind('bash', ['bash', '-c', 'ctest --preset dev']))
        self.assertIsNone(weekly.suite_kind('python3', ['python3', 'stress_suite.py.bak']))
        self.assertIsNone(weekly.suite_kind('vim', ['vim', 'tools/ci/stress_suite.py']))

    def test_a_running_ctest_is_found_and_left_alone_by_the_guard(self):
        # A renamed coreutils sleep is rejected by Rust coreutils. Set the
        # Linux process name directly and wait until it is observable.
        process = subprocess.Popen([sys.executable, '-c',
            "from pathlib import Path; import time; Path('/proc/self/comm').write_text('ctest'); "
            "print('ready', flush=True); time.sleep(30)"], stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(), 'ready')
            found = weekly.other_suites()
            self.assertIn({'pid': process.pid, 'uid': os.getuid(), 'kind': 'ctest'}, found)
            with self.assertRaisesRegex(weekly.Skip, f'ctest pid {process.pid}.*would wait'):
                weekly.wait_until_idle(lambda line: None, 60, True)
            self.assertIsNone(process.poll())
        finally:
            process.terminate()
            process.wait()
            process.stdout.close()


class Token(unittest.TestCase):
    def test_missing_loose_or_linked_tokens_are_not_read(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'github-token'
            self.assertEqual(weekly.read_token(path), (None, f'{path} is missing'))
            path.write_text('github_pat_example\n')
            path.chmod(0o644)
            token, why = weekly.read_token(path)
            self.assertIsNone(token)
            self.assertIn('mode 644, not 600', why)
            path.chmod(0o600)
            self.assertEqual(weekly.read_token(path), ('github_pat_example', None))
            link = Path(directory) / 'link'
            link.symlink_to(path)
            self.assertIn('not a regular file', weekly.read_token(link)[1])
            path.write_text('two words\n')
            self.assertIn('single token', weekly.read_token(path)[1])

    def test_missing_token_keeps_results_and_says_so(self):
        lines = []
        with tempfile.TemporaryDirectory() as directory:
            ledger = Path(directory) / 'ledger.json'
            outcome = weekly.update_tracker(Path(directory), ledger, Path(directory) / 'none', 'o/r', lines.append)
        self.assertTrue(outcome.startswith('skipped: token'))
        self.assertIn('not commenting on flaky-test issues; the results stay in', lines[0])

    def test_token_reaches_only_the_ledger_child_and_never_the_log(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tools/ci').mkdir(parents=True)
            # A stand-in flake_ledger.py that reports whether it got the token, and prints it.
            (root / 'tools/ci/flake_ledger.py').write_text(
                'import os\nprint("token " + ("set" if os.environ.get("GH_TOKEN") == "secret-value" else "unset"))\n'
                'print(os.environ.get("GH_TOKEN"))\n')
            token = root / 'token'
            token.write_text('secret-value\n')
            token.chmod(0o600)
            lines = []
            self.assertEqual(weekly.update_tracker(root, root / 'ledger.json', token, 'o/r', lines.append), 'updated')
            self.assertEqual(lines, ['token set', '[token]'])
            self.assertNotIn('GH_TOKEN', os.environ)


class Environment(unittest.TestCase):
    def test_children_never_see_the_desktop_session(self):
        session = {'HOME': '/h', 'PATH': '/bin', 'LC_TIME': 'C', 'XDG_RUNTIME_DIR': '/run/user/1',
                   'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/run/user/1/bus', 'DISPLAY': ':0',
                   'WAYLAND_DISPLAY': 'wayland-0', 'XAUTHORITY': '/run/user/1/xauth', 'XDG_SESSION_TYPE': 'wayland',
                   'XDG_CURRENT_DESKTOP': 'KDE', 'KDE_FULL_SESSION': 'true', 'GH_TOKEN': 'x', 'GITHUB_TOKEN': 'y',
                   'PUTGD_TEST_TIME_SCALE': '9'}
        self.assertEqual(weekly.headless_environment(session),
                         {'HOME': '/h', 'PATH': '/bin', 'LC_TIME': 'C', 'XDG_RUNTIME_DIR': '/run/user/1',
                          'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/run/user/1/bus'})

    def test_run_passes_the_headless_environment(self):
        lines = []
        original = dict(os.environ)
        os.environ.update(DISPLAY=':0', WAYLAND_DISPLAY='wayland-0')
        try:
            weekly.run([sys.executable, '-c', 'import os; print(os.environ.get("DISPLAY"), '
                                              'os.environ.get("WAYLAND_DISPLAY"))'], lines.append)
        finally:
            os.environ.clear()
            os.environ.update(original)
        self.assertEqual(lines[-1], 'None None')

    def test_the_unit_unsets_the_session(self):
        service = (ROOT / 'tools/lab/putgd-stress-weekly.service').read_text()
        unset = next(line for line in service.splitlines() if line.startswith('UnsetEnvironment='))
        for name in ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY', 'XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP'):
            self.assertIn(name, unset.split('=', 1)[1].split())


class Retention(unittest.TestCase):
    def test_keeps_the_newest_weeks_of_its_own_directories_only(self):
        with tempfile.TemporaryDirectory() as directory:
            results = Path(directory)
            days = ['2026-08-30', '2026-09-06', '2026-09-13', '2026-09-20', '2026-09-27']
            made = [weekly.new_run_dir(results, day) for day in days]
            second = weekly.new_run_dir(results, days[-1])
            self.assertEqual(second.name, 'weekly-2026-09-27-2')
            (results / 'weekly-2026-01-01').mkdir()  # not marked: someone else's
            (results / 'notes').mkdir()
            (results / 'weekly-2026-01-02').symlink_to(made[0])
            deleted = weekly.prune(results, 3, lambda line: None)
            self.assertEqual(deleted, made[:2])
            remaining = sorted(path.name for path in results.iterdir())
            self.assertEqual(remaining, ['notes', 'weekly-2026-01-01', 'weekly-2026-01-02', 'weekly-2026-09-13',
                                         'weekly-2026-09-20', 'weekly-2026-09-27', 'weekly-2026-09-27-2'])


class Disk(unittest.TestCase):
    def test_removes_the_previous_build_only_when_short_of_room(self):
        with tempfile.TemporaryDirectory() as directory:
            tree, state = Path(directory) / 'tree', Path(directory) / 'state'
            (tree / 'build/dev').mkdir(parents=True)
            state.mkdir()
            (state / 'build-size.json').write_text(json.dumps({'bytes': 3 * weekly.GIB}))
            lines = []
            weekly.ensure_disk(tree, state, lines.append, False, lambda path: (20 * weekly.GIB, 10 ** 6))
            self.assertTrue((tree / 'build/dev').is_dir())
            free = iter([(11 * weekly.GIB, 10 ** 6), (14 * weekly.GIB, 10 ** 6)])
            weekly.ensure_disk(tree, state, lines.append, False, lambda path: next(free))
            self.assertFalse((tree / 'build/dev').exists())
            with self.assertRaisesRegex(weekly.Skip, 'needs 10.0 GiB reserve plus a 3.0 GiB build'):
                weekly.ensure_disk(tree, state, lines.append, False, lambda path: (12 * weekly.GIB, 10 ** 6))


class Run(unittest.TestCase):
    def main(self, root, *extra):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = weekly.main(['--tree', str(root / 'tree'), '--results', str(root / 'results'),
                                '--state-dir', str(root / 'state'), '--lock-dir', str(root / 'locks'),
                                '--token-file', str(root / 'token'), '--sdk-from', str(root / 'sdk'), *extra])
        return code, output.getvalue()

    def test_dry_run_reports_a_quiet_skip_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_lock(root / 'locks', 'quiet recording slot')
            code, output = self.main(root, '--dry-run')
            self.assertEqual(code, 0)
            self.assertIn('a real run would skip this week', output)
            self.assertIn('quiet recording slot', output)
            self.assertFalse((root / 'results').exists())
            self.assertFalse((root / 'tree').exists())

    def test_a_skipped_week_is_recorded_and_pruned_like_a_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_lock(root / 'locks', 'A/V sync recording')
            code, output = self.main(root)
            self.assertEqual(code, 0)
            [run] = list((root / 'results').iterdir())
            status = json.loads((run / 'weekly.json').read_text())
            self.assertEqual(status['status'], 'skipped')
            self.assertIn('A/V sync recording', status['reason'])
            self.assertIn('skipped this week', (run / 'weekly.log').read_text())
            self.assertTrue((run / weekly.RUN_MARKER).is_file())
            self.assertFalse((root / 'tree').exists())

    def test_a_tree_it_did_not_create_is_left_alone(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tree/.git').mkdir(parents=True)
            original, weekly.busy_reason = weekly.busy_reason, lambda: None
            try:
                code, output = self.main(root, '--dry-run')
            finally:
                weekly.busy_reason = original
            self.assertIn('exists but this script did not create it', output)

    def test_second_concurrent_run_skips(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'state').mkdir()
            with open(root / 'state/weekly.lock', 'a') as held:
                weekly.fcntl.flock(held, weekly.fcntl.LOCK_EX)
                result = subprocess.run([sys.executable, str(ROOT / 'tools/ci/stress_weekly.py'), '--state-dir',
                                         str(root / 'state'), '--results', str(root / 'results'), '--dry-run'],
                                        capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('another weekly run holds', result.stdout)


class Units(unittest.TestCase):
    def test_timer_is_weekly_sunday_three_without_catch_up(self):
        timer = (ROOT / 'tools/lab/putgd-stress-weekly.timer').read_text()
        self.assertIn('OnCalendar=Sun *-*-* 03:00:00\n', timer)
        self.assertIn('Persistent=false\n', timer)
        service = (ROOT / 'tools/lab/putgd-stress-weekly.service').read_text()
        self.assertIn('ExecStart=/usr/bin/python3 -I %h/.local/libexec/putgd-stress/stress_weekly.py\n', service)
        self.assertIn('Type=oneshot\n', service)
        self.assertNotIn('Environment=GH_TOKEN', service)

    def test_installer_never_touches_the_token(self):
        script = (ROOT / 'tools/lab/install-stress-weekly.sh').read_text()
        for line in script.splitlines():
            if 'github-token' in line or '$token' in line:
                self.assertNotRegex(line, r'\b(cat|cp|echo "\$\(cat|read|chmod|install|mv|rm)\b.*\$token')
        self.assertIn('install -d -m 700', script)


if __name__ == '__main__':
    unittest.main()
