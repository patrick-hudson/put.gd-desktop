"""Synthetic ELF files and staged debug symbols for the release contracts (#124)."""
import struct
from pathlib import Path


def make_elf(path: Path, build_id: str, *, debuglink: str | None = None, debug_info: bool = False):
    """A minimal little-endian ELF64 file carrying only section headers: a GNU
    build-ID note, optionally a .gnu_debuglink and a .debug_info section."""
    sections = [(b'.note.gnu.build-id', 7, _note(bytes.fromhex(build_id)))]
    if debuglink is not None:
        link = debuglink.encode() + b'\0'
        link += b'\0' * (-len(link) % 4) + b'\0\0\0\0'  # padding, then the CRC
        sections.append((b'.gnu_debuglink', 1, link))
    if debug_info:
        sections.append((b'.debug_info', 1, b'synthetic debug information'))
    names = b'\0' + b''.join(name + b'\0' for name, _, _ in sections) + b'.shstrtab\0'
    body, offsets = bytearray(), []
    for _, _, data in sections:
        offsets.append(64 + len(body))
        body += data + b'\0' * (-len(data) % 8)
    names_offset = 64 + len(body)
    body += names + b'\0' * (-len(names) % 8)
    section_offset = 64 + len(body)
    header = b'\x7fELF' + bytes([2, 1, 1]) + b'\0' * 9
    header += struct.pack('<HHIQQQIHHHHHH', 3, 62, 1, 0, 0, section_offset, 0, 64, 0, 0, 64, len(sections) + 2, len(sections) + 1)
    table = struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    position = 1
    for (name, kind, data), offset in zip(sections, offsets):
        table += struct.pack('<IIQQQQIIQQ', position, kind, 0, 0, offset, len(data), 0, 0, 4, 0)
        position += len(name) + 1
    table += struct.pack('<IIQQQQIIQQ', position, 3, 0, 0, names_offset, len(names), 0, 0, 1, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + bytes(body) + table)


def _note(descriptor: bytes) -> bytes:
    return struct.pack('<III', 4, len(descriptor), 3) + b'GNU\0' + descriptor + b'\0' * (-len(descriptor) % 4)


def build_tree(build: Path, installed: Path, identity: dict, seed: str, products=None):
    """A Release tree with split debug files and the matching installed tree."""
    import debug_symbols
    import json
    (build / 'build-identity').mkdir(parents=True, exist_ok=True)
    (build / 'build-identity/build-info.json').write_text(json.dumps(identity))
    for index, product in enumerate(products or debug_symbols.REQUIRED):
        identifier = (seed * 40)[:38] + format(index, '02x')
        make_elf(build / product, identifier, debuglink=product + '.debug')
        make_elf(build / (product + '.debug'), identifier, debug_info=True)
        target = installed / ('usr/lib/x86_64-linux-gnu/putgd/' + product if product.endswith('.so') else 'usr/bin/' + product)
        make_elf(target, identifier, debuglink=product + '.debug')
