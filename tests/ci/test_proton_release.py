"""Proton shipping scope: Linux helper is built, tested and required in packages."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]

class ProtonRelease(unittest.TestCase):
    def test_linux_builds_provision_and_bundle(self):
        text = (ROOT / '.github/workflows/build.yml').read_text()
        linux, portable = text.split('  portable:', 1)
        self.assertIn('python3 tools/proton/provision_runtime.py', linux)
        for test in ('test_proton_sources.py', 'test_check_source_build.py'):
            self.assertIn('python3 tests/releases/' + test, linux)
        self.assertIn('cmake --preset dev -DPUTGD_BUNDLE_PROTON=ON', linux)
        self.assertIn('cmake --preset release -DBUILD_TESTING=OFF -DPUTGD_BUNDLE_PROTON=ON', linux)
        self.assertNotIn('PUTGD_BUNDLE_PROTON=ON', portable)
        self.assertNotIn('provision_runtime.py', portable)

    def test_native_and_portable_release_require_proton(self):
        text = (ROOT / '.github/workflows/release.yml').read_text()
        native = text.split('  native:', 1)[1].split('  appimage:', 1)[0]
        appimage = text.split('  appimage:', 1)[1].split('  publish:', 1)[0]
        for section in (native, appimage):
            self.assertIn('python3 tools/proton/provision_runtime.py', section)
            for test in ('test_proton_sources.py', 'test_check_source_build.py'):
                self.assertIn('python3 tests/releases/' + test, section)
        self.assertIn('-DPUTGD_BUNDLE_PROTON=ON', native)
        self.assertIn('--require-proton', native)
        self.assertIn('--bundle-proton', native)
        self.assertIn('-DPUTGD_BUNDLE_PROTON=ON', (ROOT / 'tools/releases/build-appimage.sh').read_text())
        for path in ('tools/proton/**', 'cmake/Proton.cmake'):
            self.assertIn(path, text)

    def test_native_packages_declare_browser_launcher(self):
        text = (ROOT / 'packaging/linux/CMakeLists.txt').read_text()
        self.assertIn('xdg-utils', text)
        self.assertIn('GPL-3.0', text)

if __name__ == '__main__':
    unittest.main()
