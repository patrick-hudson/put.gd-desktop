"""The GUI fuzzer's safety rules: which keys it may press and which pixels it may touch. No desktop is contacted."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
from gui_fuzz.actor import HOTKEYS, chord_allowed  # noqa: E402
from gui_fuzz.desktop import Cinnamon, Gnome, Kde  # noqa: E402
from gui_fuzz.fence_gnome import convert  # noqa: E402
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
    desk = Kde

    def world(self, windows):
        return World(FakeFence(windows), self.desk(), procs=lambda: {10: 'putgd-ui'})

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


class GnomeWitness(unittest.TestCase):
    """put.gd's selector is a Shell actor on GNOME: it becomes an overlay that opens every output, and Shell chrome stays in the way."""

    def raw(self, actors):
        return {'windows': [dict(window(10, 'putgd-ui', [100, 100, 400, 300]), hidden=False)],
                'actors': actors, 'outputs': [{'name': 'mon0', 'geom': [0, 0, 1000, 800], 'scale': 1}], 'cursor': [5, 5],
                'chrome': {'panel': [0, 0, 1000, 32], 'overview': False, 'modal': 1, 'banner': [300, 32, 400, 100]}}

    def test_selector_actor_is_an_overlay(self):
        selector = {'t': 'Gjs_putgd_put_gd_extension_RegionSelector', 'name': '', 'style': '', 'frame': [0, 0, 1000, 800], 'reactive': True}
        world = World(FakeFence(convert(self.raw([selector]))['windows']), Gnome(), procs=lambda: {10: 'putgd-ui'})
        self.assertTrue(world.overlays())
        self.assertTrue(world.inside(900, 700))

    def test_banner_and_unknown_modal_close_the_fence(self):
        snapshot = convert(self.raw([]))
        world = World(FakeFence(snapshot['windows']), Gnome(), procs=lambda: {10: 'putgd-ui'})
        self.assertFalse(world.overlays())
        self.assertFalse(world.inside(150, 150))  # a Shell modal that is not put.gd's covers the window
        self.assertFalse(Cinnamon().layer_kinds & Gnome().layer_kinds)


if __name__ == '__main__':
    unittest.main()
