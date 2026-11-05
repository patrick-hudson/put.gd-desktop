#!/usr/bin/env python3
"""Compare two PNG images within a tolerance.

The UI catalog's reference test (tools/ci/check_catalog_references.py) and the
lab drivers share this module. Its interface is stable:

    Tolerance(channel_delta, max_pixels, max_region)
    load_png(path) -> Image             RGBA, 8 bits per channel
    write_png(path, image)
    compare(expected, actual, tolerance) -> Comparison
    compare_files(expected_path, actual_path, tolerance, diff_path=None) -> Comparison
    diff_image(expected, actual, tolerance) -> Image

A pixel differs when any of its four channels moved by more than
`channel_delta`. Two images match when they have the same size, at most
`max_pixels` pixels differ, and no 8-connected group of differing pixels is
wider or taller than `max_region` pixels. Scattered rendering noise passes; a
moved, clipped or recoloured control does not.

Only the standard library is required. With Pillow and numpy installed the
work is vectorised; both paths give the same results.
"""
import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import struct
import sys
import zlib

try:
    import numpy
    from PIL import Image as _Pil
except ImportError:  # the standard-library path below
    numpy = _Pil = None

PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
# Above this many differing pixels the groups are not traced: the comparison
# fails on the pixel count anyway, and `bounds` still locates the change.
_GROUP_LIMIT = 20000


class ImageError(ValueError):
    """A file that is not a PNG this module reads."""


@dataclass(frozen=True)
class Tolerance:
    channel_delta: int = 8
    max_pixels: int = 24
    max_region: int = 6


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    rgba: bytes  # row-major, 4 bytes per pixel


@dataclass(frozen=True)
class Comparison:
    matches: bool
    reason: str               # '' when matching, otherwise why not
    expected_size: tuple      # (width, height)
    actual_size: tuple
    differing_pixels: int     # pixels beyond channel_delta
    max_channel_delta: int    # largest channel change anywhere, 0 for identical pixels
    bounds: tuple | None      # (x, y, width, height) around every differing pixel
    largest_region: tuple | None  # the same, around the widest or tallest group

    def to_json(self):
        return asdict(self)


def _chunks(data):
    if not data.startswith(PNG_SIGNATURE):
        raise ImageError('not a PNG file')
    offset = len(PNG_SIGNATURE)
    while offset + 8 <= len(data):
        length, kind = struct.unpack('>I4s', data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if len(body) != length:
            raise ImageError('truncated PNG chunk')
        yield kind, body
        offset += 12 + length
        if kind == b'IEND':
            return
    raise ImageError('PNG without IEND')


def _unfilter(raw, width, height, bpp):
    stride = width * bpp
    out = bytearray(height * stride)
    previous = bytearray(stride)
    pos = 0
    for y in range(height):
        kind = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if len(line) != stride:
            raise ImageError('truncated PNG image data')
        if kind == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif kind == 2:
            line = bytearray((a + b) & 0xFF for a, b in zip(line, previous))
        elif kind == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = previous[i]
                c = previous[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        elif kind != 0:
            raise ImageError('unknown PNG filter %d' % kind)
        out[y * stride:(y + 1) * stride] = line
        previous = line
    return bytes(out)


def _decode(data):
    header = palette = transparency = None
    compressed = []
    for kind, body in _chunks(data):
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            palette = body
        elif kind == b'tRNS':
            transparency = body
        elif kind == b'IDAT':
            compressed.append(body)
    if header is None:
        raise ImageError('PNG without IHDR')
    width, height, depth, colour, _, _, interlace = header
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colour)
    if depth != 8 or channels is None or interlace:
        raise ImageError('unsupported PNG format (depth %d, colour type %d, interlace %d)' % (depth, colour, interlace))
    pixels = _unfilter(zlib.decompress(b''.join(compressed)), width, height, channels)
    if colour == 6:
        return Image(width, height, pixels)
    out = bytearray(width * height * 4)
    if colour == 2:
        out[0::4], out[1::4], out[2::4] = pixels[0::3], pixels[1::3], pixels[2::3]
        out[3::4] = b'\xff' * (width * height)
    elif colour == 0:
        out[0::4] = out[1::4] = out[2::4] = pixels
        out[3::4] = b'\xff' * (width * height)
    elif colour == 4:
        out[0::4] = out[1::4] = out[2::4] = pixels[0::2]
        out[3::4] = pixels[1::2]
    else:
        if palette is None:
            raise ImageError('palette PNG without PLTE')
        alpha = (transparency or b'') + b'\xff' * 256
        table = [palette[3 * i:3 * i + 3] + alpha[i:i + 1] for i in range(len(palette) // 3)]
        out = bytearray(b''.join(table[i] for i in pixels))
    return Image(width, height, bytes(out))


def load_png(path):
    """Decode an 8-bit, non-interlaced PNG into RGBA."""
    if _Pil is not None:
        try:
            with _Pil.open(Path(path)) as image:
                if image.format != 'PNG':
                    raise ImageError('not a PNG file')
                image = image.convert('RGBA')
                return Image(image.width, image.height, image.tobytes())
        except FileNotFoundError:
            raise
        except (OSError, SyntaxError, ValueError) as error:  # Pillow's decode errors; one type for callers
            raise ImageError(str(error)) from error
    return _decode(Path(path).read_bytes())


def write_png(path, image):
    """Write an RGBA image as a PNG."""
    stride = image.width * 4
    raw = b''.join(b'\x00' + image.rgba[y * stride:(y + 1) * stride] for y in range(image.height))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    Path(path).write_bytes(PNG_SIGNATURE + chunk(b'IHDR', struct.pack('>IIBBBBB', image.width, image.height, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def _deltas(expected, actual):
    """Per-pixel largest channel change, as a numpy array or a list."""
    if numpy is not None:
        a = numpy.frombuffer(expected.rgba, numpy.uint8).reshape(-1, 4).astype(numpy.int16)
        b = numpy.frombuffer(actual.rgba, numpy.uint8).reshape(-1, 4).astype(numpy.int16)
        return numpy.abs(a - b).max(axis=1)
    a, b = expected.rgba, actual.rgba
    return [max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]), abs(a[i + 3] - b[i + 3]))
            for i in range(0, len(a), 4)]


def _differing(deltas, threshold):
    if numpy is not None:
        return [int(i) for i in numpy.flatnonzero(deltas > threshold)]
    return [i for i, d in enumerate(deltas) if d > threshold]


def _box(points):
    xs = [x for x, _ in points]
    ys = [y for _, y in points]
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)


def _largest_region(points):
    remaining = set(points)
    best = None
    while remaining:
        group = [remaining.pop()]
        for x, y in group:  # grows while it is walked
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    neighbour = (x + dx, y + dy)
                    if neighbour in remaining:
                        remaining.remove(neighbour)
                        group.append(neighbour)
        box = _box(group)
        if best is None or max(box[2:]) > max(best[2:]):
            best = box
    return best


def compare(expected, actual, tolerance=Tolerance()):
    """Compare two Images; see the module documentation for the rules."""
    sizes = (expected.width, expected.height), (actual.width, actual.height)
    if sizes[0] != sizes[1]:
        return Comparison(False, 'size %dx%d, expected %dx%d' % (sizes[1] + sizes[0]), *sizes, 0, 0, None, None)
    if expected.rgba == actual.rgba:
        return Comparison(True, '', *sizes, 0, 0, None, None)
    deltas = _deltas(expected, actual)
    largest = int(max(deltas))
    indices = _differing(deltas, tolerance.channel_delta)
    if not indices:
        return Comparison(True, '', *sizes, 0, largest, None, None)
    width = expected.width
    points = [(i % width, i // width) for i in indices]
    bounds = _box(points)
    region = _largest_region(points) if len(points) <= _GROUP_LIMIT else None
    problems = []
    if len(points) > tolerance.max_pixels:
        problems.append('%d pixels differ (at most %d)' % (len(points), tolerance.max_pixels))
    if region is not None and max(region[2:]) > tolerance.max_region:
        problems.append('a changed region of %dx%d at %d,%d (at most %d px across)'
                        % (region[2], region[3], region[0], region[1], tolerance.max_region))
    return Comparison(not problems, '; '.join(problems), *sizes, len(points), largest, bounds, region)


def diff_image(expected, actual, tolerance=Tolerance()):
    """The actual image faded to grey, with differing pixels in magenta.

    Images of different sizes are shown on a canvas large enough for both.
    """
    width, height = max(expected.width, actual.width), max(expected.height, actual.height)
    out = bytearray(b'\x40\x40\x40\xff' * (width * height))
    stride = actual.width * 4
    for y in range(actual.height):
        row = actual.rgba[y * stride:(y + 1) * stride]
        grey = bytes(128 + (r * 77 + g * 151 + b * 28 >> 9) for r, g, b in zip(row[0::4], row[1::4], row[2::4]))
        start = y * width * 4
        out[start:start + stride:4] = out[start + 1:start + stride:4] = out[start + 2:start + stride:4] = grey
    if (expected.width, expected.height) == (actual.width, actual.height):
        for i in _differing(_deltas(expected, actual), tolerance.channel_delta):
            out[i * 4:i * 4 + 4] = b'\xff\x00\xff\xff'
    return Image(width, height, bytes(out))


def compare_files(expected_path, actual_path, tolerance=Tolerance(), diff_path=None):
    """Compare two PNG files; write the difference image when they do not match."""
    expected, actual = load_png(expected_path), load_png(actual_path)
    result = compare(expected, actual, tolerance)
    if diff_path is not None and not result.matches:
        write_png(diff_path, diff_image(expected, actual, tolerance))
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description='Compare two PNG images within a tolerance; prints JSON.')
    parser.add_argument('expected', type=Path)
    parser.add_argument('actual', type=Path)
    parser.add_argument('--diff', type=Path, help='write a difference image here when they do not match')
    defaults = Tolerance()
    parser.add_argument('--channel-delta', type=int, default=defaults.channel_delta)
    parser.add_argument('--max-pixels', type=int, default=defaults.max_pixels)
    parser.add_argument('--max-region', type=int, default=defaults.max_region)
    args = parser.parse_args(argv)
    tolerance = Tolerance(args.channel_delta, args.max_pixels, args.max_region)
    result = compare_files(args.expected, args.actual, tolerance, args.diff)
    print(json.dumps(result.to_json(), indent=2))
    return 0 if result.matches else 1


if __name__ == '__main__':
    sys.exit(main())
