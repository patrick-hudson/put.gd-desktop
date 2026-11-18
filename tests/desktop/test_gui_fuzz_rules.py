"""The GUI fuzzer's GNOME and Cinnamon rules, on in-memory snapshots: no desktop, no D-Bus, no X server."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
from gui_fuzz import main as fuzz_main, snapshot, watch  # noqa: E402
from gui_fuzz.actor import Actor  # noqa: E402
from gui_fuzz.desktop import Cinnamon, Gnome, Kde, parse_gdctl, restore_args  # noqa: E402
from gui_fuzz.fence_gnome import convert  # noqa: E402
from gui_fuzz.snapshot import chrome_regions, pseudo  # noqa: E402
from gui_fuzz.world import Log, World  # noqa: E402

OUT = [{'name': 'mon0', 'geom': [0, 0, 1280, 800], 'scale': 1}]


class FakeFence:
    def __init__(self, windows, outputs=None):
        self.snapshot = {'windows': windows, 'outputs': outputs or OUT, 'cursor': [0, 0], 'chrome': {}}
        self.closed = []
        self.before_close = None
        self.serial = 0
        self.on_get = None

    def get(self):
        if self.on_get:
            self.on_get()
        return self.snapshot, time.time()

    def close(self, window_id):
        if self.before_close:
            self.before_close()
        self.closed.append(window_id)


def window(wid, pid, cls, frame, **extra):
    base = {'id': wid, 'pid': pid, 'cls': cls, 'title': '', 'frame': frame, 'content': frame, 'active': False, 'min': False, 'popup': False,
            'dock': False, 'desktop': False, 'normal': True, 'notif': False, 'fullscreen': False, 'kind': 'MetaWindow', 'tr': None, 'z': 0}
    base.update(extra)
    return base


def world(windows, desk, fence=None):
    fence = fence or FakeFence(windows)
    desk.fence = fence
    return World(fence, desk, procs=lambda: {10: 'putgd-ui'}), fence


OURS = window('w-ours', 10, 'putgd-ui', [100, 100, 600, 400])


class PortalWindows(unittest.TestCase):
    """S2: a portal window is put.gd's only when it is transient for a put.gd window. No time window, no pending request."""

    def nautilus(self, **extra):
        return window('w-nautilus', 77, 'org.gnome.Nautilus', [300, 200, 500, 300], **extra)

    def test_a_users_nautilus_window_is_not_putgds_whatever_put_gd_asked_for(self):
        w, fence = world([OURS], Gnome())
        if hasattr(w, 'expect_chooser'):  # the previous head let a pending chooser request make a new portal window put.gd's
            w.expect_chooser()
        fence.snapshot['windows'] = [OURS, self.nautilus()]
        self.assertFalse(w.mine(self.nautilus()))
        self.assertFalse(w.inside(400, 300))

    def test_transient_for_a_putgd_window_is_putgds_and_for_another_is_not(self):
        other = window('w-other', 55, 'firefox', [0, 0, 400, 300])
        w, _ = world([OURS, other, self.nautilus(tr='w-ours')], Gnome())
        self.assertTrue(w.mine(self.nautilus(tr='w-ours')))
        w2, _ = world([OURS, other, window('w-n2', 77, 'org.gnome.Nautilus', [0, 0, 100, 100], tr='w-other')], Gnome())
        self.assertFalse(w2.mine(window('w-n2', 77, 'org.gnome.Nautilus', [0, 0, 100, 100], tr='w-other')))

    def test_plasmas_portal_chooser_needs_a_putgd_parent_too(self):
        chooser = window('w-kde', 78, 'org.freedesktop.impl.portal.desktop.kde', [0, 0, 300, 300])
        w, _ = world([OURS, chooser], Kde())
        self.assertFalse(w.mine(chooser))
        child = dict(chooser, tr='w-ours')
        w2, _ = world([OURS, child], Kde())
        self.assertTrue(w2.mine(child))


class StrictInput(unittest.TestCase):
    """S3: input only with positive evidence that put.gd owns the target."""

    def selector(self, active, kind='ShellOverlay'):
        return pseudo('sel', 'putgd-shell-selector', [0, 0, 1280, 800], 5, kind=kind, active=active)

    def test_the_overview_above_the_selector_denies_the_pointer(self):
        overview = pseudo('ov', 'gnome-shell-overview', [0, 0, 1280, 800], 9, kind='ShellChrome')
        w, _ = world([OURS, self.selector(True), overview], Gnome())
        self.assertFalse(w.inside(500, 400))
        w2, _ = world([OURS, self.selector(True)], Gnome())
        self.assertTrue(w2.inside(500, 400))
        w3, _ = world([OURS, self.selector(False, 'KWin::LayerShellV1Window'), overview], Kde())  # Plasma's rule is unchanged: an overlay opens every output
        self.assertTrue(w3.inside(500, 400))

    def test_a_visible_selector_is_not_focus_unless_it_holds_the_grab(self):
        desktop_icons = window('w-ding', 5, 'gjs', [0, 0, 1280, 800], active=True, desktop=True)
        for desk, kind, expected in ((Gnome(), 'ShellOverlay', False), (Kde(), 'KWin::LayerShellV1Window', True)):
            w, _ = world([desktop_icons, self.selector(False, kind)], desk)
            self.assertEqual(Actor(None, w, mock.Mock(), None).mine_focused(), expected, desk.name)
        w, _ = world([window('w-ding', 5, 'gjs', [0, 0, 1280, 800], desktop=True), self.selector(True)], Gnome())
        self.assertTrue(Actor(None, w, mock.Mock(), None).mine_focused())

    def test_cinnamon_panel_and_notification_regions_deny_the_pointer(self):
        monitors = [{'geom': [0, 0, 1280, 800]}]
        regions = chrome_regions(monitors, [0, 0, 1280, 760])
        self.assertIn([0, 760, 1280, 40], regions)
        self.assertIn([860, 0, 420, 200], regions)    # top-right corner
        self.assertIn([860, 600, 420, 200], regions)  # bottom-right corner
        chrome = [pseudo('c%d' % i, 'cinnamon-chrome', r, 50 + i, kind='ShellChrome') for i, r in enumerate(regions)]
        big = window('w-big', 10, 'putgd-ui', [0, 0, 1280, 800], kind='X11Window')
        w, _ = world([big] + chrome, Cinnamon())
        self.assertFalse(w.inside(100, 780))   # over the panel
        self.assertFalse(w.inside(1200, 50))   # where a notification bubble may be
        self.assertTrue(w.inside(100, 100))
        self.assertEqual(chrome_regions(monitors, None)[0], [0, 0, 1280, 800])  # an unknown work area denies everything


class GrabStack(unittest.TestCase):
    """S3 on GNOME: the selector is the input target only when it is the top of Mutter's modal stack."""

    def raw(self, modal_actors):
        selector = {'t': 'Gjs_putgd_put_gd_extension_RegionSelector', 'name': '', 'style': '', 'frame': [0, 0, 1280, 800], 'reactive': True}
        return {'windows': [dict(window('w-ding', 5, 'gjs', [0, 0, 1280, 800], desktop=True, active=True), hidden=False)], 'actors': [selector], 'outputs': OUT,
                'cursor': [5, 5], 'chrome': {'panel': None, 'overview': False, 'modal': len(modal_actors), 'banner': None, 'modal_actors': modal_actors}}

    def mine_focused(self, raw):
        snap = convert(raw)
        w, fence = world(snap['windows'], Gnome())
        fence.snapshot = dict(snap, cursor=[5, 5])
        return Actor(None, w, mock.Mock(), None).mine_focused(), w

    def test_the_selector_on_top_of_the_stack_has_the_input(self):
        focused, w = self.mine_focused(self.raw(['Gjs_putgd_put_gd_extension_RegionSelector']))
        self.assertTrue(focused)
        self.assertTrue(w.inside(600, 400))

    def test_a_foreign_grab_above_the_selector_denies_keys_and_pointer(self):
        focused, w = self.mine_focused(self.raw(['Gjs_putgd_put_gd_extension_RegionSelector', 'Gjs_ui_modalDialog_ModalDialog']))
        self.assertFalse(focused)
        self.assertFalse(w.inside(600, 400))

    def test_a_foreign_grab_denies_keys_even_while_a_putgd_window_is_the_active_one(self):
        raw = self.raw(['Gjs_ui_modalDialog_ModalDialog'])
        raw['actors'] = []
        raw['windows'] = [dict(OURS, hidden=False, active=True)]
        focused, w = self.mine_focused(raw)
        self.assertFalse(focused)
        raw['chrome'] = dict(raw['chrome'], overview=True, modal=0, modal_actors=[])
        self.assertFalse(self.mine_focused(raw)[0])  # the overview too
        raw['chrome'] = dict(raw['chrome'], overview=False)
        self.assertTrue(self.mine_focused(raw)[0])   # nothing above put.gd's window: keys may go

    def test_a_selector_below_a_foreign_grab_is_not_the_target_either(self):
        focused, w = self.mine_focused(self.raw(['Gjs_ui_popupMenu_PopupMenu', 'Gjs_putgd_put_gd_extension_RegionSelector', 'Gjs_ui_popupMenu_PopupMenu']))
        self.assertFalse(focused)


class X11Focus(unittest.TestCase):
    """S3 on Cinnamon: only the X input focus makes a window active."""

    def test_no_resolvable_focus_means_no_active_window(self):
        windows = [window('1', 10, 'putgd-ui', [0, 0, 100, 100], active=True), window('2', 99, 'firefox', [0, 0, 100, 100])]
        snapshot.focus_active(windows, {'1': 100, '2': 200}, None)
        self.assertEqual([w['active'] for w in windows], [False, False])

    def test_a_foreign_focus_leaves_putgd_inactive_and_putgds_focus_activates_it(self):
        windows = [window('1', 10, 'putgd-ui', [0, 0, 100, 100]), window('2', 99, 'firefox', [0, 0, 100, 100])]
        snapshot.focus_active(windows, {'1': 100, '2': 200}, 200)
        self.assertEqual([w['active'] for w in windows], [False, True])
        w, _ = world(windows, Cinnamon())
        self.assertFalse(Actor(None, w, mock.Mock(), None).mine_focused())
        snapshot.focus_active(windows, {'1': 100, '2': 200}, 100)
        self.assertTrue(Actor(None, w, mock.Mock(), None).mine_focused())

    def test_a_focus_in_a_window_the_fence_does_not_list_activates_nothing(self):
        windows = [window('1', 10, 'putgd-ui', [0, 0, 100, 100], active=True)]
        snapshot.focus_active(windows, {'1': 100}, 777)
        self.assertFalse(windows[0]['active'])


class FakeDev:
    """A pointer device: the fence's cursor follows the relative motion, and a hook can move things after an event."""

    def __init__(self, fence):
        self.fence = fence
        self.down = set()
        self.presses = 0
        self.wheels = 0
        self.after = None

    def rel(self, dx, dy):
        x, y = self.fence.snapshot['cursor']
        self.fence.snapshot['cursor'] = [x + dx, y + dy]
        if self.after:
            self.after('rel')

    def button(self, name, pressed):
        self.presses += 1 if pressed else 0
        if self.after:
            self.after('button-down' if pressed else 'button-up')

    def wheel(self, step, horizontal=False):
        self.wheels += 1
        if self.after:
            self.after('wheel')

    def key(self, name, pressed):
        pass


class PointerRechecks(unittest.TestCase):
    """S11: the pointer is read again after each move and fenced before every press and wheel event, 1 px inside the window."""

    def actor(self):
        w, fence = world([OURS], Gnome())
        w.freshness = 0.0
        dev = FakeDev(fence)
        actor = Actor(dev, w, mock.Mock(), __import__('random').Random(1))
        return actor, dev, fence

    def test_a_target_on_the_windows_edge_is_denied(self):
        actor, dev, fence = self.actor()
        with self.assertRaises(Exception):
            actor.move(100, 200)          # the first column of the window
        with self.assertRaises(Exception):
            actor.move(699, 200)          # its last column
        actor.move(101, 200)

    def test_a_pointer_that_ends_up_elsewhere_after_the_move_is_denied(self):
        actor, dev, fence = self.actor()
        reads = {'n': 0, 'armed': False}

        def arm(event):
            reads['armed'] = True        # the pointer has reached the target
        dev.after = arm

        def warp():
            if reads['armed']:
                reads['n'] += 1
                if reads['n'] >= 2:      # the read that finds the move converged is the first; the check after it finds the pointer elsewhere
                    fence.snapshot['cursor'] = [900, 700]
        fence.on_get = warp
        with self.assertRaises(Exception):
            actor.move(300, 300)
        self.assertEqual(dev.presses, 0)

    def test_every_press_of_a_multi_click_is_checked(self):
        actor, dev, fence = self.actor()
        actor.move(300, 300)

        def vanish(event):
            if event == 'button-up':
                fence.snapshot['windows'] = []  # the window closes after the first click
        dev.after = vanish
        with self.assertRaises(Exception):
            actor.click(300, 300, count=3)
        self.assertEqual(dev.presses, 1)

    def test_no_fresh_snapshot_means_deny_and_never_the_cached_one(self):
        actor, dev, fence = self.actor()
        actor.move(300, 300)
        actor.world.freshness = 0.0
        stale = time.time() - 5
        fence.get = lambda: (fence.snapshot, stale)   # the fence has stopped producing snapshots
        actor.world.fresh_timeout = 0.05
        with self.assertRaises(Exception):
            actor.click(300, 300)
        self.assertEqual(dev.presses, 0)
        with self.assertRaises(Exception):
            actor.wheel(2)
        self.assertEqual(dev.wheels, 0)

    def test_a_move_that_sends_no_events_records_no_input(self):
        actor, dev, fence = self.actor()
        actor.move(300, 300)
        sent = len(actor.world.input_serials)
        self.assertGreater(sent, 0)
        actor.move(300, 300)    # already there: no device event goes out
        self.assertEqual(len(actor.world.input_serials), sent)

    def test_each_wheel_notch_is_logged_as_it_is_sent(self):
        actor, dev, fence = self.actor()
        actor.move(300, 300)

        def vanish(event):
            fence.snapshot['windows'] = []   # after the first notch
        dev.after = vanish
        actor.log.write.reset_mock()
        with self.assertRaises(Exception):
            actor.wheel(5)
        wheels = [c.args[0] for c in actor.log.write.call_args_list if c.args and c.args[0].get('op') == 'wheel']
        self.assertEqual(wheels, [{'op': 'wheel', 'n': 1, 'h': False}])
        self.assertEqual(dev.wheels, 1)

    def test_every_wheel_event_is_checked(self):
        actor, dev, fence = self.actor()
        actor.move(300, 300)

        def vanish(event):
            fence.snapshot['windows'] = []
        dev.after = vanish
        with self.assertRaises(Exception):
            actor.wheel(5)
        self.assertEqual(dev.wheels, 1)


class Fixtures(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        patcher = mock.patch.object(watch, 'run', return_value=(0, '', ''))
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self.dir.cleanup)

    def watchdog(self, windows, desk=None):
        desk = desk or Gnome()
        w, fence = world(windows, desk)
        log = Log(os.path.join(self.dir.name, 'actions.jsonl'), 1)
        findings = watch.Findings(self.dir.name, log, w, 1)
        dog = watch.Watchdog(self.dir.name, w, findings, log)
        for stream in (log.stream, findings.marks, dog.memory):
            self.addCleanup(stream.close)
        dog.prime()
        return dog, w, fence, findings, log


class CrashViewers(Fixtures):
    """S4: a crash viewer is killed only by a real PID of the expected name."""

    def test_pid_zero_and_foreign_names_are_never_killed(self):
        dog, *_ = self.watchdog([OURS])
        with mock.patch.object(os, 'kill', side_effect=AssertionError('os.kill was called')):
            self.assertFalse(dog.kill_crash_viewer({'pid': 0}))
            self.assertFalse(dog.kill_crash_viewer({'pid': None}))
            self.assertFalse(dog.kill_crash_viewer({'pid': -1}))
            sleeper = subprocess.Popen(['sleep', '5'])
            self.addCleanup(sleeper.kill)
            self.assertFalse(dog.kill_crash_viewer({'pid': sleeper.pid}))  # alive, but its comm is not a crash viewer's
            self.assertFalse(dog.kill_crash_viewer({'pid': 2 ** 22 + 12345}))  # not alive


class ForeignWindows(Fixtures):
    """S5: close only what the walk's input led to, by snapshot number; record the finding first; count a close once the window is gone."""

    def foreign(self, seq):
        return window('w-store', 88, 'snap-store', [100, 100, 600, 400], active=True, seq=seq)

    def test_a_window_that_was_there_before_the_walks_input_is_never_closed(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        w.input_serials.extend([50, 60])
        fence.snapshot['windows'] = [OURS, self.foreign(seq=3)]   # first seen in snapshot 3, long before the first input
        now = time.time()
        for step in (0, 7):
            dog.focus_step(now + step)
        self.assertIn('focus_left_putgd', [k[0] for k in findings.seen])
        self.assertEqual(fence.closed, [])

    def test_a_window_first_seen_in_the_snapshot_of_the_input_is_not_closed_but_a_later_one_is(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        w.input_serials.append(50)
        for seq, expected in ((51, []), (52, ['w-store'])):
            fence.closed.clear()
            dog.pending_close.clear()
            dog.last_close = 0
            fence.snapshot['windows'] = [OURS, self.foreign(seq)]
            dog.focus_lost_since = None
            now = time.time()
            dog.focus_step(now)
            dog.focus_step(now + 7)
            self.assertEqual(fence.closed, expected, seq)

    def test_the_finding_comes_first_and_the_count_waits_for_the_window_to_go(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        w.input_serials.append(10)
        fence.snapshot['windows'] = [OURS, self.foreign(seq=20)]
        fence.before_close = lambda: self.assertTrue(any(k[0] == 'focus_left_putgd' for k in findings.seen), 'closed before the finding was recorded')
        now = time.time()
        dog.focus_step(now)
        dog.focus_step(now + 7)
        self.assertEqual(fence.closed, ['w-store'])
        self.assertEqual(dog.foreign, {})
        fence.snapshot['windows'] = [OURS]
        dog.focus_step(now + 8)
        self.assertEqual(dog.foreign, {'snap-store': 1})

    def test_plasma_never_closes_anything(self):
        dog, w, fence, findings, log = self.watchdog([OURS], Kde())
        w.input_serials.append(10)
        fence.snapshot['windows'] = [OURS, self.foreign(seq=20)]
        now = time.time()
        dog.focus_step(now)
        dog.focus_step(now + 7)
        self.assertEqual(fence.closed, [])


class ResidentExit(Fixtures):
    """S6: only the Quit item the actor activated explains a clean exit."""

    def gone(self, desk, quit_asked, clean):
        dog, w, fence, findings, log = self.watchdog([OURS], desk)
        dog.resident = 2 ** 22 + 54321
        if quit_asked:
            log.last_quit_at = time.time()
        restarted = []
        with mock.patch.object(dog, 'clean_quit', return_value=clean), mock.patch.object(dog, 'restart_resident', side_effect=lambda now: restarted.append(now)):
            dog.resident_gone(time.time())
        return restarted, findings

    def test_a_clean_exit_without_the_quit_item_is_reported_and_fatal(self):
        restarted, findings = self.gone(Gnome(), quit_asked=False, clean=True)
        self.assertEqual(restarted, [])
        self.assertTrue(findings.fatal.is_set())
        self.assertIn('resident_exited', [k[0] for k in findings.seen])

    def test_the_quit_item_then_a_clean_exit_restarts_the_resident(self):
        restarted, findings = self.gone(Gnome(), quit_asked=True, clean=True)
        self.assertEqual(len(restarted), 1)
        self.assertFalse(findings.fatal.is_set())

    def test_the_quit_item_without_a_clean_exit_is_a_death(self):
        restarted, findings = self.gone(Gnome(), quit_asked=True, clean=False)
        self.assertEqual(restarted, [])
        self.assertIn('resident_died', [k[0] for k in findings.seen])

    def test_plasma_keeps_its_policy(self):
        restarted, findings = self.gone(Kde(), quit_asked=True, clean=True)
        self.assertEqual(restarted, [])
        self.assertIn('resident_died', [k[0] for k in findings.seen])


class StopAndStatus(Fixtures):
    """S6: a fatal finding wins over the time limit, and a last poll runs before the exit status is decided."""

    def args(self, **extra):
        values = dict(minutes=1.0, max_steps=0, keep_going=False)
        values.update(extra)
        return mock.Mock(**values)

    def test_fatal_before_time_and_step_limits(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        findings.fatal.set()
        started = time.time() - 600   # long past the time limit
        self.assertEqual(fuzz_main.stop_code(findings, self.args(), log, started, time.time()), 3)
        self.assertEqual(fuzz_main.stop_code(findings, self.args(keep_going=True), log, started, time.time()), 0)
        findings.fatal.clear()
        self.assertIsNone(fuzz_main.stop_code(findings, self.args(), log, time.time(), time.time()))

    def test_the_last_poll_can_turn_a_clean_run_into_a_failed_one(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        dog.final_poll = lambda: findings.fatal.set()
        self.assertEqual(fuzz_main.exit_status(0, dog, findings, self.args()), 3)
        findings.fatal.clear()
        dog.final_poll = lambda: None
        self.assertEqual(fuzz_main.exit_status(0, dog, findings, self.args()), 0)


class CoreDumps(Fixtures):
    """S7 and D4: one list per poll, dumps tracked by PID and time, one finding each, every shipped executable put.gd's."""

    NEMO = 'Tue 2026-11-17 21:42:59 CST 1574 1000 1000 SIGABRT present /usr/bin/nemo-desktop 4.1M'
    REGION = 'Tue 2026-11-17 21:43:59 CST 2222 1000 1000 SIGSEGV present /usr/bin/putgd-region 1.2M'
    UI = 'Tue 2026-11-17 21:44:59 CST 3333 1000 1000 SIGSEGV present /usr/bin/putgd-ui 1.0M'
    UI2 = 'Tue 2026-11-17 21:45:59 CST 3334 1000 1000 SIGSEGV present /usr/bin/putgd-ui 1.0M'

    def poll(self, dog, lines):
        calls = []

        def fake(args, timeout=10):
            calls.append(args)
            return (0, '\n'.join(lines) if args[1] == 'list' else 'PID: x', '')
        with mock.patch.object(watch, 'run', side_effect=fake):
            dog.check_cores()
        return calls

    def test_a_dump_that_replaces_one_in_the_journal_is_still_new(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        dog.seen_cores = {dog.core_key(self.NEMO)}
        calls = self.poll(dog, [self.REGION])    # the old dump rotated out and a new one came in: the count is the same
        self.assertEqual([k[0] for k in findings.seen], ['core_dump'])
        self.assertEqual(sum(1 for c in calls if c[1] == 'list'), 1)
        self.assertEqual([c[-1] for c in calls if c[1] == 'info'], ['2222'])
        self.poll(dog, [self.REGION])
        self.assertEqual(len(findings.seen), 1)  # not reported twice

    def test_two_dumps_of_one_executable_are_two_findings_and_desktop_dumps_are_apart(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        self.poll(dog, [self.NEMO, self.UI, self.UI2])
        self.assertEqual(sorted(k[0] for k in findings.seen), ['core_dump', 'core_dump', 'desktop_core_dump_nemo_desktop'])
        self.assertEqual(len(findings.seen), 3)

    def test_plasma_files_one_finding_per_new_dump_too(self):
        dog, w, fence, findings, log = self.watchdog([OURS], Kde())
        self.poll(dog, [self.NEMO, self.UI])
        self.assertEqual(sorted(k[0] for k in findings.seen), ['core_dump', 'core_dump'])

    def test_the_shell_evidence_carries_only_that_pids_dump(self):
        dog, *_ = self.watchdog([OURS])
        with mock.patch.object(watch, 'run', side_effect=lambda args, timeout=10: (0, '           PID: 4242 (gnome-shell)\n', '') if args[1] == 'info' else (0, '', '')):
            self.assertIn('4242', dog.shell_evidence(4242)['coredump_of_shell_pid'])
            self.assertEqual(dog.shell_evidence(999)['coredump_of_shell_pid'], '')
            self.assertEqual(dog.shell_evidence(None)['coredump_of_shell_pid'], '')


class Layout(unittest.TestCase):
    """S9: a GNOME layout is saved whole and put back exactly."""

    SHOW = """Monitors:
├──Monitor Virtual-1 (Red Hat, Inc. 15")
│  ├──Current mode
│  │   └──1280x800@74.994
│  └──Preferences
└──Monitor Virtual-2 (Red Hat, Inc. 15")
   ├──Current mode
   │   └──1280x800@74.994
   └──Preferences

Logical monitors:
├──Logical monitor #1
│  ├──Position: (0, 0)
│  ├──Scale: 1.5
│  ├──Transform: normal
│  ├──Primary: yes
│  └──Monitors: (1)
│      └──Virtual-1 (Red Hat, Inc. 15")
└──Logical monitor #2
   ├──Position: (853, 0)
   ├──Scale: 1.0
   ├──Transform: normal
   ├──Primary: no
   └──Monitors: (1)
       └──Virtual-2 (Red Hat, Inc. 15")
"""

    def test_positions_scales_primary_and_modes_survive_the_round_trip(self):
        layout = parse_gdctl(self.SHOW)
        args = ' '.join(restore_args(layout))
        self.assertEqual(args, 'gdctl set --logical-monitor --primary --x 0 --y 0 --scale 1.5 --transform normal --monitor Virtual-1 --mode 1280x800@74.994 '
                               '--logical-monitor --x 853 --y 0 --scale 1.0 --transform normal --monitor Virtual-2 --mode 1280x800@74.994')

    def test_an_unreadable_layout_is_not_restored(self):
        self.assertIsNone(parse_gdctl('gdctl: command not found'))


class FatalAtReservation(Fixtures):
    """S6: a fatal finding sets the flag when it is reserved, before its evidence is read, and a repeat sets it again."""

    def test_the_flag_is_set_before_the_evidence_is_collected(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        seen = []

        def slow_evidence(args, timeout=10):
            seen.append(findings.fatal.is_set())   # the first thing add() does after reserving is read the journal
            return (0, '', '')
        with mock.patch.object(watch, 'run', side_effect=slow_evidence):
            findings.add('resident_died', 'resident pid 7 is gone', fatal=True)
        self.assertTrue(seen and all(seen), seen)

    def test_a_repeat_of_a_fatal_finding_sets_it_again_and_nothing_clears_it(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        findings.add('resident_died', 'resident pid 7 is gone')           # the same finding, not yet fatal
        self.assertFalse(findings.fatal.is_set())
        findings.add('resident_died', 'resident pid 7 is gone', fatal=True)  # a duplicate key, now fatal
        self.assertTrue(findings.fatal.is_set())
        findings.add('resident_died', 'resident pid 7 is gone')
        self.assertTrue(findings.fatal.is_set())


class ReplayAndSignals(unittest.TestCase):
    """S9 on replay and S1 on teardown."""

    def test_a_replayed_monitor_command_marks_the_layout_changed(self):
        import json
        gnome, cinnamon = Gnome(), Cinnamon()
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, 'actions.jsonl')
            with open(path, 'w') as stream:
                stream.write(json.dumps({'op': 'sh', 'a': ['gdctl', 'set', '--logical-monitor', '--monitor', 'Virtual-1']}) + '\n')
                stream.write(json.dumps({'op': 'sh', 'a': ['xrandr', '--output', 'Virtual-2', '--off']}) + '\n')
            for desk in (gnome, cinnamon):
                fuzz_main.replay(path, mock.Mock(), desk)
        self.assertTrue(gnome.layout_changed)
        self.assertTrue(cinnamon.monitors_changed)

    def test_an_interruption_during_the_teardown_waits_until_it_is_done(self):
        import signal
        seen = []
        previous = signal.signal(signal.SIGTERM, lambda *_: seen.append('handler'))
        try:
            def teardown():
                os.kill(os.getpid(), signal.SIGTERM)
                seen.append('teardown finished' if not seen else 'cut short')
            fuzz_main.blocked(teardown)
        finally:
            signal.signal(signal.SIGTERM, previous)
        self.assertEqual(seen, ['teardown finished', 'handler'])


class Noise(unittest.TestCase):
    """S8: only the three expected probe lines are noise."""

    def test_exact_messages_only(self):
        for line in ('[h264_nvenc @ 0x78b85810de00] Cannot load libcuda.so.1',
                     '[VAAPI @ 0x55900eef6b80] libva: /usr/lib/x86_64-linux-gnu/dri/virtio_gpu_drv_video.so init failed',
                     '[VAAPI @ 0x55900eef6b80] Failed to initialise VAAPI connection: 2 (resource allocation failed).'):
            self.assertTrue(watch.PROBE_NOISE.match(line), line)
        for line in ('[VAAPI @ 0x1] Failed to initialise VAAPI connection: 5 (some other error).', '[AVHWDeviceContext @ 0x1] Cannot open display',
                     '[h264_nvenc @ 0x1] Cannot load libcuda.so.1 and the encoder is broken', '[VAAPI @ 0x1] Cannot allocate a surface'):
            self.assertFalse(watch.PROBE_NOISE.match(line), line)


if __name__ == '__main__':
    unittest.main()
