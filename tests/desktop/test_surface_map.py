"""The surface map inventory still describes this app, and its tool reads receipts right.

Host-only: no desktop, resident, catalog build or network. The inventory
(`tools/lab/surface_map/model.json`) names controls, methods, captures, key
bindings and lab-receipt checks; each of those must still exist in the source it
points at, so a renamed button or check fails here instead of silently leaving a
grey hole in the map.
"""
import fnmatch
import contextlib
import importlib.util
import io
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

    def test_every_planned_builder_row_maps_to_a_desktop(self):
        rows = {(d['id'], r['host'], r['placement']) for d in self.model['desktops'] for r in d['rows']}
        expected = {
            ('mint-cinnamon-x11', 'm74', 'native'), ('fedora-gnome-wayland', 'f44', 'native'),
            ('debian-kde-wayland', 'd69', 'native'), ('kubuntu-kde-wayland', 'k72', 'native'),
            ('kubuntu-kde-x11', 'k72', 'native'), ('aurora-kde-wayland', 'aurora-amd', 'native'),
            ('neon-kde-wayland', 'd69', 'guest')}
        expected.update(('ubuntu-gnome-' + session, host, 'guest')
                        for host in ('m74', 'f44', 'd69', 'k72', 'aurora-amd') for session in ('wayland', 'x11'))
        expected.update(('mint-cinnamon-x11', h, 'guest') for h in ('f44', 'd69', 'k72', 'aurora-amd'))
        expected.update((d, h, 'guest') for d in ('kubuntu-kde-wayland', 'aurora-kde-wayland') for h in ('f44', 'd69'))
        self.assertEqual(rows, expected)
        self.assertFalse(next(d for d in self.model['desktops'] if d['id']=='neon-kde-wayland')['required'])
        self.assertTrue(all('host' not in d and 'notes' in d for d in self.model['desktops']))

    def test_nightly_coverage_glob_includes_numbered_v2_files(self):
        workflow = (ROOT / '.github/workflows/nightly.yml').read_text()
        self.assertIn('tools/lab/surface_map/coverage/[0-9]*.json', workflow)
        self.assertTrue(fnmatch.fnmatchcase('2026-10-29-hosts.json', '[0-9]*.json'))
        self.assertFalse(fnmatch.fnmatchcase('_schema-example.json', '[0-9]*.json'))

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

    def test_host_cells_do_not_overwrite_each_other(self):
        rows = [self.entry('editor.undo', 'ubuntu-gnome-wayland', host=h, placement='guest',
                           guest='gnome', functional=f, at=at)
                for h, f, at in [('m74', 'failed', '2026-10-01'), ('f44', 'passed', '2026-10-02')]]
        result = surface_map.summarise(self.model, rows)['editor.undo']
        self.assertEqual(len(result['cells']), 2)
        self.assertEqual(result['desktops']['ubuntu-gnome-wayland']['functional'], 'failed')
        rows.append(self.entry('editor.undo', 'ubuntu-gnome-wayland', host='m74', placement='guest',
                               guest='gnome', functional='passed', at='2026-10-03'))
        self.assertEqual(surface_map.summarise(self.model, rows)['editor.undo']['desktops']
                         ['ubuntu-gnome-wayland']['functional'], 'passed')
        rows.append(self.entry('editor.undo', 'ubuntu-gnome-wayland', host='m74', placement='guest',
                               guest='another-golden', functional='blocked'))
        self.assertEqual(len(surface_map.summarise(self.model, rows)['editor.undo']['cells']), 3)

    def test_newer_pass_without_run_identity_drops_older_metadata(self):
        older = self.entry('editor.undo', 'ubuntu-gnome-wayland', host='f44',
                           placement='guest', functional='failed', build='failing-build',
                           recipe='undo', frames=[{'label': 'after', 'path': 'failed.png'}],
                           at='2026-10-01')
        newer = self.entry('editor.undo', 'ubuntu-gnome-wayland', host='f44',
                           placement='guest', functional='passed', at='2026-10-02')
        for missing in ({}, {k: None for k in ('frames', 'recipe', 'build')}):
            with self.subTest(missing=missing):
                cell = surface_map.summarise(self.model, [older, {**newer, **missing}])['editor.undo']['cells'][0]
                self.assertEqual(cell['functional'], 'passed')
                for field in ('frames', 'recipe', 'build'):
                    self.assertNotIn(field, cell)
        replacement = {**newer, 'frames': [], 'recipe': 'redo', 'build': 'passing-build'}
        cell = surface_map.summarise(self.model, [replacement, older])['editor.undo']['cells'][0]
        for field in ('frames', 'recipe', 'build'):
            self.assertEqual(cell[field], replacement[field])

    def test_newer_verdict_clears_old_review_queue_and_reason(self):
        older = self.entry('editor.undo', 'ubuntu-gnome-wayland', host='f44',
                           placement='guest', functional='failed', review=True,
                           review_reason='Inspect failing frame', build='abc123',
                           receipt='run.json', at='2026-10-01')
        for verdict in ([{'functional': f} for f in surface_map.FUNCTIONAL] +
                        [{'visual': v} for v in ('reviewed-ok', 'reviewed-wrong')] + [{'review': False}]):
            for identity in ({}, {'build': 'abc123'}, {'receipt': 'run.json'}):
                with self.subTest(verdict=verdict, identity=identity):
                    newer = self.entry('editor.undo', 'ubuntu-gnome-wayland', host='f44',
                                       placement='guest', at='2026-10-02', **verdict, **identity)
                    entries = [newer, older]  # Deliberately unsorted input.
                    cell = surface_map.summarise(self.model, entries)['editor.undo']['cells'][0]
                    self.assertFalse(cell.get('review'))
                    self.assertNotIn('review_reason', cell)
                    data = surface_map.build_page_data(self.model, entries, None, '.')
                    self.assertEqual(data['reviews'], [])
        newer.update(review=True, review_reason='Inspect current run')
        data = surface_map.build_page_data(self.model, [older, newer], None, '.')
        self.assertEqual(len(data['reviews']), 1)
        self.assertEqual(data['reviews'][0]['review_reason'], 'Inspect current run')

    def test_same_run_preserves_metadata_when_newest_values_are_missing_or_none(self):
        older = self.entry('editor.undo', 'ubuntu-gnome-wayland', host='f44',
                           placement='guest', functional='failed', review=True,
                           review_reason='Inspect frame', receipt='run.json',
                           frames=[{'label': 'after', 'path': 'after.png'}],
                           recipe='undo', build='abc123', at='2026-10-01')
        for identity in ({'build': 'abc123'}, {'receipt': 'run.json'}):
            for missing in ({}, {k: None for k in ('frames', 'recipe', 'build')}):
                with self.subTest(identity=identity, missing=missing):
                    newer = self.entry('editor.undo', 'ubuntu-gnome-wayland', host='f44',
                                       placement='guest', functional='passed', at='2026-10-02')
                    newer.update(missing)
                    newer.update(identity)
                    cell = surface_map.summarise(self.model, [older, newer])['editor.undo']['cells'][0]
                    self.assertEqual(cell['functional'], 'passed')
                    for field in ('frames', 'recipe', 'build'):
                        self.assertEqual(cell[field], older[field])
        metadata_only = {**newer, 'functional': None}
        data = surface_map.build_page_data(self.model, [older, metadata_only], None, '.')
        self.assertEqual(len(data['reviews']), 1)
        self.assertEqual(data['reviews'][0]['review_reason'], older['review_reason'])
        self.assertEqual(data['reviews'][0]['frames'], older['frames'])

    def test_same_run_frames_alone_preserve_review_request(self):
        older = self.entry('editor.undo', 'mint-cinnamon-x11', host='m74', placement='native',
                           review=True, review_reason='Inspect undo', receipt='run.json',
                           at='2026-10-01')
        for update in ({'visual': 'frames'}, {'visual': 'frames', 'review_reason': 'Inspect new frames'}):
            with self.subTest(update=update):
                newer = self.entry('editor.undo', 'mint-cinnamon-x11', host='m74', placement='native',
                                   receipt='run.json', at='2026-10-02', **update)
                data = surface_map.build_page_data(self.model, [newer, older], None, '.')
                self.assertEqual(len(data['reviews']), 1)
                self.assertEqual(data['reviews'][0]['review_reason'], update.get('review_reason', 'Inspect undo'))

    def test_missing_host_dimension_does_not_count_as_passed(self):
        rows = [self.entry('editor.undo', 'ubuntu-gnome-wayland', host='m74', placement='guest',
                           functional='passed', visual='reviewed-ok'),
                self.entry('editor.undo', 'ubuntu-gnome-wayland', host='f44', placement='guest',
                           visual='reviewed-ok')]
        c = surface_map.summarise(self.model, rows)['editor.undo']['desktops']['ubuntu-gnome-wayland']
        self.assertEqual((c['functional'], c['visual']), ('partial', 'reviewed-ok'))

    def test_mixed_legacy_and_host_rows_remain_available(self):
        rows = [self.entry('editor.undo', 'mint-cinnamon-x11', functional='passed'),
                self.entry('editor.redo', 'ubuntu-gnome-wayland', host='f44', placement='guest', functional='passed')]
        cells = surface_map.summarise(self.model, rows)['editor.undo']['cells']
        self.assertEqual(len(cells), 1)
        self.assertIsNone(cells[0]['host'])
        self.assertEqual(cells[0]['functional'], 'passed')

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

    def test_v1_coverage_preserves_desktop_summary(self):
        path = self.receipt('v1.json', {'schemaVersion': 1, 'campaign': 'legacy', 'entries': [
            {'interaction': 'editor.undo', 'desktop': 'mint-cinnamon-x11', 'package': 'deb',
             'functional': 'passed', 'visual': 'frames', 'at': '2026-10-01'}]})
        loaded = surface_map.load_coverage([path])
        result = surface_map.summarise(self.model, loaded)['editor.undo']
        self.assertNotIn('cells', result)
        self.assertEqual(result['desktops']['mint-cinnamon-x11'], {
            'functional': 'passed', 'visual': 'frames', 'packages': ['deb'],
            'receipts': [], 'issues': [], 'notes': []})
        self.assertNotIn('host', loaded[0])

    def test_load_infers_unique_native_placement(self):
        path = self.receipt('host-only.json', {'schemaVersion': 2, 'entries': [
            {'interaction': 'editor.undo', 'desktop': 'mint-cinnamon-x11', 'package': 'deb',
             'functional': 'passed', 'host': 'm74'}]})
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            loaded = surface_map.load_coverage([path], self.model)
        self.assertEqual(error.getvalue(), '')
        self.assertEqual(loaded[0]['placement'], 'native')
        data = surface_map.build_page_data(self.model, loaded, None, self.tmp.name)
        cell = next(i for i in data['interactions'] if i['id'] == 'editor.undo')['cells'][0]
        self.assertEqual((cell['host'], cell['placement'], cell['functional']), ('m74', 'native', 'passed'))
        self.assertEqual(surface_map.main(['build', '--coverage', str(path),
                                          '--output', str(Path(self.tmp.name) / 'native')]), 0)

    def test_load_unresolved_placement_stays_in_desktop_rollup_only(self):
        for native_count in (0, 2):
            with self.subTest(native_count=native_count):
                model = json.loads(json.dumps(self.model))
                desktop = next(d for d in model['desktops'] if d['id'] == 'mint-cinnamon-x11')
                desktop['rows'] = [{'host': 'm74', 'placement': 'native'}] * native_count
                path = self.receipt('unresolved.json', {'schemaVersion': 2, 'entries': [
                    {'interaction': 'editor.undo', 'desktop': desktop['id'], 'package': 'deb',
                     'functional': 'failed', 'host': 'm74'}]})
                error = io.StringIO()
                with contextlib.redirect_stderr(error):
                    loaded = surface_map.load_coverage([path], model)
                warning = error.getvalue().splitlines()
                self.assertEqual(len(warning), 1)
                for text in (str(path), 'entry 1', 'editor.undo', desktop['id'], 'm74', 'desktop rollup only'):
                    self.assertIn(text, warning[0])
                data = surface_map.build_page_data(model, loaded, None, self.tmp.name)
                item = next(i for i in data['interactions'] if i['id'] == 'editor.undo')
                self.assertEqual(item['desktops'][desktop['id']]['functional'], 'failed')
                self.assertNotIn('cells', item)
                self.assertFalse(data['hostCoverage'])
                self.assertTrue(all(r['placement'] for r in data['rows']))
                # A known builder pass must not hide an unresolved historical failure.
                known = surface_map.make_entry('editor.undo', desktop['id'], 'deb', host='f44',
                                               placement='guest', functional='passed', at='2099-01-01')
                item = surface_map.summarise(model, loaded + [known])['editor.undo']
                self.assertEqual(item['desktops'][desktop['id']]['functional'], 'failed')
                self.assertEqual(len(item['cells']), 1)
                self.assertEqual(item['cells'][0]['host'], 'f44')
                model_path = self.receipt('model.json', model)
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(surface_map.main(['--model', str(model_path), 'build', '--coverage', str(path),
                        '--output', str(Path(self.tmp.name) / 'unresolved')]), 0)

    def test_mark_and_import_still_reject_host_without_placement(self):
        coverage = Path(self.tmp.name) / 'invalid.json'
        receipt = self.receipt('driver-host-only.json', {'interactions': [
            {'interaction': 'editor.undo', 'desktop': 'mint-cinnamon-x11', 'package': 'deb',
             'functional': 'passed', 'host': 'm74'}]})
        for command in ([
                'mark', 'editor.undo', '--desktop', 'mint-cinnamon-x11', '--package', 'deb',
                '--functional', 'passed', '--host', 'm74'], ['import', str(receipt)]):
            with self.subTest(command=command):
                error = io.StringIO()
                with contextlib.redirect_stderr(error):
                    result = surface_map.main(command + ['--coverage', str(coverage)])
                self.assertEqual(result, 2)
                self.assertIn('host requires placement', error.getvalue())
                self.assertFalse(coverage.exists())

    def test_mark_frames_with_same_receipt_keeps_review_queue(self):
        coverage = Path(self.tmp.name) / 'review.json'
        flags = ['--coverage', str(coverage), '--desktop', 'mint-cinnamon-x11',
                 '--package', 'deb', '--host', 'm74', '--placement', 'native', '--receipt', 'run.json']
        for update in (['--review-reason', 'Inspect undo'], ['--visual', 'frames']):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(surface_map.main(['mark', 'editor.undo'] + flags + update), 0)
        data = surface_map.build_page_data(self.model, surface_map.load_coverage([coverage]), None, self.tmp.name)
        self.assertEqual(len(data['reviews']), 1)
        self.assertEqual(data['reviews'][0]['review_reason'], 'Inspect undo')

    def test_v2_driver_import_and_host_filter_review_data(self):
        row = {'interaction': 'editor.undo', 'desktop': 'ubuntu-gnome-wayland', 'package': 'source',
               'functional': 'passed', 'visual': 'frames', 'host': 'm74', 'placement': 'guest',
               'guest': 'gnome-provisioned-20261016', 'destination': 'proton', 'review': True,
               'review_reason': 'Compare before and after', 'recipe': 'editor.undo', 'build': 'abc123',
               'at': '2026-10-29T00:00:00Z', 'receipt': 'work/synthetic/receipt.json',
               'frames': [{'label': 'before', 'path': 'frames/before.png'},
                          {'label': 'after', 'path': 'frames/after.png', 'reference': '26-editor-selected',
                           'distance': 0.12, 'tolerance': 0.05}]}
        receipt = self.receipt('driver.json', {'schemaVersion': 2, 'interactions': [row, {**row, 'host': 'f44'}]})
        entries = surface_map.import_receipt(self.model, receipt)
        for k, v in row.items():
            self.assertEqual(entries[0][k], v)
        path = Path(self.tmp.name) / 'coverage.json'
        surface_map.write_coverage(path, 'v2', entries)
        self.assertEqual(json.loads(path.read_text())['schemaVersion'], 2)
        loaded = surface_map.load_coverage([path])
        data = surface_map.build_page_data(self.model, loaded, None, self.tmp.name)
        cells = next(i for i in data['interactions'] if i['id']=='editor.undo')['cells']
        self.assertEqual({c['host'] for c in cells}, {'m74', 'f44'})
        self.assertEqual(data['hosts'], list(surface_map.HOSTS))
        self.assertTrue(data['hostCoverage'])
        self.assertEqual(len(data['reviews']), 2)
        self.assertEqual(data['reviews'][0]['frames'], row['frames'])
        page = surface_map.render(data)
        self.assertIn('aria-label="Host filter"', page)
        self.assertIn('function renderReview', page)
        self.assertIn('frames/after.png', page)
        self.assertIn('Compare before and after', page)

    def test_unnamed_guest_failure_keeps_its_row_beside_named_pass(self):
        desktop = 'ubuntu-gnome-wayland'
        unnamed = surface_map.make_entry('editor.undo', desktop, 'deb', host='f44',
                                        placement='guest', functional='failed', at='2026-10-01')
        named = surface_map.make_entry('editor.undo', desktop, 'deb', host='f44',
                                      placement='guest', guest='golden', functional='passed', at='2026-10-02')
        # Round-trip existing coverage as well as newly created entries.
        path = self.receipt('existing.json', {'schemaVersion': 2, 'entries': [unnamed, named]})
        data = surface_map.build_page_data(self.model, surface_map.load_coverage([path]), None, self.tmp.name)
        rows = [r for r in data['rows'] if r['desktop']==desktop and r['host']=='f44']
        self.assertEqual({r['guest'] for r in rows}, {None, 'golden'})
        item = next(i for i in data['interactions'] if i['id']=='editor.undo')
        cells = {surface_map.cell_key(c): c for c in item['cells']}
        self.assertEqual({cells[surface_map.cell_key(r)]['functional'] for r in rows}, {'failed', 'passed'})
        self.assertEqual(item['desktops'][desktop]['functional'], 'failed')
        data = surface_map.build_page_data(self.model, [named], None, self.tmp.name)
        rows = [r for r in data['rows'] if r['desktop']==desktop and r['host']=='f44']
        self.assertEqual([r['guest'] for r in rows], ['golden'])

    def test_host_rows_preserve_required_for_planned_and_observed_cells(self):
        optional = surface_map.make_entry('editor.undo', 'neon-kde-wayland', 'deb', host='d69',
                                         placement='guest', guest='neon', functional='failed')
        data = surface_map.build_page_data(self.model, [optional], None, self.tmp.name)
        required = {d['id']: d.get('required', True) for d in self.model['desktops']}
        for row in data['rows']:
            with self.subTest(row=row):
                self.assertEqual(row['required'], required[row['desktop']])
        self.assertTrue(any(not r['required'] and r['guest']=='neon' for r in data['rows']))
        self.assertTrue(any(r['required'] and r['host']=='d69' for r in data['rows']))

    def test_native_placement_rejects_guest_in_entry_and_mark(self):
        import contextlib, io
        with self.assertRaisesRegex(surface_map.ModelError, 'guest cannot be used with native placement'):
            surface_map.make_entry('editor.undo', 'mint-cinnamon-x11', 'deb',
                                   host='m74', placement='native', guest='golden')
        path = Path(self.tmp.name) / 'invalid-mark.json'
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            result = surface_map.main(['mark', 'editor.undo', '--coverage', str(path),
                '--desktop', 'mint-cinnamon-x11', '--package', 'deb', '--functional', 'passed',
                '--host', 'm74', '--placement', 'native', '--guest', 'golden'])
        self.assertEqual(result, 2)
        self.assertIn('guest cannot be used with native placement', error.getvalue())
        self.assertFalse(path.exists())

    def test_old_driver_rows_still_import_with_cli_defaults(self):
        path = self.receipt('old-driver.json', {'interactions': [
            {'interaction': 'editor.undo', 'functional': 'passed'}]})
        entries = surface_map.import_receipt(self.model, path, 'mint-cinnamon-x11', 'deb')
        self.assertEqual(entries[0]['desktop'], 'mint-cinnamon-x11')
        self.assertNotIn('host', entries[0])

    def test_mark_v2_flags_and_review_only(self):
        path = Path(self.tmp.name) / 'mark.json'
        self.assertEqual(surface_map.main(['mark', 'editor.undo', '--coverage', str(path),
            '--desktop', 'aurora-kde-wayland', '--package', 'source', '--host', 'aurora-amd',
            '--placement', 'native', '--destination', 'local', '--review-reason', 'Desktop drawn']), 0)
        row = surface_map.load_coverage([path])[0]
        self.assertTrue(row['review'])
        self.assertEqual(row['host'], 'aurora-amd')

    def test_invalid_v2_metadata_rejected(self):
        for metadata in ({'host': 'unknown'}, {'host': 'm74'}, {'host': 'm74', 'placement': None},
                         {'placement': 'VM'}, {'destination': 'ftp'},
                         {'review': 'true'}, {'frames': ['bad']}):
            with self.subTest(metadata=metadata), self.assertRaises(surface_map.ModelError):
                surface_map.make_entry('editor.undo', 'mint-cinnamon-x11', 'deb', **metadata)

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
