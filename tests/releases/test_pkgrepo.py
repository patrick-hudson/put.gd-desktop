"""The package signer only publishes verified releases, in a safe order, and the feed only afterwards (#293)."""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import pkgrepo
import update_feed

TAG = 'v0.1.1-nightly.600'
VERSION = '0.1.1-nightly.600'
PACKAGE_VERSION = '0.1.1~nightly.600'
COMMIT = 'a' * 40
LABELS = ('ubuntu24.04', 'ubuntu26.04', 'debian13', 'fedora44')
KEYS = Path(__file__).resolve().parents[2] / 'packaging/linux/repo/keys'


def asset_name(label, version=VERSION):
    return f'putgd-{version}-{label}-x86_64.' + ('rpm' if label == 'fedora44' else 'deb')


class FakeGitHub:
    """Serves a release whose bytes live in a dict; tests then corrupt one thing."""

    def __init__(self, files, commit=COMMIT, digests=True):
        self.files, self.commit, self.digests = files, commit, digests

    def release(self, tag=TAG):
        assets = [{'name': name, 'size': len(data), 'state': 'uploaded',
                   'browser_download_url': f'https://github.com/{pkgrepo.REPOSITORY}/releases/download/{tag}/{name}',
                   **({'digest': 'sha256:' + hashlib.sha256(data).hexdigest()} if self.digests else {})}
                  for name, data in sorted(self.files.items())]
        return {'tag_name': tag, 'draft': False, 'prerelease': 'nightly' in tag, 'published_at': '2026-10-21T05:55:50Z',
                'html_url': f'https://github.com/{pkgrepo.REPOSITORY}/releases/tag/{tag}', 'assets': assets}

    def tag_commit(self, tag):
        return self.commit

    def download(self, url, destination, size):
        data = self.files[url.rsplit('/', 1)[1]]
        if len(data) != size:
            raise pkgrepo.SignerError('size')
        destination.write_bytes(data)


def release_files(tamper=None):
    files = {asset_name(label): f'{label} package bytes'.encode() for label in LABELS}
    files['putgd-0.1.1-nightly.600-x86_64.AppImage'] = b'appimage'
    builds = {'release': {'tag': TAG, 'version': VERSION, 'packageVersion': PACKAGE_VERSION, 'gitCommit': COMMIT},
              'receipts': {label: {'package': {'sha256': hashlib.sha256(files[asset_name(label)]).hexdigest(),
                                               'size': len(files[asset_name(label)]),
                                               'format': 'RPM' if label == 'fedora44' else 'DEB'}} for label in LABELS}}
    if tamper:
        tamper(files, builds)
    files['release-builds.json'] = json.dumps(builds).encode()
    files['SHA256SUMS'] = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n'
                                  for name, data in sorted(files.items())).encode()
    return files


def fake_fields(command, **_):
    """Stands in for dpkg-deb -f / rpm -qp: every package states the expected fields."""
    rpm = command[0] == 'rpm'
    out = f'putgd\n{PACKAGE_VERSION}\nx86_64\n' if rpm else f'Package: putgd\nVersion: {PACKAGE_VERSION}\nArchitecture: amd64\n'
    return subprocess.CompletedProcess(command, 0, out, '')


class Versions(unittest.TestCase):
    def test_package_order(self):
        order = ['0.1.1-nightly.9', '0.1.1-nightly.10', '0.1.1-rc.1', '0.1.1', '0.1.2-nightly.1', '0.2.0']
        self.assertEqual(sorted(order, key=pkgrepo.version_key), order)

    def test_channels(self):
        self.assertEqual(pkgrepo.repo_channel('v0.1.1-nightly.5'), 'nightly')
        self.assertEqual(pkgrepo.repo_channel('v0.1.1'), 'stable')
        self.assertIsNone(pkgrepo.repo_channel('v0.1.1-rc.1'))
        with self.assertRaises(ValueError):
            pkgrepo.repo_channel('0.1.1')

    def test_rpm_versions_from_header_names(self):
        self.assertEqual(pkgrepo.rpm_version('putgd-0.1.1~nightly.588-1.fc44.x86_64.rpm'), '0.1.1-nightly.588')
        self.assertEqual(pkgrepo.rpm_version('putgd-0.1.2-1.fc44.x86_64.rpm'), '0.1.2')


class Verification(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgrepo-')
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)

    def verify(self, github):
        return pkgrepo.verify_release(github.release(), github, self.work, fake_fields)

    def test_verified_release_names_every_package_and_its_checks(self):
        inputs = self.verify(FakeGitHub(release_files()))
        self.assertEqual(sorted(inputs['packages']), sorted(LABELS))
        self.assertEqual((inputs['commit'], inputs['channel'], inputs['packageVersion']), (COMMIT, 'nightly', PACKAGE_VERSION))
        self.assertIn('GitHub digest', inputs['packages']['debian13']['checks'])

    def test_each_mismatch_refuses_the_release(self):
        def corrupt_package(files, builds):
            files[asset_name('debian13')] = b'other bytes'

        def wrong_version(files, builds):
            builds['release']['packageVersion'] = '0.1.1~nightly.599'

        def missing_label(files, builds):
            del files[asset_name('fedora44')]
        cases = {
            'receipt': FakeGitHub(release_files(corrupt_package)),
            'version': FakeGitHub(release_files(wrong_version)),
            'missing': FakeGitHub(release_files(missing_label)),
            'commit': FakeGitHub(release_files(), commit='b' * 40),
        }
        for name, github in cases.items():
            with self.subTest(name), self.assertRaises(pkgrepo.SignerError):
                self.verify(github)

    def test_github_digest_and_sums_must_agree(self):
        github = FakeGitHub(release_files())
        release = github.release()
        next(a for a in release['assets'] if a['name'] == asset_name('ubuntu24.04'))['digest'] = 'sha256:' + '0' * 64
        with self.assertRaisesRegex(pkgrepo.SignerError, 'digest'):
            pkgrepo.verify_release(release, github, self.work, fake_fields)
        files = release_files()
        files['unlisted.txt'] = b'not in SHA256SUMS'
        with self.assertRaisesRegex(pkgrepo.SignerError, 'exactly'):
            self.verify(FakeGitHub(files))

    def test_package_fields_must_match_the_tag(self):
        def lying(command, **_):
            return subprocess.CompletedProcess(command, 0, 'Package: putgd\nVersion: 9.9.9\nArchitecture: amd64\n', '')
        github = FakeGitHub(release_files())
        with self.assertRaisesRegex(pkgrepo.SignerError, 'expected'):
            pkgrepo.verify_release(github.release(), github, self.work, lying)

    def test_drafts_candidates_and_unsafe_names_are_refused(self):
        github = FakeGitHub(release_files())
        for change in ({'draft': True}, {'tag_name': 'v0.1.1-rc.1', 'prerelease': True}, {'prerelease': False}):
            with self.subTest(change), self.assertRaises(pkgrepo.SignerError):
                pkgrepo.verify_release({**github.release(), **change}, github, self.work, fake_fields)
        release = github.release()
        release['assets'][0] = {**release['assets'][0], 'name': '../escape.deb'}
        with self.assertRaisesRegex(pkgrepo.SignerError, 'Unsafe'):
            pkgrepo.verify_release(release, github, self.work, fake_fields)


class FakeS3:
    def __init__(self):
        self.objects, self.calls = {}, []

    def put_object(self, Bucket, Key, Body, ContentType, CacheControl):
        data = Body if isinstance(Body, bytes) else Body.read()
        self.objects[Key] = (data, ContentType, CacheControl)
        self.calls.append(('put', Key))

    def delete_object(self, Bucket, Key):
        self.objects.pop(Key)
        self.calls.append(('delete', Key))


class FakeCloudFront:
    def __init__(self):
        self.batches, self.polls = [], 0

    def create_invalidation(self, DistributionId, InvalidationBatch):
        self.batches.append((DistributionId, InvalidationBatch['Paths']['Items']))
        return {'Invalidation': {'Id': f'I{len(self.batches)}'}}

    def get_invalidation(self, DistributionId, Id):
        self.polls += 1
        return {'Invalidation': {'Status': 'Completed' if self.polls % 2 == 0 else 'InProgress'}}


def config(root, **changes):
    values = dict(state=root / 'state', gnupghome=root / 'gnupg', minisign_key=root / 'minisign.key', keys=KEYS,
                  nightly_fingerprint='A' * 40, stable_fingerprint='B' * 40, delete_after=3600)
    values.update(changes)
    return pkgrepo.Config(**values)


def write_tree(tree, files):
    for relative, data in files.items():
        path = tree / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


TREE = {
    'apt/debian13/pool/main/p/putgd/putgd_0.1.1~nightly.600_amd64.deb': b'deb',
    'apt/debian13/dists/nightly/main/binary-amd64/Packages': b'Package: putgd\n',
    'apt/debian13/dists/nightly/InRelease': b'signed',
    'apt/debian13/dists/nightly/Release': b'release',
    'rpm/fedora/44/x86_64/nightly/Packages/putgd-0.1.1~nightly.600-1.fc44.x86_64.rpm': b'rpm',
    'rpm/fedora/44/x86_64/nightly/repodata/' + 'c' * 64 + '-primary.xml.zst': b'primary',
    'rpm/fedora/44/x86_64/nightly/repodata/repomd.xml': b'repomd',
    'releases/v0.1.1-nightly.600/SHA256SUMS': b'sums',
    'update/v1/nightly.json': b'{}',
}


class Publishing(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgpub-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.tree = self.root / 'tree'
        write_tree(self.tree, TREE)
        self.s3, self.cloudfront, self.now = FakeS3(), FakeCloudFront(), [1000.0]
        self.receipt = pkgrepo.new_receipt({'tag': TAG}, 'lab')

    def publisher(self, scope='lab', http=None):
        return pkgrepo.Publisher(config(self.root), scope, 'nightly', self.s3, self.cloudfront, http=http,
                                 clock=lambda: self.now[0], sleep=lambda seconds: self.now.__setitem__(0, self.now[0] + seconds))

    def test_packages_then_indexes_then_signed_metadata_and_the_feed_only_on_request(self):
        changed = self.publisher().upload(self.tree, {0, 1, 2, 3}, self.receipt)
        keys = [key for verb, key in self.s3.calls]
        self.assertTrue(all(key.startswith('lab/') for key in keys))
        self.assertNotIn('lab/update/v1/nightly.json', keys)
        order = [pkgrepo.phase(key[len('lab/'):]) for key in keys]
        self.assertEqual(order, sorted(order))
        self.assertLess(keys.index('lab/apt/debian13/dists/nightly/main/binary-amd64/Packages'),
                        keys.index('lab/apt/debian13/dists/nightly/InRelease'))
        # Immutable objects are never invalidated; everything at a fixed path is.
        self.assertNotIn('lab/apt/debian13/pool/main/p/putgd/putgd_0.1.1~nightly.600_amd64.deb', changed)
        self.assertIn('lab/rpm/fedora/44/x86_64/nightly/repodata/repomd.xml', changed)
        data, kind, cache = self.s3.objects['lab/apt/debian13/pool/main/p/putgd/putgd_0.1.1~nightly.600_amd64.deb']
        self.assertEqual((kind, cache), ('application/vnd.debian.binary-package', pkgrepo.IMMUTABLE))
        self.assertEqual(self.s3.objects['lab/rpm/fedora/44/x86_64/nightly/repodata/repomd.xml'][2], pkgrepo.SHORT)
        self.assertEqual(self.publisher().upload(self.tree, {4}, self.receipt), ['lab/update/v1/nightly.json'])

    def test_unchanged_files_are_not_uploaded_again(self):
        self.publisher().upload(self.tree, {0, 1, 2, 3, 4}, self.receipt)
        self.s3.calls.clear()
        (self.tree / 'apt/debian13/dists/nightly/InRelease').write_bytes(b'signed again')
        self.assertEqual(self.publisher().upload(self.tree, {0, 1, 2, 3, 4}, self.receipt),
                         ['lab/apt/debian13/dists/nightly/InRelease'])
        self.assertEqual(self.s3.calls, [('put', 'lab/apt/debian13/dists/nightly/InRelease')])

    def test_objects_that_leave_the_tree_are_deleted_only_after_the_grace_period(self):
        self.publisher('live').upload(self.tree, {0, 1, 2, 3, 4}, self.receipt)
        old = self.tree / 'apt/debian13/pool/main/p/putgd/putgd_0.1.1~nightly.600_amd64.deb'
        old.unlink()
        self.assertEqual(self.publisher('live').retire(self.tree, self.receipt), [])
        self.now[0] += 3599
        self.assertEqual(self.publisher('live').retire(self.tree, self.receipt), [])
        self.now[0] += 1
        self.assertEqual(self.publisher('live').retire(self.tree, self.receipt),
                         ['apt/debian13/pool/main/p/putgd/putgd_0.1.1~nightly.600_amd64.deb'])
        self.assertNotIn('apt/debian13/pool/main/p/putgd/putgd_0.1.1~nightly.600_amd64.deb', self.s3.objects)
        # A file that comes back before its deletion is kept.
        stale = self.tree / 'update/v1/nightly.json'
        stale.unlink()
        self.publisher('live').retire(self.tree, self.receipt)
        write_tree(self.tree, {'update/v1/nightly.json': b'{"new": 1}'})
        self.publisher('live').upload(self.tree, {4}, self.receipt)
        self.now[0] += 7200
        self.assertEqual(self.publisher('live').retire(self.tree, self.receipt), [])

    def test_invalidation_waits_for_completion_on_each_distribution(self):
        self.publisher().invalidate(['lab/a', 'lab/b'], self.receipt, ['D1', 'D2'])
        self.assertEqual([batch[0] for batch in self.cloudfront.batches], ['D1', 'D2'])
        self.assertEqual(self.cloudfront.batches[0][1], ['/lab/a', '/lab/b'])
        self.assertEqual(len(self.receipt['invalidations']), 2)

    def test_readback_waits_for_the_cdn_and_gives_up_at_the_deadline(self):
        served = [b'old', b'old', TREE['update/v1/nightly.json']]
        self.publisher(http=lambda url: served.pop(0)).readback(self.tree, ['update/v1/nightly.json'], self.receipt)
        self.assertEqual(self.receipt['readback'][0]['url'], 'https://pkg.put.gd/lab/update/v1/nightly.json')
        with self.assertRaisesRegex(pkgrepo.SignerError, 'does not serve'):
            self.publisher(http=lambda url: b'old').readback(self.tree, ['update/v1/nightly.json'], self.receipt)


class Metadata(unittest.TestCase):
    def test_stable_suites_are_never_signed_on_the_builder(self):
        self.assertIn('SignWith: ' + 'A' * 40, pkgrepo.distributions_stanza('nightly', 'debian13', 'A' * 40))
        self.assertNotIn('SignWith', pkgrepo.distributions_stanza('stable', 'debian13', None))

    def test_clearsigned_body_undoes_dash_escaping(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'InRelease'
            path.write_text('-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA512\n\nOrigin: put.gd\n- -dash\n'
                            '-----BEGIN PGP SIGNATURE-----\nxx\n-----END PGP SIGNATURE-----\n')
            self.assertEqual(pkgrepo.clearsigned_body(path), 'Origin: put.gd\n-dash\n')

    def test_public_keys_are_public_and_the_keyrings_match(self):
        for channel in ('nightly', 'stable'):
            armored = (KEYS / f'putgd-{channel}.asc').read_text()
            self.assertTrue(armored.startswith('-----BEGIN PGP PUBLIC KEY BLOCK-----'))
            self.assertNotIn('PRIVATE', armored + (KEYS / f'putgd-{channel}.minisign.pub').read_text())
            lines = armored.strip().splitlines()
            body = ''.join(line for line in lines[lines.index('') + 1:] if not line.startswith(('-----', '=')))
            import base64
            self.assertEqual(base64.b64decode(body), (KEYS / f'putgd-{channel}.gpg').read_bytes())

    def test_signer_feed_matches_contract(self):
        github = FakeGitHub(release_files())
        sums = update_feed.parse_sums(release_files()['SHA256SUMS'].decode())
        document = update_feed.feed('nightly', github.release(), sums, '2026-10-21T06:00:00Z')
        update_feed.check_feed(document)
        self.assertEqual(sorted(document['release']['assets']),
                         ['appimage', 'debian13', 'fedora44', 'ubuntu24.04', 'ubuntu26.04'])
        self.assertNotIn('zsync', document['release']['assets']['appimage'])
        broken = json.loads(json.dumps(document))
        broken['release']['assets']['debian13']['zsync'] = 'x'
        with self.assertRaises(ValueError):
            update_feed.check_feed(broken)
        self.assertIsNone(update_feed.feed('stable', None, {}, '2026-10-21T06:00:00Z')['release'])


TOOLS = all(shutil.which(tool) for tool in ('reprepro', 'createrepo_c', 'gpg', 'gpgv', 'minisign', 'dpkg-deb', 'rpmbuild'))


@unittest.skipUnless(TOOLS and os.environ.get('PUTGD_SIGNER_TOOLS') == '1',
                     'needs reprepro, createrepo_c, gpg, minisign, dpkg-deb and rpmbuild (PUTGD_SIGNER_TOOLS=1, signer host)')
def make_key(env, name):
    subprocess.run(['gpg', '--batch', '--pinentry-mode', 'loopback', '--passphrase', '', '--quick-generate-key',
                    f'{name} <t@example.invalid>', 'rsa3072', 'sign', '1d'], env=env, check=True, capture_output=True)
    listing = subprocess.run(['gpg', '--with-colons', '--list-keys', name], env=env, check=True, capture_output=True,
                             text=True).stdout
    return next(line.split(':')[9] for line in listing.splitlines() if line.startswith('fpr'))


def tool_fixture(root, version, package_version):
    """Throwaway nightly and stable keys (GPG and minisign) and fake DEB/RPM assets for one version."""
    gnupg = root / 'g'
    gnupg.mkdir(mode=0o700, exist_ok=True)
    env = dict(os.environ, GNUPGHOME=str(gnupg))
    keys = root / 'keys'
    keys.mkdir(exist_ok=True)
    fingerprints = {}
    for channel in ('nightly', 'stable'):
        fingerprints[channel] = make_key(env, 'test ' + channel)
        for suffix, armor in (('gpg', []), ('asc', ['--armor'])):
            (keys / f'putgd-{channel}.{suffix}').write_bytes(subprocess.run(
                ['gpg', *armor, '--export', fingerprints[channel]], env=env, check=True, capture_output=True).stdout)
        subprocess.run(['minisign', '-G', '-W', '-p', str(keys / f'putgd-{channel}.minisign.pub'), '-s',
                        str(root / f'{channel}.key')], check=True, capture_output=True)
    assets = root / 'assets'
    assets.mkdir()
    for label in LABELS[:3]:
        package = root / ('deb-' + label)
        (package / 'DEBIAN').mkdir(parents=True)
        (package / 'DEBIAN/control').write_text(
            f'Package: putgd\nVersion: {package_version}\nArchitecture: amd64\nMaintainer: t\nSection: x11\n'
            f'Priority: optional\nDescription: test {label}\n')
        subprocess.run(['dpkg-deb', '--build', '--root-owner-group', str(package), str(assets / asset_name(label, version))],
                       check=True, capture_output=True)
    spec = root / 'putgd.spec'
    spec.write_text(f'Name: putgd\nVersion: {package_version}\nRelease: 1.fc44\nSummary: t\nLicense: MIT\n'
                    'BuildArch: x86_64\n%description\nt\n%files\n')
    subprocess.run(['rpmbuild', '-bb', '--define', f'_topdir {root / "rpm"}', '--define', '_build_id_links none',
                    str(spec)], check=True, capture_output=True)
    shutil.copy2(next((root / 'rpm/RPMS').rglob('*.rpm')), assets / asset_name('fedora44', version))
    (assets / 'SHA256SUMS').write_text(''.join(f'{pkgrepo.sha256_file(path)}  {path.name}\n'
                                               for path in sorted(assets.iterdir())))
    settings = config(root, gnupghome=gnupg, minisign_key=root / 'nightly.key', keys=keys,
                      nightly_fingerprint=fingerprints['nightly'], stable_fingerprint=fingerprints['stable'])
    return settings, env, assets, fingerprints


class CompletedCloudFront(FakeCloudFront):
    def get_invalidation(self, DistributionId, Id):
        return {'Invalidation': {'Status': 'Completed'}}


class RealTools(unittest.TestCase):
    """Builds and signs real repositories from fake packages with throwaway keys; no network or AWS."""

    def test_nightly_workspace_is_signed_and_verifies(self):
        with tempfile.TemporaryDirectory(prefix='pgtools-') as directory:
            root = Path(directory)
            settings, env, assets, fingerprints = tool_fixture(root, VERSION, PACKAGE_VERSION)
            keys = settings.keys
            signer = pkgrepo.Signer(settings, github=object(), clients=(None, None))
            inputs = pkgrepo.local_inputs(TAG, assets)
            receipt = pkgrepo.new_receipt(inputs, 'lab')
            workspace = signer.build_next('lab', 'nightly', inputs, receipt)
            signer.sign_nightly(workspace, inputs, receipt)
            tree = workspace.tree
            self.assertTrue((tree / 'apt/debian13/pool/main/p/putgd' / f'putgd_{PACKAGE_VERSION}_amd64.deb').exists())
            packages = (tree / 'apt/ubuntu24.04/dists/nightly/main/binary-amd64/Packages').read_text()
            self.assertIn(f'Version: {PACKAGE_VERSION}', packages)
            self.assertIn(pkgrepo.sha256_file(assets / asset_name('ubuntu24.04')), packages)
            self.assertTrue(pkgrepo.verify_gpg(keys / 'putgd-nightly.gpg', tree / 'apt/debian13/dists/nightly/InRelease'))
            # The repository RPM is the built one plus a nightly signature; the receipt maps the digests.
            signed = next((tree / 'rpm/fedora/44/x86_64/nightly/Packages').glob('putgd-*.rpm'))
            self.assertTrue(pkgrepo.verify_rpm(keys / 'putgd-nightly.asc', signed))
            self.assertEqual(pkgrepo.rpm_digests(signed), pkgrepo.rpm_digests(assets / asset_name('fedora44')))
            self.assertEqual(receipt['rpm'][0]['githubSha256'], pkgrepo.sha256_file(assets / asset_name('fedora44')))
            self.assertEqual(receipt['rpm'][0]['repoSha256'], pkgrepo.sha256_file(signed))
            self.assertFalse(pkgrepo.verify_rpm(keys / 'putgd-nightly.asc', assets / asset_name('fedora44')))
            self.assertFalse((tree / 'rpm/fedora/44/x86_64/nightly/repodata/repomd.xml.asc').exists())
            sums = tree / f'releases/{TAG}/SHA256SUMS'
            self.assertTrue(pkgrepo.verify_minisign(keys / 'putgd-nightly.minisign.pub', sums, sums.with_name('SHA256SUMS.minisig')))
            update_feed.check_feed(json.loads((tree / 'update/v1/nightly.json').read_text()))
            # A flipped byte is refused.
            inrelease = tree / 'apt/debian13/dists/nightly/InRelease'
            inrelease.write_text(inrelease.read_text().replace('amd64', 'amd65', 1))
            self.assertFalse(pkgrepo.verify_gpg(keys / 'putgd-nightly.gpg', inrelease))
            self.assertEqual({entry['kind'] for entry in receipt['signatures']}, {'openpgp', 'minisign', 'rpm'})


if __name__ == '__main__':
    unittest.main()


@unittest.skipUnless(TOOLS and os.environ.get('PUTGD_SIGNER_TOOLS') == '1', 'signer host tools, as RealTools')
class StableRoundTrip(unittest.TestCase):
    """Stable is staged unsigned, signed elsewhere with the stable key, and published only after checking."""

    def test_stage_sign_and_publish(self):
        with tempfile.TemporaryDirectory(prefix='pgstable-') as directory:
            root = Path(directory)
            settings, env, assets, fingerprints = tool_fixture(root, '0.1.1', '0.1.1')
            sums = update_feed.parse_sums((assets / 'SHA256SUMS').read_text())
            packages = {label: {'name': asset_name(label, '0.1.1'), 'path': assets / asset_name(label, '0.1.1'),
                                'sha256': sums[asset_name(label, '0.1.1')], 'size': 1, 'checks': []} for label in LABELS}
            release = FakeGitHub({p['name']: p['path'].read_bytes() for p in packages.values()}).release('v0.1.1')
            inputs = {'tag': 'v0.1.1', 'channel': 'stable', 'version': '0.1.1', 'packageVersion': '0.1.1', 'commit': COMMIT,
                      'release': release, 'sums': sums, 'sumsPath': assets / 'SHA256SUMS', 'packages': packages,
                      'checks': [], 'source': 'test'}
            s3 = FakeS3()
            signer = pkgrepo.Signer(settings, github=object(), clients=(s3, CompletedCloudFront()), log=lambda line: None,
                                    http=lambda url: s3.objects[url.split('pkg.put.gd/', 1)[1]][0])
            stage = signer.stage_stable('lab', inputs)
            manifest = json.loads((stage / 'manifest.json').read_text())
            self.assertFalse(signer.stable_signed(stage))
            self.assertFalse(any((stage / 'workspace/tree').rglob('InRelease')))
            # The workstation's part, with the throwaway stable key.
            out = stage / 'signatures'
            tree = stage / 'workspace/tree'
            def gpg(*arguments, data):
                return subprocess.run(['gpg', '--batch', '--yes', '--local-user', fingerprints['stable'], *arguments,
                                       '--output', '-'], input=data, env=env, capture_output=True, check=True).stdout
            for relative in manifest['sign']['clearsign']:
                data = (tree / relative).read_bytes()
                (out / (relative.replace('/', '__') + '.InRelease')).write_bytes(gpg('--clearsign', data=data))
                (out / (relative.replace('/', '__') + '.gpg')).write_bytes(gpg('--armor', '--detach-sign', data=data))
            for relative in manifest['sign']['minisign']:
                copy = root / Path(relative).name
                shutil.copy2(tree / relative, copy)
                subprocess.run(['minisign', '-S', '-s', str(root / 'stable.key'), '-m', str(copy), '-x',
                                str(out / (relative.replace('/', '__') + '.minisig'))], check=True, capture_output=True)
            signed = out / manifest['rpm']['file']
            def sign_rpm(fingerprint):
                shutil.copy2(stage / 'unsigned.rpm', signed)
                subprocess.run(['rpmsign', '--define', '_gpg_name ' + fingerprint, '--define', f'_gpg_path {env["GNUPGHOME"]}',
                                '--addsign', str(signed)], env=env, check=True, capture_output=True)
            # Signed with the wrong (nightly) key: refused, nothing published.
            sign_rpm(fingerprints['nightly'])
            self.assertTrue(signer.stable_signed(stage))
            with self.assertRaisesRegex(pkgrepo.SignerError, 'channel key'):
                signer.publish_stable('lab', stage)
            self.assertFalse(any(key.endswith('InRelease') for key in s3.objects))
            sign_rpm(fingerprints['stable'])
            signer.publish_stable('lab', stage)
            self.assertFalse(stage.exists())
            published = {key: value[0] for key, value in s3.objects.items()}
            self.assertIn('lab/apt/debian13/dists/stable/InRelease', published)
            rpm_key = next(key for key in published if key.endswith('.rpm'))
            rpm = root / 'published.rpm'
            rpm.write_bytes(published[rpm_key])
            self.assertTrue(pkgrepo.verify_rpm(settings.keys / 'putgd-stable.asc', rpm))
            self.assertFalse(pkgrepo.verify_rpm(settings.keys / 'putgd-nightly.asc', rpm))
            feed = json.loads(published['lab/update/v1/stable.json'])
            self.assertEqual(feed['release']['tag'], 'v0.1.1')
            receipt = json.loads(published['lab/receipts/v0.1.1.json'])
            self.assertEqual(receipt['result'], 'published')
            mapping = receipt['rpm'][-1]
            self.assertEqual((mapping['signedWith'], mapping['repoSha256'], mapping['githubSha256']),
                             (fingerprints['stable'], hashlib.sha256(published[rpm_key]).hexdigest(), sums[asset_name('fedora44', '0.1.1')]))
