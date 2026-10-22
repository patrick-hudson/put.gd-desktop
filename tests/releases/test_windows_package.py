"""The Windows packager (#404) refuses stale executables and writes a reproducible zip."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'src/windows/packaging'))
sys.path.insert(0, str(ROOT / 'tests/releases'))
import package  # noqa: E402
import proton_windows_fixture as proton_fixture  # noqa: E402

VERSION = '26.1022.1001-nightly'
COMMIT = 'bad58d1f2cc848bc4f051b340bc0bacb5602170a'


def executable(version=VERSION, commit=COMMIT, resource=True, proton=''):
    """Bytes standing in for a PE file: the MZ header and the identity strings it embeds."""
    product = (version + '\0').encode('utf-16-le') if resource else b''
    return b'MZ' + b'\0' * 62 + version.encode() + b'\0' + commit.encode() + b'\0' + product + proton.encode()


class WindowsPackage(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgwinpkg-')
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name) / 'build'
        (self.build / 'build-identity').mkdir(parents=True)
        (self.build / 'build-identity/build-info.json').write_text(json.dumps(
            {'schemaVersion': 1, 'version': VERSION, 'gitCommit': COMMIT, 'dirty': False}))
        (self.build / 'windows-version.json').write_text(json.dumps(
            {'version': VERSION, 'fileVersion': '26.1022.0.1001', 'msiVersion': '9.176.1001'}))
        for name in package.EXECUTABLES:
            (self.build / name).write_bytes(executable())
        self.output = Path(self.temporary.name) / 'out'

    def test_zip_holds_the_portable_folder_and_is_reproducible(self):
        receipt = package.package(self.build, self.output, msi=False)
        self.assertEqual(receipt['msiVersion'], '9.176.1001')
        self.assertEqual([asset['target'] for asset in receipt['assets']], ['windows-zip'])
        path = self.output / f'putgd-{VERSION}-windows-x86_64.zip'
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(archive.namelist(), ['put.gd/LICENSE.txt', 'put.gd/build-info.json', 'put.gd/putgd-cli.exe', 'put.gd/putgd.exe'])
            self.assertIn(b'\r\n', archive.read('put.gd/LICENSE.txt'))
        self.assertEqual(receipt['assets'][0]['sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
        first = path.read_bytes()
        package.package(self.build, self.output, msi=False)
        self.assertEqual(path.read_bytes(), first)
        self.assertEqual(json.loads((self.output / 'windows-packages.json').read_text()), receipt)

    def test_stale_or_foreign_executables_are_refused(self):
        for name, data in (('putgd.exe', executable(version='26.1022.1000-nightly')),
                           ('putgd-cli.exe', executable(commit='0' * 40)),
                           ('putgd.exe', executable(resource=False)),
                           ('putgd.exe', b'not a PE file ' + executable()[2:])):
            with self.subTest(name=name):
                original = (self.build / name).read_bytes()
                (self.build / name).write_bytes(data)
                with self.assertRaises(package.PackageError):
                    package.package(self.build, self.output, msi=False)
                (self.build / name).write_bytes(original)

    def test_manifest_and_mapping_must_agree(self):
        (self.build / 'windows-version.json').write_text(json.dumps({'version': '26.1022.1000-nightly', 'fileVersion': 'x', 'msiVersion': 'y'}))
        with self.assertRaises(package.PackageError):
            package.package(self.build, self.output, msi=False)

    @unittest.skipUnless(shutil.which('wixl') and shutil.which('msiinfo'), 'wixl and msiinfo are not installed')
    def test_msi_carries_the_versions_and_the_per_user_contract(self):
        receipt = package.package(self.build, self.output)
        msi = self.output / f'putgd-{VERSION}-windows-x86_64.msi'
        self.assertEqual([asset['target'] for asset in receipt['assets']], ['windows-zip', 'windows-msi'])

        def table(name):
            return subprocess.run(['msiinfo', 'export', str(msi), name], check=True, capture_output=True, text=True).stdout

        properties = dict(line.split('\t', 1) for line in table('Property').splitlines()[3:])
        self.assertEqual(properties['ProductVersion'], '9.176.1001')
        self.assertEqual(properties['UpgradeCode'], '{7BD6CE99-5644-460D-BF82-9768D7FAD327}')
        self.assertEqual(properties['REINSTALLMODE'], 'amus')
        self.assertNotIn('ALLUSERS', properties)
        files = {row.split('\t')[0]: row.split('\t') for row in table('File').splitlines()[3:]}
        self.assertEqual(files['putgd.exe'][4], '26.1022.0.1001')
        self.assertEqual(files['putgd_cli.exe'][4], '26.1022.0.1001')
        registry = table('Registry')
        self.assertIn(f'DisplayVersion\t{VERSION}', registry)
        self.assertNotIn('CurrentVersion\\Run', registry)
        self.assertIn('NOT WIX_DOWNGRADE_DETECTED', table('LaunchCondition'))


class WindowsProtonPackage(unittest.TestCase):
    """A build bound to a Proton runtime ships that runtime and its redistribution set (#408)."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgwinpro-')
        self.addCleanup(self.temporary.cleanup)
        base = Path(self.temporary.name)
        self.build, self.runtime, self.output = base / 'build', base / 'runtime', base / 'out'
        self.manifest = proton_fixture.write_runtime(self.runtime, package.WINDOWS_REDISTRIBUTION_FILES)
        (self.build / 'build-identity').mkdir(parents=True)
        (self.build / 'build-identity/build-info.json').write_text(json.dumps(
            {'schemaVersion': 1, 'version': VERSION, 'gitCommit': COMMIT, 'dirty': False}))
        (self.build / 'windows-version.json').write_text(json.dumps(
            {'version': VERSION, 'fileVersion': '26.1022.0.1001', 'msiVersion': '9.176.1001'}))
        (self.build / 'putgd.exe').write_bytes(executable(proton=self.manifest['sha256']))
        (self.build / 'putgd-cli.exe').write_bytes(executable())
        shutil.copyfile(self.runtime / 'manifest.json', self.build / 'proton-runtime-manifest.json')

    def package(self, **options):
        return package.package(self.build, self.output, msi=False, proton_runtime=self.runtime, **options)

    def test_zip_carries_the_runtime_its_manifest_and_notices(self):
        receipt = self.package(require_proton=True)
        self.assertEqual(receipt['proton']['runtimeSha256'], self.manifest['sha256'])
        self.assertEqual(receipt['proton']['redistributionFiles'], len(package.WINDOWS_REDISTRIBUTION_FILES))
        with zipfile.ZipFile(self.output / f'putgd-{VERSION}-windows-x86_64.zip') as archive:
            names = archive.namelist()
            self.assertEqual(archive.read('put.gd/proton/putgd-proton-runtime.exe'), proton_fixture.HELPER)
        expected = {'put.gd/proton/' + name for name in ('putgd-proton-runtime.exe', 'manifest.json', 'THIRD-PARTY-NOTICES.txt')}
        expected |= {'put.gd/proton/redistribution/' + name for name in package.WINDOWS_REDISTRIBUTION_FILES}
        self.assertEqual({name for name in names if '/proton/' in name}, expected)
        self.assertIn('put.gd/putgd.exe', names)

    def test_runtime_must_be_the_one_the_build_compiled_in(self):
        other = dict(self.manifest, sha256='0' * 64)
        proton_fixture.save_manifest(self.runtime, other)
        with self.assertRaisesRegex(package.PackageError, 'not the one putgd.exe was built with'):
            self.package()

    def test_executable_must_carry_the_manifest_digest(self):
        (self.build / 'putgd.exe').write_bytes(executable())
        with self.assertRaisesRegex(package.PackageError, 'digest'):
            self.package()

    def test_helper_bytes_must_match_the_manifest(self):
        (self.runtime / 'putgd-proton-runtime.exe').write_bytes(b'MZ another helper')
        with self.assertRaisesRegex(package.PackageError, 'digest mismatch'):
            self.package()

    def test_incomplete_redistribution_is_refused(self):
        for complete, missing in ((False, None), (True, 'LICENSE.icu4c-78.3')):
            with self.subTest(complete=complete, missing=missing):
                manifest = dict(self.manifest, redistributionComplete=complete)
                if not complete:
                    manifest['redistributionNote'] = 'Not for distribution: synthetic gap'
                proton_fixture.save_manifest(self.runtime, manifest)
                shutil.copyfile(self.runtime / 'manifest.json', self.build / 'proton-runtime-manifest.json')
                if missing:
                    (self.runtime / 'redistribution' / missing).unlink()
                with self.assertRaises(package.PackageError):
                    self.package()

    def test_build_and_runtime_must_agree_on_carrying_proton(self):
        with self.assertRaisesRegex(package.PackageError, '--proton-runtime'):
            package.package(self.build, self.output, msi=False)
        (self.build / 'proton-runtime-manifest.json').unlink()
        with self.assertRaisesRegex(package.PackageError, 'without -DPUTGD_PROTON_MANIFEST'):
            self.package()
        with self.assertRaisesRegex(package.PackageError, 'without -DPUTGD_PROTON_MANIFEST'):
            package.package(self.build, self.output, msi=False, require_proton=True)

    def test_fragment_lists_exactly_the_redistribution_set(self):
        text = (ROOT / 'src/windows/packaging/proton.wxs').read_text()
        listed = re.findall(r'Source="\$\(var\.SourceDir\)/proton/redistribution/([^"]+)"', text)
        self.assertEqual(sorted(listed), sorted(package.WINDOWS_REDISTRIBUTION_FILES))

    @unittest.skipUnless(shutil.which('wixl') and shutil.which('msiinfo'), 'wixl and msiinfo are not installed')
    def test_msi_installs_and_removes_the_proton_folder(self):
        package.package(self.build, self.output, proton_runtime=self.runtime)
        msi = self.output / f'putgd-{VERSION}-windows-x86_64.msi'

        def table(name):
            return subprocess.run(['msiinfo', 'export', str(msi), name], check=True, capture_output=True, text=True).stdout

        files = {row.split('\t')[2] for row in table('File').splitlines()[3:]}
        self.assertLessEqual({'putgd-proton-runtime.exe', 'manifest.json', 'THIRD-PARTY-NOTICES.txt',
                              *package.WINDOWS_REDISTRIBUTION_FILES}, {name.split('|')[-1] for name in files})
        directories = {row.split('\t')[0]: row.split('\t') for row in table('Directory').splitlines()[3:]}
        self.assertEqual(directories['ProtonDir'][1:], ['INSTALLDIR', 'proton'])
        self.assertEqual(directories['ProtonRedistributionDir'][1], 'ProtonDir')
        removed = {row.split('\t')[3] for row in table('RemoveFile').splitlines()[3:]}
        self.assertLessEqual({'ProtonDir', 'ProtonRedistributionDir', 'INSTALLDIR'}, removed)


if __name__ == '__main__':
    unittest.main()
