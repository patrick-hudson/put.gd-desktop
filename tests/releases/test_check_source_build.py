"""Source-build recipe contracts using synthetic command-line build tools."""
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import check_source_build


class SourceBuild(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pgsource-build-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.identity = {'gitCommit': 'a' * 40, 'gitTree': 'b' * 40, 'commitCount': 7}
        self.manifest = {**self.identity, 'sourceProvenance': 'supplied', 'version': '26.1022.7-dev+gaaaaaaaaaaaa'}
        self.helper = b'#!/usr/bin/env python3\nimport json\nprint(json.dumps({"runtime":"synthetic-proton"}))\n'
        provisioner = '''import hashlib, json, pathlib, sys
output = pathlib.Path('build/dependencies/proton-runtime')
output.mkdir(parents=True)
helper = %r
(output / 'putgd-proton-runtime').write_bytes(helper)
(output / 'putgd-proton-runtime').chmod(0o755)
(output / 'manifest.json').write_text(json.dumps({'sha256': hashlib.sha256(helper).hexdigest()}))
(output / 'provision-arguments.json').write_text(json.dumps(sys.argv[1:]))
''' % self.helper
        archive = self.root / 'source.tar.gz'
        with tarfile.open(archive, 'w:gz') as stream:
            data = provisioner.encode()
            item = tarfile.TarInfo('source/tools/proton/provision_runtime.py')
            item.size = len(data)
            stream.addfile(item, io.BytesIO(data))
        self.receipt = self.root / 'source-archive.json'
        self.receipt.write_text(json.dumps({'format': 'source.tar.gz', 'artifact': {
            'file': archive.name, 'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
            'bytes': archive.stat().st_size}, 'archiveRoot': 'source', 'identity': self.identity, 'timestamp': 1792639800,
            'localBuild': {'version': self.manifest['version']}}))
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        cmake = self.bin / 'cmake'
        cmake.write_text('''#!/usr/bin/env python3
import json, os, pathlib, shutil, sys
build = pathlib.Path('build/release')
if '--build' not in sys.argv:
    (build / 'build-identity').mkdir(parents=True)
    manifest = %r
    (build / 'build-identity/build-info.json').write_text(json.dumps(manifest))
    for name in ('putgd', 'putgd-cli', 'putgd-ui', 'putgd-capture', 'putgd-s3-worker', 'putgd-b2-worker'):
        program = build / name
        program.write_text('#!/usr/bin/env python3\\nimport json\\nprint(json.dumps(' + repr(manifest) + '))\\n')
        program.chmod(0o755)
    if '-DPUTGD_BUNDLE_PROTON=ON' in sys.argv:
        (build / 'proton').mkdir()
        helper = build / 'proton/putgd-proton-runtime'
        shutil.copyfile('build/dependencies/proton-runtime/putgd-proton-runtime', helper)
        if os.environ.get('PUTGD_SYNTHETIC_TAMPER'):
            helper.write_bytes(helper.read_bytes() + b'# altered fixture\\n')
''' % self.manifest)
        cmake.chmod(0o755)
        self.environment = patch.dict(os.environ, {'PATH': str(self.bin) + os.pathsep + os.environ['PATH']})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def build(self, **kwargs):
        return check_source_build.check_build(self.receipt, self.root / 'sdk-prefix',
                                             self.root / 'sdk-source', self.root / 'output', **kwargs)

    def test_existing_source_recipe_stays_non_proton_by_default(self):
        result = self.build()
        self.assertTrue(result['passed'])
        self.assertEqual(len(result['commands']), 2)
        self.assertNotIn('rebuiltProton', result)

    def test_proton_recipe_rebuilds_extracted_helper_then_bundles_same_bytes(self):
        result = self.build(bundle_proton=True, proton_source=self.root / 'proton-sdk',
                            proton_crypto_source=self.root / 'proton-crypto', proton_offline=True)
        self.assertTrue(result['passed'])
        source = self.root / 'output/source'
        arguments = json.loads((source / 'build/dependencies/proton-runtime/provision-arguments.json').read_text())
        self.assertEqual(arguments, ['--source', str(self.root / 'proton-sdk'), '--crypto-source',
                                     str(self.root / 'proton-crypto'), '--offline'])
        self.assertEqual((source / 'build/release/proton/putgd-proton-runtime').read_bytes(), self.helper)
        self.assertEqual(result['rebuiltProton']['sha256'], hashlib.sha256(self.helper).hexdigest())
        self.assertEqual(result['rebuiltProton']['bundledSha256'], result['rebuiltProton']['sha256'])
        self.assertIn('-DPUTGD_BUNDLE_PROTON=ON', result['commands'][1])
        self.assertEqual(result['commands'][0][1], 'tools/proton/provision_runtime.py')

    def test_rejects_a_bundled_helper_that_differs_from_rebuilt_source(self):
        with patch.dict(os.environ, {'PUTGD_SYNTHETIC_TAMPER': '1'}):
            with self.assertRaisesRegex(ValueError, 'bundled bytes'):
                self.build(bundle_proton=True)
        result = json.loads((self.root / 'output/results.json').read_text())
        self.assertFalse(result['passed'])


if __name__ == '__main__':
    unittest.main()
