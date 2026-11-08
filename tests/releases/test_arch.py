"""Arch identity and stable-signing index verification contracts."""
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import arch
import pacman_repo


class Arch(unittest.TestCase):
    def test_versions_preserve_numeric_calver_and_valid_pkgver(self):
        self.assertEqual(arch.pkgver('26.1107.4000-nightly'), '26.1107.4000nightly')
        self.assertEqual(arch.pkgver('26.1107.4000'), '26.1107.4000')
        self.assertNotIn('-', arch.pkgver('26.1107.4000-dev+g123456789abc'))

    def test_database_binds_package_and_rejects_other_bytes(self):
        fields = {k: [v] for k, v in dict(pkgname='putgd', pkgver='26.1107.4000nightly-1', pkgdesc='Sharing', arch='x86_64', size='7', builddate='1', packager='put.gd').items()}
        fields['depend'] = ['qt6-base>=6.4.2']
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            package = root / 'putgd.pkg.tar.zst'
            package.write_bytes(b'payload')
            with patch.object(pacman_repo, 'info', return_value=fields):
                pacman_repo.database(root, 'nightly', package)
            data = (root / 'putgd-nightly.db.tar.gz').read_bytes()
            import hashlib
            pacman_repo.check_database(data, package.name, hashlib.sha256(b'payload').hexdigest())
            with self.assertRaises(ValueError):
                pacman_repo.check_database(data, package.name, '0' * 64)
            with self.assertRaises(ValueError):
                pacman_repo.check_database(data)
            pacman_repo.database(root, 'stable')
            pacman_repo.check_database((root / 'putgd-stable.db.tar.gz').read_bytes())
