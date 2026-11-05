#!/usr/bin/env python3
"""Exercise the actual build tool headlessly, including renderer/write failures."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

RENDERER = Path(sys.argv.pop(1)).resolve()
SIZES = (16, 22, 24, 32, 48, 64, 128, 256)


class RenderIcons(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / 'input.svg'
        self.output = self.root / 'output'
        self.source.write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">'
                               '<circle cx="12" cy="12" r="10" fill="#f0c887"/></svg>')

    def render(self):
        return subprocess.run([RENDERER, self.source, self.output], capture_output=True,
                              env=dict(os.environ, DISPLAY='', WAYLAND_DISPLAY=''), timeout=15)

    def test_all_sizes_are_pngs_and_changed_source_replaces_them(self):
        self.assertEqual(self.render().returncode, 0)
        before = {}
        for size in SIZES:
            path = self.output / f'{size}x{size}/apps/putgd.png'
            data = path.read_bytes()
            self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
            self.assertEqual(struct.unpack('>II', data[16:24]), (size, size))
            before[size] = data
        self.source.write_text(self.source.read_text().replace('#f0c887', '#008844'))
        self.assertEqual(self.render().returncode, 0)
        for size in SIZES:
            self.assertNotEqual((self.output / f'{size}x{size}/apps/putgd.png').read_bytes(), before[size])

    def test_invalid_svg_cannot_produce_successful_assets(self):
        self.source.write_text('<not-an-svg>')
        self.assertNotEqual(self.render().returncode, 0)
        self.assertFalse(self.output.exists())

    def test_output_failure_is_reported(self):
        self.output.write_text('not a directory')
        self.assertNotEqual(self.render().returncode, 0)


if __name__ == '__main__':
    unittest.main()
