"""Independent minisign verification: minisign_verify.py PUBKEY_LINE FILE SIGFILE"""
import base64, hashlib, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
pk = base64.b64decode(sys.argv[1]); assert pk[:2] == b'Ed'
keyid, key = pk[2:10], Ed25519PublicKey.from_public_bytes(pk[10:42])
lines = open(sys.argv[3]).read().splitlines()
sig = base64.b64decode(lines[1]); alg, sid, s = sig[:2], sig[2:10], sig[10:74]
assert sid == keyid, 'key id mismatch'
data = open(sys.argv[2], 'rb').read()
msg = hashlib.blake2b(data).digest() if alg == b'ED' else data
key.verify(s, msg)
tc = lines[2].removeprefix('trusted comment: ')
key.verify(base64.b64decode(lines[3]), s + tc.encode())
print('OK alg=%s keyid=%s trusted=%s' % (alg.decode(), keyid[::-1].hex().upper(), tc))
