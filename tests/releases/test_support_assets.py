"""Linked sources remain complete, immutable and verified before publishing installers."""
import copy
import hashlib
import re
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import support_assets as sources


class API:
    repository = 'owner/repo'

    def __init__(self, release, delivery):
        self.release = release
        self.metadata = {'id': 2, 'tag_name': delivery['tag'], 'draft': False, 'prerelease': True,
                         'assets': [{'id': i, 'name': name, 'size': item['bytes'], 'state': 'uploaded',
                                     'digest': 'sha256:' + item['sha256'], 'updated_at': 'fixed'}
                                    for i, (name, item) in enumerate(delivery['assets'].items(), 1)]}
        self.after = None
        self.mutations = []
        self.has_tag = self.has_release = True

    def json(self, path, **kwargs):
        if kwargs.get('method'):
            self.mutations.append((path, kwargs))
            if path == 'git/refs':
                self.has_tag = True
                return kwargs['data']
            if path == 'releases':
                self.has_release = True
                self.metadata.update(kwargs['data'])
            if kwargs['method'] == 'PATCH':
                self.metadata.update(kwargs['data'])
            return copy.deepcopy(self.metadata)
        if path.startswith('git/matching-refs/'):
            return [[{'ref': 'refs/tags/' + self.metadata['tag_name']}]] if self.has_tag else [[]]
        if path.startswith('git/ref/'):
            return {'object': {'type': 'commit', 'sha': self.release['gitCommit']}}
        if path.startswith('releases?'):
            return [[copy.deepcopy(self.metadata)]] if self.has_release else [[]]
        if path == 'releases/2' and self.after:
            return copy.deepcopy(self.after)
        return copy.deepcopy(self.metadata)


class Sources(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pg-sources-test-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.release = {'tag': 'v26.1026.7-nightly', 'version': '26.1026.7-nightly', 'gitCommit': 'a' * 40}
        for suffix in ('-source.tar.gz', '-appimage-complete-source.tar.gz'):
            (self.directory / ('putgd-' + self.release['version'] + suffix)).write_bytes(b'synthetic source')
        (self.directory / ('putgd-gnome50-' + self.release['version'] + '.zip')).write_bytes(b'synthetic extension')
        self.delivery = sources.describe(self.directory, self.release, 'owner/repo')
        self.api = API(self.release, self.delivery)

    def test_published_sources_are_reused_without_uploads_or_mutation(self):
        with patch.object(sources.subprocess, 'run') as upload:
            result = sources.publish(self.api, self.delivery, self.release, self.directory)
        self.assertEqual(result['gitCommit'], self.release['gitCommit'])
        upload.assert_not_called()
        self.assertFalse(self.api.mutations)

    def test_sources_cannot_be_selected_as_a_nightly_by_the_download_site(self):
        self.assertIsNone(re.search(r'-nightly(\.[1-9][0-9]*)?$', self.delivery['tag']))
        self.assertFalse(self.delivery['tag'].startswith('v'))

    def test_complete_draft_is_verified_before_publication(self):
        self.api.metadata['draft'] = True
        sources.publish(self.api, self.delivery, self.release, self.directory)
        self.assertEqual(self.api.mutations, [('releases/2', {'method': 'PATCH',
                         'data': {'draft': False, 'prerelease': True, 'make_latest': 'false'}})])

    def test_new_and_partial_source_drafts_upload_only_missing_files(self):
        for new in (True, False):
            with self.subTest(new=new):
                self.api = API(self.release, self.delivery)
                complete = copy.deepcopy(self.api.metadata['assets'])
                self.api.metadata.update(draft=True, assets=[] if new else complete[:1])
                self.api.has_tag = self.api.has_release = not new
                missing = {a['name'] for a in complete} - {a['name'] for a in self.api.metadata['assets']}

                def upload(command, **kwargs):
                    self.assertEqual(command[:6], ['gh', 'release', 'upload', self.delivery['tag'], '--repo', 'owner/repo'])
                    self.assertEqual({Path(p).name for p in command[6:]}, missing)
                    self.api.metadata['assets'] += [a for a in complete if a['name'] in missing]

                with patch.object(sources.subprocess, 'run', side_effect=upload) as calls:
                    sources.publish(self.api, self.delivery, self.release, self.directory)
                calls.assert_called_once()
                self.assertFalse(self.api.metadata['draft'])
                self.assertEqual(self.api.metadata['make_latest'], 'false')
                created = [data['data']['ref'] for path, data in self.api.mutations if path == 'git/refs']
                self.assertEqual(created, ['refs/tags/' + self.delivery['tag']] if new else [])

    def test_corrupt_local_sources_block_before_any_mutation(self):
        (self.directory / next(iter(self.delivery['assets']))).write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'Local source'):
            sources.publish(self.api, self.delivery, self.release, self.directory)
        self.assertFalse(self.api.mutations)

    def test_conflicting_remote_bytes_never_get_replaced(self):
        self.api.metadata['draft'] = True
        self.api.metadata['assets'][0]['digest'] = 'sha256:' + 'b' * 64
        with patch.object(sources.subprocess, 'run') as upload, self.assertRaises(ValueError):
            sources.publish(self.api, self.delivery, self.release, self.directory)
        upload.assert_not_called()
        self.assertFalse(self.api.mutations)

    def test_missing_or_mutating_public_source_is_rejected(self):
        self.api.metadata['assets'].pop()
        with self.assertRaises(ValueError):
            sources.verify_public(self.api, self.delivery, self.release)
        self.api = API(self.release, self.delivery)
        self.api.after = copy.deepcopy(self.api.metadata)
        self.api.after['assets'][0]['id'] = 999
        with self.assertRaisesRegex(ValueError, 'changed'):
            sources.verify_public(self.api, self.delivery, self.release)

    def test_wrong_identity_extra_files_and_unsafe_names_are_rejected(self):
        for field, value in [('tag', 'v26.1026.7-nightly'), ('repository', 'other/repo'),
                             ('gitCommit', 'b' * 40), ('assets', {'../payload': {}})]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                sources.validate({**self.delivery, field: value}, self.release, 'owner/repo')

    def test_proton_links_must_match_public_checksum_pinned_parts(self):
        asset = self.api.metadata['assets'][0]
        part = {'file': asset['name'], 'bytes': asset['size'], 'sha256': asset['digest'][7:],
                'url': 'https://github.com/owner/repo/releases/download/pinned/' + asset['name']}
        sources.verify_proton(self.api, {'delivery': {'parts': [part]}})
        self.api.metadata['prerelease'] = False
        with self.assertRaisesRegex(ValueError, 'prerelease'):
            sources.verify_proton(self.api, {'delivery': {'parts': [part]}})
        self.api.metadata['prerelease'] = True
        for field, value in [('sha256', 'c' * 64), ('bytes', 0), ('url', 'https://evil.invalid/source'),
                             ('url', 'https://github.com/owner/repo/releases/download/pinned/other')]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                sources.verify_proton(self.api, {'delivery': {'parts': [{**part, field: value}]}})


if __name__ == '__main__':
    unittest.main()
