"""Lab cover player contracts: output choice and the fail-closed coverage gates; no display, GTK or builder is used."""
from pathlib import Path
import contextlib
import io
import json
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
import cover_player

K72_TWO = [{'name': 'HDMI-A-1', 'rect': [0, 0, 1920, 1080], 'scale': 1},
           {'name': 'HDMI-A-2', 'rect': [1920, 0, 1422, 800], 'scale': 1}]
K72_MIRRORED = [{'name': 'HDMI-A-1', 'rect': [0, 0, 1920, 1080], 'scale': 1},
                {'name': 'HDMI-A-2', 'rect': [0, 0, 1920, 1080], 'scale': 1}]


def ppm(width, height, paint):
    """A binary PPM whose pixel (x, y) is paint(x, y)."""
    body = bytearray()
    for y in range(height):
        for x in range(width):
            body += bytes(paint(x, y))
    return b'P6\n# lab\n%d %d\n255\n' % (width, height) + bytes(body)


MAGENTA, GREY = (255, 0, 255), (40, 40, 40)


class OutputChoice(unittest.TestCase):
    def test_mirrored_outputs_are_one_logical_output(self):
        groups = cover_player.logical_outputs(K72_MIRRORED)
        self.assertEqual(groups, [{'rect': [0, 0, 1920, 1080], 'names': ['HDMI-A-1', 'HDMI-A-2']}])
        self.assertEqual(cover_player.choose_output(K72_MIRRORED)['rect'], [0, 0, 1920, 1080])

    def test_several_outputs_without_a_name_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'several_outputs: 2 distinct'):
            cover_player.choose_output(K72_TWO)

    def test_a_named_output_is_chosen_by_connector(self):
        self.assertEqual(cover_player.choose_output(K72_TWO, 'HDMI-A-2')['rect'], [1920, 0, 1422, 800])
        self.assertEqual(cover_player.choose_output(K72_MIRRORED, 'HDMI-A-2')['names'], ['HDMI-A-1', 'HDMI-A-2'])

    def test_unknown_and_missing_outputs_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'unknown_output: DP-1'):
            cover_player.choose_output(K72_TWO, 'DP-1')
        with self.assertRaisesRegex(ValueError, 'no_outputs'):
            cover_player.choose_output([])

    def test_play_refuses_before_opening_a_window(self):
        original = cover_player.session_monitors
        cover_player.session_monitors = lambda: K72_TWO
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                code = cover_player.main(['play'])
        finally:
            cover_player.session_monitors = original
        self.assertEqual(code, cover_player.EXIT_UNCOVERED)
        self.assertIn('several_outputs', json.loads(out.getvalue())['reason'])


class Coverage(unittest.TestCase):
    def test_full_half_and_empty(self):
        w, h = 64, 36
        full = bytes(MAGENTA) * (w * h)
        self.assertEqual(cover_player.coverage(w, h, full, 'rgb'), 1.0)
        half = b''.join(bytes(MAGENTA if x < w // 2 else GREY) for y in range(h) for x in range(w))
        self.assertAlmostEqual(cover_player.coverage(w, h, half, 'rgb'), 0.5, places=2)
        self.assertEqual(cover_player.coverage(w, h, half, 'rgb', [w // 2, 0, w // 2, h]), 0.0)
        self.assertEqual(cover_player.coverage(w, h, half, 'rgb', [0, 0, w // 2, h]), 1.0)

    def test_rfb_layout_is_bgrx(self):
        w, h = 16, 8
        bgrx = bytes((255, 0, 255, 0)) * (w * h)
        self.assertEqual(cover_player.coverage(w, h, bgrx, 'bgrx'), 1.0)
        green_in_rgb_order = bytes((0, 255, 0, 0)) * (w * h)
        self.assertEqual(cover_player.coverage(w, h, green_in_rgb_order, 'bgrx'), 0.0)

    def test_a_rect_beyond_the_frame_counts_as_uncovered(self):
        w, h = 32, 18
        full = bytes(MAGENTA) * (w * h)
        # A 1920-wide output checked against a smaller framebuffer must not pass.
        self.assertAlmostEqual(cover_player.coverage(w, h, full, 'rgb', [0, 0, 2 * w, h]), 0.5, places=1)
        self.assertEqual(cover_player.coverage(w, h, full, 'rgb', [w, 0, w, h]), 0.0)

    def test_sampling_keeps_large_frames_cheap_and_close(self):
        w, h = 1920, 1080
        frame = b''.join((bytes(MAGENTA) * 1440 + bytes(GREY) * 480) for _ in range(h))
        self.assertAlmostEqual(cover_player.coverage(w, h, frame, 'rgb', samples=20_000), 0.75, places=1)


class CheckFrame(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='cover-'))

    def run_check(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cover_player.main(['check-frame', *map(str, argv)])
        return code, json.loads(out.getvalue())

    def test_uncovered_frame_is_deleted_unopened(self):
        frame = self.dir / 'uncovered.ppm'
        frame.write_bytes(ppm(40, 20, lambda x, y: MAGENTA if x < 10 else GREY))
        code, report = self.run_check(frame)
        self.assertEqual(code, cover_player.EXIT_UNCOVERED)
        self.assertEqual((report['ok'], report['reason'], report['deleted']), (False, 'uncovered', True))
        self.assertFalse(frame.exists())

    def test_covered_frame_is_kept_with_its_hash(self):
        frame = self.dir / 'covered.ppm'
        frame.write_bytes(ppm(40, 20, lambda x, y: MAGENTA if x < 30 else GREY))
        code, report = self.run_check(frame, '--min-fraction', '0.7')
        self.assertEqual(code, 0)
        self.assertTrue(frame.exists())
        self.assertEqual(report['kept'], str(frame))
        self.assertEqual(len(report['sha256']), 64)

    def test_crop_keeps_only_the_covered_output(self):
        frame = self.dir / 'workspace.ppm'
        frame.write_bytes(ppm(60, 20, lambda x, y: MAGENTA if x < 40 else GREY))
        crop = self.dir / 'output.png'
        code, report = self.run_check(frame, '--rect', '0,0,40,20', '--min-fraction', '0.98', '--crop', crop)
        self.assertEqual(code, 0)
        self.assertFalse(frame.exists())
        self.assertTrue(crop.read_bytes().startswith(b'\x89PNG'))
        self.assertEqual(report['rect'], [0, 0, 40, 20])

    def test_crop_outside_the_frame_fails_closed(self):
        frame = self.dir / 'small.ppm'
        frame.write_bytes(ppm(20, 10, lambda x, y: MAGENTA))
        code, report = self.run_check(frame, '--rect', '10,0,20,10', '--min-fraction', '0.4', '--crop', self.dir / 'c.png')
        self.assertEqual(code, cover_player.EXIT_UNCOVERED)
        self.assertFalse(frame.exists())
        self.assertFalse((self.dir / 'c.png').exists())
        self.assertEqual(report['reason'], 'crop_outside_frame')

    def test_unreadable_frame_is_deleted(self):
        frame = self.dir / 'broken.ppm'
        frame.write_bytes(b'P6\n4 4\n65535\n' + b'\0' * 96)
        code, report = self.run_check(frame)
        self.assertEqual(code, cover_player.EXIT_UNCOVERED)
        self.assertFalse(frame.exists())
        self.assertTrue(report['reason'].startswith('unreadable'))


if __name__ == '__main__':
    unittest.main()
