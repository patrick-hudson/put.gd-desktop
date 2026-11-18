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
from gui_fuzz import watch  # noqa: E402
from gui_fuzz.actor import Actor  # noqa: E402
from gui_fuzz.desktop import Cinnamon, Gnome, Kde  # noqa: E402
from gui_fuzz.snapshot import chrome_regions, pseudo  # noqa: E402
from gui_fuzz.world import Log, World  # noqa: E402

OUT = [{'name': 'mon0', 'geom': [0, 0, 1280, 800], 'scale': 1}]


class FakeFence:
    def __init__(self, windows, outputs=None):
        self.snapshot = {'windows': windows, 'outputs': outputs or OUT, 'cursor': [0, 0], 'chrome': {}}
        self.closed = []
        self.before_close = None

    def get(self):
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
    """S2: a portal window is put.gd's chooser only on evidence."""

    def nautilus(self, **extra):
        return window('w-nautilus', 77, 'org.gnome.Nautilus', [300, 200, 500, 300], **extra)

    def test_a_users_nautilus_window_is_not_putgds(self):
        w, _ = world([OURS, self.nautilus()], Gnome())
        self.assertFalse(w.mine(self.nautilus()))
        self.assertFalse(w.inside(400, 300))

    def test_a_chooser_that_opens_while_putgd_has_a_request_pending_is_putgds(self):
        w, fence = world([OURS], Gnome())
        w.expect_chooser()
        fence.snapshot['windows'] = [OURS, self.nautilus()]
        self.assertTrue(w.mine(self.nautilus()))
        w.choosers.clear()  # the request is long over: the window stays accepted
        self.assertTrue(w.mine(self.nautilus()))

    def test_a_chooser_that_was_already_open_is_not(self):
        w, fence = world([OURS, self.nautilus()], Gnome())
        w.note_seen(self.nautilus())
        w.expect_chooser(at=time.time() + 30)
        self.assertFalse(w.mine(self.nautilus()))

    def test_transient_for_a_putgd_window_is_putgds_and_for_another_is_not(self):
        other = window('w-other', 55, 'firefox', [0, 0, 400, 300])
        w, _ = world([OURS, other, self.nautilus(tr='w-ours')], Gnome())
        self.assertTrue(w.mine(self.nautilus(tr='w-ours')))
        w2, _ = world([OURS, other, window('w-n2', 77, 'org.gnome.Nautilus', [0, 0, 100, 100], tr='w-other')], Gnome())
        self.assertFalse(w2.mine(window('w-n2', 77, 'org.gnome.Nautilus', [0, 0, 100, 100], tr='w-other')))

    def test_plasma_keeps_accepting_its_portal_class(self):
        chooser = window('w-kde', 78, 'org.freedesktop.impl.portal.desktop.kde', [0, 0, 300, 300])
        w, _ = world([OURS, chooser], Kde())
        self.assertTrue(w.mine(chooser))


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
    """S5: close only what the walk opened; record the finding first; count a close once the window is gone."""

    def foreign(self):
        return window('w-store', 88, 'snap-store', [100, 100, 600, 400], active=True)

    def test_a_window_that_was_there_before_the_walk_is_never_closed(self):
        old = self.foreign()
        dog, w, fence, findings, log = self.watchdog([OURS, old])
        log.last_input = time.time()
        now = time.time()
        dog.observe_windows(now)
        for step in (0, 7):
            dog.focus_step(now + step)
        self.assertIn('focus_left_putgd', [k[0] for k in findings.seen])
        self.assertEqual(fence.closed, [])

    def test_a_window_the_walk_opened_is_recorded_then_closed_and_counted_when_gone(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        new = self.foreign()
        log.last_input = time.time()
        fence.snapshot['windows'] = [OURS, new]
        now = time.time()
        dog.observe_windows(now)
        fence.before_close = lambda: self.assertTrue(any(k[0] == 'focus_left_putgd' for k in findings.seen), 'closed before the finding was recorded')
        dog.focus_step(now)
        dog.focus_step(now + 7)
        self.assertEqual(fence.closed, ['w-store'])
        self.assertEqual(dog.foreign, {})  # asked, not yet gone
        fence.snapshot['windows'] = [OURS]
        dog.focus_step(now + 8)
        self.assertEqual(dog.foreign, {'snap-store': 1})

    def test_a_window_nobody_input_explains_is_not_closed(self):
        dog, w, fence, findings, log = self.watchdog([OURS])
        fence.snapshot['windows'] = [OURS, self.foreign()]   # appeared with no input from the actor
        now = time.time()
        dog.observe_windows(now)
        dog.focus_step(now)
        dog.focus_step(now + 7)
        self.assertEqual(fence.closed, [])

    def test_plasma_never_closes_anything(self):
        dog, w, fence, findings, log = self.watchdog([OURS], Kde())
        log.last_input = time.time()
        fence.snapshot['windows'] = [OURS, self.foreign()]
        now = time.time()
        dog.observe_windows(now)
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


class CoreDumps(Fixtures):
    """S7 and D4: every shipped executable is put.gd's, and each dump is read by its own PID."""

    LIST = ('Tue 2026-11-17 21:42:59 CST 1574 1000 1000 SIGABRT present /usr/bin/nemo-desktop 4.1M\n'
            'Tue 2026-11-17 21:43:59 CST 2222 1000 1000 SIGSEGV present /usr/bin/putgd-region 1.2M\n')

    def test_classification_and_pid_lookup(self):
        calls = []

        def fake(args, timeout=10):
            calls.append(args)
            return (0, self.LIST if args[1] == 'list' else 'PID: x', '')
        dog, w, fence, findings, log = self.watchdog([OURS])
        dog.cores = 0
        with mock.patch.object(watch, 'run', side_effect=fake):
            dog.check_cores()
        kinds = sorted(k[0] for k in findings.seen)
        self.assertEqual(kinds, ['core_dump', 'desktop_core_dump_nemo_desktop'])
        infos = [c[-1] for c in calls if c[1] == 'info']
        self.assertEqual(sorted(infos), ['1574', '2222'])
        self.assertNotIn('-1', infos)

    def test_the_shell_evidence_carries_only_that_pids_dump(self):
        dog, *_ = self.watchdog([OURS])
        with mock.patch.object(watch, 'run', side_effect=lambda args, timeout=10: (0, '           PID: 4242 (gnome-shell)\n', '') if args[1] == 'info' else (0, '', '')):
            self.assertIn('4242', dog.shell_evidence(4242)['coredump_of_shell_pid'])
            self.assertEqual(dog.shell_evidence(999)['coredump_of_shell_pid'], '')
            self.assertEqual(dog.shell_evidence(None)['coredump_of_shell_pid'], '')


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
