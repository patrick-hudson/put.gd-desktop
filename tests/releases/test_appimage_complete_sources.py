import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

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
