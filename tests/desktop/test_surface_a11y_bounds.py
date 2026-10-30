#!/usr/bin/env python3
"""Bounded fresh accessibility walks on GNOME 50 / Qt 6.10 (#805, #819).

Fast host tests: no app, desktop, credentials, network or VM required.
"""
import ast
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver import target as target_module
from tools.lab.surface_driver.api import RecipeBlocked
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.profiles import load_profile

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
        with patch.dict(sys.modules, pyatspi=fake_pyatspi(*applications)), \
             patch.dict('os.environ', {'XDG_SESSION_TYPE': session}), \
             patch.object(probe, 'owned', side_effect=lambda pid: pid == 50), \
             patch.object(probe, 'compositor_windows', return_value=FRAMES), \
             patch.object(probe, 'popup_roots', return_value=[]):
            return function(query)


class WindowLevelGeometry(Walk):
    def test_wayland_geometry_never_reads_below_the_window(self):
        # #805: the generic geometry walk read every Settings page node in full.
        app, frame, nodes = settings_window()
        found = self.run_probe(probe.geometry, {'title': '^put\\.gd$', 'compositor': WITNESS}, app)
        self.assertEqual((found['name'], found['window_id']), ('put.gd', 21))
        self.assertEqual([found[k] for k in ('x', 'y', 'width', 'height')], [67, 32, 1213, 768])
        below = [child for child in frame] + list(nodes.values())[:-1] + nodes['hidden']
        self.assertEqual(sum(node.calls for node in below), 0)

    def test_wayland_windows_inventory_stays_at_window_level(self):
        app, frame, nodes = settings_window()
        rows = self.run_probe(probe.windows, {'owned': True, 'compositor': WITNESS}, app)
        self.assertEqual([row['name'] for row in rows], ['put.gd'])
        self.assertEqual(nodes['box'].calls, 0)


class BoundedCalls(Walk):
    def test_unanswered_call_blocks_naming_the_application(self):
        app, frame, nodes = settings_window()
        nodes['box'].fail = 'GDBus.Error:org.freedesktop.DBus.Error.NoReply: Did not receive a reply'
        with self.assertRaisesRegex(RuntimeError, r"incomplete: putgd-ui \(pid 50\) did not answer"):
            self.run_probe(probe.accessible, {'owned': True, 'name': 'Launch at login'}, app)

    def test_vanished_object_is_still_skipped(self):
        app, frame, nodes = settings_window()
        nodes['label'].fail = 'GDBus.Error:org.freedesktop.DBus.Error.UnknownObject: gone'
        rows = self.run_probe(probe.accessible, {'owned': True, 'name': 'Launch at login'}, app)
        self.assertEqual([row['role'] for row in rows], ['check box'])

    def test_walk_over_budget_names_the_slow_application(self):
        app, frame, nodes = settings_window()
        clock = iter(range(0, 10000))
        with patch.object(probe.time, 'monotonic', side_effect=lambda: next(clock)), \
             self.assertRaisesRegex(RuntimeError, r'exceeded its 8 s budget after \d+ nodes: putgd-ui \(pid 50\)'):
            self.run_probe(probe.accessible, {'owned': True, 'name': 'Launch at login'}, app)

    def test_budget_counts_from_the_fresh_process_start(self):
        # Setup (compositor witness, popup roots) shares the 12 s the child has.
        app, frame, nodes = settings_window()
        with patch.object(probe, 'PROCESS_STARTED', probe.time.monotonic() - probe.WALK_SECONDS - 1), \
             self.assertRaisesRegex(RuntimeError, r'exceeded its 8 s budget after 0 nodes'):
            self.run_probe(probe.accessible, {'owned': True, 'name': 'Launch at login'}, app)

    def test_unowned_applications_are_never_contacted_for_owned_queries(self):
        app, frame, nodes = settings_window()
        busy = Node('gjs-console', 'application', None, pid=77, fail='Timeout was reached')
        rows = self.run_probe(probe.accessible, {'owned': True, 'name': 'Launch at login'}, busy, app)
        self.assertEqual(len(rows), 2)
        self.assertEqual(busy.calls, 0)

    def test_libatspi_calls_get_a_short_fixed_timeout(self):
        calls = []
        atspi = SimpleNamespace(set_timeout=lambda value, startup: calls.append((value, startup)))
        with patch.dict(sys.modules, {'gi.repository.Atspi': atspi}):
            probe.bound_calls()
        self.assertEqual(calls, [(probe.CALL_TIMEOUT_MS, 0)])
        self.assertLess(probe.CALL_TIMEOUT_MS * 3, probe.WALK_SECONDS * 1000)

    def test_timed_out_child_reports_where_it_stalled(self):
        tree = ast.parse(target_module.PROBE_SERVER)
        stalled = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'stalled')
        namespace = {'re': __import__('re')}
        exec(compile(ast.fix_missing_locations(ast.Module([stalled], [])), 'server', 'exec'), namespace)
        dump = (b'Timeout (0:00:11)!\nStack (most recent call first):\n'
                b'  File "<string>", line 456 in extents\n  File "<string>", line 549 in accessible\n'
                b'  File "<string>", line 775 in geometry\n  File "<string>", line 1448 in main\n')
        self.assertEqual(namespace['stalled'](dump), ' in extents:456 <- accessible:549 <- geometry:775 <- main:1448')
        self.assertEqual(namespace['stalled'](None), ' before reporting where it stalled')


class ApplicationNames(Walk):
    def shell_query(self, names, *applications):
        for app in applications:
            app.app = SimpleNamespace(bus_name=':1.%d' % app.pid)
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


class PlacementRefusal(unittest.TestCase):
    def test_refusal_lists_what_the_compositor_shows_for_that_process(self):
        placement = probe.WaylandPlacement(WITNESS)
        placement._windows = [dict(FRAMES['windows'][0], title='Save original file as', frame=[300, 200, 1283, 1001], type=4),
                              dict(FRAMES['windows'][0], id=30, pid=99, title='other')]
        with self.assertRaisesRegex(RuntimeError, r"\(0 matches\); compositor has for pid 50: 'Save original file as' "
                                                  r"\[300, 200, 1283, 1001\] type 4$"):
            placement.toplevel(50, 'Save original file as', [0, 0, 820, 520])


class JustMappedWindow(unittest.TestCase):
    # Observed: Qt reports the History preview dialog as showing while Mutter
    # still lists it as a modal dialog (type 4) with frame [0, 0, 0, 0].
    UNMAPPED = {'windows': [dict(FRAMES['windows'][0]), dict(FRAMES['windows'][0], id=31, title='Preview', type=4,
                                                               frame=[0, 0, 0, 0], focus=False)]}
    MAPPED = {'windows': [dict(FRAMES['windows'][0]), dict(FRAMES['windows'][0], id=31, title='Preview', type=4,
                                                             frame=[200, 100, 932, 600], focus=True)]}

    def test_waits_for_the_compositor_to_map_the_window(self):
        placement = probe.WaylandPlacement(WITNESS)
        with patch.object(probe, 'compositor_windows', side_effect=[self.UNMAPPED, self.UNMAPPED, self.MAPPED]) as asked, \
             patch.object(probe.time, 'sleep'):
            where = placement.toplevel(50, 'Preview', [0, 0, 932, 563])
        self.assertEqual((where['window_id'], where['frame'], asked.call_count), (31, [200, 100, 932, 600], 3))

    def test_still_unmapped_is_refused_by_name_not_guessed(self):
        placement = probe.WaylandPlacement(WITNESS)
        placement.MAP_SETTLE = 0
        with patch.object(probe, 'compositor_windows', return_value=self.UNMAPPED):
            with self.assertRaisesRegex(RuntimeError, r"'Preview' \(pid 50\) is not mapped after 0 s \(empty frame\)"):
                placement.toplevel(50, 'Preview', [0, 0, 932, 563])

    def test_a_missing_window_is_not_waited_for(self):
        placement = probe.WaylandPlacement(WITNESS)
        with patch.object(probe, 'compositor_windows', return_value=FRAMES) as asked:
            with self.assertRaisesRegex(RuntimeError, r'missing or ambiguous \(0 matches\)'):
                placement.toplevel(50, 'Preview', [0, 0, 932, 563])
        self.assertEqual(asked.call_count, 1)


class SilentWitness(unittest.TestCase):
    def test_silent_shell_is_named_not_a_raw_dbus_error(self):
        class DBusException(Exception):
            def get_dbus_name(self):
                return 'org.freedesktop.DBus.Error.NoReply'
        def call(*args, **kwargs):
            raise DBusException('org.freedesktop.DBus.Error.NoReply: Did not receive a reply')
        proxy = SimpleNamespace(Pick=call)
        dbus = SimpleNamespace(DBusException=DBusException, SessionBus=lambda: SimpleNamespace(get_object=lambda *_: proxy))
        with patch.dict(sys.modules, dbus=dbus), \
             self.assertRaisesRegex(RuntimeError, r'^compositor witness \(gnome-shell\) did not answer Pick within 3 s$'):
            probe.compositor_pick(WITNESS, (10, 20))


    def test_a_silent_witness_is_asked_once_per_walk(self):
        placement = probe.WaylandPlacement(WITNESS)
        error = RuntimeError('compositor witness (gnome-shell) did not answer Windows within 3 s')
        with patch.object(probe, 'compositor_windows', side_effect=error) as asked:
            for title in ('put.gd', 'Preview'):
                with self.assertRaisesRegex(RuntimeError, 'did not answer Windows'):
                    placement.toplevel(50, title, [0, 0, 900, 600])
        self.assertEqual(asked.call_count, 1)


class QtAccessibleId(Walk):
    def control(self, selector, candidates=(), **extra):
        app, frame, nodes = settings_window()
        query = {'selector': selector, 'candidates': list(candidates), 'title': '^put.gd$', 'pid': 50,
                 'compositor': WITNESS, **extra}
        return self.run_probe(probe.control_geometry, query, app), nodes

    def test_object_name_comes_from_the_accessible_id(self):
        # #819: Qt 6.10 exports no objectName attribute; label and box share the name.
        row, _ = self.control('launchAtLogin', [{'name': 'launchAtLogin', 'class': 'QCheckBox', 'text': 'Launch at login'}])
        self.assertEqual((row['role'], row['objectName']), ('check box', 'launchAtLogin'))
        self.assertTrue(row['accessibleId'].endswith('.PreferenceValue.launchAtLogin'))
        self.assertEqual(row['rect'], [67 + 968, 32 + 227, 24, 24])

    def test_class_names_and_container_ids_are_not_object_names(self):
        self.assertEqual(probe.id_object_name('A.b.QWidget.QLabel', 'label'), '')
        self.assertEqual(probe.id_object_name('A.b.putgd::Ui::Surface', 'filler'), '')
        self.assertEqual(probe.id_object_name('', 'check box'), '')
        # Observed: the list reports 'Settings categories', its rows the view's path.
        for role in ('list item', 'page tab', 'table cell', 'tree item'):
            self.assertEqual(probe.id_object_name('A.rail.settingsNavigation', role), '')
        self.assertEqual(probe.id_object_name('A.b.PreferenceValue.launchAtLogin', 'check box'), 'launchAtLogin')

    def test_view_rows_need_name_and_role(self):
        # A view row repeats its list's id; 'General' is also the page heading.
        with self.assertRaisesRegex(RuntimeError, r'ambiguous \(2 matches\)'):
            self.control('text:General')
        row, _ = self.control('text:General', role='list item')
        self.assertEqual((row['role'], row['objectName'], row['rect']), ('list item', '', [67 + 16, 32 + 124, 184, 38]))

    def test_control_reads_no_text_or_key_bindings(self):
        app, frame, nodes = settings_window()
        reads = []
        for node in (nodes['box'], nodes['label']):
            node.queryText = lambda reads=reads: reads.append('text')
            node.queryAction = lambda reads=reads: reads.append('action')
        query = {'selector': 'launchAtLogin', 'candidates': [], 'title': '^put.gd$', 'pid': 50, 'compositor': WITNESS}
        row = self.run_probe(probe.control_geometry, query, app)
        self.assertEqual((row['objectName'], reads), ('launchAtLogin', []))


class Target:
    def __init__(self):
        self.queries = []

    def probe(self, name, query, source, timeout=15):
        self.queries.append((name, query))
        return {'ok': True, 'result': {'rect': [1, 2, 3, 4]}}


class LiveControlsProfile(unittest.TestCase):
    def context(self, live):
        environment = {'desktop': 'ubuntu-gnome-wayland', 'host': 'boat', 'placement': 'boat', 'guest': 'gnome-wayland',
                       'package': 'deb', 'destination': 'local', 'build': 'x'}
        profile = dict(load_profile('ubuntu-gnome-wayland'), live_controls=live)
        target = Target()
        ctx = DriverContext(target, environment, profile, ROOT / 'tests', ROOT / 'build')
        return ctx, target

    def test_profile_live_control_queries_exact_name_and_role(self):
        ctx, target = self.context({'settingsSection0': {'name': 'General', 'role': 'list item'}})
        ctx._live_control('settingsSection0', 'settings', {'pid': 50}, [{'name': 'other'}])
        name, query = target.queries[-1]
        self.assertEqual(name, 'control')
        self.assertEqual((query['selector'], query['role'], query['candidates'], query['pid']),
                         ('text:General', 'list item', [], 50))

    def test_other_controls_keep_catalog_candidates(self):
        ctx, target = self.context({'settingsSection0': {'name': 'General', 'role': 'list item'}})
        ctx._live_control('launchAtLogin', 'settings', {'pid': 50}, [{'name': 'launchAtLogin'}])
        query = target.queries[-1][1]
        self.assertEqual((query['selector'], query['candidates']), ('launchAtLogin', [{'name': 'launchAtLogin'}]))
        self.assertNotIn('role', query)

    def test_incomplete_entry_is_blocked_not_guessed(self):
        ctx, target = self.context({'settingsSection0': {'name': 'General'}})
        with self.assertRaisesRegex(RecipeBlocked, 'needs a name and a role'):
            ctx._live_control('settingsSection0', 'settings', {'pid': 50}, [])
        self.assertEqual(target.queries, [])


if __name__ == '__main__':
    unittest.main()
