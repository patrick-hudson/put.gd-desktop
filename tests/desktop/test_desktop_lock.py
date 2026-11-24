"""Desktop lock contracts on a temporary lock directory; no builder or desktop is contacted."""
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
import desktop_lock as lock

START = datetime.datetime(2026, 10, 19, 3, 0, tzinfo=datetime.timezone.utc)


def inventory(*args, **kwargs):
    """collect_inventory without this machine's own /tmp."""
    return lock.collect_inventory(*args, **dict({'tmp': lambda: None}, **kwargs))


def deb(version):
    return {'kind': 'package', 'id': 'putgd', 'key': 'package:deb:' + version, 'detail': 'deb putgd ' + version}


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

    def test_release_refuses_while_a_cover_player_runs_and_passes_once_it_is_stopped(self):
        pgrep = [(1, '')]
        run = lambda argv: pgrep[0] if argv[0] == 'pgrep' else (1, '')
        home = self.directory.parent
        desktop = lock.Desktop(self.directory, 'k72', self.clock,
                               lambda: inventory(home, run, own=set(), environment=lambda pid: {'HOME'}))
        self.acquire(desktop=desktop)
        pgrep[0] = (0, '900 /usr/bin/python3 /tmp/pgd.BANaL2/cover_player.py play --title Synthetic test\n')
        with self.assertRaises(lock.LockError) as refused:
            desktop.release('local_a', verified_clean=True)
        self.assertEqual([item['id'] for item in refused.exception.details['check']['left']], ['900'])
        pgrep[0] = (1, '')
        desktop.release('local_a', verified_clean=True)
        self.assertFalse((self.directory / 'desktop.lock').exists())

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

    def test_a_package_restored_to_its_acquire_version_is_not_new(self):
        self.items = [deb('1.0')]
        self.acquire()
        self.items = [deb('2.0')]
        with self.assertRaises(lock.LockError) as refused:
            self.desktop.release('local_a', True)
        self.assertEqual(refused.exception.details['check']['left'], [deb('2.0')])
        self.assertEqual(refused.exception.details['check']['removed'], [])
        self.items = [deb('1.0')]
        reply = self.desktop.release('local_a', True)
        self.assertEqual((reply['check']['preexisting'], reply['check']['removed']), ([deb('1.0')], []))

    def test_a_package_installed_during_the_lock_is_left(self):
        self.acquire()
        self.items = [deb('1.0')]
        with self.assertRaises(lock.LockError) as refused:
            self.desktop.release('local_a', True)
        self.assertEqual(refused.exception.details['check']['left'], [deb('1.0')])

    def test_a_removed_package_refuses_release_until_a_reason_is_given(self):
        self.items = [deb('1.0')]
        self.acquire()
        self.items = []
        with self.assertRaises(lock.LockError) as refused:
            self.desktop.release('local_a', True)
        self.assertIn('--removed-ok', str(refused.exception))
        self.assertEqual(refused.exception.details['check']['removed'], [deb('1.0')])
        self.assertTrue((self.directory / 'desktop.lock').exists())
        with self.assertRaises(lock.LockError):
            self.desktop.release('local_a', True, removed_ok='  ')
        reply = self.desktop.release('local_a', True, removed_ok='testing the no-package path')
        receipt = json.loads(Path(reply['receipt']).read_text())
        self.assertEqual(receipt['removedOk'], 'testing the no-package path')
        self.assertEqual(receipt['check']['removed'], [deb('1.0')])

    def test_an_old_baseline_without_a_package_never_reports_a_removal(self):
        self.acquire()
        reply = self.desktop.release('local_a', True)
        self.assertNotIn('removedOk', json.loads(Path(reply['receipt']).read_text()))

    def test_the_baseline_is_taken_only_once_the_desktop_is_free(self):
        self.items = [deb('1.0')]
        self.acquire()
        taken = []
        second = lock.Desktop(self.directory, 'f44', self.clock, lambda: (taken.append(1), ([], []))[1])
        with self.assertRaises(lock.LockError):
            self.acquire('local_b', desktop=second)
        self.assertEqual(taken, [])
        self.desktop.release('local_a', True)
        self.acquire('local_b', desktop=second)
        self.assertEqual(taken, [1])

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
            items, notes = inventory(home, self.fake(outputs), own={100},
                                                    environment=environments.get, parent={}.get)
        self.assertEqual([(item['kind'], item['id']) for item in items], [
            ('process', '200'), ('process', '400'), ('process', '500'), ('flatpak', '2563577312'),
            ('file', str(home / '.local/share/applications/putgd-record-lab.desktop')), ('package', 'putgd')])
        self.assertEqual(len(notes), 1)
        self.assertTrue(notes[0].startswith('2 process(es) outside the desktop session'))
        self.assertTrue(notes[0].endswith(': 300 700'))

    def test_a_confined_helper_is_judged_by_the_process_that_started_it(self):
        # #1938: putgd-ocr is non-dumpable, so its environment is unreadable; another session's
        # SSH benchmark helper failed a release check that the editor's helper should still fail.
        outputs = {'pgrep': (0, '210 /usr/bin/putgd-ocr\n310 /home/patrick/work/r1930/bin/putgd-ocr.n\n'
                                '410 /usr/bin/putgd-ocr\n510 /usr/bin/putgd-ocr\n610 /usr/bin/putgd-ocr\n'),
                   'flatpak': (0, ''), 'rpm': (1, ''), 'dpkg-query': (1, '')}
        environments = {'210': None, '200': {'WAYLAND_DISPLAY', 'HOME'},  # the editor's helper
                        '310': None, '300': None, '301': {'HOME', 'SSH_CONNECTION'},  # SSH harness, two levels up
                        '410': None,  # orphaned to init: can't be told apart
                        '510': None, '500': False,  # its parent exited meanwhile
                        '610': None, '600': {'HOME'}}  # adopted by a subreaper: no sign of SSH
        parents = {'210': '200', '310': '300', '300': '301', '410': '1', '510': '500', '610': '600'}
        with tempfile.TemporaryDirectory() as folder:
            items, notes = inventory(Path(folder), self.fake(outputs), own=set(),
                                                    environment=environments.get, parent=parents.get)
        self.assertEqual([item['id'] for item in items], ['210', '410', '510', '610'])
        self.assertTrue(notes[0].endswith('shared SSH work): 310'))

    def test_reports_cover_players_and_orphaned_lab_test_x_servers_but_not_a_running_test_or_users_xvfb(self):
        # #1925: 15 magenta covers and 11 orphaned stress-run Xvfb servers outlived released locks.
        cover = '/usr/bin/python3 /tmp/pgda.PhRUBf/cover_player.py play --title Synthetic storage acceptance test'
        orphan = 'Xvfb :230 -screen 0 1280x800x24 -noreset -nolisten tcp -auth /tmp/pgst._nyllchp/xvfb-run.2vhfjZ/Xauthority'
        running = '/usr/bin/Xvfb :231 -screen 0 1280x800x24 -nolisten tcp -auth /tmp/pgst.7a1k4u9f/xvfb-run.HrftW2/Xauthority'
        self.assertRegex(orphan, lock.PROCESS_PATTERN)
        self.assertRegex(running, lock.PROCESS_PATTERN)
        for user in ('Xvfb :99 -screen 0 1280x800x24 -nolisten tcp -auth /tmp/xvfb-run.Q1w2e3/Xauthority',
                     'Xvfb :98 -auth /tmp/pgadmin-dev/personal.auth'):
            self.assertIsNone(lock.LAB_XVFB.match(user))
            self.assertNotRegex(user, lock.PROCESS_PATTERN)
        tests = 'python3 -m unittest tests/desktop/test_cover_player.py'
        outputs = {'pgrep': (0, f'200 {cover}\n300 {orphan}\n400 {running}\n500 {tests}\n'),
                   'flatpak': (0, ''), 'rpm': (1, ''), 'dpkg-query': (1, '')}
        environments = {'200': {'HOME'}, '300': {'HOME'}, '400': {'HOME'}, '500': {'HOME'}}
        with tempfile.TemporaryDirectory() as folder:
            items, notes = inventory(Path(folder), self.fake(outputs), own=set(),
                                                    environment=environments.get, orphaned={'300'}.__contains__)
        self.assertEqual([(item['kind'], item['id']) for item in items], [('process', '200'), ('process', '300')])
        self.assertTrue(notes[0].endswith('shared SSH work): 400 500'))

    def test_a_process_is_orphaned_once_its_parent_exits(self):
        self.assertFalse(lock.process_orphaned(os.getpid()))
        self.assertFalse(lock.process_orphaned(2**31))
        child = subprocess.Popen([sys.executable, '-c', 'import os,subprocess,sys\n'
                                  'p=subprocess.Popen([sys.executable,"-c","import time;time.sleep(30)"])\n'
                                  'print(p.pid,flush=True)'], stdout=subprocess.PIPE, text=True)
        grandchild = int(child.stdout.readline())
        child.stdout.close()
        child.wait(10)
        try:
            self.assertTrue(lock.process_orphaned(grandchild))
        finally:
            os.kill(grandchild, 15)

    def test_reads_environment_names_and_tells_an_exited_process_from_an_unreadable_one(self):
        self.assertIn('PATH', lock.process_environment(os.getpid()))
        self.assertIs(lock.process_environment(2**31), False)

    def test_missing_pgrep_fails_closed_and_missing_flatpak_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(lock.LockError):
                inventory(Path(folder), self.fake({}))
            items, notes = inventory(Path(folder), self.fake({'pgrep': (1, '')}))
        self.assertEqual(items, [])
        self.assertEqual(notes, ['flatpak is not installed', 'no putgd package installed'])

    def test_says_when_no_putgd_package_is_installed_even_with_config_files_left(self):
        with tempfile.TemporaryDirectory() as folder:
            outputs = {'pgrep': (1, ''), 'flatpak': (0, ''), 'rpm': (1, ''),
                       'dpkg-query': (0, 'rc  26.1122.6374~dev+gffeead0fc1f3')}
            items, notes = inventory(Path(folder), self.fake(outputs))
            self.assertEqual((items, notes), ([], ['no putgd package installed '
                                                   '(config files of deb 26.1122.6374~dev+gffeead0fc1f3 only)']))
            outputs['dpkg-query'] = (1, '')
            self.assertEqual(inventory(Path(folder), self.fake(outputs))[1],
                             ['no putgd package installed'])


class RamBackedTmp(LockTest):
    """#1978: campaign scratch in the builders' tmpfs /tmp starved the k72 and f44 nightly guests."""
    GIB = 2**30

    def tmp(self, *entries, total=None):
        listed = [(path, size, inode) for inode, (path, size) in enumerate(entries, 1)]
        return lambda: {'total': sum(size for _, size, _ in listed) if total is None else total,
                        'entries': listed, 'complete': True}

    def test_large_entries_of_ours_are_items_and_a_large_total_gets_a_note(self):
        quiet = {'pgrep': (1, ''), 'flatpak': (0, ''), 'rpm': (1, ''), 'dpkg-query': (1, '')}
        tmp = self.tmp(('/tmp/s18.oqU1yV', 3 * self.GIB), ('/tmp/i1675', 200 * 2**20), ('/tmp/pgr.jHs4d5', 2**28))
        items, notes = lock.collect_inventory(Path('/nonexistent'), lambda argv: quiet.get(argv[0], (None, '')),
                                              own=set(), tmp=tmp, memory=lambda: 9 * self.GIB)
        self.assertEqual([(item['kind'], item['id'], item['detail']) for item in items],
                         [('tmp', '/tmp/s18.oqU1yV', '3.0 GB in RAM-backed /tmp'),
                          ('tmp', '/tmp/pgr.jHs4d5', '256.0 MB in RAM-backed /tmp')])
        self.assertEqual(items[0]['key'], 'tmp:/tmp/s18.oqU1yV:1')
        self.assertIn('/tmp is a tmpfs (RAM): your files there take 3.4 GB, with 9.0 GB of memory available', notes[-1])
        self.assertIn('~/work/tmp', notes[-1])
        small = self.tmp(('/tmp/a', 2**28), ('/tmp/b', 2**28), total=900 * 2**20)
        self.assertFalse(any('tmpfs' in note for note in lock.collect_inventory(
            Path('/nonexistent'), lambda argv: quiet.get(argv[0], (None, '')), own=set(), tmp=small)[1]))

    def test_new_large_tmp_scratch_holds_the_lock_but_not_the_nightlys(self):
        self.acquire()
        self.items.append({'kind': 'tmp', 'id': '/tmp/e48.9IxJxV', 'key': 'tmp:/tmp/e48.9IxJxV:7',
                           'detail': '4.5 GB in RAM-backed /tmp'})
        with self.assertRaises(lock.LockError) as refused:
            self.desktop.release('local_a', verified_clean=True)
        self.assertIn('~/work/tmp', str(refused.exception))
        reply = self.desktop.release('local_a', verified_clean=True, tmp_blocks=False)
        self.assertEqual([item['id'] for item in reply['check']['left']], ['/tmp/e48.9IxJxV'])
        self.assertFalse((self.directory / 'desktop.lock').exists())

    def test_tmp_there_at_acquire_does_not_block_and_not_mine_excuses_it(self):
        entry = {'kind': 'tmp', 'id': '/tmp/ta2k', 'key': 'tmp:/tmp/ta2k:3', 'detail': '1.0 GB in RAM-backed /tmp'}
        self.items.append(entry)
        self.acquire()
        self.desktop.release('local_a', verified_clean=True)
        self.items[:] = []
        self.acquire(session='local_b')
        self.items.append(dict(entry, key='tmp:/tmp/ta2k:4'))  # made again since: a new inode
        reply = self.desktop.release('local_b', verified_clean=True, not_mine=['/tmp/ta2k'])
        self.assertEqual([item['id'] for item in reply['check']['notMine']], ['/tmp/ta2k'])

    def test_usage_counts_only_our_files_on_a_tmpfs_root(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'tmp'
            (root / 'run/deep').mkdir(parents=True)
            (root / 'run/deep/blob').write_bytes(b'x' * 300000)
            (root / 'loose.ppm').write_bytes(b'y' * 100000)
            (root / 'link').symlink_to(root / 'run/deep/blob')
            (root / 'dir-link').symlink_to(root / 'run')
            mounts = Path(folder) / 'mounts'
            mounts.write_text(f'/dev/sda1 {folder} ext4 rw 0 0\ntmpfs {root} tmpfs rw 0 0\n')
            usage = lock.tmp_usage(str(root), mounts=str(mounts))
            self.assertTrue(usage['complete'])
            sizes = {Path(path).name: size for path, size, _ in usage['entries']}
            self.assertEqual(set(sizes), {'run', 'loose.ppm', 'link', 'dir-link'})
            self.assertGreaterEqual(sizes['run'], 300000)
            self.assertGreaterEqual(sizes['loose.ppm'], 100000)
            self.assertEqual((sizes['link'], sizes['dir-link']), (0, 0))
            self.assertEqual(usage['total'], sum(sizes.values()))
            self.assertEqual(lock.tmp_usage(str(root), uid=os.getuid() + 1, mounts=str(mounts))['total'], 0)
            # Not a tmpfs (or mounted over by a disk) means nothing to report.
            mounts.write_text(f'tmpfs {root} tmpfs rw 0 0\n/dev/sda2 {root} ext4 rw 0 0\n')
            self.assertIsNone(lock.tmp_usage(str(root), mounts=str(mounts)))
            self.assertIsNone(lock.tmp_usage(str(root), mounts=str(Path(folder) / 'missing')))

    def test_a_slow_walk_stops_and_says_its_sizes_are_lower_bounds(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'a').mkdir()
            (root / 'b').mkdir()
            mounts = root / 'mounts'
            mounts.write_text(f'tmpfs {root} tmpfs rw 0 0\n')
            ticks = iter(range(100))
            usage = lock.tmp_usage(str(root), mounts=str(mounts), seconds=0.5, clock=lambda: next(ticks))
        self.assertFalse(usage['complete'])
        with tempfile.TemporaryDirectory() as folder:  # one flat directory: the budget holds inside it too
            root = Path(folder)
            (root / 'flat').mkdir()
            for index in range(50):
                (root / 'flat' / str(index)).write_bytes(b'z')
            mounts = root / 'mounts'
            mounts.write_text(f'tmpfs {root} tmpfs rw 0 0\n')
            ticks = iter(range(1000))
            self.assertFalse(lock.tmp_usage(str(root), mounts=str(mounts), seconds=10,
                                            clock=lambda: next(ticks))['complete'])
        class Failing:
            def __enter__(self): return self
            def __exit__(self, *exc): return False
            def __iter__(self): raise OSError('readdir failed')
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(lock.os, 'scandir', return_value=Failing()):
            (Path(folder) / 'mounts').write_text(f'tmpfs {folder} tmpfs rw 0 0\n')
            self.assertEqual(lock.tmp_usage(folder, mounts=str(Path(folder) / 'mounts')),
                             {'total': 0, 'entries': [], 'complete': False})
        quiet = {'pgrep': (1, ''), 'flatpak': (0, ''), 'rpm': (1, ''), 'dpkg-query': (1, '')}
        notes = lock.collect_inventory(Path('/nonexistent'), lambda argv: quiet.get(argv[0], (None, '')), own=set(),
                                       tmp=lambda: usage)[1]
        self.assertIn('the /tmp size check stopped after 10 s; its sizes are lower bounds', notes)


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

    def test_last_event_ignores_other_files_kept_in_history(self):
        self.acquire('local_desktop')
        self.desktop.release('local_desktop', True)
        # A retired scratch note sorts after every stamped receipt by name.
        (self.directory / 'history' / 'plasma1364-scratch-retired.json').write_text('{"note": "retired"}\n')
        (self.directory / 'history' / '2099-01-01T00-00-00Z-scratch-retired.json').write_text('{"note": "retired"}\n')
        self.assertEqual(self.desktop.status()['last']['event'], 'release')

    def test_last_event_takes_the_later_receipt_of_one_second(self):
        self.acquire('local_desktop')
        self.desktop.release('local_desktop', True)
        self.acquire('local_desktop')
        self.desktop.release('local_desktop', True)
        names = sorted(path.name for path in (self.directory / 'history').glob('*.json'))
        self.assertTrue(any(name.endswith('-release-1.json') for name in names), names)
        self.assertTrue(self.desktop.status()['last']['receipt'].endswith('-release-1.json'))

    def test_last_event_takes_a_release_after_a_break_of_the_same_second(self):
        self.acquire('local_desktop')
        self.clock.advance(minutes=46)
        self.desktop.break_lock('local_x', 'stopped heartbeating')
        self.acquire('local_desktop')
        self.desktop.release('local_desktop', True)
        history = self.directory / 'history'
        brk = next(history.glob('*-break.json'))
        rel = next(history.glob('*-release.json'))
        os.utime(brk, ns=(1, 1))  # the break was written first
        os.utime(rel, ns=(2, 2))
        self.assertEqual(self.desktop.status()['last']['event'], 'release')
        os.utime(brk, ns=(3, 3))  # and the other way round
        self.assertEqual(self.desktop.status()['last']['event'], 'break')
        later = brk.with_name(brk.name.replace('-break.json', '-break-1.json'))
        brk.rename(later)  # a suffixed break written before an unsuffixed release
        os.utime(later, ns=(1, 1))
        self.assertEqual(self.desktop.status()['last']['event'], 'release')


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

    def run_main(self, *argv, process=None, minutes='10'):
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
                                  '--minutes', minutes, '--pins', pins, *argv], run=run, popen=popen)
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
        def give_up():  # the holder releases mid-command; the host is as the acquire found it (on a builder,
            # its own putgd package and sessions: the stand-in acquire inventories the machine the test runs on)
            unchanged = self.desktop.read_baseline()
            lock.Desktop(self.directory, 'k72', self.clock, lambda: (unchanged, [])).release('local_a', True)
        broken = FakeProcess([lock.subprocess.TimeoutExpired('ssh', 1), 0], during=give_up)
        code, _ = self.run_main('--', 'sleep', '1200', process=broken)
        self.assertEqual((code, broken.terminated), (1, True))

    def test_an_unconfirmed_renewal_stops_the_command_before_the_lease_lapses(self):
        timeout, now = lock.subprocess.TimeoutExpired('ssh', 1), [0]
        timeouts = []
        real_call = lock.call

        def during():  # each wait spends its full interval; renewals then find the host unreachable
            now[0] += 20
            (self.pins / 'k72' / 'known_hosts').unlink(missing_ok=True)

        def call(*args, timeout=90, **kwargs):
            timeouts.append(timeout)
            reply = real_call(*args, timeout=timeout, **kwargs)
            if len(timeouts) == 1:
                now[0] += 15  # the acquire's reply arrives 15 s after the host granted the lease
            return reply

        process = FakeProcess([timeout, timeout, timeout, 0], during=during)
        with mock.patch.object(lock, 'monotonic', lambda: now[0]), mock.patch.object(lock, 'call', call):
            code, _ = self.run_main('--', 'sleep', '1200', process=process, minutes='1')
        self.assertEqual((code, process.terminated), (1, True))
        self.assertEqual(process.waits, [20, 20])  # a third of the 1-minute lease; stopped by 56 s, before 60 s
        self.assertEqual(timeouts[1:], [20, 1])  # each renewal gives up 5 s before the expiry the host set

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
