"""Retention is conservative, dependency-aware, and never mutates git tags."""
import copy
import gzip
import hashlib
import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import prune_nightlies as prune


def release(tag, number=1, **extra):
    return {'id': number, 'tag_name': tag, 'draft': False, 'prerelease': True,
            'published_at': f'2026-10-31T01:{number:02}:00Z', 'assets': [], **extra}


class API:
    repository = 'owner/repo'

    def __init__(self, releases):
        self.releases = releases
        self.data = {}
        self.mutations = []

    def json(self, path, **kwargs):
        if kwargs.get('method'):
            self.mutations.append((path, kwargs['method']))
            return None
        assert path == 'releases?per_page=100' and kwargs['paginate']
        return [copy.deepcopy(self.releases[:4]), copy.deepcopy(self.releases[4:])]

    def asset_bytes(self, asset_id):
        return self.data[asset_id]

    def manifest(self, rel, name, value):
        data = json.dumps(value).encode()
        number = len(self.data) + 1
        self.data[number] = data
        rel['assets'].append({'id': number, 'name': name, 'size': len(data),
                              'state': 'uploaded', 'digest': 'sha256:' + hashlib.sha256(data).hexdigest()})

    def delivery(self, rel, source):
        self.manifest(rel, 'source-delivery.json', {
            'schemaVersion': 1, 'repository': self.repository, 'tag': source['tag_name'],
            'url': f'https://github.com/{self.repository}/releases/tag/{source["tag_name"]}',
            'assets': {'source.tar.gz': {}}})
        source['assets'] = [{'name': 'source.tar.gz'}]


class Retention(unittest.TestCase):
    def setUp(self):
        self.releases = [release(f'v26.1031.{n}-nightly', n) for n in range(1, 8)]
        self.api = API(self.releases)
        self.site = {'repo': self.api.repository, 'nightly': None, 'stable': None}

    def plan(self, indexes=None):
        return prune.plan(self.api, self.releases, indexes or {}, self.site)

    def deleted(self, **kwargs):
        return {item['tag'] for item in self.plan(**kwargs)['delete']}

    def test_keep_five_numeric_versions_with_pagination_not_lexical_order(self):
        self.releases.append(release('v26.1031.10-nightly', 10))
        self.assertEqual(self.deleted(), {f'v26.1031.{n}-nightly' for n in (1, 2, 3)})
        self.assertEqual(len(prune.inventory(self.api)), 8)

    def test_stable_proton_unknown_legacy_drafts_and_orphan_sources_are_preserved(self):
        for n, tag in enumerate(('v26.1031.8', 'proton-sources-shared', 'unknown',
                                 'v0.1.1-nightly.88', 'sources-v26.1031.98-nightly-files'), 20):
            self.releases.append(release(tag, n, prerelease=False))
        self.releases.append(release('v26.1031.99-nightly', 40, draft=True))
        self.releases.append(release('v26.1031.100-nightly', 41, prerelease=False))
        self.assertEqual(self.deleted(), {'v26.1031.1-nightly', 'v26.1031.2-nightly'})

    def test_package_repositories_and_site_protect_older_versions_and_pairs(self):
        for number in (1, 2):
            source = release(prune.source_tag(self.releases[number - 1]['tag_name']), 10 + number)
            self.api.delivery(self.releases[number - 1], source)
            self.releases.append(source)
        self.site['nightly'] = {'tag': self.releases[1]['tag_name']}
        self.assertEqual(self.deleted(indexes={'apt': ['v26.1031.1-nightly']}), set())

    def test_site_published_at_selection_is_preserved_after_republishing_old_tag(self):
        self.releases[0]['published_at'] = '2026-11-01T00:00:00Z'
        self.assertEqual(self.deleted(), {'v26.1031.2-nightly'})

    def test_only_paired_deleted_installer_sources_can_be_deleted(self):
        for number in (1, 7):
            source = release(prune.source_tag(self.releases[number - 1]['tag_name']), 10 + number)
            self.api.delivery(self.releases[number - 1], source)
            self.releases.append(source)
        self.assertEqual(self.deleted(), {'v26.1031.1-nightly', 'v26.1031.2-nightly',
                                         'sources-v26.1031.1-nightly-files'})

    def test_kept_source_delivery_and_proton_catalog_links_protect_transitive_dependencies(self):
        source = release(prune.source_tag(self.releases[0]['tag_name']), 10)
        self.releases.append(source)
        self.api.delivery(self.releases[-2], source)
        self.api.manifest(source, 'proton-source-catalog.json', {
            'schemaVersion': 1, 'delivery': {'parts': [{
                'url': 'https://github.com/owner/repo/releases/download/v26.1031.2-nightly/part1',
                'file': 'part1'}]}})
        self.releases[1]['assets'].append({'name': 'part1'})
        self.assertEqual(self.deleted(), {'v26.1031.1-nightly'})
        self.assertIn(source['tag_name'], self.plan()['keep'])

    def test_broken_or_missing_manifests_and_links_abort(self):
        self.api.manifest(self.releases[-1], 'source-delivery.json', {
            'schemaVersion': 1, 'repository': self.api.repository, 'tag': 'absent', 'assets': {'source': {}}})
        with self.assertRaisesRegex(ValueError, 'no public release'):
            self.plan()
        self.releases[-1]['assets'] = []
        self.releases.append(release(prune.source_tag(self.releases[6]['tag_name']), 20))
        with self.assertRaisesRegex(ValueError, 'no source-delivery'):
            self.plan()

    def test_catalogs_with_local_parts_are_checked_and_manifests_are_authenticated(self):
        rel = self.releases[-1]
        self.api.manifest(rel, 'proton-source-catalog.json', {
            'schemaVersion': 1, 'delivery': {'parts': [{'file': 'part1'}]}})
        with self.assertRaisesRegex(ValueError, 'no asset'):
            self.plan()
        rel['assets'].append({'name': 'part1'})
        self.plan()
        rel['assets'][0]['digest'] = 'sha256:' + '0' * 64
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            self.plan()

    def test_dry_run_never_mutates_and_live_deletes_only_release_ids_installers_first(self):
        source = release(prune.source_tag(self.releases[0]['tag_name']), 10)
        self.releases.append(source)
        with patch.object(prune, 'repository_tags', return_value={}), patch('sys.stdout', new=io.StringIO()):
            result = prune.run(self.api, dry_run=True, get=lambda _: json.dumps(self.site).encode())
            self.assertEqual(len(result['delete']), 3)
            self.assertEqual(self.api.mutations, [])
            prune.run(self.api, dry_run=False, get=lambda _: json.dumps(self.site).encode())
        self.assertEqual(self.api.mutations, [('releases/1', 'DELETE'), ('releases/2', 'DELETE'),
                                             ('releases/10', 'DELETE')])

    def test_changed_protection_input_or_unreachable_repository_aborts_all_deletions(self):
        with patch.object(prune, 'repository_tags', side_effect=[{}, {'rpm': ['v26.1031.1-nightly']}]), \
                patch('sys.stdout', new=io.StringIO()):
            with self.assertRaisesRegex(ValueError, 'inputs changed'):
                prune.run(self.api, dry_run=False, get=lambda _: json.dumps(self.site).encode())
        with patch.object(prune, 'repository_tags', side_effect=OSError('unreachable')):
            with self.assertRaises(OSError):
                prune.run(self.api, dry_run=False)
        self.assertFalse(self.api.mutations)

    def test_invalid_proton_url_and_missing_target_asset_abort(self):
        rel = self.releases[-1]
        for url in ('https://elsewhere.test/part1',
                    'https://github.com/owner/repo/releases/download/v26.1031.1-nightly/wrong-file',
                    'https://github.com/owner/repo/releases/download/v26.1031.1-nightly/part1'):
            rel['assets'] = []
            self.api.manifest(rel, 'proton-source-catalog.json', {
                'schemaVersion': 1, 'delivery': {'parts': [{'file': 'part1', 'url': url}]}})
            with self.subTest(url=url), self.assertRaises(ValueError):
                self.plan()

    def test_failed_installer_deletion_leaves_its_sources_untouched(self):
        self.releases.append(release(prune.source_tag(self.releases[0]['tag_name']), 10))
        original = self.api.json
        attempted = []
        def fail(path, **kwargs):
            if kwargs.get('method'):
                attempted.append(path)
                raise OSError('deletion failed')
            return original(path, **kwargs)
        with patch.object(prune, 'repository_tags', return_value={}), \
                patch.object(self.api, 'json', side_effect=fail), patch('sys.stdout', new=io.StringIO()):
            with self.assertRaises(OSError):
                prune.run(self.api, dry_run=False, get=lambda _: json.dumps(self.site).encode())
        # The installer is deleted first; its failure stops before any source release.
        self.assertEqual(attempted, ['releases/1'])
        self.assertFalse(self.api.mutations)

    def test_concurrent_release_asset_change_aborts_before_deletion(self):
        original = self.api.json
        reads = []
        def changed(path, **kwargs):
            result = original(path, **kwargs)
            if not kwargs.get('method'):
                reads.append(path)
                if len(reads) == 2:
                    result[0][0]['assets'].append({'id': 88, 'name': 'changed'})
            return result
        with patch.object(prune, 'repository_tags', return_value={}), \
                patch.object(self.api, 'json', side_effect=changed), patch('sys.stdout', new=io.StringIO()):
            with self.assertRaisesRegex(ValueError, 'inputs changed'):
                prune.run(self.api, dry_run=False, get=lambda _: json.dumps(self.site).encode())
        self.assertFalse(self.api.mutations)


class Repositories(unittest.TestCase):
    def test_apt_reads_every_putgd_stanza_including_stable_and_empty_suites(self):
        self.assertEqual(prune.apt_tags(b''), set())
        self.assertEqual(prune.apt_tags(b'Package: putgd\nVersion: 26.1031.20~nightly\n\n'
                                        b'Package: putgd\nVersion: 26.1030.19\n'),
                         {'v26.1031.20-nightly', 'v26.1030.19'})
        for data in (b'<html>error</html>', b'Version: 26.1031.20', b'Package: putgd',
                     b'Package: putgd\nVersion: nonsense'):
            with self.subTest(data=data), self.assertRaises((ValueError, KeyError)):
                prune.apt_tags(data)

    def fixture(self):
        xml = (f'<metadata xmlns="{prune.PRIMARY_NS[1:-1]}" packages="2">'
               '<package><name>putgd</name><version ver="26.1031.20~nightly"/></package>'
               '<package><name>putgd</name><version ver="26.1030.19"/></package></metadata>').encode()
        compressed = gzip.compress(xml)
        repomd = (f'<repomd xmlns="{prune.REPO_NS[1:-1]}"><data type="primary">'
                  '<location href="repodata/test-primary.xml.gz"/>'
                  f'<checksum type="sha256">{hashlib.sha256(compressed).hexdigest()}</checksum>'
                  f'<size>{len(compressed)}</size><open-size>{len(xml)}</open-size>'
                  '</data></repomd>').encode()
        return xml, compressed, repomd

    def test_rpm_follows_primary_location_and_checks_compressed_bytes_and_package_count(self):
        xml, compressed, repomd = self.fixture()
        get = lambda url: repomd if url.endswith('repomd.xml') else compressed
        self.assertEqual(prune.rpm_tags('https://pkg.test/', get),
                         {'v26.1031.20-nightly', 'v26.1030.19'})
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            prune.rpm_tags('https://pkg.test/', lambda url: repomd if url.endswith('repomd.xml') else b'bad')
        with self.assertRaisesRegex(ValueError, 'Incomplete RPM'):
            prune.primary_tags(xml.replace(b'packages="2"', b'packages="3"'))
        self.assertEqual(prune.primary_tags(
            f'<metadata xmlns="{prune.PRIMARY_NS[1:-1]}" packages="0"/>'.encode()), set())

    def test_all_eight_repository_indexes_are_required_and_kept(self):
        seen = []
        def get(url):
            seen.append(url)
            return b'Package: putgd\nVersion: 26.1031.20~nightly\n'
        with patch.object(prune, 'rpm_tags', return_value={'v26.1030.19'}) as rpm:
            result = prune.repository_tags(get)
        self.assertEqual(len(seen), 6)
        self.assertEqual(len(result), 8)
        self.assertEqual(rpm.call_count, 2)
        self.assertTrue(any('/debian13/dists/stable/' in url for url in seen))


class Workflow(unittest.TestCase):
    def test_pruning_runs_only_after_successful_publication_with_existing_write_token(self):
        text = (Path(__file__).resolve().parents[2] / '.github/workflows/nightly.yml').read_text()
        publish = text.split('\n  publish:\n')[1].split('\n  surface-map:\n')[0]
        self.assertLess(publish.index('publish.py --tag'), publish.index('prune_nightlies.py'))
        self.assertIn('GH_TOKEN: ${{ github.token }}', publish)
        self.assertIn('contents: write', publish)
        step = publish.split('- name: Prune older nightly')[1].split('- name: Retain nightly')[0]
        self.assertNotIn('if: always()', step)
        self.assertNotIn('--dry-run', step)
        self.assertIn('zstd', step)
        self.assertIn('if: always()', publish.split('- name: Retain nightly retention plan')[1])


if __name__ == '__main__':
    unittest.main()
