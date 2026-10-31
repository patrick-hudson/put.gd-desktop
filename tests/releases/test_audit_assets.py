"""Exact download checks fail closed on missing, extra or changed release bytes."""
import hashlib
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
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
    def test_fixed_boundary_and_union(self):
        self.assertTrue(audit_assets.version_in_range('1.4.2', '>=1.0.21, <1.4.4'))
        self.assertFalse(audit_assets.version_in_range('1.4.4', '>=1.0.21, <1.4.4'))
        self.assertTrue(audit_assets.version_in_range('1.4.2', '<1.0.0 || >=1.3.14 <1.4.4'))
        with self.assertRaises(ValueError):
            audit_assets.version_in_range('1.4.2', '^1.0')


if __name__ == '__main__':
    unittest.main()
