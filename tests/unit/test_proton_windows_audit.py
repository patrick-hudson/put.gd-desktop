"""Windows Proton runtime inventory and source catalog checks (#408), with a synthetic helper."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
sys.path.insert(0, str(ROOT / 'tests/releases'))
from audit_proton import WINDOWS_REDISTRIBUTION_FILES, audit_proton_windows, windows_catalog_mismatches  # noqa: E402
import proton_windows_fixture as fixture  # noqa: E402

NOTICES = ROOT / 'tools/proton/notices'


def provisioner():
    sys.path.insert(0, str(ROOT / 'tools/proton'))
    spec = importlib.util.spec_from_file_location('provision_runtime', ROOT / 'tools/proton/provision_runtime.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WindowsProtonAuditTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pg-proton-win-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'put.gd'
        self.manifest = fixture.write_runtime(self.root / 'proton', WINDOWS_REDISTRIBUTION_FILES)
        self.linux = json.loads((NOTICES / 'SOURCES.json').read_text())

    def audit(self, **options):
        return audit_proton_windows(self.root, required=True, linux_catalog=self.linux, **options)

    def test_complete_folder_binds_helper_notices_and_catalog(self):
        result = self.audit(expected_manifest=self.manifest)
        self.assertEqual(result['helper'], 'proton/putgd-proton-runtime.exe')
        self.assertEqual(result['runtimeSha256'], self.manifest['sha256'])
        self.assertEqual(result['redistributionFiles'], 16)

    def test_absent_runtime_is_allowed_unless_required(self):
        shutil.rmtree(self.root / 'proton')
        self.assertIsNone(audit_proton_windows(self.root))
        with self.assertRaisesRegex(ValueError, 'Required Proton'):
            audit_proton_windows(self.root, required=True)

    def test_incomplete_manifest_is_not_redistributable(self):
        manifest = dict(self.manifest, redistributionComplete=False, redistributionNote='Not for distribution: gap')
        fixture.save_manifest(self.root / 'proton', manifest)
        with self.assertRaisesRegex(ValueError, 'gap'):
            self.audit()

    def test_linux_manifest_is_refused(self):
        fixture.save_manifest(self.root / 'proton', dict(self.manifest, platform='linux-x64'))
        with self.assertRaisesRegex(ValueError, 'windows-x64'):
            self.audit()

    def test_changed_helper_or_notice_is_refused(self):
        for path in ('putgd-proton-runtime.exe', 'THIRD-PARTY-NOTICES.txt', 'redistribution/LICENSE.icu4c-78.3'):
            with self.subTest(path=path):
                target = self.root / 'proton' / path
                original = target.read_bytes()
                target.write_bytes(original + b'changed')
                with self.assertRaisesRegex(ValueError, 'digest mismatch'):
                    self.audit()
                target.write_bytes(original)

    def test_missing_or_extra_files_are_refused(self):
        (self.root / 'proton/stdout-fixture-test.exe').write_bytes(b'MZ')
        with self.assertRaisesRegex(ValueError, 'Unexpected Proton files'):
            self.audit()
        (self.root / 'proton/stdout-fixture-test.exe').unlink()
        (self.root / 'proton/redistribution/REBUILD-WINDOWS.md').unlink()
        with self.assertRaisesRegex(ValueError, 'Required Proton'):
            self.audit()

    def test_catalog_must_pin_this_helper_and_bun(self):
        catalog_path = self.root / 'proton/redistribution/SOURCES-WINDOWS.json'
        for field, value in (('helperSha256', '0' * 64), ('bunTargetRuntime', {'executableSha256': '0' * 64})):
            with self.subTest(field=field):
                catalog = json.loads(catalog_path.read_text())
                catalog[field] = value
                catalog_path.write_text(json.dumps(catalog))
                manifest = dict(self.manifest)
                manifest['redistributionFilesSha256'] = dict(manifest['redistributionFilesSha256'],
                                                             **{'SOURCES-WINDOWS.json': fixture.digest(catalog_path)})
                fixture.save_manifest(self.root / 'proton', manifest)
                with self.assertRaisesRegex(ValueError, 'source catalog'):
                    self.audit()

    def test_manifest_must_match_the_build(self):
        with self.assertRaisesRegex(ValueError, 'expected runtime manifest'):
            self.audit(expected_manifest=dict(self.manifest, sha256='0' * 64))


class WindowsCatalogContract(unittest.TestCase):
    """The checked-in Windows materials agree with the Linux catalog and with each other."""

    def setUp(self):
        self.catalog = json.loads((NOTICES / 'SOURCES-WINDOWS.json').read_text())
        self.scope = json.loads((NOTICES / 'BUN-WINDOWS-SCOPE.json').read_text())

    def test_windows_catalog_shares_the_linux_packets_and_overlay(self):
        linux = json.loads((NOTICES / 'SOURCES.json').read_text())
        self.assertEqual(windows_catalog_mismatches(self.catalog, linux), [])
        self.assertEqual(self.catalog['platform'], 'windows-x64')
        self.assertIs(self.catalog['materialsReady'], True)

    def test_overlay_hashes_are_the_current_helper_sources(self):
        for item in self.catalog['applicationSourceOverlay']['files']:
            with self.subTest(source=item['source']):
                self.assertEqual(hashlib.sha256((ROOT / item['source']).read_bytes()).hexdigest(), item['sha256'])

    def test_scope_describes_the_catalog_bun_and_the_shipped_notices(self):
        self.assertEqual(self.scope['bunExecutable']['executableSha256'],
                         self.catalog['bunTargetRuntime']['executableSha256'])
        self.assertEqual(self.scope['noticeAggregate']['sha256'], fixture.digest(NOTICES / 'BUN-THIRD-PARTY-NOTICES.txt.gz'))
        self.assertEqual(self.scope['noticeAggregate']['provenanceSha256'], fixture.digest(NOTICES / 'BUN-NOTICE-PROVENANCE.json.gz'))
        for addition in self.scope['additionalNotices']:
            self.assertEqual(addition['sha256'], fixture.digest(NOTICES / addition['file']))
        self.assertEqual(self.scope['cargo']['unresolved'], [])
        self.assertFalse([p for p in self.scope['cargo']['packages'] if not p['noticeBasis']])

    def test_catalog_runtime_is_the_locked_package(self):
        lock = json.loads((ROOT / 'tools/proton/package-lock.json').read_text())
        locked = lock['packages']['node_modules/@oven/bun-windows-x64']
        runtime = self.catalog['bunTargetRuntime']
        self.assertEqual((runtime['version'], runtime['integrity'], runtime['resolved']),
                         (locked['version'], locked['integrity'], locked['resolved']))

    def test_provisioner_and_audit_ship_the_same_set(self):
        module = provisioner()
        self.assertEqual(tuple(module.WINDOWS_REDISTRIBUTION_FILES), WINDOWS_REDISTRIBUTION_FILES)
        for name in WINDOWS_REDISTRIBUTION_FILES:
            self.assertTrue((NOTICES / name).is_file(), name)

    def test_provisioner_reports_what_keeps_a_build_from_shipping(self):
        module = provisioner()
        lock = json.loads((ROOT / 'tools/proton/runtime-lock.json').read_text())
        with tempfile.TemporaryDirectory(prefix='pg-proton-gaps-') as temporary:
            manifest = fixture.write_runtime(Path(temporary), WINDOWS_REDISTRIBUTION_FILES)
            redistribution = Path(temporary) / 'redistribution'
            self.assertEqual(module.windows_redistribution_gaps(manifest, lock, redistribution), [])
            changed = dict(manifest, sha256='0' * 64)
            self.assertIn('pins helper', module.windows_redistribution_gaps(changed, lock, redistribution)[0])
            other_bun = dict(manifest, bunTargetRuntime=dict(manifest['bunTargetRuntime'], executableSha256='c' * 64))
            self.assertEqual(len(module.windows_redistribution_gaps(other_bun, lock, redistribution)), 2)
            (redistribution / 'LICENSE.icu4c-78.3').unlink()
            self.assertEqual(module.windows_redistribution_gaps(manifest, lock, redistribution),
                             ['redistribution/LICENSE.icu4c-78.3 is missing'])


if __name__ == '__main__':
    unittest.main()
