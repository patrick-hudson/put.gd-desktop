import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class ProtonInstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pg-p3-install-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.runtime = self.root / 'build/dependencies/proton-runtime'
        self.runtime.mkdir(parents=True)
        original = REPO / 'build/dependencies/proton-runtime'
        self.manifest = json.loads((original / 'manifest.json').read_text())
        for name in ['putgd-proton-runtime', 'manifest.json', 'THIRD-PARTY-NOTICES.txt']:
            shutil.copyfile(original / name, self.runtime / name)
        shutil.copytree(original / 'redistribution', self.runtime / 'redistribution')
        tools = self.root / 'tools/proton'
        tools.mkdir(parents=True)
        for name in list(self.manifest['helperSourceSha256']) + ['runtime-lock.json', 'package-lock.json']:
            shutil.copyfile(REPO / 'tools/proton' / name, tools / name)
        shutil.copytree(REPO / 'tools/proton/notices', tools / 'notices')
        cpp = self.root / 'src/storage/proton/ProtonProvider.cpp'
        cpp.parent.mkdir(parents=True)
        cpp.write_text('// Unbuilt install-rule fixture.\n')
        (self.root / 'stub.cpp').write_text('int main() { return 0; }\n')
        (self.root / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.25)
project(ProtonInstallRules LANGUAGES CXX)
include(GNUInstallDirs)
add_library(putgd_core INTERFACE)
add_library(Qt6::Core INTERFACE IMPORTED)
add_executable(putgd EXCLUDE_FROM_ALL stub.cpp)
add_executable(putgd_ui EXCLUDE_FROM_ALL stub.cpp)
set(BUILD_TESTING OFF)
set(PUTGD_BUNDLE_PROTON ON CACHE BOOL "")
include("{REPO / 'cmake/Proton.cmake'}")
''')

    def configure(self):
        return subprocess.run(['cmake', '-S', str(self.root), '-B', str(self.root / 'cmake-build'),
                               '-G', 'Ninja', '-DCMAKE_INSTALL_PREFIX=/usr'], capture_output=True, text=True)

    def test_exact_runtime_and_redistribution_assets_install_together(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        installed = self.root / 'installed'
        result = subprocess.run(['cmake', '--install', str(self.root / 'cmake-build'), '--prefix', str(installed)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        copies = list(installed.rglob('putgd-proton-runtime'))
        self.assertEqual(len(copies), 1)
        self.assertEqual(hashlib.sha256(copies[0].read_bytes()).hexdigest(), self.manifest['sha256'])
        for name, expected in self.manifest['redistributionFilesSha256'].items():
            path = copies[0].parent / 'redistribution' / name
            self.assertTrue(path.is_file(), name)
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), expected, name)
        self.assertTrue(list(installed.rglob('BUN-THIRD-PARTY-NOTICES.txt.gz')))
        self.assertTrue(list(installed.rglob('LICENSE.crypto-GPL-3.0')))
        catalog = json.loads((copies[0].parent / 'redistribution/SOURCES.json').read_text())
        self.assertTrue(catalog['materialsReady'])
        self.assertTrue(catalog['artifacts'])

    def test_changed_license_approval_is_rejected_before_install(self):
        lock_path = self.root / 'tools/proton/runtime-lock.json'
        lock = json.loads(lock_path.read_text())
        lock['cryptoLicenseSha256'] = '0' * 64
        lock_path.write_text(json.dumps(lock))
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('runtime-lock.json changed', result.stdout + result.stderr)

    def test_stale_bun_source_catalog_is_rejected_before_install(self):
        path = self.root / 'tools/proton/notices/SOURCES.json'
        catalog = json.loads(path.read_text())
        catalog['sourcePins']['bun'] = '0' * 40
        path.write_text(json.dumps(catalog))
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('catalog does not match', result.stdout + result.stderr)

    def test_changed_runtime_notice_is_rejected_before_install(self):
        path = self.runtime / 'redistribution/BUN-THIRD-PARTY-NOTICES.txt.gz'
        with path.open('ab') as stream:
            stream.write(b'synthetic tamper')
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('redistribution', (result.stdout + result.stderr).lower())


if __name__ == '__main__':
    unittest.main()
