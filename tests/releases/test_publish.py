#!/usr/bin/env python3
"""Publication contracts: remote draft bytes must equal accepted package bytes."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
spec = importlib.util.spec_from_file_location('release_publish', ROOT / 'tools/releases/publish.py')
publish = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publish)


class RemoteDraftTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pg-publish-test-')
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.files = {'putgd-test.deb': b'accepted package', 'release-builds.json': b'{}'}
        self.listed = {name: hashlib.sha256(body).hexdigest() for name, body in self.files.items()}
        self.files['SHA256SUMS'] = ''.join(d + '  ' + n + '\n' for n, d in self.listed.items()).encode()
        for name, body in self.files.items():
            (self.directory / name).write_bytes(body)
        self.metadata = {'id': 42, 'tag_name': 'v0.1.1-rc.1', 'draft': True,
                         'assets': [{'id': i, 'name': n, 'size': len(b), 'state': 'uploaded',
                                     'updated_at': '2026-10-12T00:00:00Z'}
                                    for i, (n, b) in enumerate(self.files.items(), 1)]}
        self.remote_files = dict(self.files)
        self.tag = {'object': {'type': 'commit', 'sha': 'a' * 40}}
        self.calls = []
        self.after = None

    def api(self, *arguments):
        endpoint = arguments[-1]
        if 'releases?per_page=' in endpoint:
            self.assertEqual(arguments[:2], ('--paginate', '--slurp'))
            return [[copy.deepcopy(self.metadata)]]
        if '/git/' in endpoint:
            return copy.deepcopy(self.tag)
        self.calls.append(endpoint)
        self.assertEqual(endpoint, 'repos/{owner}/{repo}/releases/42')
        return copy.deepcopy(self.after or self.metadata)

    def download(self, command, **kwargs):
        self.assertEqual(command[:6], ['gh', 'api', '--method', 'GET', '-H',
                                      'Accept: application/octet-stream'])
        asset_id = int(command[-1].rsplit('/', 1)[1])
        asset = next(a for a in self.metadata['assets'] if a['id'] == asset_id)
        kwargs['stdout'].write(self.remote_files[asset['name']])
        return subprocess.CompletedProcess(command, 0)

    def verify(self):
        with patch.object(publish, 'github_json', side_effect=self.api), patch.object(publish.subprocess, 'run', side_effect=self.download):
            return publish.verify_remote_draft('v0.1.1-rc.1', self.directory, self.listed, 'a' * 40)

    def test_command_requires_exact_acceptance_before_public_mutation(self):
        release = {'gitCommit': 'a' * 40}
        (self.directory / 'release-builds.json').write_text(json.dumps({'release': release}))
        self.listed['release-builds.json'] = publish.sha256(self.directory / 'release-builds.json')
        (self.directory / 'SHA256SUMS').write_text(''.join(d + '  ' + n + '\n' for n, d in self.listed.items()))
        acceptance = self.directory / 'acceptance.json'
        accepted = {'tag': 'v0.1.1-rc.1', 'gitCommit': 'a' * 40, 'status': 'passed', 'assetSha256': self.listed}
        argv = ['publish.py', '--tag', 'v0.1.1-rc.1', '--directory', str(self.directory), '--publish', '--acceptance', str(acceptance)]
        for field, bad in [('tag', 'other'), ('gitCommit', 'b' * 40), ('status', 'failed'), ('assetSha256', {})]:
            with self.subTest(field=field):
                invalid = {**accepted, field: bad}
                acceptance.write_text(json.dumps(invalid))
                with patch.object(sys, 'argv', argv), patch.object(publish, 'verify', return_value=release), patch.object(publish, 'verify_remote_draft') as remote, patch.object(publish.subprocess, 'run') as mutate:
                    with self.assertRaises(ValueError):
                        publish.main()
                    remote.assert_not_called()
                    mutate.assert_not_called()
        acceptance.write_text(json.dumps(accepted))
        with patch.object(sys, 'argv', argv), patch.object(publish, 'verify', return_value=release), patch.object(publish, 'verify_remote_draft', return_value={'releaseId': 42}) as remote, patch.object(publish.subprocess, 'run') as mutate:
            publish.main()
            remote.assert_called_once_with('v0.1.1-rc.1', self.directory, self.listed, 'a' * 40)
            mutate.assert_called_once_with(['gh', 'release', 'edit', 'v0.1.1-rc.1', '--draft=false'], check=True)
        self.assertTrue(json.loads((self.directory / 'publication.json').read_text())['published'])
        with patch.object(sys, 'argv', argv), patch.object(publish, 'verify', return_value=release), patch.object(publish, 'verify_remote_draft', side_effect=ValueError('remote bytes differ')), patch.object(publish.subprocess, 'run') as mutate:
            with self.assertRaisesRegex(ValueError, 'remote bytes differ'):
                publish.main()
            mutate.assert_not_called()


    def test_first_draft_is_created_verified_and_uploaded_without_replacement(self):
        draft = copy.deepcopy(self.metadata)
        draft['assets'] = []
        release = {'gitCommit': 'a' * 40, 'version': '0.1.1-rc.1', 'prerelease': True}
        notes = self.directory / 'notes.md'
        notes.write_text('Synthetic release notes\n')
        with patch.object(publish, 'remote_tag_commit', return_value='a' * 40), patch.object(publish, 'matching_releases', side_effect=[[], [draft]]), patch.object(publish.subprocess, 'run') as commands:
            publish.stage_draft('v0.1.1-rc.1', self.directory, self.listed, release, notes)
        create, upload = [call.args[0] for call in commands.call_args_list]
        self.assertEqual(create[:4], ['gh', 'release', 'create', 'v0.1.1-rc.1'])
        for flag in ('--verify-tag', '--draft', '--prerelease'):
            self.assertIn(flag, create)
        self.assertEqual(upload[:4], ['gh', 'release', 'upload', 'v0.1.1-rc.1'])
        self.assertEqual({Path(path).name for path in upload[4:]}, set(self.files))
        self.assertNotIn('--clobber', upload)

    def test_partial_draft_resume_uploads_only_missing_verified_files(self):
        partial = copy.deepcopy(self.metadata)
        partial['assets'] = partial['assets'][:1]
        release = {'gitCommit': 'a' * 40, 'version': '0.1.1-rc.1', 'prerelease': True}
        commands = []
        def run(command, **kwargs):
            commands.append(command)
            if command[:2] == ['gh', 'api']:
                return self.download(command, **kwargs)
            return subprocess.CompletedProcess(command, 0)
        with patch.object(publish, 'remote_tag_commit', return_value='a' * 40), patch.object(publish, 'matching_releases', return_value=[partial]), patch.object(publish.subprocess, 'run', side_effect=run):
            publish.stage_draft('v0.1.1-rc.1', self.directory, self.listed, release, self.directory / 'notes.md')
        uploaded = commands[-1]
        self.assertEqual(uploaded[:4], ['gh', 'release', 'upload', 'v0.1.1-rc.1'])
        self.assertEqual({Path(p).name for p in uploaded[4:]}, set(self.files) - {'putgd-test.deb'})
        self.assertNotIn('--clobber', uploaded)

    def test_conflicting_partial_draft_is_never_replaced(self):
        partial = copy.deepcopy(self.metadata)
        partial['assets'] = partial['assets'][:1]
        self.remote_files['putgd-test.deb'] = b'corrupt package!'
        release = {'gitCommit': 'a' * 40}
        with patch.object(publish, 'remote_tag_commit', return_value='a' * 40), patch.object(publish, 'matching_releases', return_value=[partial]), patch.object(publish.subprocess, 'run', side_effect=self.download):
            with self.assertRaisesRegex(ValueError, 'bytes differ'):
                publish.stage_draft('v0.1.1-rc.1', self.directory, self.listed, release, self.directory / 'notes.md')

    def test_matching_draft_bytes_and_remote_commit_pass(self):
        result = self.verify()
        self.assertEqual(result['releaseId'], 42)
        self.assertEqual(set(result['sha256']), set(self.files))

    def test_same_size_corrupt_remote_package_is_rejected(self):
        self.remote_files['putgd-test.deb'] = b'corrupt package!'
        self.assertEqual(len(self.remote_files['putgd-test.deb']), len(self.files['putgd-test.deb']))
        with self.assertRaisesRegex(ValueError, 'bytes differ'):
            self.verify()

    def test_missing_or_extra_asset_is_rejected(self):
        for extra in (False, True):
            with self.subTest(extra=extra):
                original = copy.deepcopy(self.metadata)
                if extra:
                    self.metadata['assets'].append({'name': 'unaccepted.rpm'})
                else:
                    self.metadata['assets'].pop()
                with self.assertRaisesRegex(ValueError, 'inventory differs'):
                    self.verify()
                self.metadata = original

    def test_annotated_remote_tag_resolves_to_accepted_commit(self):
        original = self.api
        def api(*arguments):
            endpoint = arguments[-1]
            if '/git/ref/tags/' in endpoint:
                return {'object': {'type': 'tag', 'sha': 'c' * 40}}
            if '/git/tags/' in endpoint:
                return {'object': {'type': 'commit', 'sha': 'a' * 40}}
            return original(*arguments)
        with patch.object(publish, 'github_json', side_effect=api), patch.object(publish.subprocess, 'run', side_effect=self.download):
            self.assertEqual(publish.verify_remote_draft('v0.1.1-rc.1', self.directory, self.listed, 'a' * 40)['releaseId'], 42)

    def test_multiple_matching_drafts_are_rejected(self):
        with patch.object(publish, 'github_json', return_value=[[self.metadata, self.metadata]]), patch.object(publish.subprocess, 'run') as mutate:
            with self.assertRaisesRegex(ValueError, 'exactly one'):
                publish.verify_remote_draft('v0.1.1-rc.1', self.directory, self.listed, 'a' * 40)
            mutate.assert_not_called()

    def test_wrong_remote_commit_is_rejected(self):
        self.tag['object']['sha'] = 'b' * 40
        with self.assertRaisesRegex(ValueError, 'Remote tag differs'):
            self.verify()

    def test_changed_asset_during_verification_is_rejected(self):
        self.after = copy.deepcopy(self.metadata)
        self.after['assets'][0]['id'] = 100
        with self.assertRaisesRegex(ValueError, 'changed during'):
            self.verify()

    def test_already_public_release_is_rejected(self):
        self.metadata['draft'] = False
        with self.assertRaisesRegex(ValueError, 'matching existing draft'):
            self.verify()

    def test_corrupt_checksum_asset_is_rejected(self):
        self.remote_files['SHA256SUMS'] = b'invalid checksum file'
        with self.assertRaisesRegex(ValueError, 'bytes differ'):
            self.verify()


if __name__ == '__main__':
    unittest.main()
