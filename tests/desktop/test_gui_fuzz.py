"""The GUI fuzzer's safety rules: which keys it may press and which pixels it may touch. No desktop is contacted."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
from gui_fuzz.actor import HOTKEYS, chord_allowed  # noqa: E402
from gui_fuzz.world import World  # noqa: E402


class FakeFence:
    def __init__(self, windows, outputs=None):
        self.snapshot = {'windows': windows, 'outputs': outputs or [{'name': 'V1', 'geom': [0, 0, 1000, 800], 'scale': 1}], 'cursor': [0, 0]}

    def get(self):
        return self.snapshot, 0.0


def window(pid, cls, frame, **extra):
    base = {'id': str(pid) + cls, 'pid': pid, 'cls': cls, 'title': '', 'frame': frame, 'active': False, 'min': False, 'popup': False,
            'dock': False, 'normal': True, 'kind': 'KWin::XdgToplevelWindow', 'tr': None}
    base.update(extra)
    return base


class Keys(unittest.TestCase):
    def test_forbidden_chords_cannot_be_sent(self):
        for chord in ('alt+tab', 'alt+f4', 'ctrl+alt+f3', 'ctrl+q', 'alt+space', 'print', 'meta+l', 'f5', 'ctrl+alt+delete', 'shift+f10'):
            self.assertFalse(chord_allowed(chord, True), chord)

    def test_plain_keys_need_put_gd_focus_but_hotkeys_do_not(self):
        self.assertFalse(chord_allowed('ctrl+a', False))
        self.assertTrue(chord_allowed('ctrl+a', True))
        for chord in HOTKEYS.values():
            self.assertTrue(chord_allowed(chord, False), chord)


class Fence(unittest.TestCase):
    def world(self, windows):
        return World(FakeFence(windows), procs=lambda: {10: 'putgd-ui'})

    def test_only_the_topmost_window_must_be_put_gd(self):
        ours = window(10, 'putgd-ui', [100, 100, 400, 300])
        toast = window(99, 'org.kde.plasmashell', [300, 200, 200, 100])
        world = self.world([ours, toast])
        self.assertTrue(world.inside(150, 150))
        self.assertFalse(world.inside(350, 250))  # a notification lies over the window
        self.assertFalse(world.inside(50, 50))    # the desktop

    def test_overlay_opens_every_output(self):
        strips = [window(10, 'putgd-region', [0, 0, 1000, 800], kind='KWin::LayerShellV1Window')]
        self.assertTrue(self.world(strips).inside(900, 700))


if __name__ == '__main__':
    unittest.main()
