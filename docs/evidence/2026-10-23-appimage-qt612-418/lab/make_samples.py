#!/usr/bin/env python3
"""Malformed inputs for the #418 Qt bump: one per fixed Qt CVE class we can
reproduce from its advisory text, plus seeded fuzz of TIFF and ICNS. Output: a new directory."""
import random, struct, sys, zlib
from pathlib import Path

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=False)

# CVE-2026-8168: deep element nesting exhausts the stack (parse, cycle check, render, destroy).
depth = 200_000
(out / 'cve-2026-8168-deep-nesting.svg').write_text(
    '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">' + '<g>' * depth +
    '<rect width="64" height="64" fill="red"/>' + '</g>' * depth + '</svg>\n')
# CVE-2025-10728: a <pattern> whose content is filled with itself renders recursively.
(out / 'cve-2025-10728-pattern-recursion.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
<defs><pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">
<rect width="8" height="8" fill="url(#p)"/></pattern></defs>
<rect width="64" height="64" fill="url(#p)"/></svg>
''')
# CVE-2025-10729: a <pattern> outside a structural node is deleted after creation, then used.
(out / 'cve-2025-10729-pattern-orphan.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
<rect width="64" height="64"><pattern id="q" width="8" height="8" patternUnits="userSpaceOnUse">
<rect width="4" height="4" fill="blue"/></pattern></rect>
<rect width="64" height="64" fill="url(#q)"/></svg>
''')
# CVE-2026-6210: a marker reference that names a non-marker element (type confusion).
(out / 'cve-2026-6210-marker-confusion.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
<defs><linearGradient id="m"><stop offset="0" stop-color="red"/></linearGradient>
<rect id="r" width="4" height="4"/></defs>
<path d="M4 4 L60 60 L4 60" stroke="black" marker-start="url(#m)" marker-mid="url(#r)" marker-end="url(#m)"/></svg>
''')

def tiff(width=32, height=32):
    """A small valid baseline RGB TIFF, little endian, one strip."""
    pixels = bytes((x * 8) % 256 for x in range(width * height * 3))
    entries = [(256, 3, 1, width), (257, 3, 1, height), (258, 3, 3, 8 + 0), (259, 3, 1, 1), (262, 3, 1, 2),
               (273, 4, 1, 0), (277, 3, 1, 3), (278, 3, 1, height), (279, 4, 1, len(pixels)), (284, 3, 1, 1)]
    ifd_offset = 8
    ifd_size = 2 + 12 * len(entries) + 4
    bits_offset = ifd_offset + ifd_size
    data_offset = bits_offset + 6
    body = bytearray(b'II*\x00' + struct.pack('<I', ifd_offset))
    body += struct.pack('<H', len(entries))
    for tag, kind, count, value in entries:
        if tag == 258:
            body += struct.pack('<HHII', tag, kind, count, bits_offset)
        elif tag == 273:
            body += struct.pack('<HHII', tag, kind, count, data_offset)
        elif kind == 3:
            body += struct.pack('<HHIHH', tag, kind, count, value, 0)
        else:
            body += struct.pack('<HHII', tag, kind, count, value)
    body += struct.pack('<I', 0) + struct.pack('<HHH', 8, 8, 8) + pixels
    return bytes(body)

base = tiff()
(out / 'control-valid.tiff').write_bytes(base)
rng = random.Random(418)
header = 8 + 2 + 12 * 10 + 4 + 6
for index in range(24):
    data = bytearray(base)
    for _ in range(rng.randint(1, 12)):
        # mostly the header and IFD, where the parser decides sizes and offsets
        position = rng.randrange(header) if rng.random() < 0.85 else rng.randrange(len(data))
        data[position] = rng.choice((0, 0xff, 0x7f, 0x80, rng.randrange(256)))
    (out / f'fuzz-tiff-{index:02d}.tiff').write_bytes(bytes(data))

def icns(blocks):
    body = b''.join(name + struct.pack('>I', 8 + len(payload) if length is None else length) + payload
                    for name, length, payload in blocks)
    return b'icns' + struct.pack('>I', 8 + len(body)) + body
png = (b'\x89PNG\r\n\x1a\n' + struct.pack('>I', 13) + b'IHDR' + struct.pack('>IIBBBBB', 16, 16, 8, 2, 0, 0, 0) +
       struct.pack('>I', zlib.crc32(b'IHDR' + struct.pack('>IIBBBBB', 16, 16, 8, 2, 0, 0, 0))))
# CVE-2025-5683: block headers whose length the validation mis-bounds (OSS-Fuzz QTBUG-136707).
(out / 'cve-2025-5683-icns-block-length.icns').write_bytes(icns([(b'ic04', 0xfffffff0, png)]))
(out / 'icns-block-length-zero.icns').write_bytes(icns([(b'is32', 3, b'\x00' * 64)]))
(out / 'icns-rle-overrun.icns').write_bytes(icns([(b'is32', None, b'\x7f' + b'\xff' * 3 + b'\xff\x00' * 40), (b's8mk', None, b'\x00' * 8)]))
for index in range(8):
    data = bytearray(icns([(b'is32', None, bytes(rng.randrange(256) for _ in range(300))), (b's8mk', None, b'\xff' * 256)]))
    for _ in range(rng.randint(1, 6)):
        data[rng.randrange(len(data))] = rng.randrange(256)
    (out / f'fuzz-icns-{index:02d}.icns').write_bytes(bytes(data))
for path in sorted(out.iterdir()):
    print(path.stat().st_size, path.name)
