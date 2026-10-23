"""repo.py TAG DIR KEYDIR: compare the pkg.put.gd apt and dnf copies with the
release assets in DIR/github and verify InRelease and the RPM signature with the
keys in KEYDIR (the keys the packages ship, extracted by extract.sh).

Writes DIR/repo.json. Run with python3 -I.
"""
import gzip, hashlib, json, lzma, os, re, subprocess, sys, tempfile, urllib.request
import xml.etree.ElementTree as ET

tag, root, keydir = sys.argv[1:4]
version = tag[1:]
channel = 'nightly' if tag.endswith('-nightly') else 'stable'
base = 'https://pkg.put.gd'
out = {'tag': tag, 'channel': channel, 'apt': {}, 'dnf': {}}


def get(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def gpgv(keyring, signed, detached=None):
    # A detached signature comes first, then the data it signs.
    args = ['gpgv', '--keyring', keyring] + ([detached, signed] if detached else [signed])
    r = subprocess.run(args, capture_output=True, text=True)
    return {'ok': r.returncode == 0, 'output': [l for l in r.stderr.splitlines() if 'Good signature' in l or 'using' in l or 'BAD' in l]}


work = tempfile.mkdtemp(prefix='repo-', dir=root)
for label in ['ubuntu24.04', 'ubuntu26.04', 'debian13']:
    dists = f'{base}/apt/{label}/dists/{channel}'
    entry = {}
    inrelease = get(f'{dists}/InRelease')
    release, release_gpg = get(f'{dists}/Release'), get(f'{dists}/Release.gpg')
    paths = {n: os.path.join(work, f'{label}-{n}') for n in ['InRelease', 'Release', 'Release.gpg']}
    for n, data in zip(paths, [inrelease, release, release_gpg]):
        open(paths[n], 'wb').write(data)
    # The key the DEB installs (keyring) and the key inline in putgd.sources (dearmored by extract.sh).
    # The other channel's key is the negative control: it must not verify.
    for keyname in [f'{label}-archive-keyring.gpg', f'{label}-sources-signed-by.gpg', 'repo-putgd-nightly.gpg', 'repo-putgd-stable.gpg']:
        keyring = os.path.join(keydir, keyname)
        entry[f'InRelease with {keyname}'] = gpgv(keyring, paths['InRelease'])
        entry[f'Release.gpg with {keyname}'] = gpgv(keyring, paths['Release'], paths['Release.gpg'])
    text = inrelease.decode()
    sums = dict((m.group(3), (m.group(1), int(m.group(2)))) for m in re.finditer(r'^ ([0-9a-f]{64}) +(\d+) (\S+)$', text.split('SHA256:')[1], re.M))
    entry['date'] = re.search(r'^Date: (.*)$', text, re.M).group(1)
    packages = get(f'{dists}/main/binary-amd64/Packages')
    entry['Packages sha256 in InRelease'] = sums.get('main/binary-amd64/Packages', [None])[0] == sha256(packages)
    stanzas = [dict(re.findall(r'^([A-Za-z0-9-]+): (.*)$', s, re.M)) for s in packages.decode().split('\n\n') if s.strip()]
    entry['versions'] = [s['Version'] for s in stanzas]
    asset = os.path.join(root, 'github', f'putgd-{version}-{label}-x86_64.deb')
    want = open(asset, 'rb').read()
    match = [s for s in stanzas if s.get('SHA256') == sha256(want)]
    entry['assetListed'] = bool(match)
    if match:
        pool = get(f'{base}/apt/{label}/{match[0]["Filename"]}')
        entry['pool'] = {'file': match[0]['Filename'], 'sha256': sha256(pool), 'identicalToAsset': pool == want,
                         'Depends': match[0].get('Depends'), 'Recommends': match[0].get('Recommends')}
    dep11 = get(f'{dists}/main/dep11/Components-amd64.yml')
    entry['dep11 sha256 in InRelease'] = sums.get('main/dep11/Components-amd64.yml', [None])[0] == sha256(dep11)
    entry['dep11'] = dep11.decode()
    out['apt'][label] = entry

repo = f'{base}/rpm/fedora/44/x86_64/{channel}'
repomd = get(f'{repo}/repodata/repomd.xml')
ns = {'r': 'http://linux.duke.edu/metadata/repo', 'c': 'http://linux.duke.edu/metadata/common'}
md = ET.fromstring(repomd)
data = {d.get('type'): (d.find('r:location', ns).get('href'), d.find('r:checksum', ns).text) for d in md.findall('r:data', ns)}
out['dnf']['repomdTypes'] = sorted(data)
try:
    get(f'{repo}/repodata/repomd.xml.asc')
    out['dnf']['repomd.xml.asc'] = True
except Exception as error:
    out['dnf']['repomd.xml.asc'] = str(error)
href, checksum = data['primary']
primary_raw = get(f'{repo}/{href}')
out['dnf']['primary checksum'] = sha256(primary_raw) == checksum
def unpack(name, raw):
    if name.endswith('.gz'):
        return gzip.decompress(raw)
    if name.endswith('.xz'):
        return lzma.decompress(raw)
    if name.endswith('.zst'):
        return subprocess.run(['zstd', '-dc'], input=raw, capture_output=True, check=True).stdout
    return raw


primary = ET.fromstring(unpack(href, primary_raw))
pkgs = []
for p in primary.findall('c:package', ns):
    v = p.find('c:version', ns)
    pkgs.append({'version': f"{v.get('ver')}-{v.get('rel')}", 'href': p.find('c:location', ns).get('href'),
                 'sha256': p.find('c:checksum', ns).text})
out['dnf']['packages'] = pkgs
asset = os.path.join(root, 'github', f'putgd-{version}-fedora44-x86_64.rpm')
want_ver = version.replace('-', '~')
mine = [p for p in pkgs if p['version'].startswith(want_ver + '-')]
if mine:
    path = os.path.join(work, 'repo.rpm')
    open(path, 'wb').write(get(f"{repo}/{mine[0]['href']}"))
    q = '%{SHA256HEADER} %{PAYLOADSHA256}\n'
    qa = subprocess.run(['rpm', '-qp', '--nosignature', '--qf', q, asset], capture_output=True, text=True).stdout.strip()
    qr = subprocess.run(['rpm', '-qp', '--nosignature', '--qf', q, path], capture_output=True, text=True).stdout.strip()
    rpm = {'file': mine[0]['href'], 'sha256': sha256(open(path, 'rb').read()), 'assetSha256': sha256(open(asset, 'rb').read()),
           'headerAndPayloadDigestsEqual': qa == qr and bool(qa), 'digests': qa}
    for keyname in ['fedora44-RPM-GPG-KEY-putgd-nightly', 'fedora44-RPM-GPG-KEY-putgd-stable', 'fedora44-RPM-GPG-KEY-putgd']:
        if not os.path.exists(os.path.join(keydir, keyname)):
            continue
        db = tempfile.mkdtemp(prefix='rpmdb-', dir=work)
        subprocess.run(['rpmkeys', '--dbpath', db, '--import', os.path.join(keydir, keyname)], check=True)
        for which, f in [('repo', path), ('asset', asset)]:
            r = subprocess.run(['rpmkeys', '--dbpath', db, '-Kv', f], capture_output=True, text=True)
            rpm[f'{which} with {keyname}'] = {'rc': r.returncode, 'out': [l.strip() for l in r.stdout.splitlines() if 'Signature' in l or 'Header' in l or 'NOKEY' in l or 'digest' in l.lower()]}
    out['dnf']['rpm'] = rpm
for kind in ['appstream', 'appstream-icons']:
    if kind in data:
        raw = get(f'{repo}/{data[kind][0]}')
        out['dnf'][kind] = {'href': data[kind][0], 'checksumOk': sha256(raw) == data[kind][1]}
        if kind == 'appstream':
            out['dnf']['appstreamXml'] = unpack(data[kind][0], raw).decode()
json.dump(out, open(os.path.join(root, 'repo.json'), 'w'), indent=1)
print(json.dumps({k: v for k, v in out.items()}, indent=1, default=str)[:6000])
