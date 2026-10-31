"""Exact download checks fail closed on missing, extra or changed release bytes."""
import hashlib
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import io

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import audit_assets


class DownloadChecks(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / 'package.deb').write_bytes(b'exact downloaded bytes')
        self.digest = hashlib.sha256(b'exact downloaded bytes').hexdigest()
        (self.root / 'SHA256SUMS').write_text(self.digest + '  package.deb\n')
        self.expected = {'SHA256SUMS', 'package.deb'}

    def test_complete_checksums(self):
        result = audit_assets.checksums(self.root, self.expected)
        self.assertEqual(result['package.deb']['sha256'], self.digest)

    def test_missing_extra_duplicate_or_changed(self):
        for text in ('', self.digest + '  ../package.deb\n',
                     (self.digest + '  package.deb\n') * 2, '0' * 64 + '  package.deb\n'):
            with self.subTest(text=text):
                (self.root / 'SHA256SUMS').write_text(text)
                with self.assertRaises(ValueError):
                    audit_assets.checksums(self.root, self.expected)
        (self.root / 'extra').touch()
        with self.assertRaises(ValueError):
            audit_assets.checksums(self.root, self.expected)

    def test_symlink_is_rejected(self):
        package = self.root / 'package.deb'
        package.unlink()
        package.symlink_to('/dev/null')
        with self.assertRaises(ValueError):
            audit_assets.checksums(self.root, self.expected)

    def test_authenticated_list_resolves_draft_before_asset_snapshot(self):
        metadata = {'id': 123, 'tag_name': 'v26.1031.3384', 'draft': True,
                    'assets': [{'id': 456, 'name': 'package.deb', 'state': 'uploaded',
                        'size': (self.root / 'package.deb').stat().st_size,
                        'digest': 'sha256:' + self.digest}]}
        with patch.object(audit_assets, 'run', side_effect=[json.dumps([metadata]), json.dumps(metadata)]) as api:
            snapshot = audit_assets.remote_snapshot('owner/repo', 'v26.1031.3384', self.root, {'package.deb'})
        self.assertTrue(snapshot['draft'])
        self.assertIn('releases?per_page=100', api.call_args_list[0].args[-1])
        self.assertEqual(api.call_args_list[1].args[-1], 'repos/owner/repo/releases/123')

    def test_rebuild_must_reproduce_both_runtime_copies(self):
        catalog = {'helperSha256': self.digest}
        audit_assets.verify_rebuilt_proton({'rebuiltProton': {'sha256': self.digest, 'bundledSha256': self.digest}}, catalog)
        for built in ({}, {'rebuiltProton': {'sha256': self.digest}},
                      {'rebuiltProton': {'sha256': self.digest, 'bundledSha256': '0' * 64}}):
            with self.assertRaises(ValueError):
                audit_assets.verify_rebuilt_proton(built, catalog)


class SourceChecks(unittest.TestCase):
    def make_archive(self, directory, path, body=b'source', recorded=None):
        artifact = directory / 'source.tar.gz'
        identity = {'gitCommit': 'a' * 40}
        content = {'source/source-info.json': json.dumps({'identity': identity, 'files': [
            {'path': path, 'bytes': len(body), 'sha256': hashlib.sha256(recorded or body).hexdigest()}]}).encode(),
            'source/' + path: body}
        with tarfile.open(artifact, 'w:gz') as archive:
            for name, value in content.items():
                member = tarfile.TarInfo(name)
                member.size = len(value)
                archive.addfile(member, io.BytesIO(value))
        return artifact, {'archiveRoot': 'source', 'identity': identity, 'members': [
            {'path': path, 'bytes': len(body), 'sha256': hashlib.sha256(recorded or body).hexdigest()},
            {'path': 'source-info.json', 'bytes': len(content['source/source-info.json']),
             'sha256': hashlib.sha256(content['source/source-info.json']).hexdigest()}]}

    def test_source_hashes_and_private_path_exclusion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact, receipt = self.make_archive(root, 'src/main.cpp')
            self.assertEqual(audit_assets.source_export(artifact, receipt)['files'], 1)
            for path, recorded in (('work/local.txt', None), ('src/credentials.txt', None),
                                   ('src/main.cpp', b'other')):
                artifact, receipt = self.make_archive(root, path, recorded=recorded)
                with self.subTest(path=path), self.assertRaises(ValueError):
                    audit_assets.source_export(artifact, receipt)


class AdvisoryRanges(unittest.TestCase):
    def test_official_bun_advisory_catches_scanner_miss_and_fixed_boundary(self):
        advisory = {'ghsa_id': 'GHSA-test', 'severity': 'high', 'withdrawn_at': None,
                    'vulnerabilities': [{'package': {'name': 'bun'},
                        'vulnerable_version_range': '>= 1.0.21, < 1.4.4', 'patched_versions': '1.4.4'}]}
        with patch.object(audit_assets, 'run', return_value=json.dumps([advisory]) + '\n[]'):
            findings = audit_assets.bun_advisories({'1.4.2': ['appimage', 'debian13'], '1.4.4': ['fixed']})
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0]['packages'], ['appimage', 'debian13'])
        self.assertEqual(findings[0]['fixedIn'], '1.4.4')

    def test_fixed_boundary_and_union(self):
        self.assertTrue(audit_assets.version_in_range('1.4.2', '>= 1.0.21, < 1.4.4'))
        self.assertTrue(audit_assets.version_in_range('1.4.2', '>= 1.3.14, < 1.4.4'))
        self.assertFalse(audit_assets.version_in_range('1.4.4', '>=1.0.21, <1.4.4'))
        self.assertTrue(audit_assets.version_in_range('1.4.2', '<1.0.0 || >=1.3.14 <1.4.4'))
        with self.assertRaises(ValueError):
            audit_assets.version_in_range('1.4.2', '^1.0')


if __name__ == '__main__':
    unittest.main()
