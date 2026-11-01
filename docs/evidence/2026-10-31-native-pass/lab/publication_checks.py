"""Check published evidence bytes and the publication-only frame hardening.

These checks do not execute a historical campaign or establish desktop admission.
Run from the repository root with PYTHONPATH=. python3 <this file>.
"""
import hashlib
import json
from pathlib import Path
import re
import tempfile
import unittest
from types import SimpleNamespace

from PIL import Image
from tools.lab.surface_driver.receipts import Evidence
from tools.lab.surface_driver.recipes.outcomes import pattern_visible

ROOT = Path(__file__).resolve().parents[1]


class PublicationChecks(unittest.TestCase):
    def test_notification_inside_pattern_is_masked_before_saving(self):
        image = Image.new('RGB', (80, 60), (255, 0, 255))
        rect = (10, 10, 60, 40)
        for x in range(10, 70):
            for y in range(10, 50):
                image.putpixel((x, y), (x, y, 0))
        image.paste((255, 255, 255), (20, 20, 50, 35))
        # The historical heuristic permits this synthetic foreign notification.
        self.assertTrue(pattern_visible(image, rect))
        patch = (ROOT/'lab/native-local.patch').read_text()
        added = patch.split('+    if ctx.target.physical:\n', 1)[1].split('\n     _frame', 1)[0]
        code = 'if ctx.target.physical:\n' + '\n'.join(line[1:] for line in added.splitlines())
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            ctx = SimpleNamespace(target=SimpleNamespace(physical=True), out=out,
                                  recipe=SimpleNamespace(name='publication-test'), evidence=Evidence(out, local=True))
            exec(compile(code, 'published-pattern-frame', 'exec'), {'ctx':ctx, 'image':image})
            paths = list(out.rglob('*-verified-pattern.png'))
            self.assertEqual(len(paths), 1)
            with Image.open(paths[0]) as saved:
                self.assertIsNone(saved.getbbox())

    def test_source_index_matches_published_bytes(self):
        for entry in json.loads((ROOT/'lab-source-index.json').read_text()):
            with self.subTest(path=entry['path']):
                data = (ROOT/entry['path']).read_bytes()
                self.assertEqual(entry['sha256'], hashlib.sha256(data).hexdigest())
                self.assertEqual(entry['bytes'], len(data))

    def test_every_available_checksum_file(self):
        for manifest in ROOT.parent.rglob('SHA256SUMS'):
            for line in manifest.read_text().splitlines():
                match = re.fullmatch(r'([0-9a-f]{64})\s+\*?(.+)', line)
                if not line or line.startswith('#'):
                    continue
                self.assertIsNotNone(match, str(manifest))
                with self.subTest(manifest=str(manifest), path=match[2]):
                    self.assertEqual(hashlib.sha256((manifest.parent/match[2]).read_bytes()).hexdigest(), match[1])

    def test_terminal_publication_maps_match_sanitized_files(self):
        for path in ROOT.rglob('terminal.json'):
            data = json.loads(path.read_text())
            self.assertIn('original private', data['fileDigestScope'])
            for name, expected in data['publishedFiles'].items():
                self.assertEqual(hashlib.sha256((path.parent/name).read_bytes()).hexdigest(), expected)

    def test_growth_driver_matches_original_receipt(self):
        receipt = json.loads((ROOT/'raw/m74/b2/growth.json').read_text())
        source = (ROOT/'lab/attempts/growth-native_acceptance.py').read_bytes()
        self.assertEqual(receipt['driverSha256'], hashlib.sha256(source).hexdigest())


if __name__ == '__main__':
    unittest.main()
