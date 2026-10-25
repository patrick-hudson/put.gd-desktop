"""Proton package inventory checks using synthetic binaries, not desktop proof."""
import copy
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
from audit_proton import REDISTRIBUTION_FILES, audit_proton


class ProtonPackageAuditTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pg-proton-pkg-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.private = self.root / 'usr/libexec/putgd/proton'
        self.private.mkdir(parents=True)
        self.docs = self.root / 'usr/share/doc/putgd/proton'
        shutil.copytree(ROOT / 'tools/proton/notices', self.private / 'redistribution')
        lock = json.loads((ROOT / 'tools/proton/runtime-lock.json').read_text())
        self.manifest = {
            'schemaVersion': 1, 'protocolVersion': 1,
            'sourceReference': lock['sourceReference'],
            'cryptoSourceReference': lock['cryptoSourceReference'],
            'cryptoVersion': lock['cryptoVersion'],
            'dependencyNoticesSha256': self.digest(ROOT / 'tools/proton/notices/THIRD-PARTY-NOTICES.txt'),
            'redistributionFilesSha256': {name: self.digest(self.private / 'redistribution' / name)
                                          for name in REDISTRIBUTION_FILES},
        }
        self.helper = self.private / 'putgd-proton-runtime'
        self.helper.write_bytes(b'synthetic Proton helper; no network or credential access')
        self.helper.chmod(0o755)
        self.manifest['sha256'] = self.digest(self.helper)
        shutil.copyfile(ROOT / 'tools/proton/notices/THIRD-PARTY-NOTICES.txt',
                        self.private / 'THIRD-PARTY-NOTICES.txt')
        self.catalog = json.loads((self.private / 'redistribution/SOURCES.json').read_text())
        self.catalog['helperSha256'] = self.manifest['sha256']
        self.save_catalog()
        self.save_manifest()

    @staticmethod
    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def save_catalog(self):
        path = self.private / 'redistribution/SOURCES.json'
        path.write_text(json.dumps(self.catalog))
        self.manifest['redistributionFilesSha256']['SOURCES.json'] = self.digest(path)
        shutil.copytree(self.private / 'redistribution', self.docs, dirs_exist_ok=True)
        shutil.copyfile(self.private / 'THIRD-PARTY-NOTICES.txt', self.docs / 'THIRD-PARTY-NOTICES.txt')

    def save_manifest(self):
        text = json.dumps(self.manifest)
        (self.private / 'manifest.json').write_text(text)
        shared = self.root / 'usr/share/putgd/proton-runtime-manifest.json'
        shared.parent.mkdir(parents=True, exist_ok=True)
        shared.write_text(text)

    def test_complete_inventory_binds_helper_notices_and_source_catalog(self):
        result = audit_proton(self.root, required=True, expected_manifest=self.manifest)
        self.assertEqual(result['runtimeSha256'], self.manifest['sha256'])
        self.assertEqual(result['redistributionFiles'], 13)
        self.assertEqual(result['helper'], 'usr/libexec/putgd/proton/putgd-proton-runtime')

    def test_absent_optional_runtime_is_allowed_but_required_runtime_is_rejected(self):
        empty = self.root / 'empty'
        empty.mkdir()
        self.assertIsNone(audit_proton(empty))
        with self.assertRaisesRegex(ValueError, 'Required Proton'):
            audit_proton(empty, required=True)

    def test_incomplete_present_runtime_is_rejected_without_requirement_flag(self):
        self.helper.unlink()
        with self.assertRaisesRegex(ValueError, 'Required Proton'):
            audit_proton(self.root)

    def test_package_processing_cannot_change_helper_bytes(self):
        self.helper.write_bytes(b'stripped or replaced')
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            audit_proton(self.root, required=True)

    def test_helper_must_be_executable(self):
        self.helper.chmod(0o644)
        with self.assertRaisesRegex(ValueError, '0755'):
            audit_proton(self.root, required=True)

    def test_notices_cannot_be_missing_or_tampered(self):
        name = 'LICENSE.crypto-GPL-3.0'
        (self.private / 'redistribution' / name).unlink()
        with self.assertRaisesRegex(ValueError, 'Required Proton'):
            audit_proton(self.root, required=True)

    def test_public_notice_copy_must_match_runtime_copy(self):
        (self.docs / 'REBUILD.md').write_text('stale rebuild instructions')
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            audit_proton(self.root, required=True)

    def test_primary_dependency_notice_must_match_manifest(self):
        (self.private / 'THIRD-PARTY-NOTICES.txt').write_text('stale notices')
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            audit_proton(self.root, required=True)

    def test_manifest_copies_must_agree(self):
        path = self.root / 'usr/share/putgd/proton-runtime-manifest.json'
        other = copy.deepcopy(self.manifest)
        other['cryptoVersion'] = 'old'
        path.write_text(json.dumps(other))
        with self.assertRaisesRegex(ValueError, 'manifest copies'):
            audit_proton(self.root, required=True)

    def test_package_must_match_expected_runtime_manifest(self):
        wrong = {**self.manifest, 'cryptoVersion': 'old'}
        with self.assertRaisesRegex(ValueError, 'expected runtime manifest'):
            audit_proton(self.root, required=True, expected_manifest=wrong)

    def test_manifest_must_record_the_complete_notice_set(self):
        del self.manifest['redistributionFilesSha256']['LICENSE.crypto-GPL-3.0']
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, 'redistribution manifest'):
            audit_proton(self.root, required=True)

    def test_source_catalog_must_match_selected_helper_and_pins(self):
        for field in ('helperSha256', 'sourcePins'):
            original = copy.deepcopy(self.catalog)
            self.catalog[field] = 'wrong' if field == 'helperSha256' else {**self.catalog[field], 'crypto': 'wrong'}
            self.save_catalog()
            self.save_manifest()
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'source catalog'):
                audit_proton(self.root, required=True)
            self.catalog = original

    def test_source_catalog_must_be_ready(self):
        self.catalog['materialsReady'] = False
        self.save_catalog()
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, 'source catalog'):
            audit_proton(self.root, required=True)


if __name__ == '__main__':
    unittest.main()
