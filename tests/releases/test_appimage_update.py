"""AppImage update information follows the release channel and the .zsync binds the exact image (#293)."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import appimage_update
from appimage_fixture import make_appimage, make_zsync

NAME = 'putgd-0.1.1-nightly.600-x86_64.AppImage'
URL = 'https://pkg.put.gd/releases/v0.1.1-nightly.600/' + NAME


class AppImageUpdate(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgupd-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_channel_permalink_follows_tag(self):
        self.assertEqual(appimage_update.update_information('v0.1.1-nightly.600'),
                         'zsync|https://app.put.gd/dl/nightly/appimage.zsync')
        # Release candidate users follow stable.
        for tag in ('v0.1.1-rc.2', 'v0.1.1'):
            self.assertEqual(appimage_update.update_information(tag),
                             'zsync|https://app.put.gd/dl/stable/appimage.zsync')
        for tag in ('', 'v0.1.1-dev.3', '0.1.1', 'v0.1.1-nightly.0'):
            with self.assertRaises(ValueError):
                appimage_update.update_information(tag)

    def test_targets_embed_nothing_without_tag_and_absolute_release_url_with_one(self):
        self.assertIsNone(appimage_update.targets(NAME))
        self.assertEqual(appimage_update.targets(NAME, 'v0.1.1-nightly.600'),
                         ('zsync|https://app.put.gd/dl/nightly/appimage.zsync', URL))
        self.assertEqual(appimage_update.targets(NAME, None, origin='http://127.0.0.1:8765/'),
                         ('zsync|http://127.0.0.1:8765/appimage.zsync', 'http://127.0.0.1:8765/' + NAME))
        for repository in ('', 'owner', 'a/b/c', 'own er/name'):
            with self.assertRaisesRegex(ValueError, 'OWNER/NAME'):
                appimage_update.targets(NAME, 'v0.1.1', repository)
        with self.assertRaisesRegex(ValueError, 'asset name'):
            appimage_update.asset_url(appimage_update.REPOSITORY, 'v0.1.1', '../' + NAME)
        with self.assertRaisesRegex(ValueError, 'origin'):
            appimage_update.targets(NAME, origin='file:///tmp')
        # A tagged lab build: the live layout under the signer's lab prefix (#293 phase 3).
        self.assertEqual(appimage_update.targets(NAME, 'v0.1.1-nightly.600', base='https://pkg.put.gd/lab/'),
                         ('zsync|https://pkg.put.gd/lab/dl/nightly/appimage.zsync',
                          'https://pkg.put.gd/lab/releases/v0.1.1-nightly.600/' + NAME))
        for base in ('http://pkg.put.gd/lab/', 'https://pkg.put.gd/lab', 'https://pkg.put.gd/lab?x/'):
            with self.assertRaisesRegex(ValueError, 'base'):
                appimage_update.targets(NAME, 'v0.1.1-nightly.600', base=base)

    def test_reads_embedded_update_information(self):
        image = self.root / NAME
        make_appimage(image, 'zsync|https://app.put.gd/dl/nightly/appimage.zsync')
        self.assertEqual(appimage_update.read_update_information(image),
                         'zsync|https://app.put.gd/dl/nightly/appimage.zsync')
        make_appimage(image)
        self.assertEqual(appimage_update.read_update_information(image), '')
        image.write_bytes(b'not an ELF file' * 8)
        with self.assertRaisesRegex(ValueError, 'ELF64'):
            appimage_update.read_update_information(image)

    def test_zsync_must_describe_exact_image_bytes_and_url(self):
        image = self.root / NAME
        make_appimage(image, payload=b'x' * 6001)
        zsync = self.root / (NAME + '.zsync')
        make_zsync(zsync, image, NAME, URL)
        self.assertEqual(appimage_update.verify_zsync(zsync, image, NAME, URL)['Length'], str(image.stat().st_size))
        cases = {'another image or URL': dict(url=URL + '.old'),
                 'other image bytes': dict(length=image.stat().st_size - 1),
                 'zsync 0.6.2': dict(version='2.0.0-alpha-1')}
        for message, change in cases.items():
            make_zsync(zsync, image, NAME, change.get('url', URL), length=change.get('length'),
                       version=change.get('version', '0.6.2'))
            with self.assertRaisesRegex(ValueError, message):
                appimage_update.verify_zsync(zsync, image, NAME, URL)
        # zsync2's zsyncmake2 drops the trailing partial block: header and SHA-1
        # then describe a truncated image, which an updater would install.
        make_zsync(zsync, image, NAME, URL, length=image.stat().st_size // 2048 * 2048)
        with self.assertRaisesRegex(ValueError, 'other image bytes'):
            appimage_update.verify_zsync(zsync, image, NAME, URL)
        make_zsync(zsync, image, NAME, URL)
        zsync.write_bytes(zsync.read_bytes()[:-6])
        with self.assertRaisesRegex(ValueError, 'whole image'):
            appimage_update.verify_zsync(zsync, image, NAME, URL)
        with self.assertRaisesRegex(ValueError, 'another image'):
            appimage_update.verify_zsync(zsync, image, 'putgd-other-x86_64.AppImage', URL)


if __name__ == '__main__':
    unittest.main()
