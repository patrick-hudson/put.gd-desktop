"""Desktop lock contracts on a temporary lock directory; no builder or desktop is contacted."""
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
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

    def test_lock_file_is_readable_key_value_lines(self):
        self.acquire()
        self.assertEqual((self.directory / 'desktop.lock').read_text(), (
            'owner: holder\nsession: local_a\nhost: f44\npurpose: GNOME region capture\n'
            'acquired: 2026-10-19T03:00:00Z\nexpires: 2026-10-19T03:30:00Z\nheartbeat: 2026-10-19T03:00:00Z\n'))
        with self.assertRaises(lock.LockError):
            self.make().acquire('holder', 'local_a', 'two\nlines', 10)


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

    def test_rejects_an_unknown_resource(self):
        with self.assertRaises(lock.LockError):
            lock.Desktop(self.directory, 'f44', self.clock, resource='s3')


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

    def test_the_script_runs_its_operation_when_piped_into_python(self):
        with tempfile.TemporaryDirectory() as folder:
            def call(**payload):
                return lock.call('d69', dict(payload, directory=folder), command=[sys.executable, '-'])
            reply = call(action='acquire', owner='o', session='local_a', purpose='p', minutes=5)
            self.assertTrue(reply['ok'], reply)
            self.assertEqual((reply['host'], reply['lock']['host']), ('d69', 'd69'))
            reply = call(action='acquire', owner='o', session='local_b', purpose='p', minutes=5)
            self.assertFalse(reply['ok'])
            self.assertEqual(reply['lock']['session'], 'local_a')
            self.assertEqual(call(action='heartbeat', session='local_a')['state'], 'held')
            self.assertIn('session: local_a', (Path(folder) / 'desktop.lock').read_text())

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

    def test_status_reports_unreachable_hosts_with_exit_code_3(self):
        def unreachable(command, **kwargs):
            return subprocess.CompletedProcess(command, 255, '', 'ssh: connect to host 192.168.68.72: No route\n')
        with tempfile.TemporaryDirectory() as folder:
            for host in lock.HOSTS:
                (Path(folder) / host).mkdir()
                (Path(folder) / host / 'known_hosts').write_text('')
            with mock.patch('sys.stdout'):
                self.assertEqual(lock.main(['status', 'k72', '--pins', folder], run=unreachable), 3)
            reply = lock.call('k72', {'action': 'status'}, pins=folder, run=unreachable)
        self.assertIn('No route', reply['message'])

    def test_describe_tells_a_refused_acquirer_not_to_wait(self):
        reply = {'ok': False, 'host': 'f44', 'action': 'acquire', 'message': 'f44 is already locked',
                 'state': 'held', 'expiresInSeconds': 600, 'heartbeatAgeSeconds': 60,
                 'lock': {'owner': 'o', 'session': 'local_a', 'acquired': 'x', 'purpose': 'p'}}
        text = '\n'.join(lock.describe(reply))
        self.assertIn('expires in 10 min', text)
        self.assertIn("Don't poll the holder or wait idle", text)


if __name__ == '__main__':
    unittest.main()
