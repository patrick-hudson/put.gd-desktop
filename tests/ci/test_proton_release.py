"""Proton shipping scope: Linux helper is built, tested and required in packages."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ProtonRelease(unittest.TestCase):
    def test_linux_tests_provision_and_bundle(self):
        text = (ROOT / '.github/workflows/ci.yml').read_text()
        linux = text.split('  linux:\n', 1)[1].split('  portable:\n', 1)[0]
        portable = text.split('  portable:\n', 1)[1].split('  packages:\n', 1)[0]
        contracts = text.split('  contracts:\n', 1)[1].split('  linux:\n', 1)[0]
        self.assertIn('python3 tools/proton/provision_runtime.py', linux)
        self.assertIn('cmake --preset dev -DPUTGD_BUNDLE_PROTON=ON', linux)
        self.assertIn('python3 tools/ci/contracts.py proton', contracts)
        self.assertNotIn('PUTGD_BUNDLE_PROTON=ON', portable)
        self.assertNotIn('provision_runtime.py', portable)

    def test_proton_contract_suites_are_selected(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('contracts', ROOT / 'tools/ci/contracts.py')
        contracts = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(contracts)
        self.assertEqual(contracts.suites('proton'),
                         ['tools/proton/test_crypto_source.py', 'tools/proton/test_redistribution.py'])
        release = contracts.suites('release')
        for suite in ('tests/unit/test_proton_package_audit.py', 'tests/releases/test_proton_sources.py',
                      'tests/releases/test_check_source_build.py', 'tests/ci/test_proton_release.py'):
            self.assertIn(suite, release)

    def test_native_and_portable_release_require_proton(self):
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
