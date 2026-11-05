"""appimage_probe.py IMAGE: runtime size (squashfs offset), ELF sections of the runtime, sha1."""
import hashlib, json, struct, sys
p = sys.argv[1]; b = open(p, 'rb').read()
assert b[:4] == b'\x7fELF' and b[8:11] == b'AI\x02', 'not a type-2 AppImage'
shoff, = struct.unpack_from('<Q', b, 0x28); shentsize, shnum, shstrndx = struct.unpack_from('<HHH', b, 0x3A)
offset = shoff + shentsize * shnum
def sh(i):
    name, typ, flags, addr, off, size = struct.unpack_from('<IIQQQQ', b, shoff + i * shentsize)
    return name, off, size
_, stroff, _ = sh(shstrndx)
secs = {}
for i in range(shnum):
    n, off, size = sh(i)
    name = b[stroff + n: b.index(b'\0', stroff + n)].decode()
    secs[name] = (off, size)
def text(name):
    off, size = secs[name]; return b[off:off + size].rstrip(b'\0').decode(errors='replace')
out = {'bytes': len(b), 'squashfsOffset': offset, 'squashfsMagic': b[offset:offset + 4].decode(errors='replace'),
       'sha1': hashlib.sha1(b).hexdigest(), 'sha256': hashlib.sha256(b).hexdigest(),
       'runtimeSha256': hashlib.sha256(b[:offset]).hexdigest(),
       'upd_info': text('.upd_info') if '.upd_info' in secs else None,
       'sha256_sig_len': secs.get('.sha256_sig', (0, 0))[1],
       'sig_key_len': secs.get('.sig_key', (0, 0))[1],
       'sha256_sig_empty': all(c == 0 for c in b[secs['.sha256_sig'][0]:sum(secs['.sha256_sig'])]) if '.sha256_sig' in secs else None,
       'sections': sorted(secs)}
print(json.dumps(out, indent=1))
