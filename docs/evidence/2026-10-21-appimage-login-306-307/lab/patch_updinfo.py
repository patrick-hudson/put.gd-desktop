#!/usr/bin/env python3
"""patch_updinfo.py IMAGE TEXT: overwrite IMAGE's .upd_info section in place (lab copies only)."""
import struct, sys
path, text = sys.argv[1], sys.argv[2].encode()
with open(path, 'r+b') as f:
    h = f.read(64); off, = struct.unpack_from('<Q', h, 0x28); size, count, names = struct.unpack_from('<HHH', h, 0x3A)
    f.seek(off); table = f.read(size * count)
    secs = [struct.unpack_from('<IIQQQQ', table, i * size) for i in range(count)]
    f.seek(secs[names][4]); strings = f.read(secs[names][5])
    for s in secs:
        if strings[s[0]:strings.index(b'\0', s[0])] == b'.upd_info':
            assert len(text) < s[5]
            f.seek(s[4]); f.write(text + b'\0' * (s[5] - len(text))); print('patched', s[5], 'bytes'); break
    else: raise SystemExit('no .upd_info')
