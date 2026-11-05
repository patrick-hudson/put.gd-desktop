#!/usr/bin/env python3
"""U1 of the Linux update profile: the live infrastructure agrees on one nightly.

Read-only. Needs python3, gpgv, minisign and zstd; the public keys come from the
checkout's packaging/linux/repo/keys/. Prints one JSON document; every check is
{"name", "ok", ...} and the exit code is 1 when any check fails.

  u1_infra.py TAG [--keys DIR] [--deep]

--deep downloads the mirror's AppImage and checks its SHA-256 and the .zsync's SHA-1
(about 110 MB); without it the mirror image is checked by length only.
"""
import argparse, gzip, hashlib, io, json, re, subprocess, sys, tempfile, urllib.error, urllib.request
from pathlib import Path

REPO = 'patrick-hudson/put.gd-desktop'
PKG, APP = 'https://pkg.put.gd', 'https://app.put.gd'
LABELS = ('ubuntu24.04', 'ubuntu26.04', 'debian13')
TARGETS = LABELS + ('fedora44', 'appimage')
checks = []


def check(name, ok, **details):
    checks.append({'name': name, 'ok': bool(ok), **details})
    return ok


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k): return None


def fetch(url, head=False, follow=True):
    request = urllib.request.Request(url, method='HEAD' if head else 'GET', headers={'User-Agent': 'putgd-u1-check'})
    opener = urllib.request.build_opener() if follow else urllib.request.build_opener(NoRedirect)
    try:
        with opener.open(request, timeout=60) as r:
            return r.status, dict(r.headers), (b'' if head else r.read())
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), b''


def minisign(data, sig, key):
    with tempfile.TemporaryDirectory() as d:
        Path(d, 'f').write_bytes(data); Path(d, 'f.minisig').write_bytes(sig)
        r = subprocess.run(['minisign', '-V', '-p', str(key), '-m', f'{d}/f'], capture_output=True, text=True)
    comment = next((l.split(':', 1)[1].strip() for l in sig.decode(errors='replace').splitlines() if l.startswith('trusted comment:')), None)
    return r.returncode == 0, comment


def gpgv(clearsigned, keyring):
    r = subprocess.run(['gpgv', '--keyring', str(keyring), '--output', '-'], input=clearsigned, capture_output=True)
    return r.returncode == 0, r.stdout


def release_hashes(release_text):
    """SHA256 section of a Release file: {path: (sha256, size)}."""
    out, section = {}, None
    for line in release_text.splitlines():
        if not line.startswith(' '):
            section = line.rstrip(':'); continue
        if section == 'SHA256':
            digest, size, path = line.split()
            out[path] = (digest, int(size))
    return out


def stanzas(text):
    for block in text.strip().split('\n\n'):
        fields = {}
        for line in block.splitlines():
            if line and not line.startswith(' ') and ':' in line:
                k, v = line.split(':', 1); fields[k] = v.strip()
        yield fields


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('tag'); p.add_argument('--keys', default=str(Path(__file__).resolve().parents[4] / 'packaging/linux/repo/keys'))
    p.add_argument('--deep', action='store_true')
    args = p.parse_args()
    keys, tag = Path(args.keys), args.tag
    version = tag[1:]
    channel = 'nightly' if tag.endswith('-nightly') else 'stable'
    debversion = version.replace('-nightly', '~nightly')

    # The GitHub release: its assets, their digests and SHA256SUMS.
    status, _, body = fetch(f'https://api.github.com/repos/{REPO}/releases/tags/{tag}')
    release = json.loads(body) if status == 200 else {}
    assets = {a['name']: a for a in release.get('assets', [])}
    _, _, sums_body = fetch(assets['SHA256SUMS']['browser_download_url']) if 'SHA256SUMS' in assets else (0, {}, b'')
    sums = {line.split()[1].lstrip('*'): line.split()[0] for line in sums_body.decode().splitlines() if line.strip()}
    check('github release', status == 200 and sums, tag=tag, commit=release.get('target_commitish'), published=release.get('published_at'),
          assets=len(assets))
    for name, digest in sums.items():
        given = (assets.get(name) or {}).get('digest')
        check(f'github digest {name}', given == 'sha256:' + digest, sums=digest, api=given)
    names = {t: f'putgd-{version}-x86_64.AppImage' if t == 'appimage' else
             f'putgd-{version}-{t}-x86_64.' + ('rpm' if t == 'fedora44' else 'deb') for t in TARGETS}

    # The feeds, on both hosts.
    for host in (APP, PKG):
        for ch in ('nightly', 'stable'):
            s1, _, feed = fetch(f'{host}/update/v1/{ch}.json')
            s2, _, sig = fetch(f'{host}/update/v1/{ch}.json.minisig')
            ok, comment = minisign(feed, sig, keys / f'putgd-{ch}.minisign.pub') if s1 == s2 == 200 else (False, None)
            doc = json.loads(feed) if s1 == 200 else {}
            named = (doc.get('release') or {}).get('tag')
            expect = tag if ch == channel else named
            fine = ok and named == expect and (ch != channel or all((doc['release']['assets'].get(t) or {}).get('sha256') == sums.get(names[t])
                                                                     for t in TARGETS if names[t]))
            check(f'feed {host}/{ch}', fine, status=[s1, s2], signature=ok, trustedComment=comment, names=named,
                  generatedAt=doc.get('generatedAt'))

    # /dl/<channel>/<target> permalinks.
    for target in TARGETS + ('appimage.zsync',):
        s, headers, body = fetch(f'{APP}/dl/{channel}/{target}', follow=False)
        location = headers.get('Location') or headers.get('location')
        if target == 'appimage.zsync':
            mirror = fetch(f'{PKG}/releases/{tag}/{names["appimage"]}.zsync')[2]
            check(f'dl {target}', s == 200 and body == mirror, status=s, sameAsMirror=body == mirror, bytes=len(body))
        else:
            want = f'https://github.com/{REPO}/releases/download/{tag}/{names[target]}'
            check(f'dl {target}', s in (301, 302, 307, 308) and location == want, status=s, location=location)

    # The release mirror on pkg.put.gd.
    s, _, msums = fetch(f'{PKG}/releases/{tag}/SHA256SUMS')
    s2, _, msig = fetch(f'{PKG}/releases/{tag}/SHA256SUMS.minisig')
    ok, comment = minisign(msums, msig, keys / f'putgd-{channel}.minisign.pub') if s == s2 == 200 else (False, None)
    check('mirror SHA256SUMS', msums == sums_body and ok and comment == f'put.gd {tag} {channel}', status=[s, s2],
          sameAsGithub=msums == sums_body, signature=ok, trustedComment=comment)
    image = names['appimage']
    s, headers, _ = fetch(f'{PKG}/releases/{tag}/{image}', head=True)
    length = int(headers.get('Content-Length') or headers.get('content-length') or 0)
    s3, _, zsync = fetch(f'{PKG}/releases/{tag}/{image}.zsync')
    header = dict(l.split(': ', 1) for l in zsync.split(b'\n\n', 1)[0].decode().splitlines() if ': ' in l)
    zsha = hashlib.sha256(zsync).hexdigest()
    fine = s == s3 == 200 and zsha == sums.get(image + '.zsync') and header.get('URL') == f'{PKG}/releases/{tag}/{image}' and int(header.get('Length', -1)) == length
    detail = {'status': [s, s3], 'imageLength': length, 'zsyncSha256': zsha, 'zsyncURL': header.get('URL'), 'zsyncLength': header.get('Length'),
              'zsyncSha1': header.get('SHA-1')}
    if args.deep:
        _, _, data = fetch(f'{PKG}/releases/{tag}/{image}')
        detail.update(imageSha256=hashlib.sha256(data).hexdigest(), imageSha1=hashlib.sha1(data).hexdigest())
        fine = fine and detail['imageSha256'] == sums.get(image) and detail['imageSha1'] == header.get('SHA-1')
    check('mirror AppImage and .zsync', fine, **detail)

    # apt: each label's suite.
    for label in LABELS:
        base = f'{PKG}/apt/{label}/dists/{channel}'
        s, _, inrelease = fetch(f'{base}/InRelease')
        ok, text = gpgv(inrelease, keys / f'putgd-{channel}.gpg') if s == 200 else (False, b'')
        hashes = release_hashes(text.decode())
        _, _, packages = fetch(f'{base}/main/binary-amd64/Packages')
        packages_ok = hashes.get('main/binary-amd64/Packages', ('',))[0] == hashlib.sha256(packages).hexdigest()
        entry = next((f for f in stanzas(packages.decode()) if f.get('Package') == 'putgd'), {})
        _, _, components = fetch(f'{base}/main/dep11/Components-amd64.yml.gz')
        dep11_ok = hashes.get('main/dep11/Components-amd64.yml.gz', ('',))[0] == hashlib.sha256(components).hexdigest()
        dep11 = gzip.decompress(components).decode() if components else ''
        pool = entry.get('Filename')
        s4, h4, _ = fetch(f'{PKG}/apt/{label}/{pool}', head=True) if pool else (0, {}, b'')
        fine = ok and packages_ok and dep11_ok and entry.get('Version') == debversion and entry.get('SHA256') == sums.get(names[label]) \
            and 'ID: gd.put.Putgd' in dep11 and s4 == 200 and int(h4.get('Content-Length') or 0) == int(entry.get('Size', -1))
        check(f'apt {label}', fine, inRelease=ok, packagesHash=packages_ok, version=entry.get('Version'), sha256=entry.get('SHA256'),
              githubSha256=sums.get(names[label]), dep11Hash=dep11_ok, dep11Component='ID: gd.put.Putgd' in dep11, pool=[pool, s4])

    # dnf: repomd, primary and the AppStream entries. The RPM in the repository is the GitHub asset plus the
    # channel key's signature, so its SHA-256 is compared with the publication receipt, which names both.
    base = f'{PKG}/rpm/fedora/44/x86_64/{channel}'
    s, _, repomd = fetch(f'{base}/repodata/repomd.xml')
    data = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(
        r'<data type="([^"]+)">\s*<checksum type="sha256">([0-9a-f]+)</checksum>.*?<location href="([^"]+)"/>', repomd.decode(), re.S)}
    _, _, primary_z = fetch(f'{base}/{data["primary"][1]}')
    primary_ok = hashlib.sha256(primary_z).hexdigest() == data['primary'][0]
    primary = subprocess.run(['zstd', '-dc'], input=primary_z, capture_output=True).stdout.decode() if data['primary'][1].endswith('.zst') \
        else gzip.decompress(primary_z).decode()
    rpmver = version.replace('-nightly', '~nightly')
    packages = re.findall(r'<package type="rpm">.*?<version epoch="\d+" ver="([^"]+)" rel="([^"]+)"/>.*?<checksum type="sha256" pkgid="YES">([0-9a-f]+)</checksum>'
                          r'.*?<location href="([^"]+)"/>', primary, re.S)
    newest = next((p for p in packages if p[0] == rpmver), None)
    _, _, receipt_body = fetch(f'{PKG}/receipts/{tag}.json')
    receipt = json.loads(receipt_body) if receipt_body else {}
    rpm = (receipt.get('rpm') or [{}])[0]
    appstream = data.get('appstream')
    _, _, catalog_z = fetch(f'{base}/{appstream[1]}') if appstream else (0, {}, b'')
    catalog = gzip.decompress(catalog_z).decode() if catalog_z else ''
    fine = s == 200 and primary_ok and newest and rpm.get('githubSha256') == sums.get(names['fedora44']) \
        and newest[2] == receipt.get('catalog', {}).get('fedora44', {}).get('packageSha256') and '<id>gd.put.Putgd</id>' in catalog \
        and hashlib.sha256(catalog_z).hexdigest() == appstream[0] and 'appstream-icons' in data
    check('dnf fedora44', fine, versions=[f'{v}-{r}' for v, r, *_ in packages], newest=newest, primaryHash=primary_ok,
          receiptGithubSha256=rpm.get('githubSha256'), receiptCatalogPackageSha256=receipt.get('catalog', {}).get('fedora44', {}).get('packageSha256'),
          appstream=bool(appstream), catalogComponent='<id>gd.put.Putgd</id>' in catalog)
    check('receipt', receipt.get('tag') == tag and receipt.get('result') == 'published' and receipt.get('scope') == 'live',
          commit=(receipt.get('source') or {}).get('commit'), finishedAt=receipt.get('finishedAt'), result=receipt.get('result'))

    failed = [c['name'] for c in checks if not c['ok']]
    print(json.dumps({'tag': tag, 'ok': not failed, 'failed': failed, 'checks': checks}, indent=1))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
