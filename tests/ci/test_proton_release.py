"""Proton shipping scope: Linux helper is built, tested and required in packages."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ProtonRelease(unittest.TestCase):
    def test_native_and_appimage_release_require_proton(self):
        text = (ROOT / '.github/workflows/package.yml').read_text()
        native = text.split('  native:\n', 1)[1].split('  appimage:\n', 1)[0]
        appimage = text.split('  appimage:\n', 1)[1].split('  collect:\n', 1)[0]
        for section in (native, appimage):
            self.assertIn('python3 tools/proton/provision_runtime.py', section)
        self.assertIn('-DPUTGD_BUNDLE_PROTON=ON', native)
        self.assertIn('--require-proton', native)
        self.assertIn('--bundle-proton', native)
        self.assertIn('-DPUTGD_BUNDLE_PROTON=ON', (ROOT / 'tools/releases/build-appimage.sh').read_text())
        changes = (ROOT / 'tools/ci/changes.py').read_text()
        for path in ("'tools/proton/'", "'cmake/'"):
            self.assertIn(path, changes)

    def test_native_packages_declare_browser_launcher(self):
        text = (ROOT / 'packaging/linux/CMakeLists.txt').read_text()
        self.assertIn('xdg-utils', text)
        self.assertIn('GPL-3.0', text)


if __name__ == '__main__':
    unittest.main()
