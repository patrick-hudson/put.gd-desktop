"""tools/ci/image_diff.py: tolerant PNG comparison shared by the catalog test and lab drivers."""
import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('image_diff', ROOT / 'tools/ci/image_diff.py')
image_diff = importlib.util.module_from_spec(spec)
sys.modules['image_diff'] = image_diff
spec.loader.exec_module(image_diff)
Image, Tolerance = image_diff.Image, image_diff.Tolerance


def solid(width, height, colour=(32, 36, 45, 255)):
    return Image(width, height, bytes(colour) * (width * height))


def paint(image, x, y, w, h, colour):
    data = bytearray(image.rgba)
    for row in range(y, y + h):
        for col in range(x, x + w):
            i = (row * image.width + col) * 4
            data[i:i + 4] = bytes(colour)
    return Image(image.width, image.height, bytes(data))


def encode(width, height, channels, colour_type, pixels, filters, extra=b''):
    """A PNG whose rows use the given filter types, so the decoder sees each one."""
    stride = width * channels
    previous = bytearray(stride)
    raw = bytearray()
    for y in range(height):
        line = pixels[y * stride:(y + 1) * stride]
        kind = filters[y % len(filters)]
        out = bytearray(stride)
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = previous[i]
            c = previous[i - channels] if i >= channels else 0
            if kind == 0:
                predictor = 0
            elif kind == 1:
                predictor = a
            elif kind == 2:
                predictor = b
            elif kind == 3:
                predictor = (a + b) >> 1
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
            out[i] = (line[i] - predictor) & 0xFF
        raw += bytes([kind]) + out
        previous = bytearray(line)

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    return (image_diff.PNG_SIGNATURE + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, colour_type, 0, 0, 0))
            + extra + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))


@contextlib.contextmanager
def standard_library_only():
    with mock.patch.object(image_diff, 'numpy', None), mock.patch.object(image_diff, '_Pil', None):
        yield


class Decoding(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def write(self, name, data):
        path = Path(self.dir.name) / name
        path.write_bytes(data)
        return path

    def test_every_filter_type_decodes(self):
        width, height = 7, 10
        rgb = bytes((x * 37 + y * 11 + c * 101) & 0xFF for y in range(height) for x in range(width) for c in range(3))
        path = self.write('rgb.png', encode(width, height, 3, 2, rgb, [0, 1, 2, 3, 4]))
        expected = bytearray()
        for i in range(0, len(rgb), 3):
            expected += rgb[i:i + 3] + b'\xff'
        with standard_library_only():
            image = image_diff.load_png(path)
        self.assertEqual((image.width, image.height, image.rgba), (width, height, bytes(expected)))
        if image_diff._Pil is not None:
            self.assertEqual(image_diff.load_png(path), image)

    def test_grey_grey_alpha_and_palette(self):
        with standard_library_only():
            grey = image_diff.load_png(self.write('g.png', encode(2, 1, 1, 0, bytes([0, 200]), [1])))
            self.assertEqual(grey.rgba, bytes([0, 0, 0, 255, 200, 200, 200, 255]))
            grey_alpha = image_diff.load_png(self.write('ga.png', encode(1, 1, 2, 4, bytes([90, 7]), [0])))
            self.assertEqual(grey_alpha.rgba, bytes([90, 90, 90, 7]))
            plte = struct.pack('>I', 6) + b'PLTE' + bytes([1, 2, 3, 4, 5, 6])
            plte += struct.pack('>I', zlib.crc32(b'PLTE' + bytes([1, 2, 3, 4, 5, 6])))
            trns = struct.pack('>I', 1) + b'tRNS' + b'\x10' + struct.pack('>I', zlib.crc32(b'tRNS\x10'))
            palette = image_diff.load_png(self.write('p.png', encode(2, 1, 1, 3, bytes([0, 1]), [0], plte + trns)))
            self.assertEqual(palette.rgba, bytes([1, 2, 3, 16, 4, 5, 6, 255]))

    def test_write_round_trips(self):
        image = paint(solid(5, 4), 1, 1, 2, 2, (250, 10, 20, 128))
        path = Path(self.dir.name) / 'out.png'
        image_diff.write_png(path, image)
        with standard_library_only():
            self.assertEqual(image_diff.load_png(path), image)

    def test_rejects_what_it_cannot_read(self):
        with standard_library_only():
            with self.assertRaises(image_diff.ImageError):
                image_diff.load_png(self.write('x.png', b'GIF89a not a png'))
            sixteen = encode(1, 1, 3, 2, bytes(3), [0]).replace(struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0),
                                                                  struct.pack('>IIBBBBB', 1, 1, 16, 2, 0, 0, 0))
            with self.assertRaises(image_diff.ImageError):
                image_diff.load_png(self.write('16.png', sixteen))
            with self.assertRaises(FileNotFoundError):
                image_diff.load_png(Path(self.dir.name) / 'missing.png')


class Comparing(unittest.TestCase):
    """Each case runs on the standard-library path and, when present, the numpy path."""

    def check(self, expected, actual, tolerance=Tolerance()):
        with standard_library_only():
            plain = image_diff.compare(expected, actual, tolerance)
        if image_diff.numpy is not None:
            self.assertEqual(image_diff.compare(expected, actual, tolerance), plain)
        return plain

    def test_identical(self):
        result = self.check(solid(40, 30), solid(40, 30))
        self.assertTrue(result.matches)
        self.assertEqual((result.differing_pixels, result.max_channel_delta, result.bounds), (0, 0, None))

    def test_noise_within_the_channel_delta_matches(self):
        base = solid(40, 30)
        noisy = paint(paint(base, 3, 3, 30, 20, (36, 40, 49, 255)), 0, 0, 1, 1, (40, 44, 53, 255))
        result = self.check(base, noisy)
        self.assertTrue(result.matches, result.reason)
        self.assertEqual((result.differing_pixels, result.max_channel_delta), (0, 8))

    def test_scattered_pixels_under_the_caps_match(self):
        base = solid(60, 40)
        noisy = base
        for x, y in [(2, 2), (20, 5), (40, 30), (55, 38), (10, 25), (11, 26)]:
            noisy = paint(noisy, x, y, 1, 1, (200, 200, 200, 255))
        result = self.check(base, noisy)
        self.assertTrue(result.matches, result.reason)
        self.assertEqual(result.differing_pixels, 6)
        self.assertEqual(result.bounds, (2, 2, 54, 37))
        self.assertEqual(max(result.largest_region[2:]), 2)

    def test_moved_control_fails(self):
        background = solid(80, 50)
        before = paint(background, 10, 10, 20, 12, (90, 140, 255, 255))
        after = paint(background, 12, 10, 20, 12, (90, 140, 255, 255))
        result = self.check(before, after)
        self.assertFalse(result.matches)
        self.assertIn('pixels differ', result.reason)
        self.assertIn('changed region', result.reason)

    def test_recoloured_control_fails(self):
        background = solid(80, 50)
        before = paint(background, 10, 10, 20, 12, (90, 140, 255, 255))
        after = paint(background, 10, 10, 20, 12, (90, 160, 255, 255))
        result = self.check(before, after)
        self.assertFalse(result.matches)
        self.assertEqual(result.differing_pixels, 240)
        self.assertEqual(result.largest_region, (10, 10, 20, 12))

    def test_clipped_edge_fails_on_region_alone(self):
        before = paint(solid(80, 50), 5, 5, 1, 20, (255, 255, 255, 255))
        result = self.check(before, solid(80, 50))
        self.assertFalse(result.matches)
        self.assertEqual(result.differing_pixels, 20)
        self.assertNotIn('pixels differ', result.reason)
        self.assertIn('changed region of 1x20', result.reason)

    def test_size_change_fails(self):
        result = self.check(solid(10, 10), solid(10, 11))
        self.assertFalse(result.matches)
        self.assertEqual(result.reason, 'size 10x11, expected 10x10')

    def test_tolerance_is_configurable(self):
        before = paint(solid(30, 30), 5, 5, 6, 5, (90, 140, 255, 255))
        after = paint(solid(30, 30), 5, 5, 6, 5, (90, 150, 255, 255))
        self.assertFalse(self.check(before, after).matches)
        self.assertTrue(self.check(before, after, Tolerance(channel_delta=10)).matches)
        self.assertTrue(self.check(before, after, Tolerance(max_pixels=30)).matches)

    def test_large_changes_skip_grouping(self):
        result = self.check(solid(200, 120), solid(200, 120, (255, 255, 255, 255)))
        self.assertFalse(result.matches)
        self.assertIsNone(result.largest_region)
        self.assertEqual(result.bounds, (0, 0, 200, 120))


class Files(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: [p.unlink() for p in self.dir.iterdir()] and self.dir.rmdir())

    def test_diff_image_marks_changes_in_magenta(self):
        before = solid(20, 10)
        after = paint(before, 4, 4, 3, 2, (255, 255, 255, 255))
        image_diff.write_png(self.dir / 'e.png', before)
        image_diff.write_png(self.dir / 'a.png', after)
        with standard_library_only():
            result = image_diff.compare_files(self.dir / 'e.png', self.dir / 'a.png', diff_path=self.dir / 'd.png')
            self.assertTrue(result.matches)
            self.assertFalse((self.dir / 'd.png').exists())
            result = image_diff.compare_files(self.dir / 'e.png', self.dir / 'a.png', Tolerance(max_pixels=2),
                                              self.dir / 'd.png')
            self.assertFalse(result.matches)
            diff = image_diff.load_png(self.dir / 'd.png')
        self.assertEqual((diff.width, diff.height), (20, 10))
        magenta = [i // 4 for i in range(0, len(diff.rgba), 4) if diff.rgba[i:i + 4] == b'\xff\x00\xff\xff']
        self.assertEqual(sorted(magenta), sorted(y * 20 + x for y in (4, 5) for x in (4, 5, 6)))

    def test_cli_reports_json_and_exit_status(self):
        image_diff.write_png(self.dir / 'e.png', solid(8, 8))
        image_diff.write_png(self.dir / 'a.png', paint(solid(8, 8), 0, 0, 8, 8, (0, 0, 0, 255)))
        output = io.StringIO()
        with standard_library_only(), contextlib.redirect_stdout(output):
            status = image_diff.main([str(self.dir / 'e.png'), str(self.dir / 'a.png'), '--diff', str(self.dir / 'd.png')])
        self.assertEqual(status, 1)
        self.assertIn('"matches": false', output.getvalue())
        self.assertTrue((self.dir / 'd.png').exists())
        with standard_library_only(), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(image_diff.main([str(self.dir / 'e.png'), str(self.dir / 'e.png')]), 0)


if __name__ == '__main__':
    unittest.main()
