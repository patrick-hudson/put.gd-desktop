#!/usr/bin/env python3
"""Differential check of put.gd's Minisign verifier (src/cli/Minisign.cpp) against the reference `minisign` tool.

The reference makes the keys and signatures (prehashed "ED" and legacy "Ed", trusted comments, edge-case messages); every
case is then verified by the reference and by put.gd (a small batch driver linked to libputgd_minisign), and the two must
accept and reject the same inputs. Cases are the genuine files plus hand-made mutations (line endings, whitespace, missing
or extra lines, comments, base64 variants, wrong key, swapped algorithm, empty files) and seeded byte-level mutations.

  python3 -I tools/lab/minisign_differential.py --build build/release --minisign ~/work/s3diff-1/minisign-root/usr/bin/minisign \
      --work /path/to/new/empty/dir [--seed 1] [--mutations 1500]

Prints one line per disagreement and a summary. Exit 0: the two agree on every case; 1: they differ. A disagreement where put.gd
accepts what the reference refuses is the serious direction (a forged update would pass); the reverse only refuses a valid one.
Synthetic keys only (made here, no password), nothing outside --work is written.
"""
import argparse
import base64
import itertools
import json
from pathlib import Path
import random
import subprocess
import sys

MESSAGES = {
    'empty': b'',
    'one-byte': b'x',
    'newline': b'\n',
    'nul-bytes': b'\x00\x00\x00',
    'sums': b''.join(b'%064x  putgd-1.0-%d.x86_64.rpm\n' % (i * 7919, i) for i in range(8)),
    'binary': bytes(range(256)) * 17,
    'large': bytes((i * 31) & 255 for i in range(1 << 20)),
}
COMMENTS = {
    'plain': 'put.gd v1.0.0 stable',
    'empty': '',
    'unicode': 'put.gd é中\U0001f600 nightly',
    'spaces': '  leading and trailing  ',
    'colon': 'trusted comment: nested',
    'long': 'c' * 900,
    'tab': 'a\tb',
}


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, **kwargs)


class Key:
    def __init__(self, minisign, directory, name):
        self.public, self.secret = directory / (name + '.pub'), directory / (name + '.key')
        done = run([minisign, '-G', '-W', '-f', '-p', str(self.public), '-s', str(self.secret)])
        if done.returncode:
            raise SystemExit('minisign -G failed: %s' % done.stderr.decode(errors='replace'))
        self.minisign = minisign

    def sign(self, message_path, signature_path, comment, prehash, untrusted='signature from minisign secret key'):
        command = [self.minisign, '-S', '-W', '-s', str(self.secret), '-m', str(message_path), '-x', str(signature_path), '-t', comment, '-c', untrusted]
        if not prehash:
            command.insert(2, '-l')
        done = run(command)
        if done.returncode:
            raise SystemExit('minisign -S failed (%s): %s' % ('ED' if prehash else 'Ed', done.stderr.decode(errors='replace')))


class Reference:
    def __init__(self, minisign):
        self.minisign = minisign

    def verify(self, message, signature, public_text, public_path):
        command = [self.minisign, '-V', '-m', str(message), '-x', str(signature)]
        command += ['-p', str(public_path)] if public_path else ['-P', public_text]
        done = run(command)
        trusted = ''
        for line in done.stdout.decode(errors='surrogateescape').split('\n'):
            if line.startswith('Trusted comment: '):
                trusted = line[len('Trusted comment: '):]
        return done.returncode == 0, trusted, (done.stderr or done.stdout).decode(errors='replace').strip()[:120]


class Driver:
    """put.gd's verifier in one process, fed one case per line."""

    def __init__(self, path):
        self.process = subprocess.Popen([str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def verify(self, message, signature, public_path):
        # Binary: a lone CR in a trusted comment must not be taken for the end of the reply line.
        self.process.stdin.write(('%s %s %s\n' % (message, signature, public_path)).encode())
        self.process.stdin.flush()
        line = self.process.stdout.readline().decode(errors='surrogateescape').rstrip('\n')
        if not line:
            raise SystemExit('the put.gd driver died on %s %s %s' % (message, signature, public_path))
        return line.startswith('OK'), line[3:] if line.startswith('OK ') else '', line

    def close(self):
        self.process.stdin.close()
        self.process.wait(10)


def compile_driver(build, work, source):
    out = work / 'minisign_verify_driver'
    flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'Qt6Core', 'libcrypto'], capture_output=True, text=True, check=True).stdout.split()
    library = next(Path(build).glob('libputgd_minisign.a'), None)
    if library is None:
        raise SystemExit('%s/libputgd_minisign.a is missing: build the test_minisign target first' % build)
    command = ['g++', '-std=c++20', '-fPIC', '-O1', '-I', str(source / 'src'), str(source / 'tools/lab/minisign_verify_driver.cpp'), str(library)] + flags + ['-o', str(out)]
    done = run(command)
    if done.returncode:
        raise SystemExit('driver compile failed: %s' % done.stderr.decode(errors='replace')[-800:])
    return out


def text_variants(good):
    """Hand-made mutations of a signature file: (label, bytes)."""
    lines = good.split(b'\n')
    # lines: untrusted comment, signature, trusted comment, global signature, ''
    variants = [
        ('crlf', good.replace(b'\n', b'\r\n')),
        ('no-final-newline', good.rstrip(b'\n')),
        ('extra-blank-lines', good + b'\n\n'),
        ('extra-garbage-line', good + b'garbage\n'),
        ('leading-blank-line', b'\n' + good),
        ('trailing-spaces-on-signature', b'\n'.join([lines[0], lines[1] + b'  ', lines[2], lines[3], b''])),
        ('trailing-space-on-trusted', b'\n'.join([lines[0], lines[1], lines[2] + b' ', lines[3], b''])),
        ('trailing-cr-on-trusted', b'\n'.join([lines[0], lines[1], lines[2] + b'\r', lines[3], b''])),
        ('empty-untrusted', b'\n'.join([b'untrusted comment:', lines[1], lines[2], lines[3], b''])),
        ('untrusted-no-space', b'\n'.join([b'untrusted comment:x', lines[1], lines[2], lines[3], b''])),
        ('untrusted-uppercase', b'\n'.join([lines[0].replace(b'untrusted', b'Untrusted'), lines[1], lines[2], lines[3], b''])),
        ('no-untrusted-line', b'\n'.join(lines[1:])),
        ('trusted-no-space', b'\n'.join([lines[0], lines[1], lines[2].replace(b'trusted comment: ', b'trusted comment:'), lines[3], b''])),
        ('trusted-two-spaces', b'\n'.join([lines[0], lines[1], lines[2].replace(b'trusted comment: ', b'trusted comment:  '), lines[3], b''])),
        ('trusted-uppercase', b'\n'.join([lines[0], lines[1], lines[2].replace(b'trusted', b'Trusted'), lines[3], b''])),
        ('swapped-lines-2-3', b'\n'.join([lines[0], lines[1], lines[3], lines[2], b''])),
        ('three-lines', b'\n'.join(lines[:3])),
        ('two-lines', b'\n'.join(lines[:2])),
        ('empty-file', b''),
        ('only-newlines', b'\n\n\n\n'),
        ('base64-with-space', b'\n'.join([lines[0], lines[1][:20] + b' ' + lines[1][20:], lines[2], lines[3], b''])),
        ('base64-url-alphabet', b'\n'.join([lines[0], lines[1].replace(b'+', b'-').replace(b'/', b'_'), lines[2], lines[3], b''])),
        ('signature-unpadded', b'\n'.join([lines[0], lines[1].rstrip(b'='), lines[2], lines[3], b''])),
        ('global-unpadded', b'\n'.join([lines[0], lines[1], lines[2], lines[3].rstrip(b'='), b''])),
        ('nul-in-untrusted', b'\n'.join([lines[0] + b'\x00', lines[1], lines[2], lines[3], b''])),
        ('nul-in-trusted', b'\n'.join([lines[0], lines[1], lines[2] + b'\x00', lines[3], b''])),
        ('duplicate-signature-line', b'\n'.join([lines[0], lines[1], lines[1], lines[2], lines[3], b''])),
        ('tab-after-comment-prefix', b'\n'.join([lines[0], lines[1], lines[2].replace(b'trusted comment: ', b'trusted comment:\t'), lines[3], b''])),
    ]
    sig = base64.b64decode(lines[1])
    def with_signature(raw):
        return b'\n'.join([lines[0], base64.b64encode(raw), lines[2], lines[3], b''])
    variants += [
        ('algorithm-swapped', with_signature((b'Ed' if sig[:2] == b'ED' else b'ED') + sig[2:])),
        ('algorithm-unknown', with_signature(b'XX' + sig[2:])),
        ('algorithm-lowercase', with_signature(b'ed' + sig[2:])),
        ('signature-short', with_signature(sig[:-1])),
        ('signature-long', with_signature(sig + b'\x00')),
        ('keyid-flipped', with_signature(sig[:2] + bytes([sig[2] ^ 1]) + sig[3:])),
    ]
    return variants


def public_variants(public_text, other_public_text):
    lines = public_text.split('\n')
    key_line = lines[1]
    raw = base64.b64decode(key_line)
    def make(r):
        return 'untrusted comment: minisign public key\n' + base64.b64encode(r).decode() + '\n'
    return [
        ('crlf', public_text.replace('\n', '\r\n')),
        ('no-final-newline', public_text.rstrip('\n')),
        ('extra-line', public_text + 'extra\n'),
        ('key-first-comment-second', key_line + '\n' + lines[0] + '\n'),
        ('wrong-key', other_public_text),
        ('algorithm-ED', make(b'ED' + raw[2:])),
        ('algorithm-xx', make(b'XX' + raw[2:])),
        ('short', make(raw[:-1])),
        ('long', make(raw + b'\x00')),
        ('keyid-flipped', make(raw[:2] + bytes([raw[2] ^ 1]) + raw[3:])),
        ('keybyte-flipped', make(raw[:-1] + bytes([raw[-1] ^ 1]))),
        ('empty', ''),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', required=True, help='a build directory with libputgd_minisign.a')
    parser.add_argument('--minisign', default='minisign', help='the reference minisign binary')
    parser.add_argument('--work', required=True, help='a new empty directory for keys, messages and cases')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--mutations', type=int, default=1500, help='seeded byte-level mutations of genuine signature and key files')
    options = parser.parse_args()
    work = Path(options.work)
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        raise SystemExit('--work must be an empty directory')
    source = Path(__file__).resolve().parents[2]
    driver = Driver(compile_driver(options.build, work, source))
    reference = Reference(options.minisign)
    rng = random.Random(options.seed)
    key, other = Key(options.minisign, work, 'k1'), Key(options.minisign, work, 'k2')
    public_text = key.public.read_text()
    counts = {'cases': 0, 'accepted_both': 0, 'rejected_both': 0}
    differences = []
    serial = itertools.count()

    def check(label, message, signature, public_path, public_text_value=None):
        """Verify one case with both tools; message/signature/public_path are paths."""
        counts['cases'] += 1
        use_text = public_text_value is not None
        ref_ok, ref_comment, ref_note = reference.verify(message, signature, public_text_value if use_text else None, None if use_text else public_path)
        got_ok, got_comment, got_line = driver.verify(message, signature, public_path)
        if ref_ok and got_ok:
            counts['accepted_both'] += 1
            if ref_comment != got_comment:
                differences.append((label, 'comment', ref_comment[:60], got_comment[:60]))
        elif not ref_ok and not got_ok:
            counts['rejected_both'] += 1
        else:
            kind = 'put.gd ACCEPTS what the reference refuses' if got_ok else 'put.gd refuses what the reference accepts'
            differences.append((label, kind, ref_note, got_line, 'files: %s %s %s' % (Path(message).name, Path(signature).name, Path(public_path).name)))

    def write(name, data):
        path = work / ('%s-%d' % (name, next(serial)))
        path.write_bytes(data)
        return path

    # Genuine files in every combination.
    genuine = {}
    for (mname, message), (cname, comment), prehash in itertools.product(MESSAGES.items(), COMMENTS.items(), (True, False)):
        if mname == 'large' and (cname != 'plain'):
            continue
        message_path = work / ('m-' + mname)
        if not message_path.exists():
            message_path.write_bytes(message)
        signature_path = work / ('s-%s-%s-%s' % (mname, cname, 'ED' if prehash else 'Ed'))
        try:
            key.sign(message_path, signature_path, comment, prehash)
        except SystemExit as error:
            # The reference may refuse a case (a comment it cannot sign); then there is nothing to compare.
            print('reference could not sign %s/%s/%s: %s' % (mname, cname, 'ED' if prehash else 'Ed', str(error)[:100]))
            continue
        genuine[(mname, cname, prehash)] = (message_path, signature_path)
        check('genuine %s/%s/%s' % (mname, cname, 'ED' if prehash else 'Ed'), message_path, signature_path, key.public)
        # The wrong key and a tampered message.
        check('wrong-key %s/%s' % (mname, cname), message_path, signature_path, other.public)
        if message:
            tampered = write('t', bytes([message[0] ^ 1]) + message[1:])
            check('tampered-message %s/%s' % (mname, cname), tampered, signature_path, key.public)
        else:
            check('nonempty-for-empty %s/%s' % (mname, cname), write('t', b'x'), signature_path, key.public)
        check('empty-for-signed %s' % mname, write('t', b''), signature_path, key.public) if message else None

    # Hand-made mutations of the signature file and of the key file, over the interesting messages.
    for mname in ('empty', 'one-byte', 'sums'):
        for prehash in (True, False):
            entry = genuine.get((mname, 'plain', prehash))
            if not entry:
                continue
            message_path, signature_path = entry
            good = signature_path.read_bytes()
            for label, data in text_variants(good):
                check('sigfile %s/%s/%s' % (label, mname, 'ED' if prehash else 'Ed'), message_path, write('v', data), key.public)
            for label, text in public_variants(public_text, other.public.read_text()):
                path = write('p', text.encode())
                check('pubfile %s/%s/%s' % (label, mname, 'ED' if prehash else 'Ed'), message_path, signature_path, path, public_text_value=None)
            # The public key as a bare string (-P) as put.gd's built-in key is.
            check('pubstring %s/%s' % (mname, 'ED' if prehash else 'Ed'), message_path, signature_path, write('p', (public_text.split('\n')[1]).encode()),
                  public_text_value=public_text.split('\n')[1])
    # Trusted comments with odd bytes the reference might trim: signed that way, so the global signature covers them.
    for label, comment in (('trailing-space', 'a '), ('leading-space', ' a'), ('cr', 'a\rb'), ('tab-end', 'a\t'), ('only-spaces', '   ')):
        message_path = work / 'm-sums'
        signature_path = work / ('sc-' + label)
        try:
            key.sign(message_path, signature_path, comment, True)
        except SystemExit:
            continue
        check('odd-comment %s' % label, message_path, signature_path, key.public)

    # Seeded byte-level mutations (flip, delete, insert, truncate) of the genuine files.
    pool = [(m, s) for (mn, cn, ph), (m, s) in genuine.items() if mn in ('empty', 'one-byte', 'sums') and cn in ('plain', 'unicode')]
    public_bytes = public_text.encode()
    for _ in range(options.mutations):
        message_path, signature_path = rng.choice(pool)
        data = bytearray(signature_path.read_bytes())
        target = rng.choice(('sig', 'sig', 'pub'))
        if target == 'pub':
            data = bytearray(public_bytes)
        for _ in range(rng.choice((1, 1, 1, 2, 3))):
            operation = rng.choice(('flip', 'flip', 'flip', 'delete', 'insert', 'truncate', 'dup'))
            if not data:
                break
            at = rng.randrange(len(data))
            if operation == 'flip':
                data[at] ^= 1 << rng.randrange(8)
            elif operation == 'delete':
                del data[at]
            elif operation == 'insert':
                data.insert(at, rng.choice((0x20, 0x0a, 0x0d, 0x00, 0x3d, 0x41, 0xff)))
            elif operation == 'truncate':
                del data[at:]
            else:
                data[at:at] = data[at:at + rng.randrange(1, 8)]
        if target == 'sig':
            check('mutated-sig', message_path, write('x', bytes(data)), key.public)
        else:
            check('mutated-pub', message_path, signature_path, write('x', bytes(data)))

    driver.close()
    for difference in differences:
        print('DIFFERENCE %s' % ' | '.join(str(part) for part in difference))
    forged = [d for d in differences if 'ACCEPTS' in d[1]]
    print(json.dumps({**counts, 'differences': len(differences), 'put_gd_accepts_more': len(forged)}))
    return 1 if differences else 0


if __name__ == '__main__':
    sys.exit(main())
