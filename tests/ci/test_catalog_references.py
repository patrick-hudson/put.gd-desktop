"""Render-only catalog completeness checks, using synthetic manifests."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('check_catalog_render', ROOT / 'tools/ci/check_catalog_render.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


class Checking(unittest.TestCase):
    def test_output_is_replaced_only_when_it_is_ours(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            foreign = root / 'foreign'
            foreign.mkdir()
            (foreign / 'notes.txt').write_text('keep')
            with self.assertRaises(SystemExit):
                check.prepare_output(foreign)
            self.assertEqual((foreign / 'notes.txt').read_text(), 'keep')
            ours = root / 'ours'
            ours.mkdir()
            (ours / check.REPORT).write_text('{}')
            (ours / 'old.png').write_text('x')
            check.prepare_output(ours)
            self.assertEqual(list(ours.iterdir()), [])
            check.prepare_output(root / 'new' / 'dir')
            self.assertTrue((root / 'new' / 'dir').is_dir())

    def test_main_requires_complete_renders(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'dark'

            def manifest_with(**changes):
                base = {'kind': 'synthetic-ui-catalog', 'sourceReads': 0, 'platform': 'offscreen',
                        'scope': dict(check.FULL_SCOPE), 'captures': [{'file': '10-same.png'}],
                        'images': ['10-same.png']}
                base.update(changes)
                return base

            def run(document, status=0):
                def render(*args):
                    (output / '10-same.png').write_bytes(b'synthetic image placeholder')
                    if document is not None:
                        (output / 'manifest.json').write_text(json.dumps(document))
                    return status, 0.1

                with mock.patch.object(check, 'render', side_effect=render), \
                        mock.patch('sys.stdout', new_callable=io.StringIO), \
                        mock.patch('sys.stderr', new_callable=io.StringIO):
                    result = check.main(['--catalog', 'unused', '--scheme', 'dark', '--output', str(output)])
                self.assertEqual(json.loads((output / check.REPORT).read_text())['ok'], result == 0)
                return result

            for document in (None, {'kind': 'synthetic-ui-catalog'},
                             manifest_with(kind='not-a-synthetic-catalog'), manifest_with(sourceReads=1),
                             manifest_with(captures=[{'file': '10-same.png'}, {'file': 'missing.png'}]),
                             manifest_with(images=['10-same.png', 'missing.png']),
                             manifest_with(images=[]), manifest_with(captures=[]),
                             manifest_with(scope=dict(check.FULL_SCOPE, editorOnly=True)),
                             manifest_with(scope=dict(check.FULL_SCOPE, review=False)),
                             manifest_with(scope=None), manifest_with(platform='xcb'),
                             manifest_with(skippedCaptures=[{'state': 'hidden', 'reason': 'inspector_hidden'}])):
                with self.subTest(document=document):
                    self.assertEqual(run(document), 1)
            self.assertEqual(run(manifest_with(), status=1), 1)
            self.assertEqual(run(manifest_with()), 0)


if __name__ == '__main__':
    unittest.main()
