"""The surface map inventory still describes this app, and its tool reads receipts right.

Host-only: no desktop, resident, catalog build or network. The inventory
(`tools/lab/surface_map/model.json`) names controls, methods, captures, key
bindings and lab-receipt checks; each of those must still exist in the source it
points at, so a renamed button or check fails here instead of silently leaving a
grey hole in the map.
"""
import fnmatch
import importlib.util
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'tools/lab/surface_map'
spec = importlib.util.spec_from_file_location('surface_map', TOOL / 'surface_map.py')
surface_map = importlib.util.module_from_spec(spec)
spec.loader.exec_module(surface_map)

KEY_TOKEN = re.compile(r"^(?:(?:Ctrl|Shift|Alt|Meta)\+)*(?:[A-Z0-9]|F\d{1,2}|Esc|Return|Enter|Tab|Space|Backspace|Delete|Del|Insert|Home|End|PageUp|PageDown|Left|Right|Up|Down|Arrows|Menu|Ctrl|[\[\]=+\-])$")


def literals(*paths):
    """Every C++ or Python string literal in the given source files."""
    found = set()
    for path in paths:
        text = (ROOT / path).read_text(encoding='utf-8')
        found.update(re.findall(r'"((?:[^"\\\n]|\\.)*)"', text))
        found.update(re.findall(r"'((?:[^'\\\n]|\\.)*)'", text))
    return found


def named_in(name, pool):
    """`name` is a literal, or is built from two or three literals (\"record\"+title(action)+\"Shortcut\")."""
    if name in pool:
        return True
    for prefix in pool:
        if prefix and name.startswith(prefix) and len(prefix) < len(name):
            rest = name[len(prefix):]
            # Qt-style title casing: "s3" + title("objectPrefix") -> s3ObjectPrefix.
            for candidate in {rest, rest[0].lower() + rest[1:]}:
                if candidate in pool or any(candidate.endswith(suffix) and candidate[:-len(suffix)] in pool
                                            for suffix in pool if suffix and len(suffix) < len(candidate)):
                    return True
    return False


def source_files(*roots):
    for root in roots:
        for path in sorted((ROOT / root).rglob('*')):
            if path.suffix in ('.cpp', '.h', '.py', '.js') and path.is_file():
                yield path.relative_to(ROOT).as_posix()


class ModelContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = surface_map.load_model()
        cls.src = literals(*source_files('src'))
        cls.catalog = (ROOT / 'tests/gui/ui_catalog.cpp').read_text(encoding='utf-8')
        # Synthetic states (the tray's upload menu) are worded in the catalog, not the app.
        cls.labels = cls.src | literals('tests/gui/ui_catalog.cpp')
        cls.server = literals('src/app/LocalServer.cpp')

    def test_sources_exist(self):
        missing = sorted({i['source'] for i in self.model['interactions'] if 'source' in i and not (ROOT / i['source']).exists()})
        self.assertEqual(missing, [])

    def test_captures_are_catalog_states(self):
        for surface in self.model['surfaces']:
            capture = surface.get('capture')
            if capture:
                self.assertTrue('"%s"' % capture in self.catalog or '"%s.png"' % capture in self.catalog,
                                'capture %s is not a ui_catalog state' % capture)

    def test_control_selectors_exist_in_source(self):
        for item in self.model['interactions']:
            for selector in surface_map.selectors(item):
                with self.subTest(interaction=item['id'], selector=selector):
                    if selector.startswith('text:'):
                        label = selector[5:]
                        self.assertTrue(label in self.labels or label.replace('&', '&&') in self.labels,
                                        'label not in src: ' + label)
                    elif selector.startswith('glob:'):
                        parts = [part for part in selector[5:].split('*') if part and not set(part) & set('?[]')]
                        self.assertTrue(any(fnmatch.fnmatchcase(n, selector[5:]) for n in self.src)
                                        or all(part in self.src for part in parts), 'no source literal matches ' + selector)
                    elif selector.startswith(('class:', 'text-glob:')):
                        pass  # runtime-built labels and Qt class names have no source literal to hold them to
                    else:
                        self.assertTrue(named_in(selector, self.src), 'objectName not in src: ' + selector)

    def test_methods_are_dispatched(self):
        for item in self.model['interactions']:
            method = item.get('method')
            if not method:
                continue
            with self.subTest(interaction=item['id'], method=method):
                if method.endswith('.*'):
                    prefix = method[:-1]
                    self.assertTrue(any(l.startswith(prefix) for l in self.server), 'no method starts with ' + prefix)
                else:
                    self.assertIn(method, self.server)

    def test_keys_are_well_formed(self):
        for item in self.model['interactions']:
            for key in item.get('keys', []):
                with self.subTest(interaction=item['id'], key=key):
                    self.assertRegex(key, KEY_TOKEN)

    def test_receipt_selectors_name_real_checks(self):
        receipts = self.model['receipts']
        edit = literals('tools/lab/edit_acceptance.py')
        mcp = literals('tools/lab/mcp_editor_e2e.py')
        lab = literals('tools/lab/lab.py')
        tray = literals('tools/lab/tray_progress.py')
        native = literals('tools/lab/native_acceptance.py')
        for selector in receipts.get('edit_acceptance', {}):
            self.assertIn(selector.split('.', 1)[1], edit, selector)
        for selector in receipts.get('mcp_editor_e2e', {}):
            self.assertIn(selector.split('.', 1)[1], mcp, selector)
        for selector in receipts.get('lab_run', {}):
            self.assertIn(selector.split(':', 1)[1], lab, selector)
        for selector in receipts.get('tray_progress', {}):
            name = selector.split(':', 1)[-1]
            self.assertIn(name, tray, selector)
        for selector in receipts.get('native_acceptance', {}):
            for token in selector.split(':', 1)[1].split():
                self.assertIn(token, native, selector)

    def test_every_interaction_has_a_surface_card_home(self):
        kinds = {s['id']: s['kind'] for s in self.model['surfaces']}
        for item in self.model['interactions']:
            if kinds[item['surface']] == 'channel':
                self.assertIn(item['kind'], ('shortcut', 'cli', 'mcp', 'event'), item['id'])


class Aggregation(unittest.TestCase):
    def setUp(self):
        self.model = surface_map.load_model()
        self.required = [d['id'] for d in self.model['desktops'] if d.get('required', True)]

    def entry(self, interaction, desktop, **kw):
        return surface_map.make_entry(interaction, desktop, kw.pop('package', 'appimage'), **kw)

    def test_untested_without_entries(self):
        summary = surface_map.summarise(self.model, [])
        self.assertEqual(summary['editor.undo'], {'functional': 'untested', 'visual': 'none', 'desktops': {}})
        self.assertEqual(summary['cli.version']['visual'], 'n/a')

    def test_partial_until_every_required_desktop_passes(self):
        entries = [self.entry('editor.undo', d, functional='passed', visual='reviewed-ok') for d in self.required[:-1]]
        summary = surface_map.summarise(self.model, entries)
        self.assertEqual((summary['editor.undo']['functional'], summary['editor.undo']['visual']), ('partial', 'frames'))
        entries.append(self.entry('editor.undo', self.required[-1], functional='passed', visual='reviewed-ok'))
        summary = surface_map.summarise(self.model, entries)
        self.assertEqual((summary['editor.undo']['functional'], summary['editor.undo']['visual']), ('passed', 'reviewed-ok'))

    def test_failure_anywhere_wins_and_newest_entry_counts(self):
        entries = [self.entry('editor.undo', d, functional='passed') for d in self.required]
        entries.append(self.entry('editor.undo', self.required[0], functional='failed', at='2099-01-01T00:00:00Z'))
        self.assertEqual(surface_map.summarise(self.model, entries)['editor.undo']['functional'], 'failed')
        entries.append(self.entry('editor.undo', self.required[0], functional='passed', at='2099-01-02T00:00:00Z'))
        self.assertEqual(surface_map.summarise(self.model, entries)['editor.undo']['functional'], 'passed')

    def test_a_desktop_with_only_one_dimension_is_missing_the_other(self):
        entries = [self.entry('editor.undo', self.required[0], functional='passed', visual='reviewed-ok')]
        entries += [self.entry('editor.undo', d, visual='reviewed-ok') for d in self.required[1:]]
        summary = surface_map.summarise(self.model, entries)['editor.undo']
        self.assertEqual((summary['functional'], summary['visual']), ('partial', 'reviewed-ok'))
        entries = [self.entry('editor.undo', d, functional='passed') for d in self.required]
        entries.append(self.entry('editor.undo', self.required[0], visual='reviewed-ok'))
        self.assertEqual(surface_map.summarise(self.model, entries)['editor.undo']['visual'], 'frames')

    def test_optional_desktops_do_not_gate(self):
        optional = [d['id'] for d in self.model['desktops'] if not d.get('required', True)]
        self.assertTrue(optional)
        entries = [self.entry('editor.undo', d, functional='passed', visual='reviewed-ok') for d in self.required]
        entries.append(self.entry('editor.undo', optional[0], functional='failed'))
        self.assertEqual(surface_map.summarise(self.model, entries)['editor.undo']['functional'], 'passed')

    def test_mark_rejects_unknown_values(self):
        with self.assertRaises(surface_map.ModelError):
            surface_map.make_entry('editor.undo', 'mint-cinnamon-x11', 'snap', functional='passed')
        with self.assertRaises(surface_map.ModelError):
            surface_map.make_entry('editor.undo', 'mint-cinnamon-x11', 'deb', functional='maybe')


class ReceiptImport(unittest.TestCase):
    def setUp(self):
        self.model = surface_map.load_model()
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def receipt(self, name, doc):
        path = Path(self.tmp.name) / name
        path.write_text(json.dumps(doc), encoding='utf-8')
        return path

    def test_edit_acceptance_checks_map_to_editor_interactions(self):
        path = self.receipt('results.json', {
            'schemaVersion': 1, 'scenario': 'edit-region-independent-paste', 'ok': False,
            'checks': {'keyboardUndo': {'ok': True}, 'keyboardRedo': {'ok': False, 'detail': 'x'}, 'unknownCheck': {'ok': True}},
            'witnesses': [{'path': 'a.ppm', 'sha256': '0'}]})
        entries = surface_map.import_receipt(self.model, path, 'mint-cinnamon-x11', 'deb')
        by = {(e['interaction'], e['functional']) for e in entries}
        self.assertIn(('editor.undo', 'passed'), by)
        self.assertIn(('editor.redo', 'failed'), by)
        self.assertTrue(all(e['visual'] == 'frames' and e['source'] == 'edit_acceptance' for e in entries))

    def test_lab_run_scenarios(self):
        path = self.receipt('results.json', {'schemaVersion': 1, 'testId': 't', 'profile': 'cinnamon-x11',
                                             'scenario': 'region-cancel-escape-independent-paste', 'ok': True, 'witness': {'path': 'final-head0.ppm'}})
        entries = surface_map.import_receipt(self.model, path, 'mint-cinnamon-x11', 'deb')
        self.assertEqual({e['interaction'] for e in entries}, {'region.key-esc'})
        self.assertEqual(entries[0]['visual'], 'frames')

    def test_tray_progress_coverage_and_protocol(self):
        path = self.receipt('results.json', {'schemaVersion': 1, 'protocolPassed': True,
                                             'coverage': {'queue': {}, 'cancel': {}, 'local-failure': {}}, 'witnesses': []})
        entries = surface_map.import_receipt(self.model, path, 'fedora-gnome-wayland', 'rpm')
        ids = {e['interaction'] for e in entries}
        self.assertTrue({'tray.icon-uploading', 'tray.menu-cancel-upload', 'tray.icon-attention'} <= ids)
        self.assertTrue(all(e['functional'] == 'passed' for e in entries))

    def test_native_acceptance_command(self):
        path = self.receipt('tray.json', {'driverSha256': 'x', 'host': 'h', 'at': '2026-10-29T00:00:00Z',
                                          'command': 'tray', 'ok': True, 'result': {'action': 'quit'}})
        entries = surface_map.import_receipt(self.model, path, 'debian-kde-wayland', 'appimage')
        self.assertEqual([e['interaction'] for e in entries], ['tray.menu-quit'])

    def test_unknown_receipt_is_refused(self):
        path = self.receipt('other.json', {'hello': 'world'})
        with self.assertRaises(surface_map.ModelError):
            surface_map.import_receipt(self.model, path, 'debian-kde-wayland', 'appimage')

    def test_coverage_round_trip_and_page(self):
        coverage = Path(self.tmp.name) / 'coverage.json'
        entry = surface_map.make_entry('editor.undo', 'mint-cinnamon-x11', 'deb', functional='passed', visual='frames')
        surface_map.write_coverage(coverage, 'unit', [entry])
        loaded = surface_map.load_coverage([coverage])
        self.assertEqual(loaded[0]['campaign'], 'unit')
        out = Path(self.tmp.name) / 'out'
        data = surface_map.build_page_data(self.model, loaded, None, out)
        page = surface_map.render(data)
        self.assertNotIn('/*__DATA__*/null', page)
        self.assertIn('"editor.undo"', page)
        self.assertEqual(data['counts']['partial'], 1)
        self.assertEqual(data['counts']['total'], len(self.model['interactions']))


class CatalogCheck(unittest.TestCase):
    def run_check(self, directory):
        import contextlib, io
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return surface_map.main(['check', '--catalog', str(directory)])

    def catalog(self, root, platform, skip=()):
        model = surface_map.load_model()
        captures = []
        for surface in model['surfaces']:
            if surface.get('capture') and surface['capture'] not in skip:
                (root / (surface['capture'] + '.png')).write_bytes(b'png')
                entry = {'file': surface['capture'] + '.png'}
                # The region selector capture has no control selectors to validate, so an
                # empty sidecar is a valid declared one.
                if surface['capture'] == '44-region-keyboard':
                    (root / '44-region-keyboard.controls.json').write_text(
                        json.dumps({'width': 1, 'height': 1, 'controls': [], 'keys': []}), encoding='utf-8')
                    entry['controls'] = '44-region-keyboard.controls.json'
                captures.append(entry)
        (root / 'manifest.json').write_text(json.dumps({'kind': 'synthetic-ui-catalog', 'platform': platform,
                                                        'captures': captures}), encoding='utf-8')

    def test_missing_or_empty_catalog_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.run_check(Path(tmp) / 'absent'), 1)
            self.assertEqual(self.run_check(tmp), 1)

    def test_declared_controls_sidecar_must_exist(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.catalog(Path(tmp), 'offscreen')
            self.assertEqual(self.run_check(tmp), 0)
            (Path(tmp) / '44-region-keyboard.controls.json').unlink()
            self.assertEqual(self.run_check(tmp), 1)

    def test_only_documented_captures_may_be_absent_natively(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.catalog(Path(tmp), 'xcb', skip={'00-tray-upload-status'})
            self.assertEqual(self.run_check(tmp), 0)
        with tempfile.TemporaryDirectory() as tmp:
            self.catalog(Path(tmp), 'offscreen', skip={'00-tray-upload-status'})
            self.assertEqual(self.run_check(tmp), 1)
        with tempfile.TemporaryDirectory() as tmp:
            self.catalog(Path(tmp), 'xcb', skip={'about'})
            self.assertEqual(self.run_check(tmp), 1)


class ControlMatching(unittest.TestCase):
    def test_selectors_and_labels(self):
        controls = {'width': 10, 'height': 10, 'controls': [
            {'name': 'clearRegionShortcut', 'kind': 'button', 'text': '', 'rect': [0, 0, 1, 1]},
            {'name': '', 'kind': 'button', 'text': 'Capture region && edit', 'rect': [0, 0, 1, 1]},
            {'name': 'x', 'kind': 'choice', 'text': 'Medium', 'rect': [0, 0, 1, 1]}],
            'keys': [{'keys': ['Ctrl+Z', 'Undo', 'Alt+Backspace']}, {'keys': ['Enter']}, {'keys': ['Ctrl+Q']}]}
        self.assertTrue(surface_map.match_control(controls, 'glob:clear*Shortcut'))
        self.assertTrue(surface_map.match_control(controls, 'text:Capture region & edit'))
        self.assertIsNone(surface_map.match_control(controls, 'text:Capture region && edit'))
        items = [{'id': 'a', 'control': ['glob:clear*Shortcut', 'text:Capture region & edit'], 'keys': ['Ctrl+Z', 'Return']}]
        missing_controls, missing_keys = surface_map.unmodelled(controls, items)
        self.assertEqual([c['name'] for c in missing_controls], ['x'])
        self.assertEqual([k['keys'] for k in missing_keys], ['Ctrl+Q'])


if __name__ == '__main__':
    unittest.main()
