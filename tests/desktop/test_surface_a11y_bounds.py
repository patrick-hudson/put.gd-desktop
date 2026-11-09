#!/usr/bin/env python3
"""Bounded fresh accessibility walks on GNOME 50 / Qt 6.10 (#805, #819).

Fast host tests: no app, desktop, credentials, network or VM required.
"""
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver import probe

WITNESS = {'kind': 'gnome-lab-witness', 'shell_application': 'gnome-shell*'}
FRAMES = {'windows': [{'id': 21, 'title': 'put.gd', 'pid': 50, 'wm_class': 'putgd', 'type': 0,
                       'frame': [67, 32, 1213, 768], 'focus': True, 'minimized': False, 'stack': 3,
                       'transient_for': None}]}


class Node(list):
    """A live accessible that counts every call the walk makes into it."""
    def __init__(self, name, role, rect=None, pid=50, children=(), identifier='', showing=True, fail=None):
        super().__init__(children)
        self._name, self.role, self.rect, self.pid = name, role, rect, pid
        self.identifier, self.showing, self.fail = identifier, showing, fail
        self.path = '/n/%x' % id(self)
        self.calls = 0

    def _touch(self):
        self.calls += 1
        if self.fail:
            raise RuntimeError(self.fail)

    @property
    def name(self):
        self._touch()
        return self._name

    @property
    def description(self):
        self._touch()
        return ''

    def __iter__(self):
        self._touch()
        return list.__iter__(self)

    def getRoleName(self):
        self._touch()
        return self.role

    def get_process_id(self):
        return self.pid  # the a11y bus daemon answers this, not the application

    def get_accessible_id(self):
        self._touch()
        return self.identifier

    def getAttributes(self):
        self._touch()
        return []

    def getState(self):
        self._touch()
        showing = self.showing
        return SimpleNamespace(contains=lambda state: state == 'enabled' or (showing and state in ('visible', 'showing')))

    def queryComponent(self):
        self._touch()
        return SimpleNamespace(getExtents=lambda _: self.rect)

    def queryText(self):
        self._touch()
        raise NotImplementedError

    def queryAction(self):
        self._touch()
        raise NotImplementedError


def fake_pyatspi(*applications):
    desktop = Node('main', 'desktop frame', [0, 0, 1920, 1080], pid=1, children=applications)
    return SimpleNamespace(Registry=SimpleNamespace(getDesktop=lambda _: desktop), DESKTOP_COORDS=0,
                           STATE_VISIBLE='visible', STATE_SHOWING='showing', STATE_ENABLED='enabled')


def settings_window():
    """The Qt 6.10 Settings tree observed on GNOME 50 (#805/#819), trimmed."""
    prefix = 'QApplication.putgdMainWindow.QWidget.mainPages.settingsPanel'
    rail = prefix + '.settingsRail.settingsNavigation'
    general = Node('General', 'list item', [16, 124, 184, 38], identifier=rail)
    capture = Node('Capture', 'list item', [16, 162, 184, 38], identifier=rail)
    navigation = Node('Settings categories', 'list', [16, 124, 184, 152], identifier='Settings categories',
                      children=[general, capture])
    column = prefix + '.QWidget.settingsTabs.QScrollArea.generalPreferenceColumn'
    heading = Node('General', 'label', [248, 124, 760, 36], identifier=column + '.settingsPageHeadingGeneral')
    label = Node('Launch at login', 'label', [264, 221, 688, 36], identifier=column + '.QWidget.QLabel')
    box = Node('Launch at login', 'check box', [968, 227, 24, 24], identifier=column + '.QWidget.PreferenceValue.launchAtLogin')
    row = Node('', 'filler', [264, 221, 728, 36], identifier=column + '.QWidget', children=[label, box])
    hidden = [Node('Hidden control %d' % i, 'push button', [0, 0, 0, 0], showing=False) for i in range(40)]
    page = Node('', 'filler', [232, 100, 960, 660], identifier=column, children=[heading, row])
    hidden_page = Node('', 'filler', [0, 0, 0, 0], showing=False, children=hidden)
    frame = Node('put.gd', 'frame', [0, 0, 1213, 768], identifier='QApplication.putgdMainWindow',
                 children=[navigation, page, hidden_page])
    app = Node('putgd-ui', 'application', None, children=[frame])
    return app, frame, {'general': general, 'heading': heading, 'label': label, 'box': box, 'hidden': hidden}


class Walk(unittest.TestCase):
    def run_probe(self, function, query, *applications, session='wayland'):
        api = fake_pyatspi(*applications)
        with patch.dict(sys.modules, pyatspi=api), \
             patch.object(probe, 'desktop_root', return_value=api.Registry.getDesktop(0)), \
             patch.dict('os.environ', {'XDG_SESSION_TYPE': session}), \
             patch.object(probe, 'owned', side_effect=lambda pid: pid == 50), \
             patch.object(probe, 'compositor_windows', return_value=FRAMES), \
             patch.object(probe, 'popup_roots', return_value=[]):
            return function(query)


class BoundedCalls(Walk):
    def test_unowned_applications_are_never_contacted_for_owned_queries(self):
        app, frame, nodes = settings_window()
        busy = Node('gjs-console', 'application', None, pid=77, fail='Timeout was reached')
        rows = self.run_probe(probe.accessible, {'owned': True, 'name': 'Launch at login'}, busy, app)
        self.assertEqual(len(rows), 2)
        self.assertEqual(busy.calls, 0)


class ApplicationNames(Walk):
    def shell_query(self, names, *applications):
        for app in applications:
            app.bus = ':1.%d' % app.pid  # RawNode's bus name
        with patch.object(probe, 'application_names', return_value=names) as asked, \
             patch.object(probe, 'executable', side_effect=lambda pid: {60: ['evolution-alarm-notify'],
                                                                          61: ['gnome-shell']}.get(pid, [])):
            rows = self.run_probe(probe.accessible, {'application': 'gnome-shell*', 'name': 'put.gd'}, *applications)
        self.assertEqual(asked.call_count, 1)
        return rows

    def shell(self):
        icon = Node('put.gd', 'menu', [1140, 0, 36, 32], pid=40)
        return Node('gnome-shell', 'application', None, pid=40, children=[Node('', 'window', [0, 0, 1920, 1080], pid=40, children=[icon])])

    def test_silent_foreign_applications_are_never_contacted(self):
        # #805 tray rows: GTK session services that never answer cost nothing.
        shell = self.shell()
        silent = Node('', 'application', None, pid=60, fail='Timeout was reached')
        rows = self.shell_query({':1.40': 'gnome-shell', ':1.60': None}, silent, shell)
        self.assertEqual([(r['name'], r['role']) for r in rows], [('put.gd', 'menu')])
        self.assertEqual(silent.calls, 0)

    def test_named_foreign_applications_are_skipped_without_a_walk(self):
        other = Node('evolution-alarm-notify', 'application', None, pid=62, children=[Node('put.gd', 'menu', [0, 0, 1, 1], pid=62)])
        rows = self.shell_query({':1.40': 'gnome-shell', ':1.62': 'evolution-alarm-notify'}, other, self.shell())
        self.assertEqual(len(rows), 1)
        self.assertEqual(other.calls, 0)

    def test_silent_application_that_could_match_blocks_the_walk(self):
        silent = Node('', 'application', None, pid=61, fail='Timeout was reached')
        with self.assertRaisesRegex(RuntimeError, r'incomplete: gnome-shell \(pid 61\) did not answer'):
            self.shell_query({':1.40': 'gnome-shell', ':1.61': None}, silent, self.shell())
        self.assertEqual(silent.calls, 0)


SLOW_SERVER = r'''
import sys, time, json
for count, line in enumerate(sys.stdin):
    time.sleep(json.loads(line)['query'].get('delay', 0))
    sys.stdout.write(json.dumps({'ok': True, 'result': count}) + '\n'); sys.stdout.flush()
'''


if __name__ == '__main__':
    unittest.main()
