import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
import appimage_complete_sources as complete
from complete_source_fixture import add_inventory


class CompleteSources(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pgcomplete-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.receipt = add_inventory({'build': {'version': '26.1024.1134-dev', 'gitCommit': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()},
                                      'artifact': {'file': 'synthetic.AppImage', 'sha256': 'a' * 64, 'bytes': 30}})
        self.path = self.root / 'appimage-build.json'
        self.path.write_text(json.dumps(self.receipt))

    def fake_git(self, source, cache, target):
        target.mkdir()
        (target / 'tree.tar').write_bytes(b'synthetic pinned ' + source['commit'].encode())
        return [{'file': 'tree.tar', 'treePath': '', 'commit': source['commit']}]

    def fake_apt(self, package, version, target):
        data = b'synthetic orig source'
        (target / 'orig.tar.gz').write_bytes(data)
        (target / 'source.dsc').write_text('Source: ' + package + '\nVersion: ' + version + '\nChecksums-Sha256:\n ' + hashlib.sha256(data).hexdigest() + ' ' + str(len(data)) + ' orig.tar.gz\n')

    def build(self, output):
        return complete.build(self.path, output, 1700000000, self.root / 'cache', ROOT / 'packaging/appimage/runtime-source.json',
                              git=self.fake_git, apt=self.fake_apt, fetch=lambda item, path: path.write_bytes(b'synthetic runtime source'))

    def test_complete_inventory_determinism_and_collector_binding(self):
        output = self.root / 'output'
        result = self.build(output)
        self.assertEqual(complete.verify(output, result, self.receipt).name, result['file'])
        again = self.build(self.root / 'again')
        self.assertEqual(result['sha256'], again['sha256'])
        changed = json.loads(json.dumps(self.receipt))
        changed['notices']['inventory']['components'][-1]['source']['sourceVersion'] = 'new'
        with self.assertRaisesRegex(ValueError, 'inventory differs'):
            complete.verify(output, result, changed)
        (output / result['file']).write_bytes(b'changed source bytes')
        with self.assertRaisesRegex(ValueError, 'bytes differ'):
            complete.verify(output, result, self.receipt)

    def test_missing_source_mapping_or_qt_pin_is_rejected(self):
        for field in ('source',):
            image = json.loads(json.dumps(self.receipt))
            image['notices']['inventory']['components'][2][field] = {}
            with self.assertRaisesRegex(ValueError, 'no complete source mapping'):
                complete.inventory(image)
        self.receipt['toolLock']['buildInputs']['qtLicenseSources'][0]['commit'] = 'f' * 40
        with self.assertRaisesRegex(ValueError, 'Qt source inventory'):
            complete.inventory(self.receipt)

    def test_source_package_paths_cannot_escape_staging(self):
        self.receipt['notices']['inventory']['components'][-1]['source']['sourcePackage'] = '../outside'
        with self.assertRaisesRegex(ValueError, 'no exact source package'):
            complete.inventory(self.receipt)

    def test_partial_media_inventory_cannot_pass_as_complete(self):
        self.receipt['notices']['inventory']['components'] = self.receipt['notices']['inventory']['components'][:3]
        with self.assertRaisesRegex(ValueError, 'Incomplete'):
            complete.inventory(self.receipt)

    def test_distro_sources_need_exact_identity_and_valid_checksums(self):
        target = self.root / 'distro'
        target.mkdir()
        self.fake_apt('gtk+3.0', '3.24', target)
        complete.validate_dsc(target, 'gtk+3.0', '3.24')
        with self.assertRaisesRegex(ValueError, 'identity differs'):
            complete.validate_dsc(target, 'gtk+3.0', '3.25')
        (target / 'orig.tar.gz').write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'checksum differs'):
            complete.validate_dsc(target, 'gtk+3.0', '3.24')

    def test_runtime_binary_change_requires_new_source_closure(self):
        self.receipt['toolLock']['tools'][0]['sha256'] = 'b' * 64
        with self.assertRaisesRegex(ValueError, 'another runtime binary'):
            complete.runtime_inputs(self.receipt, ROOT / 'packaging/appimage/runtime-source.json')


class RuntimeArchiveMirrors(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='pgruntime-fetch-')
        self.addCleanup(temp.cleanup)
        self.target = Path(temp.name) / 'source.tar.gz'
        self.data = b'exact pinned runtime source'
        self.item = {'file': 'source.tar.gz', 'url': 'https://upstream.invalid/source.tar.gz',
                     'mirrors': ['https://mirror.invalid/source.tar.gz'], 'algorithm': 'sha512',
                     'digest': hashlib.sha512(self.data).hexdigest()}

    def test_wrong_primary_bytes_require_the_same_pin_from_the_mirror(self):
        with patch.object(complete.urllib.request, 'urlopen', side_effect=[io.BytesIO(b'wrong'), io.BytesIO(self.data)]) as fetch:
            complete.fetch_archive(self.item, self.target)
        self.assertEqual(self.target.read_bytes(), self.data)
        self.assertEqual([c.args[0] for c in fetch.call_args_list], [self.item['url'], self.item['mirrors'][0]])

    def test_bad_mirror_bytes_cannot_bypass_verification(self):
        with patch.object(complete.urllib.request, 'urlopen', side_effect=[io.BytesIO(b'wrong'), io.BytesIO(b'also wrong')]), \
                self.assertRaisesRegex(ValueError, 'pinned checksum'):
            complete.fetch_archive(self.item, self.target)

    def test_transport_failure_can_use_a_verified_mirror(self):
        with patch.object(complete.urllib.request, 'urlopen', side_effect=[complete.urllib.error.URLError('fixture'), io.BytesIO(self.data)]):
            complete.fetch_archive(self.item, self.target)
        self.assertEqual(self.target.read_bytes(), self.data)

    def test_mirror_urls_must_be_https_without_credentials(self):
        for unsafe in ['http://mirror.invalid/source.tar.gz', 'https://user:secret@mirror.invalid/source.tar.gz']:
            self.item['mirrors'] = [unsafe]
            with patch.object(complete.urllib.request, 'urlopen') as fetch, self.assertRaisesRegex(ValueError, 'Unsafe'):
                complete.fetch_archive(self.item, self.target)
            fetch.assert_not_called()

    def test_valid_primary_does_not_fetch_the_mirror(self):
        with patch.object(complete.urllib.request, 'urlopen', return_value=io.BytesIO(self.data)) as fetch:
            complete.fetch_archive(self.item, self.target)
        self.assertEqual(fetch.call_count, 1)
        self.assertEqual(self.target.read_bytes(), self.data)


class ExactSourceInputs(unittest.TestCase):
    def test_signed_snapshot_keeps_the_requested_version(self):
        entry = {'source_package_name': 'apparmor', 'source_package_version': '4.0-8',
                 'date_published': '2026-10-01T00:00:00+00:00',
                 'date_superseded': '2026-10-03T00:00:00+00:00',
                 'self_link': 'https://api.launchpad.net/1.0/source-publication'}
        response = io.BytesIO(json.dumps({'entries': [entry]}).encode())
        results = [subprocess.CalledProcessError(100, 'apt-get'),
                   subprocess.CompletedProcess([], 0), subprocess.CompletedProcess([], 0)]
        with patch.object(complete.subprocess, 'run', side_effect=results) as run, \
                patch.object(complete.urllib.request, 'urlopen', return_value=response):
            result = complete.apt_source('apparmor', '4.0-8', Path('/synthetic'))
        self.assertEqual(result['snapshot'], '20261002T000000Z')
        self.assertIn('apparmor=4.0-8', run.call_args_list[-1].args[0])
        self.assertIn('--snapshot', run.call_args_list[-1].args[0])
        snapshot_env = run.call_args_list[-1].kwargs['env']
        self.assertIn('APT_CONFIG', snapshot_env)
        self.assertNotEqual(Path(snapshot_env['APT_CONFIG']).parent, Path('/etc/apt'))
        self.assertEqual(run.call_args_list[-2].kwargs['env']['APT_CONFIG'], snapshot_env['APT_CONFIG'])
        self.assertTrue(all('--allow-unauthenticated' not in c.args[0] for c in run.call_args_list))

    def test_another_publication_cannot_substitute_a_source_version(self):
        response = io.BytesIO(json.dumps({'entries': [
            {'source_package_name': 'apparmor', 'source_package_version': 'different'}]}).encode())
        with patch.object(complete.subprocess, 'run', side_effect=subprocess.CalledProcessError(100, 'apt-get')) as run, \
                patch.object(complete.urllib.request, 'urlopen', return_value=response), \
                self.assertRaisesRegex(ValueError, 'exact retired version'):
            complete.apt_source('apparmor', '4.0-8', Path('/synthetic'))
        self.assertEqual(run.call_count, 1)

    def test_git_archive_preserves_source_and_rejects_tag_object_pins(self):
        with tempfile.TemporaryDirectory(prefix='pgtrees-') as temp:
            root = Path(temp)
            repo = root / 'repo'
            repo.mkdir()
            (repo / 'LICENSE').write_text('synthetic licence')
            (repo / 'program.cpp').write_text('synthetic source')
            subprocess.run(['git', 'init', '-q', str(repo)], check=True)
            subprocess.run(['git', '-C', str(repo), 'add', '.'], check=True)
            subprocess.run(['git', '-C', str(repo), '-c', 'user.name=Fixture',
                            '-c', 'user.email=fixture@example.invalid', '-c', 'commit.gpgsign=false',
                            'commit', '-qm', 'fixture'], check=True)
            commit = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
            source = {'repository': 'https://example.invalid/source.git', 'commit': commit}
            cache = root / 'cache'
            cache.mkdir()
            key = hashlib.sha256((source['repository'] + '@' + commit).encode()).hexdigest()
            repo.rename(cache / key)
            records = complete.git_source(source, cache, root / 'out')
            with complete.tarfile.open(root / 'out' / records[0]['file']) as archive:
                self.assertEqual(set(archive.getnames()), {'LICENSE', 'program.cpp'})
            subprocess.run(['git', '-C', str(cache / key), '-c', 'user.name=Fixture',
                            '-c', 'user.email=fixture@example.invalid', '-c', 'tag.gpgsign=false',
                            'tag', '-am', 'annotated release', 'v1'], check=True)
            tag = subprocess.check_output(['git', '-C', str(cache / key), 'rev-parse', 'v1'], text=True).strip()
            source['commit'] = tag
            tag_key = hashlib.sha256((source['repository'] + '@' + tag).encode()).hexdigest()
            (cache / tag_key).symlink_to(cache / key, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, 'differs from component commit'):
                complete.git_source(source, cache, root / 'wrong')


if __name__ == '__main__':
    unittest.main()
