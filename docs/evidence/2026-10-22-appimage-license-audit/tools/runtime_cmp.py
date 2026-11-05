"""runtime_cmp.py IMAGE PINNED_RUNTIME: are the image's runtime bytes the pinned runtime apart from the writable sections?"""
import struct, sys
WRITABLE = {'.upd_info', '.digest_md5', '.sha256_sig', '.sig_key'}
def blank(b):
    b = bytearray(b); shoff, = struct.unpack_from('<Q', b, 0x28); ent, num, stri = struct.unpack_from('<HHH', b, 0x3A)
    sec = lambda i: struct.unpack_from('<IIQQQQ', b, shoff + i * ent)
    stroff = sec(stri)[4]
    for i in range(num):
        n, _, _, _, off, size = sec(i)
        if bytes(b[stroff + n: b.index(b'\0', stroff + n)]).decode() in WRITABLE:
            b[off:off + size] = bytes(size)
    return bytes(b), shoff + ent * num
pinned, plen = blank(open(sys.argv[2], 'rb').read())
image, ilen = blank(open(sys.argv[1], 'rb').read()[:plen])
print('runtime length', plen, 'image runtime length', ilen, 'identical outside writable sections:', pinned[:plen] == image[:plen])
