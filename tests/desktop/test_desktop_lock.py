"""Desktop lock contracts on a temporary lock directory; no builder or desktop is contacted."""
import datetime
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
import desktop_lock as lock

START = datetime.datetime(2026, 10, 19, 3, 0, tzinfo=datetime.timezone.utc)


def process(pid, command='putgd-ui --view history'):
    return {'kind': 'process', 'id': str(pid), 'key': f'process:{pid}:100', 'detail': command}


class Clock:
    def __init__(self):
        self.now = START

    def __call__(self):
        return self.now

    def advance(self, **delta):
        self.now += datetime.timedelta(**delta)


class LockTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.directory = Path(folder.name) / 'locks'
        self.clock = Clock()
        self.items = []
        self.desktop = self.make()

    def make(self):
        return lock.Desktop(self.directory, 'f44', self.clock, lambda: (list(self.items), []))

    def acquire(self, session='local_a', minutes=30, desktop=None):
        return (desktop or self.desktop).acquire('holder', session, 'GNOME region capture', minutes)


class ExpiryAndHeartbeat(LockTest):
    def test_expires_at_the_requested_time_and_never_beyond_45_minutes(self):
        record = self.acquire(minutes=45)['lock']
        self.assertEqual(record['expires'], '2026-10-19T03:45:00Z')
        self.clock.advance(minutes=40)
        self.desktop.heartbeat('local_a')
        self.clock.advance(minutes=4, seconds=59)
        self.assertEqual(self.desktop.status()['state'], 'held')
        self.clock.advance(seconds=1)
        state = self.desktop.status()
        self.assertEqual((state['state'], state['breakable']), ('expired', True))
        for minutes in (0, 46, '30'):
            with self.assertRaises(lock.LockError):
                lock.Desktop(self.directory / str(minutes), 'f44', self.clock,
                             lambda: ([], [])).acquire('x', 'y', 'z', minutes)

    def test_heartbeat_older_than_15_minutes_is_stale(self):
        self.acquire(minutes=45)
        self.clock.advance(minutes=15)
        self.assertEqual(self.desktop.status()['state'], 'held')
        self.clock.advance(seconds=1)
        state = self.desktop.status()
        self.assertEqual((state['state'], state['breakable']), ('stale', True))
        self.desktop.heartbeat('local_a')
        self.assertEqual(self.desktop.status()['state'], 'held')

    def test_heartbeat_records_and_clears_a_blocked_reason(self):
        self.acquire()
        self.clock.advance(minutes=5)
        reply = self.desktop.heartbeat('local_a', blocked='waiting for the CI AppImage')
        self.assertEqual((reply['state'], reply['lock']['heartbeat']), ('blocked', '2026-10-19T03:05:00Z'))
        self.assertIn('blocked: waiting for the CI AppImage', (self.directory / 'desktop.lock').read_text())
        self.assertFalse(self.desktop.status()['breakable'])
        self.assertEqual(self.desktop.heartbeat('local_a')['state'], 'held')
        self.assertNotIn('blocked', (self.directory / 'desktop.lock').read_text())

    def test_extend_moves_expiry_from_now_and_refreshes_the_heartbeat(self):
        self.acquire(minutes=10)
        self.clock.advance(minutes=8)
        reply = self.desktop.extend('local_a', 45)
        self.assertEqual(reply['lock']['expires'], '2026-10-19T03:53:00Z')
        self.assertEqual(reply['lock']['heartbeat'], '2026-10-19T03:08:00Z')
        self.assertFalse(reply['wasExpired'])
        with self.assertRaises(lock.LockError):
            self.desktop.extend('local_a', 46)

    def test_only_the_holding_session_can_heartbeat_extend_or_release(self):
        self.acquire()
        for operation in (lambda: self.desktop.heartbeat('local_b'), lambda: self.desktop.extend('local_b', 5),
                          lambda: self.desktop.release('local_b', True)):
            with self.assertRaises(lock.LockError) as refused:
                operation()
            self.assertIn('held by holder (local_a)', str(refused.exception))
        self.assertTrue((self.directory / 'desktop.lock').exists())


class AtomicAcquire(LockTest):
    def test_a_held_lock_refuses_a_second_session_and_names_the_holder(self):
        self.acquire()
        with self.assertRaises(lock.LockError) as refused:
            self.acquire('local_b')
        self.assertEqual(refused.exception.details['lock']['session'], 'local_a')

    def test_concurrent_acquires_have_exactly_one_winner(self):
        results, barrier = [], threading.Barrier(8)

        def contend(number):
            desktop = self.make()
            barrier.wait()
            try:
                self.acquire(f'local_{number}', desktop=desktop)
                results.append(number)
            except lock.LockError:
                pass

        threads = [threading.Thread(target=contend, args=(number,)) for number in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(len(results), 1)
        self.assertIn(f'session: local_{results[0]}', (self.directory / 'desktop.lock').read_text())

    def test_never_replaces_a_lock_created_outside_the_mutex(self):
        self.directory.mkdir()
        (self.directory / 'desktop.lock').write_text('owner: manual\n')
        original = lock.Desktop.state
        lock.Desktop.state = lambda desktop, now: {'state': 'free', 'breakable': False}
        try:
            with self.assertRaises(lock.LockError):
                self.acquire()
        finally:
            lock.Desktop.state = original
        self.assertEqual((self.directory / 'desktop.lock').read_text(), 'owner: manual\n')
        self.assertEqual(sorted(path.name for path in self.directory.iterdir()), ['.mutex', 'desktop.lock'])


class ReleaseCleanupCheck(LockTest):
    def test_release_needs_verified_clean_and_reports_what_is_left(self):
        self.items = [process(11, 'gnome-shell-helper putgd')]
        self.acquire()
        self.items.append(process(22))
        with self.assertRaises(lock.LockError) as refused:
            self.desktop.release('local_a', verified_clean=True)
        check = refused.exception.details['check']
        self.assertEqual([item['id'] for item in check['left']], ['22'])
        self.assertEqual([item['id'] for item in check['preexisting']], ['11'])
        self.assertTrue((self.directory / 'desktop.lock').exists())
        self.items.pop()
        with self.assertRaises(lock.LockError) as refused:
            self.desktop.release('local_a')
        self.assertIn('--verified-clean', str(refused.exception))
        self.assertTrue((self.directory / 'desktop.lock').exists())
        reply = self.desktop.release('local_a', verified_clean=True)
        self.assertFalse((self.directory / 'desktop.lock').exists())
        self.assertFalse((self.directory / 'desktop.baseline.json').exists())
        receipt = json.loads(Path(reply['receipt']).read_text())
        self.assertEqual((receipt['event'], receipt['releasedBy']), ('release', 'local_a'))
        self.assertEqual(self.desktop.status()['last']['event'], 'release')

    def test_a_reused_pid_is_not_mistaken_for_a_preexisting_process(self):
        self.items = [process(11)]
        self.acquire()
        self.items = [dict(process(11), key='process:11:999')]
        with self.assertRaises(lock.LockError) as refused:
            self.desktop.release('local_a', verified_clean=True)
        self.assertEqual([item['id'] for item in refused.exception.details['check']['left']], ['11'])

    def test_not_mine_excuses_only_the_named_items_and_is_recorded(self):
        self.acquire()
        self.items = [process(31, 'putgd_unit_tests'), process(32)]
        with self.assertRaises(lock.LockError):
            self.desktop.release('local_a', True, not_mine=['31'])
        self.items.pop()
        reply = self.desktop.release('local_a', True, not_mine=['31', '99'])
        self.assertEqual([item['id'] for item in reply['check']['notMine']], ['31'])
        self.assertEqual(reply['check']['unmatchedNotMine'], ['99'])
        receipt = json.loads(Path(reply['receipt']).read_text())
        self.assertEqual(receipt['check']['notMine'][0]['detail'], 'putgd_unit_tests')

    def test_not_mine_accepts_exact_process_keys_without_excusing_pid_reuse(self):
        self.acquire()
        self.items = [process(31)]
        old_key = self.items[0]['key']
        self.items[0]['key'] = 'process:31:999'
        with self.assertRaises(lock.LockError):
            self.desktop.release('local_a', True, not_mine=[old_key])
        reply = self.desktop.release('local_a', True, not_mine=['process:31:999'])
        self.assertEqual(reply['check']['notMine'], self.items)
        self.assertEqual(reply['check']['unmatchedNotMine'], [])

    def test_dynamic_ownership_is_checked_after_the_release_inventory(self):
        self.acquire()
        events = []
        def inventory():
            events.append('inventory')
            return [process(31)], []
        def ownership():
            events.append('ownership')
            return ['process:31:100']
        self.desktop.inventory = inventory
        reply = self.desktop.release('local_a', True, not_mine=ownership)
        self.assertEqual(events, ['inventory', 'ownership'])
        self.assertEqual(reply['check']['notMine'], [process(31)])

    def test_a_lost_baseline_counts_everything_as_left(self):
        self.items = [process(11)]
        self.acquire()
        (self.directory / 'desktop.baseline.json').unlink()
        with self.assertRaises(lock.LockError):
            self.desktop.release('local_a', True)


class BreakOnlyWhenStale(LockTest):
    def test_a_live_or_blocked_lock_cannot_be_broken(self):
        with self.assertRaises(lock.LockError):
            self.desktop.break_lock('local_c', 'nobody holds it')
        self.acquire()
        self.desktop.heartbeat('local_a', blocked='waiting for Patrick')
        self.clock.advance(minutes=14)
        with self.assertRaises(lock.LockError) as refused:
            self.desktop.break_lock('local_c', 'want the desktop')
        self.assertIn('live lock', str(refused.exception))
        self.assertTrue((self.directory / 'desktop.lock').exists())

    def test_breaking_a_stale_lock_writes_a_receipt_with_the_leftovers(self):
        self.acquire(minutes=45)
        self.items = [process(41)]
        self.clock.advance(minutes=16)
        reply = self.desktop.break_lock('local_coordinator', 'holder silent for 10 min after notice')
        self.assertEqual(reply['broken']['state'], 'stale')
        self.assertFalse((self.directory / 'desktop.lock').exists())
        receipt = json.loads(Path(reply['receipt']).read_text())
        self.assertEqual((receipt['event'], receipt['brokenBy'], receipt['lock']['session']),
                         ('break', 'local_coordinator', 'local_a'))
        self.assertEqual(receipt['reason'], 'holder silent for 10 min after notice')
        self.assertEqual([item['id'] for item in receipt['check']['left']], ['41'])
        self.assertEqual(self.acquire('local_b')['lock']['session'], 'local_b')

    def test_an_expired_lock_can_be_broken_and_receipts_never_collide(self):
        self.acquire(minutes=1)
        self.clock.advance(minutes=1)
        first = self.desktop.break_lock('local_c', 'expired')
        self.assertEqual(first['broken']['state'], 'expired')
        second = self.desktop.receipt('break', {})
        self.assertNotEqual(first['receipt'], second)
        self.assertEqual(len(list((self.directory / 'history').glob('*-break*.json'))), 2)

    def test_an_unreadable_lock_is_breakable_only_once_it_is_old(self):
        self.directory.mkdir()
        path = self.directory / 'desktop.lock'
        path.write_text('owner: half written\n')
        written = START.timestamp()
        os.utime(path, (written, written))
        self.assertEqual(self.desktop.status()['state'], 'invalid')
        with self.assertRaises(lock.LockError):
            self.desktop.break_lock('local_c', 'unreadable')
        self.clock.advance(minutes=16)
        self.assertEqual(self.desktop.break_lock('local_c', 'unreadable')['broken']['state'], 'invalid')


class HostInventory(unittest.TestCase):
    def fake(self, outputs):
        return lambda argv: outputs.get(argv[0], (None, ''))

    def test_lists_desktop_processes_flatpaks_entries_and_packages_but_not_itself_or_ssh_work(self):
        environments = {'200': {'WAYLAND_DISPLAY', 'HOME'}, '300': {'HOME'}, '400': {'HOME'},
                        '500': None, '600': False, '700': {'HOME', 'QT_QPA_PLATFORM'}}
        with tempfile.TemporaryDirectory() as folder:
            home = Path(folder)
            (home / '.local/share/applications').mkdir(parents=True)
            (home / '.local/share/applications/putgd-record-lab.desktop').write_text('')
            (home / '.local/share/applications/firefox.desktop').write_text('')
            outputs = {
                'pgrep': (0, '100 /usr/bin/python3 -\n200 putgd-ui --view history\n'
                             '300 /bin/sh -c /usr/bin/c++ -o putgd_core.dir/Settings.cpp.o\n'
                             '400 gst-launch-1.0 videotestsrc pattern=ball ! autovideosink\n'
                             '500 /usr/bin/python3 tools/lab/host_input.py serve --user patrick\n'
                             '600 putgd --exited-before-the-check\n'
                             '700 putgd --headless-ssh-test\n'),
                'flatpak': (0, 'Instance   PID  Application\n2563577312 900 gd.put.putgd\n'),
                'rpm': (1, 'package putgd is not installed\n'),
                'dpkg-query': (0, 'ii  0.1.1-dev.369'),
            }
            items, notes = lock.collect_inventory(home, self.fake(outputs), own={100},
                                                    environment=environments.get)
        self.assertEqual([(item['kind'], item['id']) for item in items], [
            ('process', '200'), ('process', '400'), ('process', '500'), ('flatpak', '2563577312'),
            ('file', str(home / '.local/share/applications/putgd-record-lab.desktop')), ('package', 'putgd')])
        self.assertEqual(len(notes), 1)
        self.assertTrue(notes[0].startswith('2 process(es) outside the desktop session'))
        self.assertTrue(notes[0].endswith(': 300 700'))

    def test_reads_environment_names_and_tells_an_exited_process_from_an_unreadable_one(self):
        self.assertIn('PATH', lock.process_environment(os.getpid()))
        self.assertIs(lock.process_environment(2**31), False)

    def test_missing_pgrep_fails_closed_and_missing_flatpak_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(lock.LockError):
                lock.collect_inventory(Path(folder), self.fake({}))
            items, notes = lock.collect_inventory(Path(folder), self.fake({'pgrep': (1, '')}))
        self.assertEqual(items, [])
        self.assertEqual(notes, ['flatpak is not installed'])


class ProtonSessionLock(LockTest):
    def proton(self):
        return lock.Desktop(self.directory, 'f44', self.clock, resource='proton')

    def test_is_independent_of_the_desktop_lock(self):
        self.acquire('local_desktop')
        proton = self.proton()
        record = proton.acquire('lender', 'local_proton', 'lend to run gnome-1', 30)['lock']
        self.assertEqual(record['resource'], 'proton')
        self.assertEqual((self.directory / 'proton.lock').read_text(), (
            'owner: lender\nsession: local_proton\nhost: f44\npurpose: lend to run gnome-1\n'
            'acquired: 2026-10-19T03:00:00Z\nexpires: 2026-10-19T03:30:00Z\nheartbeat: 2026-10-19T03:00:00Z\n'
            'resource: proton\n'))
        self.assertIn('session: local_desktop', (self.directory / 'desktop.lock').read_text())
        with self.assertRaises(lock.LockError) as refused:
            self.proton().acquire('other', 'local_other', 'host app check', 10)
        self.assertIn('f44 Proton session is already locked', str(refused.exception))
        self.assertEqual(self.desktop.status()['lock']['session'], 'local_desktop')
        self.assertEqual(self.proton().status()['lock']['session'], 'local_proton')

    def test_takes_no_inventory_and_release_needs_the_carry_back_confirmed(self):
        def never(home):
            raise AssertionError('a Proton session lock takes no desktop inventory')
        patcher = mock.patch.object(lock, 'collect_inventory', never)
        patcher.start()
        self.addCleanup(patcher.stop)
        proton = self.proton()
        proton.acquire('lender', 'local_proton', 'lend', 30)
        self.assertFalse((self.directory / 'desktop.baseline.json').exists())
        with self.assertRaises(lock.LockError) as refused:
            proton.release('local_proton')
        self.assertIn('no lent copy is outstanding', str(refused.exception))
        reply = proton.release('local_proton', verified_clean=True)
        self.assertTrue(reply['receipt'].endswith('-proton-release.json'))
        body = json.loads(Path(reply['receipt']).read_text())
        self.assertEqual((body['resource'], body['event'], body['releasedBy']), ('proton', 'release', 'local_proton'))
        self.assertFalse((self.directory / 'proton.lock').exists())

    def test_last_event_and_breaking_stay_per_resource(self):
        self.acquire('local_desktop')
        self.desktop.release('local_desktop', True)
        proton = self.proton()
        self.assertIsNone(proton.status()['last'])
        proton.acquire('lender', 'local_proton', 'lend', 10)
        with self.assertRaises(lock.LockError):
            proton.break_lock('local_x', 'not stale yet')
        self.clock.advance(minutes=11)
        broken = proton.break_lock('local_x', 'lender stopped heartbeating')
        self.assertEqual(broken['broken']['state'], 'expired')
        self.assertEqual(proton.status()['last']['event'], 'break')
        self.assertEqual(self.desktop.status()['last']['event'], 'release')
        self.assertEqual(self.desktop.status()['state'], 'free')


class QuietLease(LockTest):
    def quiet(self):
        return lock.Desktop(self.directory, 'aurora-amd', self.clock, resource='quiet')

    def holder(self, session=None):
        return lock.quiet_holder(self.directory, session, self.clock)

    def test_records_the_load_refuses_a_second_session_and_releases_without_a_check(self):
        self.acquire()  # the desktop lock is independent
        with mock.patch.object(lock.os, 'getloadavg', return_value=(20.5, 3.0, 1.0)), \
                mock.patch.object(lock.os, 'cpu_count', return_value=16):
            reply = self.quiet().acquire('timer', 'local_timing', 'fluidity pair #1808', 30)
        self.assertEqual((reply['lock']['resource'], reply['lock']['load']), ('quiet', '20.50 3.00 1.00 (16 CPUs)'))
        self.assertIn('above the 16 CPUs', reply['notes'][0])
        self.assertIn('load: 20.50 3.00 1.00 (16 CPUs)\n', (self.directory / 'quiet.lock').read_text())
        with self.assertRaisesRegex(lock.LockError, 'Do other work and retry acquire later; never sleep holding nothing'):
            self.quiet().acquire('other', 'local_other', 'second timing', 10)
        self.clock.advance(minutes=10)
        self.assertEqual(self.quiet().heartbeat('local_timing')['state'], 'held')
        self.assertEqual(self.quiet().extend('local_timing', 20)['lock']['expires'], '2026-10-19T03:30:00Z')
        receipt = self.quiet().release('local_timing')['receipt']
        self.assertTrue(receipt.endswith('-quiet-release.json'))
        self.assertEqual(self.quiet().status()['state'], 'free')
        self.assertEqual(self.desktop.status()['state'], 'held')

    def test_heavy_work_waits_only_for_another_sessions_live_lease(self):
        self.assertIsNone(self.holder(''))
        self.quiet().acquire('timer', 'local_timing', 'fluidity pair', 30)
        self.assertIn('timer (local_timing) holds this host\'s quiet lease for "fluidity pair"', self.holder(''))
        self.assertIn('never sleep waiting', self.holder('local_other'))
        self.assertIsNone(self.holder('local_timing'))
        with mock.patch.dict(os.environ, {lock.SESSION_VARIABLE: 'local_timing'}):
            self.assertIsNone(self.holder())
        self.clock.advance(minutes=16)  # stale: breakable, but its timing may still be running
        self.assertIn('(stale,', self.holder(''))
        self.quiet().break_lock('local_other', 'timer stopped heartbeating')
        self.assertIsNone(self.holder(''))
        self.quiet().acquire('timer', 'local_timing', 'short', 5)
        self.clock.advance(minutes=5)
        self.assertIsNone(self.holder(''))  # expired

    def test_a_guarded_helper_refuses_to_start(self):
        sys.path.insert(0, str(Path(lock.__file__).parent))
        import rr_loop
        self.assertIs(rr_loop.desktop_lock, lock)
        self.quiet().acquire('timer', 'local_timing', 'fluidity pair', 30)
        argv = ['rr_loop.py', '--build', 'build/rrb', '--test', 'any', '--run-dir', str(self.directory / 'rr')]
        with mock.patch.object(lock, 'LOCK_DIR', str(self.directory)), mock.patch.object(lock, 'now_utc', self.clock), \
                mock.patch.object(sys, 'argv', argv), mock.patch.dict(os.environ, {lock.SESSION_VARIABLE: 'local_b'}), \
                mock.patch.object(rr_loop.run_scope, 'available', side_effect=AssertionError('started')):
            with self.assertRaisesRegex(SystemExit, 'rr_loop not started: timer .local_timing. holds'):
                rr_loop.main()
            os.environ[lock.SESSION_VARIABLE] = 'local_timing'  # the holder's own run passes the guard
            with self.assertRaisesRegex(AssertionError, 'started'):
                rr_loop.main()

    def test_guard_command_checks_the_host_over_the_script_channel(self):
        with tempfile.TemporaryDirectory() as folder:
            def call(**payload):
                return lock.call('aurora-amd', dict(payload, directory=folder), command=[sys.executable, '-'])
            self.assertTrue(call(action='guard', resource='quiet', session='')['ok'])
            self.assertTrue(call(action='acquire', owner='o', session='local_a', purpose='timing', minutes=5,
                                 resource='quiet')['ok'])
            refused = call(action='guard', resource='quiet', session='local_b')
            self.assertFalse(refused['ok'])
            self.assertIn('Do other work and retry later', '\n'.join(lock.describe(refused)))
            self.assertTrue(call(action='guard', resource='quiet', session='local_a')['ok'])
            self.assertEqual(sorted(path.name for path in Path(folder).glob('*.lock')), ['quiet.lock'])

class HeldAndRun(LockTest):
    """`held` and `run`: lab work starts only while the caller's session holds the builder (#1865)."""

    def test_held_is_true_only_for_the_holding_session_while_the_lock_is_current(self):
        with self.assertRaisesRegex(lock.LockError, 'not locked'):
            self.desktop.held('local_a')
        self.acquire(minutes=30)
        self.assertEqual(self.desktop.held('local_a')['state'], 'held')
        with self.assertRaisesRegex(lock.LockError, 'held by holder .local_a., not local_b'):
            self.desktop.held('local_b')
        self.clock.advance(minutes=16)
        with self.assertRaisesRegex(lock.LockError, 'stale under local_a'):
            self.desktop.held('local_a')

    def test_a_helper_refuses_unless_its_session_holds_this_host(self):
        with mock.patch.dict(os.environ, {lock.SESSION_VARIABLE: ''}):
            self.assertIsNone(lock.lock_problem(self.directory / 'none', clock=self.clock))  # a guest: no locks here
            self.acquire(session='local_a')
            with self.assertRaisesRegex(SystemExit, 'record_run not started: PUTGD_LAB_SESSION is not set'):
                with mock.patch.object(lock, 'now_utc', self.clock):
                    lock.refuse_unless_held('record_run', self.directory)
        with mock.patch.object(lock, 'now_utc', self.clock):
            with self.assertRaisesRegex(SystemExit, 'record_run not started: .* not local_b. Run it through'):
                lock.refuse_unless_held('record_run', self.directory, 'local_b')
            lock.refuse_unless_held('record_run', self.directory, 'local_a')

    def test_a_workstation_driver_checks_the_host_its_own_transport_reaches(self):
        def problem(home, session):
            script = lock.held_check_script(session)
            output = lock.subprocess.run(['sh', '-s'], input=script, capture_output=True, text=True,
                                         env=dict(os.environ, HOME=str(home)), check=True).stdout
            return json.loads(output.strip().splitlines()[-1])['problem']
        with tempfile.TemporaryDirectory() as home:
            self.assertIsNone(problem(home, ''))  # no lab lock directory: not a shared builder
            locks = Path(home) / 'putgd-lab' / 'locks'
            lock.Desktop(locks, 'k72', inventory=lambda: ([], [])).acquire('o', 'local_a', 'p', 5)
            self.assertIn('is not set', problem(home, ''))
            self.assertIn("not local_b", problem(home, "local_b"))
            self.assertIsNone(problem(home, 'local_a'))

    def run_main(self, *argv, process=None):
        """Run the CLI with a local lock directory standing in for k72; returns (code, commands started)."""
        started = []

        def run(command, input, **kwargs):
            return lock.subprocess.run([sys.executable, '-'], input=input, **kwargs)

        def popen(command):
            started.append(command)
            return process or FakeProcess([0])

        with tempfile.TemporaryDirectory() as pins:
            self.pins = Path(pins)
            (Path(pins) / 'k72').mkdir()
            (Path(pins) / 'k72' / 'known_hosts').write_text('')
            with mock.patch.object(lock, 'remote_script',
                                   lambda payload: real_script(dict(payload, directory=str(self.directory)))), \
                    mock.patch('sys.stderr'):
                code = lock.main(['run', 'k72', '--owner', 'o', '--session', 'local_a', '--purpose', 'row',
                                  '--minutes', '10', '--pins', pins, *argv], run=run, popen=popen)
        return code, started

    def test_a_refused_acquire_never_runs_the_command(self):
        self.acquire(session='local_other')
        code, started = self.run_main('--', 'dpkg', '-r', 'putgd')
        self.assertEqual((code, started), (1, []))
        self.assertEqual(self.desktop.status()['lock']['session'], 'local_other')

    def test_runs_the_command_with_its_session_and_keeps_the_lock(self):
        code, started = self.run_main('--', 'hostname')
        self.assertEqual(code, 0)
        self.assertEqual(started[0][-3:], ['patrick@192.168.68.72', "export PUTGD_LAB_SESSION=local_a;", 'hostname'])
        _, started = self.run_main('--', 'printf', '[%s]', 'first second; rm x')
        self.assertEqual(started[0][-1], "printf '[%s]' 'first second; rm x'")  # words survive the remote shell
        self.assertIn('BatchMode=yes', started[0])
        self.assertEqual(self.desktop.held('local_a')['lock']['purpose'], 'row')
        code, started = self.run_main('--', 'true')  # the holder's re-run renews its own lock
        self.assertEqual((code, len(started)), (0, 1))

    def test_renews_the_lock_while_a_long_command_runs_and_stops_once_it_is_lost(self):
        old = '2026-01-01T00:00:00Z'

        def age():  # the lock's last renewal, long ago
            path = self.directory / 'desktop.lock'
            path.write_text(path.read_text().replace('heartbeat: ', f'heartbeat: {old}\nx: '))

        process = FakeProcess([lock.subprocess.TimeoutExpired('ssh', 1), 3], during=age)
        code, _ = self.run_main('--', 'sleep', '1200', process=process)
        self.assertEqual(code, 3)  # the command's own exit code
        self.assertEqual(process.waits, [200] * 2)  # a third of the 10-minute lease
        self.assertNotEqual(lock.parse_lock((self.directory / 'desktop.lock').read_text())['heartbeat'], old)
        broken = FakeProcess([lock.subprocess.TimeoutExpired('ssh', 1), 0],
                             during=lambda: self.desktop.release('local_a', verified_clean=True))
        code, _ = self.run_main('--', 'sleep', '1200', process=broken)
        self.assertEqual((code, broken.terminated), (1, True))
        timeout = lock.subprocess.TimeoutExpired('ssh', 1)
        unreachable = FakeProcess([timeout, timeout, 0],
                                  during=lambda: (self.pins / 'k72' / 'known_hosts').unlink(missing_ok=True))
        code, _ = self.run_main('--', 'sleep', '1200', process=unreachable)
        self.assertEqual((code, unreachable.terminated, len(unreachable.waits)), (1, True, 2))

    def test_release_gives_the_lock_back_after_the_command(self):
        code, _ = self.run_main('--release', '--resource', 'quiet', '--', 'true')
        self.assertEqual(code, 0)
        self.assertEqual(list(self.directory.glob('*.lock')), [])

    def test_held_command_checks_the_host(self):
        with tempfile.TemporaryDirectory() as folder:
            def call(**payload):
                return lock.call('d69', dict(payload, directory=folder), command=[sys.executable, '-'])
            self.assertFalse(call(action='held', session='local_a')['ok'])
            self.assertTrue(call(action='acquire', owner='o', session='local_a', purpose='p', minutes=5)['ok'])
            self.assertTrue(call(action='held', session='local_a')['ok'])
            refused = call(action='held', session='local_b')
            self.assertFalse(refused['ok'])
            self.assertIn('not local_b', '\n'.join(lock.describe(refused)))


class FakeProcess:
    """The command's SSH: each wait returns the next exit code or raises the next timeout."""

    def __init__(self, outcomes, during=lambda: None):
        self.outcomes, self.during, self.waits, self.terminated = list(outcomes), during, [], False

    def wait(self, timeout=None):
        if self.terminated:
            return -15
        self.waits.append(timeout)
        outcome = self.outcomes.pop(0)
        if isinstance(outcome, BaseException):
            self.during()
            raise outcome
        return outcome

    def terminate(self):
        self.terminated = True


class RemoteProtocol(unittest.TestCase):
    def test_ssh_uses_the_pinned_host_key_and_never_trusts_on_first_use(self):
        command = lock.ssh_command('k72', '/keys/id', '/pins')
        self.assertEqual(command[-3:], ['patrick@192.168.68.72', 'python3', '-'])
        for option in ('IdentitiesOnly=yes', 'BatchMode=yes', 'StrictHostKeyChecking=yes',
                       'UserKnownHostsFile=/pins/k72/known_hosts'):
            self.assertIn(option, command)
        with tempfile.TemporaryDirectory() as folder:
            def never(*args, **kwargs):
                raise AssertionError('SSH must not run without a pinned host key')
            reply = lock.call('f44', {'action': 'status'}, pins=folder, run=never)
        self.assertTrue(reply['unreachable'])
        self.assertIn('never trust on first use', reply['message'])

    def test_the_proton_resource_travels_with_the_script(self):
        with tempfile.TemporaryDirectory() as folder:
            def call(**payload):
                return lock.call('k72', dict(payload, directory=folder), command=[sys.executable, '-'])
            reply = call(action='acquire', owner='o', session='local_a', purpose='p', minutes=5, resource='proton')
            self.assertTrue(reply['ok'], reply)
            self.assertEqual(reply['resource'], 'proton')
            self.assertEqual(call(action='status')['state'], 'free')
            self.assertEqual(call(action='status', resource='proton')['state'], 'held')
            self.assertEqual(sorted(path.name for path in Path(folder).glob('*.lock')), ['proton.lock'])
            self.assertIn('k72 Proton session (Kubuntu', '\n'.join(lock.describe(reply)))


class GnomeTrayHost(LockTest):
    """A GNOME desktop is locked only while its tray host serves, unless a test skips it on purpose."""

    NOT_ENABLED = {'ready': False, 'extension': 'appindicatorsupport@rgcjonas.gmail.com', 'installed': True,
                   'enabled': False, 'state': 'INACTIVE', 'hostRegistered': False}

    def test_refuses_without_the_tray_host_and_names_the_one_line_fix(self):
        desktop = lock.Desktop(self.directory, 'f44', self.clock, lambda: ([], []),
                               tray_host=lambda: dict(self.NOT_ENABLED))
        with self.assertRaises(lock.LockError) as refused:
            desktop.acquire('holder', 'local_a', 'GNOME tray row', 30)
        self.assertEqual(desktop.status()['state'], 'free')
        reply = lock.explain_tray_host(dict(refused.exception.details, ok=False, message=str(refused.exception),
                                            host='f44', action='acquire'))
        text = '\n'.join(lock.describe(reply))
        self.assertEqual(text.count('\n'), 0, text)
        self.assertIn('f44 has no tray host: appindicatorsupport@rgcjonas.gmail.com is not enabled', text)
        self.assertIn('gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com', text)
        self.assertIn('--no-tray-host', text)

    def test_locks_once_the_tray_host_serves(self):
        ready = dict(self.NOT_ENABLED, ready=True, enabled=True, state='ACTIVE', hostRegistered=True)
        desktop = lock.Desktop(self.directory, 'f44', self.clock, lambda: ([], []),
                               tray_host=lambda: self.clock.advance(seconds=7) or ready)
        reply = dict(desktop.acquire('holder', 'local_a', 'GNOME tray row', 30), ok=True, host='f44', action='acquire')
        self.assertEqual(desktop.status()['state'], 'held')
        self.assertEqual(reply['lock']['acquired'], '2026-10-19T03:00:07Z')  # the probe's time is not the lease's
        self.assertIn('StatusNotifierWatcher host registered', '\n'.join(lock.describe(reply)))

    def test_the_probe_runs_only_once_the_desktop_is_free_under_the_mutex(self):
        calls = []
        self.acquire(session='other')
        desktop = lock.Desktop(self.directory, 'f44', self.clock, lambda: ([], []),
                               tray_host=lambda: calls.append('probe') or {'ready': True})
        with self.assertRaises(lock.LockError):
            desktop.acquire('holder', 'local_a', 'GNOME tray row', 30)
        self.assertEqual(calls, [])  # a sample taken before the holder released would be stale
        self.desktop.release('other', verified_clean=True)
        desktop.acquire('holder', 'local_a', 'GNOME tray row', 30)
        self.assertEqual(calls, ['probe'])

    def test_a_missing_shell_is_not_reported_as_a_missing_extension(self):
        problem, fix = lock.tray_host_module().missing({'shell': False, 'installed': False})
        self.assertIn('GNOME Shell is not running', problem)
        self.assertNotIn('dnf', fix)
        self.assertIn('--no-tray-host', fix)  # a re-login needs the desktop lock first

    def acquire_through_main(self, *extra, host='f44'):
        """Run the CLI against a local lock directory; the probe is a stand-in for f44 before the fix."""
        sent = []
        real = lock.tray_host_module()
        tray = types.SimpleNamespace(probe_source=lambda: STAND_IN_PROBE, missing=real.missing)

        def run(command, input, **kwargs):
            sent.append(input)
            return lock.subprocess.run([sys.executable, '-'], input=input, **kwargs)

        with tempfile.TemporaryDirectory() as pins:
            (Path(pins) / host).mkdir()
            (Path(pins) / host / 'known_hosts').write_text('')
            with mock.patch.object(lock, 'tray_host_module', lambda: tray), \
                    mock.patch.object(lock, 'remote_script',
                                      lambda payload: real_script(dict(payload, directory=str(self.directory)))), \
                    mock.patch('sys.stdout'):
                code = lock.main(['acquire', host, '--owner', 'o', '--session', 'local_a', '--purpose', 'row',
                                  '--minutes', '5', '--json', '--pins', pins, *extra], run=run)
        return code, sent

    def test_the_cli_probes_a_gnome_desktop_and_refuses_when_the_check_fails(self):
        code, sent = self.acquire_through_main()
        self.assertEqual(code, 1)
        self.assertIn('STAND_IN_PROBE', sent[0])
        self.assertEqual(self.desktop.status()['state'], 'free')

    def test_no_tray_host_skips_the_probe_and_records_the_reason_in_the_purpose(self):
        code, sent = self.acquire_through_main('--no-tray-host', 'no-tray prompt fallback')
        self.assertEqual(code, 0)
        self.assertNotIn('STAND_IN_PROBE', sent[0])
        self.assertEqual(self.desktop.status()['lock']['purpose'], 'row [no tray host: no-tray prompt fallback]')

    def test_other_desktops_are_not_probed_and_reject_the_flag(self):
        code, sent = self.acquire_through_main(host='k72')
        self.assertEqual(code, 0)
        self.assertNotIn('STAND_IN_PROBE', sent[0])
        with mock.patch('sys.stderr'), self.assertRaises(SystemExit):
            lock.main(['acquire', 'k72', '--owner', 'o', '--session', 's', '--purpose', 'p', '--minutes', '5',
                       '--no-tray-host', 'why'], run=None)

    def test_the_probe_reports_its_own_failure_as_not_ready(self):
        report = lock.probe_tray_host('raise SystemExit(3)')
        self.assertFalse(report['ready'])
        self.assertIn('no output', report['error'])


# Prints what the real probe printed on f44 before the fix: installed, not enabled.
STAND_IN_PROBE = ("import json;STAND_IN_PROBE=1;print(json.dumps(dict(extension='appindicatorsupport@rgcjonas.gmail.com',"
                  "installed=True,enabled=False,state='INACTIVE',hostRegistered=False)));raise SystemExit(1)")
real_script = lock.remote_script


if __name__ == '__main__':
    unittest.main()
