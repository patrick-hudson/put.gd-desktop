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

    def test_installer_names_the_openssl_soname_the_package_links(self):
        elf = {'usr/bin/putgd-cli': {'needed': ['libcrypto.so.4', 'libc.so.6']}, 'usr/lib/putgd/libs2n.so': {'needed': ['libcrypto.so.4']}}
        self.assertEqual(arch.linked_libcrypto(elf), 'libcrypto.so=4-64')
        with self.assertRaises(ValueError):
            arch.linked_libcrypto({**elf, 'usr/bin/x': {'needed': ['libcrypto.so.3']}})
        with self.assertRaises(ValueError):
            arch.linked_libcrypto({'usr/bin/putgd-cli': {'needed': ['libc.so.6']}})
        self.assertEqual(arch.installer_libcrypto("x=1\narch_libcrypto='libcrypto.so=4-64'\n"), 'libcrypto.so=4-64')
        for text in ('', "arch_libcrypto='libcrypto.so=4'\n", "arch_libcrypto='libcrypto.so=4-64'\narch_libcrypto='libcrypto.so=5-64'\n"):
            with self.assertRaises(ValueError):
                arch.installer_libcrypto(text)
        # The shipped installer names exactly one soname.
        shipped = Path(__file__).resolve().parents[2] / 'packaging/install.sh'
        self.assertRegex(arch.installer_libcrypto(shipped.read_text()), r'libcrypto\.so=\d+-64')

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
