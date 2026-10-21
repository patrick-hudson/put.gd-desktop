"""Synthetic AppImages with update information and their .zsync files (#293)."""
import hashlib
import struct
from pathlib import Path


def make_appimage(path: Path, information: str = '', payload: bytes = b'synthetic squashfs payload'):
    """A minimal ELF64 'runtime' with a 1024-byte .upd_info section, followed by
    a payload standing in for the squashfs image."""
    update = information.encode().ljust(1024, b'\0')
    names = b'\0.upd_info\0.shstrtab\0'
    update_offset = 64
    names_offset = update_offset + len(update)
    section_offset = names_offset + len(names) + (-len(names) % 8)
    header = b'\x7fELF' + bytes([2, 1, 1]) + b'\0' * 9
    header += struct.pack('<HHIQQQIHHHHHH', 2, 62, 1, 0, 0, section_offset, 0, 64, 0, 0, 64, 3, 2)
    table = struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    table += struct.pack('<IIQQQQIIQQ', 1, 1, 0, 0, update_offset, len(update), 0, 0, 1, 0)
    table += struct.pack('<IIQQQQIIQQ', 11, 3, 0, 0, names_offset, len(names), 0, 0, 1, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + update + names + b'\0' * (-len(names) % 8) + table + payload)
    path.chmod(0o755)


def make_zsync(path: Path, image: Path, name: str, url: str, *, block_size: int = 2048,
               length: int | None = None, version: str = '0.6.2'):
    """A zsync 0.6.2 control file for image; the block checksums are placeholders
    of the right size (collect.py checks the header and the block count)."""
    data = image.read_bytes()
    length = len(data) if length is None else length
    header = (f'zsync: {version}\nFilename: {name}\nMTime: Tue, 20 Oct 2026 18:00:00 +0000\n'
              f'Blocksize: {block_size}\nLength: {length}\nHash-Lengths: 2,2,4\nURL: {url}\n'
              f'SHA-1: {hashlib.sha1(data[:length]).hexdigest()}\n\n')
    path.write_bytes(header.encode() + b'\x5a' * (-(-length // block_size) * 6))
