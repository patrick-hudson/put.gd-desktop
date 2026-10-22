"""The repositories' AppStream catalog describes exactly put.gd from the published package (#314)."""
import gzip
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
import appstream_catalog
import sign_stable

COMPOSED = """<?xml version="1.0" encoding="utf-8"?>
<components version="1.0">
  <component type="desktop-application">
    <id>gd.put.Putgd</id>
    <name>put.gd</name>
    <summary>Capture and share screen regions and clipboard content</summary>
    <description>
      <p>
      A native Linux sharing app.
      Hit a hotkey.
    </p>
    </description>
    <icon type="cached" width="64" height="64">gd.put.Putgd.png</icon>
    <icon type="cached" width="64" height="64" scale="2">gd.put.Putgd.png</icon>
    <icon type="stock">putgd</icon>
  </component>
</components>
"""

RELEASE = """Origin: put.gd
Label: put.gd
Suite: nightly
Codename: nightly
Date: Thu, 22 Oct 2026 07:41:44 UTC
Architectures: amd64
Components: main
Description: put.gd nightly packages for debian13
MD5Sum:
 ecce95acc3122f0cbcd2b22a3bdf61d0 1541 main/binary-amd64/Packages
 4749344bb03f32a64a208c187faeb1b7  763 main/binary-amd64/Packages.gz
SHA1:
 6eaf4b6aba6320db7d3f52bae27bbb93db50ba0d 1541 main/binary-amd64/Packages
 e062a544566a962a27b2fdf7bad18ce70f35d992  763 main/binary-amd64/Packages.gz
SHA256:
 0749f6958939d4f0b7c7b0e8934651bd6adc3fc55aa3dc377148c1bff1f332de 1541 main/binary-amd64/Packages
 8a2d4732c13dd82d65a95ba8c580d5a3812a38c1a824924c9c3e36b9d1788aca  763 main/binary-amd64/Packages.gz
"""


class Catalog(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='pgcat-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.icons = {}
        for size in ('64x64', '64x64@2'):
            (self.root / size).mkdir()
            (self.root / size / 'gd.put.Putgd.png').write_bytes(b'png ' + size.encode())
            self.icons[size] = self.root / size

    def test_finished_catalog_names_the_origin_and_package_and_reads_cleanly(self):
        text = appstream_catalog.finish(COMPOSED, 'putgd-debian13-nightly', self.icons)
        root = appstream_catalog.ET.fromstring(text)
        self.assertEqual(root.get('origin'), 'putgd-debian13-nightly')
        component = root.find('component')
        self.assertEqual([child.tag for child in component][:2], ['id', 'pkgname'])
        self.assertEqual(component.findtext('pkgname'), 'putgd')
        self.assertEqual(component.find('description/p').text, 'A native Linux sharing app. Hit a hotkey.')

    def test_anything_but_exactly_put_gd_with_its_rendered_icons_is_refused(self):
        other = COMPOSED.replace('<id>gd.put.Putgd</id>', '<id>org.example.Other</id>')
        twice = COMPOSED.replace('</components>', COMPOSED.split('<components version="1.0">')[1])
        stock_only = '\n'.join(line for line in COMPOSED.splitlines() if 'type="cached"' not in line)
        for name, text, icons in (('other', other, self.icons), ('twice', twice, self.icons),
                                  ('missing icon', COMPOSED, {'64x64': self.icons['64x64']}),
                                  ('stock only', stock_only, self.icons), ('not a catalog', '<component/>', self.icons)):
            with self.subTest(name), self.assertRaises(appstream_catalog.CatalogError):
                appstream_catalog.finish(text, 'putgd-nightly', icons)

    def test_archives_are_deterministic_and_root_owned(self):
        files = {'64x64/gd.put.Putgd.png': self.icons['64x64'] / 'gd.put.Putgd.png'}
        first = appstream_catalog.gzip_bytes(appstream_catalog.tar_bytes(files))
        self.assertEqual(first, appstream_catalog.gzip_bytes(appstream_catalog.tar_bytes(files)))
        with tarfile.open(fileobj=io.BytesIO(gzip.decompress(first))) as tar:
            members = tar.getmembers()
        self.assertEqual([(m.name, m.uid, m.gid, m.mtime, m.mode) for m in members],
                         [('64x64/gd.put.Putgd.png', 0, 0, 0, 0o644)])


class Release(unittest.TestCase):
    def test_catalog_is_listed_under_every_hash_and_reprepro_lines_stay(self):
        with tempfile.TemporaryDirectory(prefix='pgrel-') as directory:
            dists = Path(directory)
            (dists / 'main/dep11').mkdir(parents=True)
            components = dists / 'main/dep11/Components-amd64.yml.gz'
            components.write_bytes(b'yaml')
            release = dists / 'Release'
            # An earlier run's entry for a file that has since gone is dropped.
            release.write_text(RELEASE.replace('SHA256:\n', 'SHA256:\n ' + '0' * 64 + ' 9 main/dep11/icons-48x48.tar.gz\n'))
            appstream_catalog.list_in_release(release, dists, ['main/dep11/Components-amd64.yml.gz'])
            text = release.read_text()
            for line in RELEASE.splitlines():
                self.assertIn(line, text.splitlines())
            self.assertNotIn('icons-48x48', text)
            self.assertIn(f' {hashlib.md5(b"yaml").hexdigest()} 4 main/dep11/Components-amd64.yml.gz\nSHA1:', text)
            self.assertIn(f' {hashlib.sha1(b"yaml").hexdigest()} 4 main/dep11/Components-amd64.yml.gz\nSHA256:', text)
            # The workstation's stable check reads the same section.
            hashes = sign_stable.release_hashes(text)
            self.assertEqual(hashes['main/dep11/Components-amd64.yml.gz'], hashlib.sha256(b'yaml').hexdigest())
            self.assertEqual(hashes['main/binary-amd64/Packages'], '0749f6958939d4f0b7c7b0e8934651bd6adc3fc55aa3dc377148c1bff1f332de')
            # Running again leaves one entry per section.
            appstream_catalog.list_in_release(release, dists, ['main/dep11/Components-amd64.yml.gz'])
            self.assertEqual(release.read_text(), text)

    def test_release_without_hash_sections_is_refused(self):
        with tempfile.TemporaryDirectory(prefix='pgrel-') as directory:
            release = Path(directory) / 'Release'
            release.write_text('Origin: put.gd\n')
            with self.assertRaises(appstream_catalog.CatalogError):
                appstream_catalog.list_in_release(release, Path(directory), [])


def crafted_tar(entries):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.GNU_FORMAT) as tar:
        for name, kind, data in entries:
            info = tarfile.TarInfo(name)
            info.type = kind
            if kind == tarfile.SYMTYPE:
                info.linkname = '/etc/passwd'
            info.size = len(data) if kind == tarfile.REGTYPE else 0
            tar.addfile(info, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
    return buffer.getvalue()


class Unpack(unittest.TestCase):
    def unpack(self, data, name='putgd.deb'):
        directory = Path(tempfile.mkdtemp(prefix='pgunpack-'))
        self.addCleanup(shutil.rmtree, directory)
        (directory / name).write_bytes(b'package')

        def run(command, stdout=None, **_):
            stdout.write(data)
            return subprocess.CompletedProcess(command, 0)
        return appstream_catalog.unpack(directory / name, directory / 'root', run)

    def test_only_the_appstream_inputs_are_extracted_as_plain_files(self):
        root = self.unpack(crafted_tar([
            ('./usr/share/metainfo/gd.put.Putgd.metainfo.xml', tarfile.REGTYPE, b'<component/>'),
            ('./usr/share/icons/hicolor/scalable/apps/putgd.svg', tarfile.REGTYPE, b'<svg/>'),
            ('./usr/share/icons/hicolor/scalable/apps/link.svg', tarfile.SYMTYPE, b''),
            ('./usr/bin/putgd', tarfile.REGTYPE, b'ELF'),
            ('./usr/share/applications/', tarfile.DIRTYPE, b'')]))
        self.assertEqual(sorted(p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file() or p.is_symlink()),
                         ['usr/share/icons/hicolor/scalable/apps/putgd.svg', 'usr/share/metainfo/gd.put.Putgd.metainfo.xml'])

    def test_paths_outside_the_package_root_are_never_written(self):
        root = self.unpack(crafted_tar([('usr/share/metainfo/../../../../escape.xml', tarfile.REGTYPE, b'x'),
                                        ('/usr/share/metainfo/absolute.xml', tarfile.REGTYPE, b'y')]))
        self.assertEqual([p.relative_to(root.parent).as_posix() for p in root.parent.rglob('*.xml')],
                         ['root/usr/share/metainfo/absolute.xml'])
        with self.assertRaises(appstream_catalog.CatalogError):
            self.unpack(b'', name='putgd.tar')


def fixture_metainfo():
    text = (ROOT / 'packaging/linux/gd.put.Putgd.metainfo.xml.in').read_text()
    return text.replace('@PUTGD_METAINFO_LICENSE@', 'MIT').replace(
        '@PUTGD_METAINFO_RELEASES@', '  <releases>\n    <release version="26.1022.968~nightly" date="2026-10-22"/>\n  </releases>\n')


def fixture_root(root):
    """The files a real putgd package carries for AppStream: metainfo, desktop file, scalable icon."""
    (root / 'usr/share/metainfo').mkdir(parents=True)
    (root / 'usr/share/metainfo/gd.put.Putgd.metainfo.xml').write_text(fixture_metainfo())
    (root / 'usr/share/applications').mkdir(parents=True)
    (root / 'usr/share/applications/putgd.desktop').write_text(
        (ROOT / 'packaging/linux/app.desktop.in').read_text().replace('@CMAKE_INSTALL_FULL_BINDIR@', '/usr/bin'))
    (root / 'usr/share/icons/hicolor/scalable/apps').mkdir(parents=True)
    shutil.copy2(ROOT / 'packaging/linux/putgd.svg', root / 'usr/share/icons/hicolor/scalable/apps/putgd.svg')


def compose_available():
    return (shutil.which('appstreamcli') and shutil.which('dpkg-deb')
            and subprocess.run(['appstreamcli', 'compose', '--help'], capture_output=True).returncode == 0)


@unittest.skipUnless(os.environ.get('PUTGD_SIGNER_TOOLS') == '1' and compose_available(),
                     'needs appstreamcli compose (appstream-compose) and dpkg-deb (PUTGD_SIGNER_TOOLS=1, signer host)')
class RealCompose(unittest.TestCase):
    def test_dep11_from_a_deb_validates_and_its_icons_are_flat(self):
        with tempfile.TemporaryDirectory(prefix='pgcompose-') as directory:
            work = Path(directory)
            package = work / 'deb'
            fixture_root(package)
            (package / 'DEBIAN').mkdir()
            (package / 'DEBIAN/control').write_text('Package: putgd\nVersion: 1\nArchitecture: amd64\nMaintainer: t\n'
                                                    'Description: t\n')
            subprocess.run(['dpkg-deb', '--build', '--root-owner-group', str(package), str(work / 'putgd.deb')],
                           check=True, capture_output=True)
            dists = work / 'dists'
            (work / 'compose').mkdir()
            files = appstream_catalog.write_dep11(work / 'putgd.deb', 'putgd-debian13-nightly', dists, work / 'compose')
            # apt skips an optional target whose uncompressed name Release doesn't list (#314 lab, k72).
            for name in ('Components-amd64.yml', 'icons-64x64.tar', 'icons-128x128.tar'):
                self.assertIn('main/dep11/' + name, files)
                self.assertIn('main/dep11/' + name + '.gz', files)
                self.assertEqual(gzip.decompress((dists / 'main/dep11' / (name + '.gz')).read_bytes()),
                                 (dists / 'main/dep11' / name).read_bytes())
            yml = gzip.decompress((dists / 'main/dep11/Components-amd64.yml.gz').read_bytes()).decode()
            for line in ('File: DEP-11', 'Origin: putgd-debian13-nightly', 'ID: gd.put.Putgd', 'Package: putgd',
                         '  C: put.gd', '  - name: gd.put.Putgd.png', '  stock: putgd'):
                self.assertIn(line + '\n', yml)
            with tarfile.open(dists / 'main/dep11/icons-64x64.tar.gz') as tar:
                self.assertEqual(tar.getnames(), ['gd.put.Putgd.png'])
                self.assertEqual(tar.extractfile('gd.put.Putgd.png').read()[:8], b'\x89PNG\r\n\x1a\n')
            again = appstream_catalog.write_dep11(work / 'putgd.deb', 'putgd-debian13-nightly', dists, work / 'again')
            self.assertEqual(again, files)


if __name__ == '__main__':
    unittest.main()
