"""Synthetic Git/GitHub protocol; no hosted builds or release mutations."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
import quiet_git  # noqa: F401  (disables git background maintenance, #99)
import nightly
import github_api
import release_tag

SHA = 'a' * 40
REPO = 'fixture/putgd'


class API:
    """Remote tags, refs and releases as one synthetic repository."""
    repository = REPO
    def __init__(self):
        self.downloaded = []
        self.after_release = None
        self.release_list = []
        self.assets = {}
        self.commit = SHA
        self.tags = {}
        self.created = []
    def json(self, path, **kwargs):
        if path.startswith('git/matching-refs/tags/'):
            prefix = unquote(path[len('git/matching-refs/tags/'):])
            return [[{'ref': 'refs/tags/' + name, 'object': {'type': 'commit', 'sha': sha}}
                     for name, sha in sorted(self.tags.items()) if name.startswith(prefix)]]
        if path.startswith('git/ref/tags/'):
            name = unquote(path[len('git/ref/tags/'):])
            if self.tags and name not in self.tags:
                raise subprocess.CalledProcessError(1, ['gh', 'api', path])
            return {'object': {'sha': self.tags.get(name, self.commit), 'type': 'commit'}}
        if path == 'git/refs' and kwargs.get('method') == 'POST':
            name = kwargs['data']['ref'][len('refs/tags/'):]
            self.tags[name] = kwargs['data']['sha']
            self.created.append(name)
            return {'ref': kwargs['data']['ref']}
        if path.startswith('releases/'):
            return self.after_release or self.release_list[0]
        if path.startswith('releases?'):
            return [self.release_list]
        raise AssertionError(path)
    def asset_bytes(self, asset_id):
        self.downloaded.append(asset_id)
        return self.assets[asset_id]


class GitHubProtocol(unittest.TestCase):
    def test_calls_are_repository_scoped_with_bounded_time(self):
        api = github_api.GitHub(REPO)
        with patch.object(subprocess, 'check_output', return_value=json.dumps({'ok': True})) as command:
            api.json('git/refs', method='POST', data={'ref': 'refs/tags/v0.1.1', 'sha': SHA})
        self.assertEqual(command.call_args.args[0], ['gh', 'api', 'repos/' + REPO + '/git/refs',
                                                     '--method', 'POST', '--input', '-'])
        self.assertEqual(command.call_args.kwargs['timeout'], 60)
        self.assertEqual(json.loads(command.call_args.kwargs['input']), {'ref': 'refs/tags/v0.1.1', 'sha': SHA})
        with self.assertRaises(ValueError):
            github_api.GitHub('../other/repo')

    def test_tag_lookup_is_exact_and_rejects_unexpected_refs(self):
        api = API()
        api.tags = {'v0.1.1-rc.1': SHA, 'v0.1.10': 'b' * 40}
        self.assertFalse(github_api.tag_exists(api, 'v0.1.1'))
        self.assertTrue(github_api.tag_exists(api, 'v0.1.1-rc.1'))
        self.assertEqual(github_api.matching_tags(api, 'v0.1.1'), ['v0.1.1-rc.1', 'v0.1.10'])
        with patch.object(api, 'json', return_value=[[{'ref': 'refs/heads/v0.1.1'}]]):
            with self.assertRaises(ValueError):
                github_api.matching_tags(api, 'v0.1.1')


class Fixture(unittest.TestCase):
    """One clean full-history Git checkout whose HEAD, committed 2026-10-12 UTC, is 26.1012.1."""
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgnightly-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'source'
        self.root.mkdir()
        (self.root / 'CMakeLists.txt').write_text('project(putgd LANGUAGES CXX)\n')
        self.git('init', '-q')
        self.git('config', 'user.name', 'Synthetic fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('add', '.')
        subprocess.run(['git', '-C', str(self.root), 'commit', '-qm', 'Fixture'], check=True,
                       env=dict(os.environ, GIT_AUTHOR_DATE='2026-10-12T03:04:05Z',
                                GIT_COMMITTER_DATE='2026-10-12T03:04:05Z'))
        self.sha = self.git('rev-parse', 'HEAD')
        self.api = API()
        self.api.commit = self.sha
        self.api.tags = {'unrelated': 'c' * 40}
        # The synthetic remote is local: fetching a created tag creates it here.
        fetch = subprocess.run
        def run(command, **kwargs):
            if command[:4] == ['git', '-C', str(self.root), 'fetch']:
                name = command[-1].split(':', 1)[1][len('refs/tags/'):]
                self.git('tag', '-f', name, self.api.tags[name])
                return subprocess.CompletedProcess(command, 0)
            return fetch(command, **kwargs)
        patcher = patch.object(subprocess, 'run', side_effect=run)
        patcher.start()
        self.addCleanup(patcher.stop)
    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True).strip()


class NightlyPlan(Fixture):
    def tag(self):
        return 'v26.1012.1-nightly'

    def test_preparation_names_immutable_head_identity(self):
        prepared = nightly.prepare(self.root)
        self.assertEqual(prepared['tag'], self.tag())
        self.assertEqual(prepared['sourceSha'], self.sha)
        self.assertEqual(prepared['commitCount'], 1)
        self.assertEqual(prepared['baseVersion'], '26.1012.1')

    def test_dirty_or_shallow_source_is_rejected(self):
        (self.root / 'untracked.txt').write_text('dirty\n')
        with self.assertRaisesRegex(ValueError, 'clean source'):
            nightly.prepare(self.root)

    def test_changed_source_creates_one_tag_and_builds(self):
        result = nightly.plan(self.api, self.root, 'refs/heads/nightly')
        self.assertFalse(result['skip'])
        self.assertEqual(self.api.created, [self.tag()])
        self.assertEqual(self.api.tags[self.tag()], self.sha)

    def test_published_unchanged_source_skips_without_creating_tags(self):
        self.api.tags[self.tag()] = self.sha
        with patch.object(nightly, 'published_nightly', return_value={'releaseId': 66}) as public:
            result = nightly.plan(self.api, self.root, 'refs/heads/nightly')
        self.assertTrue(result['skip'])
        self.assertEqual(result['publishedNightly'], {'releaseId': 66})
        self.assertEqual(public.call_args.args[1], self.tag())
        self.assertEqual(self.api.created, [])

    def test_unpublished_existing_tag_is_rebuilt_without_moving_it(self):
        self.api.tags[self.tag()] = self.sha
        with patch.object(nightly, 'published_nightly', return_value=None):
            result = nightly.plan(self.api, self.root, 'refs/heads/nightly')
        self.assertFalse(result['skip'])
        self.assertEqual(self.api.created, [])

    def test_existing_tag_on_another_source_never_mutates(self):
        self.api.tags[self.tag()] = 'b' * 40
        with self.assertRaisesRegex(ValueError, 'another source'):
            nightly.plan(self.api, self.root, 'refs/heads/nightly')
        self.assertEqual(self.api.created, [])

    def test_a_stable_release_or_pre_calendar_nightly_does_not_stop_nightlies(self):
        self.api.tags.update({'v26.1012.1': 'b' * 40, 'v0.1.1': 'c' * 40, 'v0.1.1-nightly.925': 'd' * 40})
        self.assertFalse(nightly.plan(self.api, self.root, 'refs/heads/nightly')['skip'])
        self.assertEqual(self.api.created, [self.tag()])

    def test_a_version_older_than_an_existing_nightly_is_refused(self):
        # A rewritten committer date must not publish a nightly that sorts below the last one.
        self.api.tags['v26.1013.5-nightly'] = 'b' * 40
        with self.assertRaisesRegex(ValueError, 'would not sort after the existing nightly v26.1013.5-nightly'):
            nightly.plan(self.api, self.root, 'refs/heads/nightly')
        self.assertEqual(self.api.created, [])
        self.assertEqual(nightly.newest_nightly(self.api), 'v26.1013.5-nightly')

    def test_only_the_nightly_branch_publishes_nightlies(self):
        for ref in ('refs/heads/main', 'refs/heads/feature', 'refs/tags/v0.1.1'):
            with self.subTest(ref=ref), self.assertRaisesRegex(ValueError, 'nightly branch'):
                nightly.plan(self.api, self.root, ref)
        self.assertEqual(self.api.created, [])

    def test_cli_writes_outputs_and_summary(self):
        with tempfile.TemporaryDirectory(prefix='pgout-') as directory:
            outputs, summary = Path(directory) / 'out', Path(directory) / 'summary'
            with patch.dict(os.environ, {'GITHUB_REPOSITORY': REPO, 'GITHUB_REF': 'refs/heads/nightly',
                                         'GITHUB_OUTPUT': str(outputs), 'GITHUB_STEP_SUMMARY': str(summary)}), \
                 patch.object(sys, 'argv', ['nightly.py', 'plan', '--root', str(self.root)]), \
                 patch.object(nightly, 'GitHub', return_value=self.api), patch('builtins.print'):
                nightly.main()
            self.assertEqual(outputs.read_text(), f'tag={self.tag()}\nsource_sha={self.sha}\nskip=false\n')
            self.assertIn(self.tag(), summary.read_text())


class StableRelease(Fixture):
    def cut(self):
        return release_tag.cut(self.api, self.root, 'refs/heads/main')

    def test_stable_tag_comes_from_head_and_is_reused_on_rerun(self):
        self.assertEqual(release_tag.choose(self.root, self.sha), 'v26.1012.1')
        result = self.cut()
        self.assertEqual((result['tag'], result['created'], result['release']['channel']), ('v26.1012.1', True, 'stable'))
        self.assertEqual(self.api.created, ['v26.1012.1'])
        rerun = self.cut()
        self.assertEqual((rerun['tag'], rerun['created']), ('v26.1012.1', False))
        self.assertEqual(self.api.created, ['v26.1012.1'])

    def test_stable_tag_on_another_commit_is_never_moved(self):
        self.api.tags['v26.1012.1'] = 'b' * 40
        with self.assertRaisesRegex(ValueError, 'already identifies'):
            self.cut()
        self.assertEqual(self.api.created, [])

    def test_only_main_cuts_stable_releases(self):
        for ref in ('refs/heads/nightly', 'refs/pull/1/merge'):
            with self.subTest(ref=ref), self.assertRaisesRegex(ValueError, 'main only'):
                release_tag.cut(self.api, self.root, ref)
        self.assertEqual(self.api.created, [])


class PublicNightly(unittest.TestCase):
    def setUp(self):
        self.api = API()
        self.tag = 'v26.1012.2-nightly'
        self.release = {'tag': self.tag, 'version': self.tag[1:], 'channel': 'nightly', 'gitCommit': SHA}
        files = {name: name.encode() for name in nightly.public_inventory(self.tag) if name != 'SHA256SUMS'}
        files['release-builds.json'] = json.dumps({'release': self.release}).encode()
        files['SHA256SUMS'] = ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
                                    for name, data in sorted(files.items())).encode()
        self.api.release_list = [{'id': 66, 'tag_name': self.tag, 'draft': False, 'prerelease': True,
                                 'assets': [{'id': i + 1, 'name': name, 'size': len(data), 'state': 'uploaded'}
                                            for i, (name, data) in enumerate(files.items())]}]
        self.api.assets = {i + 1: data for i, data in enumerate(files.values())}
    def test_only_complete_verified_public_nightly_skips(self):
        receipt = nightly.published_nightly(self.api, self.tag, self.release)
        self.assertEqual(receipt['releaseId'], 66)
        self.assertEqual(len(receipt['assetSha256']), 16)
        self.api.release_list[0]['draft'] = True
        self.assertIsNone(nightly.published_nightly(self.api, self.tag, self.release))
        self.api.release_list = []
        self.assertIsNone(nightly.published_nightly(self.api, self.tag, self.release))
    def test_download_count_changes_are_harmless_but_asset_identity_changes_fail(self):
        self.api.after_release = copy.deepcopy(self.api.release_list[0])
        self.api.after_release['assets'][0]['download_count'] = 99
        nightly.published_nightly(self.api, self.tag, self.release)
        self.api.after_release['assets'][0]['id'] = 999
        with self.assertRaises(ValueError):
            nightly.published_nightly(self.api, self.tag, self.release)

    def test_server_digests_avoid_payload_downloads_and_reject_wrong_digest(self):
        for asset in self.api.release_list[0]['assets']:
            asset['digest'] = 'sha256:' + hashlib.sha256(self.api.assets[asset['id']]).hexdigest()
        nightly.published_nightly(self.api, self.tag, self.release)
        by_name = {asset['name']: asset['id'] for asset in self.api.release_list[0]['assets']}
        self.assertEqual(set(self.api.downloaded), {by_name['SHA256SUMS'], by_name['release-builds.json']})
        self.api.release_list[0]['assets'][0]['digest'] = 'sha256:' + 'b' * 64
        with self.assertRaises(ValueError):
            nightly.published_nightly(self.api, self.tag, self.release)

    def test_corrupt_incomplete_wrong_channel_or_source_cannot_skip(self):
        original = copy.deepcopy(self.api.release_list)
        for mode in ('missing', 'corrupt', 'channel', 'source', 'stable'):
            with self.subTest(mode=mode):
                self.api.release_list = copy.deepcopy(original)
                assets = self.api.release_list[0]['assets']
                saved = dict(self.api.assets)
                if mode == 'missing':
                    assets.pop()
                elif mode == 'corrupt':
                    self.api.assets[assets[0]['id']] = b'corrupt'
                elif mode == 'stable':
                    self.api.release_list[0]['prerelease'] = False
                else:
                    self.release['channel' if mode == 'channel' else 'gitCommit'] = 'stable' if mode == 'channel' else 'b' * 40
                with self.assertRaises(ValueError):
                    nightly.published_nightly(self.api, self.tag, self.release)
                self.api.assets = saved
                self.release.update(channel='nightly', gitCommit=SHA)


if __name__ == '__main__':
    unittest.main()
