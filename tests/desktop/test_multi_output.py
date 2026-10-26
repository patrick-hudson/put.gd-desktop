"""Strict pixel and wall-completeness oracles; these are not desktop evidence."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/lab'))
import multi_output as campaign
import wall_probe


class PixelOracle(unittest.TestCase):
    def test_fractional_density_and_actual_negative_screen_coordinates(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow unavailable')
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder) / 'result.png'
            Image.new('RGBA', (15, 15), (*campaign.COLORS[0], 255)).save(p)
            m = [{'x': -100, 'y': -100, 'width': 128, 'height': 128}]
            self.assertTrue(campaign.pixel_oracle(p, m, [-90, -90, 10, 10], [1.5])['ok'])

    def test_one_wrong_rgb_pixel_fails_even_when_alpha_is_equal(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow unavailable; graphical driver requires it on its builder')
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder) / 'result.png'
            image = Image.new('RGBA', (10, 10), (*campaign.COLORS[0], 255)); image.save(p)
            monitors = [{'x': 0, 'y': 0, 'width': 128, 'height': 128}]
            self.assertTrue(campaign.pixel_oracle(p, monitors, [10, 10, 10, 10], [1])['ok'])
            image.putpixel((5, 5), (*campaign.COLORS[1], 255)); image.save(p)
            result = campaign.pixel_oracle(p, monitors, [10, 10, 10, 10], [1])
            self.assertFalse(result['ok']); self.assertEqual(result['mismatches'], 1)

    def test_gap_alpha_and_second_output_have_separate_checks(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow unavailable')
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder) / 'result.png'
            image = Image.new('RGBA', (30, 10), (0, 0, 0, 0))
            image.paste((*campaign.COLORS[0], 255), (0, 0, 10, 10))
            image.paste((*campaign.COLORS[4], 255), (20, 0, 30, 10)); image.save(p)
            monitors = [{'x': 0, 'y': 0, 'width': 10, 'height': 10},
                        {'x': 20, 'y': 0, 'width': 10, 'height': 10}]
            result = campaign.pixel_oracle(p, monitors, [0, 0, 30, 10], [1, 1])
            self.assertTrue(result['ok']); self.assertEqual(result['transparentGapPixels'], 100)
            image.putpixel((15, 5), (0, 0, 0, 255)); image.save(p)
            self.assertFalse(campaign.pixel_oracle(p, monitors, [0, 0, 30, 10], [1, 1])['ok'])


class AllHeadsWall(unittest.TestCase):
    def layout(self):
        return [{'name': 'head0', 'x': 0, 'y': 0, 'width': 10, 'height': 10},
                {'name': 'head1', 'x': 10, 'y': 0, 'width': 10, 'height': 10}]

    def frame(self, changes=()):
        data = bytearray(bytes([40, 30, 220, 0]) * 200)
        for x in changes:
            start = x * 4; data[start:start + 4] = bytes([255, 255, 255, 0])
        return (20, 10, bytes(data))

    def test_one_fresh_head_does_not_certify_the_other(self):
        result = wall_probe.check_frames(self.frame(), self.frame([1]), self.layout())
        self.assertFalse(result['ok'])
        self.assertTrue(result['heads'][0]['ok']); self.assertFalse(result['heads'][1]['ok'])
        self.assertTrue(wall_probe.check_frames(self.frame(), self.frame([1, 11]), self.layout())['ok'])

    def test_old_w0_stream_extent_fails_after_output_change(self):
        result = wall_probe.check_frames((10, 10, b''), self.frame([1, 11]), self.layout())
        self.assertEqual(result['reason'], 'stream-size-stale-or-incomplete')
        self.assertFalse(result['ok'])


if __name__ == '__main__':
    unittest.main()
