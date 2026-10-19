#!/usr/bin/env python3
"""Publication contracts: remote draft bytes must equal accepted package bytes."""
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import call, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
spec = importlib.util.spec_from_file_location('release_publish', ROOT / 'tools/releases/publish.py')
publish = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publish)
import release_bundle


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
        self.files = {name: ('synthetic ' + name).encode() for name in
                      publish.public_inventory('v0.1.1-rc.1') - {'SHA256SUMS'}}
        for name, body in self.files.items():
            (self.directory / name).write_bytes(body)
        self.listed = {name: hashlib.sha256(body).hexdigest() for name, body in self.files.items()}
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


    def test_candidate_and_stable_incomplete_inventory_block_public_mutation(self):
        for tag in ('v0.1.1-rc.1', 'v0.1.1'):
            with self.subTest(tag=tag):
                release = {'gitCommit': 'a' * 40}
                (self.directory / 'release-builds.json').write_text(json.dumps({'release': release}))
                self.listed['release-builds.json'] = publish.sha256(self.directory / 'release-builds.json')
                (self.directory / 'SHA256SUMS').write_text(''.join(d + '  ' + n + '\n' for n, d in self.listed.items()))
                acceptance = self.directory / 'acceptance.json'
                acceptance.write_text(json.dumps({'tag': tag, 'gitCommit': 'a' * 40,
                    'status': 'passed', 'assetSha256': self.listed}))
                argv = ['publish.py', '--tag', tag, '--directory', str(self.directory),
                        '--publish', '--acceptance', str(acceptance)]
                with patch.object(sys, 'argv', argv), patch.object(publish, 'verify', return_value=release), \
                        patch.object(publish, 'verify_remote_draft', return_value={'releaseId': 42}) as remote, \
                        patch.object(publish.subprocess, 'run') as mutate:
                    with self.assertRaisesRegex(ValueError, 'inventory'):
                        publish.main()
                    remote.assert_not_called()
                    mutate.assert_not_called()

    def test_each_source_part_is_required_for_candidate_and_stable(self):
        for tag in ('v0.1.1-rc.1', 'v0.1.1'):
            for part in ('putgd-proton-corresponding-source.tar.gz.part1',
                         'putgd-proton-corresponding-source.tar.gz.part2'):
                with self.subTest(tag=tag, missing=part):
                    names = publish.public_inventory(tag) - {'SHA256SUMS'}
                    self.assertIn(part, names)
                    release = {'gitCommit': 'a' * 40}
                    for name in names:
                        (self.directory / name).write_bytes(('synthetic ' + name).encode())
                    (self.directory / 'release-builds.json').write_text(json.dumps({'release': release}))
                    listed = {name: publish.sha256(self.directory / name) for name in names - {part}}
                    (self.directory / 'SHA256SUMS').write_text(''.join(d + '  ' + n + '\n' for n, d in listed.items()))
                    acceptance = self.directory / 'acceptance.json'
                    acceptance.write_text(json.dumps({'tag': tag, 'gitCommit': 'a' * 40,
                        'status': 'passed', 'assetSha256': listed}))
                    argv = ['publish.py', '--tag', tag, '--directory', str(self.directory),
                            '--publish', '--acceptance', str(acceptance)]
                    with patch.object(sys, 'argv', argv), patch.object(publish, 'verify', return_value=release), \
                            patch.object(publish, 'verify_remote_draft') as remote, \
                            patch.object(publish.subprocess, 'run') as mutate:
                        with self.assertRaisesRegex(ValueError, 'inventory'):
                            publish.main()
                        remote.assert_not_called()
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

    def test_created_draft_visibility_delay_retries_reads_without_recreating(self):
        draft = {**self.metadata, 'assets': []}
        release = {'gitCommit': 'a' * 40, 'version': '0.1.1-rc.1', 'prerelease': True}
        for missing_reads in (0, 2, 4):
            with self.subTest(missing_reads=missing_reads), \
                    patch.object(publish, 'remote_tag_commit', return_value='a' * 40), \
                    patch.object(publish, 'matching_releases',
                                 side_effect=[[]] + [[]] * missing_reads + [[draft]]) as lookup, \
                    patch.object(publish.subprocess, 'run') as commands, patch('time.sleep') as sleep:
                publish.stage_draft('v0.1.1-rc.1', self.directory, self.listed, release, self.directory / 'notes.md')
                self.assertEqual(lookup.call_count, missing_reads + 2)
                self.assertEqual(sleep.call_args_list, [call(2 ** i) for i in range(missing_reads)])
                self.assertEqual([c.args[0][2] for c in commands.call_args_list], ['create', 'upload'])

    def test_delayed_nightly_draft_still_requires_prerelease_and_staging_recheck(self):
        tag = 'v0.1.1-nightly.2'
        release = {'gitCommit': 'a' * 40, 'version': tag[1:], 'prerelease': True, 'channel': 'nightly'}
        for prerelease in (True, False):
            draft = {**self.metadata, 'tag_name': tag, 'assets': [], 'prerelease': prerelease}
            with self.subTest(prerelease=prerelease), \
                    patch.object(publish, 'remote_tag_commit', return_value='a' * 40), \
                    patch.object(publish, 'matching_releases', side_effect=[[], [], [draft], [draft]]) as lookup, \
                    patch.object(publish.subprocess, 'run') as commands, patch('time.sleep') as sleep:
                if prerelease:
                    publish.stage_draft(tag, self.directory, self.listed, release, self.directory / 'notes.md')
                    self.assertEqual(lookup.call_count, 4)
                    self.assertEqual(commands.call_count, 2)
                else:
                    with self.assertRaisesRegex(ValueError, 'Nightly draft must be a prerelease'):
                        publish.stage_draft(tag, self.directory, self.listed, release, self.directory / 'notes.md')
                    self.assertEqual(lookup.call_count, 3)
                    self.assertEqual(commands.call_count, 1)
                sleep.assert_called_once_with(1)

    def test_created_draft_visibility_retry_is_bounded_and_never_uploads_unverified(self):
        release = {'gitCommit': 'a' * 40, 'version': '0.1.1-rc.1', 'prerelease': True}
        with patch.object(publish, 'remote_tag_commit', return_value='a' * 40), \
                patch.object(publish, 'matching_releases', return_value=[]) as lookup, \
                patch.object(publish.subprocess, 'run') as commands, patch('time.sleep') as sleep:
            with self.assertRaisesRegex(ValueError, 'Draft creation succeeded.*inspect existing drafts before rerunning'):
                publish.stage_draft('v0.1.1-rc.1', self.directory, self.listed, release, self.directory / 'notes.md')
        self.assertEqual(lookup.call_count, 6)  # Initial check plus five post-create reads.
        self.assertEqual(sleep.call_args_list, [call(1), call(2), call(4), call(8)])
        self.assertEqual(commands.call_count, 1)
        self.assertEqual(commands.call_args.args[0][2], 'create')

    def test_created_draft_conflicts_and_api_errors_are_not_retried(self):
        release = {'gitCommit': 'a' * 40, 'version': '0.1.1-rc.1', 'prerelease': True}
        error = subprocess.CalledProcessError(1, ['gh', 'api'])
        for result in ([self.metadata, self.metadata], [{**self.metadata, 'draft': False}], error):
            with self.subTest(result=result), \
                    patch.object(publish, 'remote_tag_commit', return_value='a' * 40), \
                    patch.object(publish, 'matching_releases', side_effect=[[], result]) as lookup, \
                    patch.object(publish.subprocess, 'run') as commands, patch('time.sleep') as sleep:
                with self.assertRaises(subprocess.CalledProcessError if result is error else ValueError):
                    publish.stage_draft('v0.1.1-rc.1', self.directory, self.listed, release, self.directory / 'notes.md')
                self.assertEqual(lookup.call_count, 2)
                sleep.assert_not_called()
                self.assertEqual(commands.call_count, 1)

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


class NightlyPublicationTest(unittest.TestCase):
    """Drive the CLI against synthetic GitHub protocol state, without live mutations."""
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pg-nightly-publish-')
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.tag = 'v0.1.1-nightly.42'
        self.release = {'schemaVersion': 1, 'tag': self.tag, 'version': self.tag[1:],
                        'baseVersion': '0.1.1', 'packageVersion': self.tag[1:].replace('-nightly.', '~nightly.'),
                        'channel': 'nightly', 'prerelease': True, 'gitCommit': 'a' * 40,
                        'gitTree': 'b' * 40, 'commitCount': 42}
        names = sorted(publish.public_inventory(self.tag) - {'SHA256SUMS'})
        self.files = {name: ('synthetic ' + name).encode() for name in names}
        self.files['release-builds.json'] = json.dumps({'release': self.release}).encode()
        self.listed = {name: hashlib.sha256(data).hexdigest() for name, data in self.files.items()}
        self.files['SHA256SUMS'] = ''.join(d + '  ' + n + '\n' for n, d in sorted(self.listed.items())).encode()
        for name, data in self.files.items():
            (self.directory / name).write_bytes(data)
        self.metadata = None
        self.remote_files = {}
        self.commands = []
        self.commit = 'a' * 40
        self.after_edit = None
        self.environment = {'GITHUB_REPOSITORY': 'fixture/putgd', 'GITHUB_RUN_ID': '77',
                            'GITHUB_RUN_ATTEMPT': '1', 'GITHUB_SERVER_URL': 'https://github.com',
                            'GITHUB_WORKFLOW_REF': 'fixture/putgd/.github/workflows/nightly.yml@refs/heads/nightly'}

    def metadata_for(self, draft, names=None):
        ordered = sorted(self.files)
        return {'id': 42, 'tag_name': self.tag, 'draft': draft, 'prerelease': True,
            'assets': [{'id': ordered.index(name) + 1, 'name': name, 'size': len(self.files[name]),
                        'state': 'uploaded', 'updated_at': '2026-10-12T00:00:00Z'}
                       for name in (ordered if names is None else names)]}

    def output(self, command, **kwargs):
        endpoint = next(value for value in command if value.startswith('repos/'))
        path = endpoint.split('/', 3)[3]
        if path.startswith('releases/assets/'):
            asset = next(a for a in self.metadata['assets'] if a['id'] == int(path.rsplit('/', 1)[1]))
            return self.remote_files[asset['name']]
        if path.startswith('git/'):
            value = {'object': {'type': 'commit', 'sha': self.commit}}
        elif path.startswith('releases?'):
            value = [[self.metadata]] if self.metadata else [[]]
        elif path.startswith('releases/'):
            value = self.metadata
        else:
            raise AssertionError(command)
        return json.dumps(value)

    def command(self, command, **kwargs):
        if command[:2] == ['gh', 'api']:
            kwargs['stdout'].write(self.output(command))
            return subprocess.CompletedProcess(command, 0)
        self.commands.append(command)
        if command[:3] == ['gh', 'release', 'create']:
            self.metadata = self.metadata_for(True, [])
        elif command[:3] == ['gh', 'release', 'upload']:
            names = [Path(path).name for path in command[4:]]
            self.remote_files.update({name: (self.directory / name).read_bytes() for name in names})
            self.metadata['assets'] += self.metadata_for(True, names)['assets']
        elif command[:3] == ['gh', 'release', 'edit']:
            self.metadata['draft'] = False
            if self.after_edit:
                self.metadata.update(self.after_edit)
        else:
            raise AssertionError(command)
        return subprocess.CompletedProcess(command, 0)

    def invoke(self, *extra):
        argv = ['publish.py', '--tag', self.tag, '--directory', str(self.directory), '--nightly', *extra]
        with patch.object(sys, 'argv', argv), patch.dict(os.environ, self.environment, clear=True), \
             patch.object(publish, 'verify', return_value=self.release), \
             patch.object(subprocess, 'check_output', side_effect=self.output), \
             patch.object(subprocess, 'run', side_effect=self.command):
            publish.main()

    def test_cli_publishes_all_thirteen_verified_assets_as_nightly_prerelease(self):
        self.invoke()
        self.assertEqual(self.remote_files, self.files)
        self.assertEqual(len(self.remote_files), 13)
        self.assertFalse(self.metadata['draft'])
        self.assertTrue(self.metadata['prerelease'])
        self.assertIn('--latest=false', self.commands[0])
        self.assertIn('--prerelease', self.commands[0])
        self.assertEqual(self.commands[-1], ['gh', 'release', 'edit', self.tag,
                         '--draft=false', '--prerelease', '--latest=false'])
        receipt = json.loads((self.directory / 'publication.json').read_text())
        self.assertEqual(receipt['channel'], 'nightly')
        self.assertEqual(receipt['gitCommit'], 'a' * 40)
        self.assertEqual(receipt['producer'], {
            'repository': 'fixture/putgd', 'runId': 77, 'runAttempt': 1,
            'workflowRef': 'fixture/putgd/.github/workflows/nightly.yml@refs/heads/nightly',
            'url': 'https://github.com/fixture/putgd/actions/runs/77'})
        self.assertEqual(receipt['verifiedRemoteDraft']['sha256'],
                         {**self.listed, 'SHA256SUMS': hashlib.sha256(self.files['SHA256SUMS']).hexdigest()})
        notes = (self.directory / 'release-notes.md').read_text()
        self.assertIn('--title', self.commands[0])
        self.assertEqual(self.commands[0][self.commands[0].index('--title') + 1], 'put.gd 0.1.1 nightly 42')
        for text in ('Nightly 42 of 0.1.1', 'a' * 40, 'actions/runs/77', 'graphical', 'memory', 'S3'):
            self.assertIn(text, notes)


    def test_missing_or_invalid_run_identity_blocks_before_mutation(self):
        for key, value in (('GITHUB_RUN_ID', None), ('GITHUB_RUN_ID', '0'), ('GITHUB_RUN_ATTEMPT', 'x'),
                           ('GITHUB_WORKFLOW_REF', 'other/repo/.github/workflows/nightly.yml@refs/heads/nightly'),
                           ('GITHUB_WORKFLOW_REF', None)):
            with self.subTest(key=key, value=value):
                original = dict(self.environment)
                if value is None:
                    del self.environment[key]
                else:
                    self.environment[key] = value
                with self.assertRaisesRegex(ValueError, 'run identity'):
                    self.invoke()
                self.assertEqual(self.commands, [])
                self.assertIsNone(self.metadata)
                self.environment = original

    def test_automated_mode_rejects_stable_and_candidate_before_mutation(self):
        original_tag, original_release = self.tag, self.release
        for tag, channel in [('v0.1.1', 'stable'), ('v0.1.1-rc.1', 'candidate')]:
            with self.subTest(tag=tag):
                self.tag = tag
                self.release = {**original_release, 'tag': tag, 'channel': channel}
                with self.assertRaisesRegex(ValueError, 'strict nightly'):
                    self.invoke()
                self.assertEqual(self.commands, [])
        self.tag, self.release = original_tag, original_release

    def test_partial_draft_retry_verifies_existing_and_uploads_only_missing(self):
        existing = sorted(self.files)[:2]
        self.metadata = self.metadata_for(True, existing)
        self.remote_files = {name: self.files[name] for name in existing}
        self.invoke()
        self.assertEqual(self.remote_files, self.files)
        self.assertEqual([command[2] for command in self.commands], ['upload', 'edit'])
        self.assertEqual({Path(path).name for path in self.commands[0][4:]}, set(self.files) - set(existing))
        self.assertNotIn('--clobber', self.commands[0])

    def test_conflicting_partial_draft_and_wrong_flags_are_preserved(self):
        name = next(iter(self.files))
        self.metadata = self.metadata_for(True, [name])
        self.remote_files = {name: bytes([self.files[name][0] ^ 1]) + self.files[name][1:]}
        with self.assertRaisesRegex(ValueError, 'bytes differ'):
            self.invoke()
        self.assertEqual(self.commands, [])
        self.remote_files[name] = self.files[name]
        self.metadata['prerelease'] = False
        with self.assertRaisesRegex(ValueError, 'must be a prerelease'):
            self.invoke()
        self.assertEqual(self.commands, [])

    def test_public_retry_is_verified_noop_only_for_original_inventory(self):
        self.metadata = self.metadata_for(False)
        self.remote_files = dict(self.files)
        self.invoke()
        self.assertEqual(self.commands, [])
        receipt = json.loads((self.directory / 'publication.json').read_text())
        self.assertTrue(receipt['unchanged'])
        name = next(iter(self.listed))
        (self.directory / name).write_bytes(b'different rebuilt bytes')
        changed = {**self.listed, name: publish.sha256(self.directory / name)}
        (self.directory / 'SHA256SUMS').write_text(''.join(d + '  ' + n + '\n' for n, d in sorted(changed.items())))
        with self.assertRaisesRegex(ValueError, 'original collected inventory'):
            self.invoke()
        self.assertEqual(self.commands, [])

    def test_public_corrupt_remote_bytes_or_missing_original_handoff_fail(self):
        self.metadata = self.metadata_for(False)
        self.remote_files = dict(self.files)
        name = next(iter(self.listed))
        self.remote_files[name] = b'corrupt bytes'
        with self.assertRaises(ValueError):
            self.invoke()
        self.assertEqual(self.commands, [])
        self.remote_files[name] = self.files[name]
        (self.directory / name).unlink()
        with self.assertRaises(FileNotFoundError):
            self.invoke()
        self.assertEqual(self.commands, [])

    def test_post_mutation_metadata_failure_is_not_acknowledged_as_success(self):
        for change in ({'draft': True}, {'prerelease': False}, {'tag_name': 'other'}):
            with self.subTest(change=change):
                self.metadata = None
                self.remote_files = {}
                self.after_edit = change
                with self.assertRaises(ValueError):
                    self.invoke()
                self.assertFalse((self.directory / 'publication.json').exists())

    def test_positive_inventory_is_required_before_any_remote_mutation(self):
        checksums = (self.directory / 'SHA256SUMS').read_text().splitlines()
        (self.directory / 'SHA256SUMS').write_text('\n'.join(checksums[:-1]) + '\n')
        with self.assertRaisesRegex(ValueError, 'all thirteen'):
            self.invoke()
        self.assertEqual(self.commands, [])


    def test_changed_tag_or_draft_during_partial_verification_blocks_upload(self):
        original = self.command
        name = next(iter(self.files))
        for change in ('tag', 'metadata'):
            with self.subTest(change=change):
                self.metadata = self.metadata_for(True, [name])
                self.remote_files = {name: self.files[name]}
                self.commit = 'a' * 40
                def changed(command, **kwargs):
                    result = original(command, **kwargs)
                    if command[:2] == ['gh', 'api']:
                        if change == 'tag':
                            self.commit = 'b' * 40
                        else:
                            self.metadata['prerelease'] = False
                    return result
                with patch.object(self, 'command', side_effect=changed):
                    with self.assertRaisesRegex(ValueError, 'changed during staging'):
                        self.invoke()
                self.assertEqual(self.commands, [])

    def test_stable_and_candidate_still_require_acceptance_to_publish(self):
        for tag, channel in [('v0.1.1', 'stable'), ('v0.1.1-rc.1', 'candidate')]:
            release = {**self.release, 'tag': tag, 'channel': channel, 'version': tag[1:]}
            for name in publish.public_inventory(tag) - {'SHA256SUMS'}:
                (self.directory / name).write_bytes(('synthetic ' + name).encode())
            (self.directory / 'release-builds.json').write_text(json.dumps({'release': release}))
            listed = {name: publish.sha256(self.directory / name)
                      for name in publish.public_inventory(tag) - {'SHA256SUMS'}}
            (self.directory / 'SHA256SUMS').write_text(''.join(d + '  ' + n + '\n' for n, d in sorted(listed.items())))
            argv = ['publish.py', '--tag', tag, '--directory', str(self.directory), '--publish']
            with self.subTest(tag=tag), patch.object(sys, 'argv', argv), \
                 patch.object(publish, 'verify', return_value=release), \
                 patch.object(subprocess, 'run') as mutate, patch.object(sys, 'stderr', io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    publish.main()
                self.assertEqual(error.exception.code, 2)
                mutate.assert_not_called()


    def test_candidate_and_stable_drafts_record_their_run_without_publishing(self):
        for tag, channel, prerelease in [('v0.1.1-rc.1', 'candidate', True), ('v0.1.1', 'stable', False)]:
            with self.subTest(tag=tag):
                self.tag, self.metadata, self.remote_files, self.commands = tag, None, {}, []
                release = {**self.release, 'tag': tag, 'channel': channel, 'version': tag[1:],
                           'prerelease': prerelease}
                for name in publish.public_inventory(tag) - {'SHA256SUMS'}:
                    (self.directory / name).write_bytes(('synthetic ' + name).encode())
                (self.directory / 'release-builds.json').write_text(json.dumps({'release': release}))
                listed = {name: publish.sha256(self.directory / name)
                          for name in publish.public_inventory(tag) - {'SHA256SUMS'}}
                (self.directory / 'SHA256SUMS').write_text(''.join(d + '  ' + n + '\n' for n, d in sorted(listed.items())))
                self.files = {name: (self.directory / name).read_bytes() for name in [*listed, 'SHA256SUMS']}
                self.environment['GITHUB_WORKFLOW_REF'] = 'fixture/putgd/.github/workflows/release.yml@refs/heads/main'
                argv = ['publish.py', '--tag', tag, '--directory', str(self.directory)]
                with patch.object(sys, 'argv', argv), patch.dict(os.environ, self.environment, clear=True), \
                     patch.object(publish, 'verify', return_value=release), \
                     patch.object(subprocess, 'check_output', side_effect=self.output), \
                     patch.object(subprocess, 'run', side_effect=self.command):
                    publish.main()
                self.assertEqual([command[2] for command in self.commands], ['create', 'upload'])
                self.assertEqual('--prerelease' in self.commands[0], prerelease)
                self.assertTrue(self.metadata['draft'])
                staged = json.loads((self.directory / 'staging.json').read_text())
                self.assertEqual((staged['tag'], staged['channel'], staged['draft']), (tag, channel, True))
                self.assertEqual(staged['producer']['runId'], 77)
                notes = (self.directory / 'release-notes.md').read_text()
                self.assertIn('actions/runs/77', notes)
                self.assertIn('release candidate' if channel == 'candidate' else 'x86_64 release', notes)

    def test_nightly_cannot_use_acceptance_mode_to_bypass_producer_verification(self):
        argv = ['publish.py', '--tag', self.tag, '--directory', str(self.directory),
                '--publish', '--acceptance', str(self.directory / 'acceptance.json')]
        with patch.object(sys, 'argv', argv), patch.object(publish, 'verify', return_value=self.release), \
             patch.object(subprocess, 'run') as mutate, patch.object(sys, 'stderr', io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                publish.main()
            self.assertEqual(error.exception.code, 2)
            mutate.assert_not_called()



if __name__ == '__main__':
    unittest.main()
