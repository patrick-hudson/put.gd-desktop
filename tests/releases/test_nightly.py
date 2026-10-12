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

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
import nightly
import github_evidence as evidence

SHA = 'a' * 40
REPO = 'fixture/putgd'


class API:
    repository = REPO
    def __init__(self):
        self.dispatched = []
        self.downloaded = []
        self.package_runs = []
        self.after_release = None
        self.workflow_value = {'id': 12, 'path': '.github/workflows/build.yml', 'state': 'active'}
        self.run = {'id': 45, 'workflow_id': 12, 'path': '.github/workflows/build.yml',
                    'head_sha': SHA, 'repository': {'full_name': REPO},
                    'head_repository': {'full_name': REPO}, 'status': 'completed',
                    'conclusion': 'success', 'run_attempt': 1}
        self.runs = [self.run]
        self.jobs = [{'id': i + 1, 'name': name, 'status': 'completed', 'conclusion': 'success'}
                     for i, name in enumerate(evidence.BUILD_JOBS)]
        self.release_list = []
        self.assets = {}
        self.commit = SHA
    def workflow(self, path):
        value = dict(self.workflow_value)
        if path.endswith('release.yml'):
            value.update(id=13, path=path)
        return value
    def json(self, path, **kwargs):
        if path.startswith('git/ref/'):
            return {'object': {'sha': self.commit, 'type': 'commit'}}
        if path.endswith('/jobs?filter=latest&per_page=100'):
            return [{'jobs': self.jobs}]
        if path.startswith('actions/runs/'):
            return self.run
        if path.startswith('actions/workflows/') and '/runs?' in path:
            return [{'workflow_runs': self.package_runs if '/13/' in path else self.runs}]
        if path.startswith('releases/'):
            return self.after_release or self.release_list[0]
        if path.startswith('releases?'):
            return [self.release_list]
        raise AssertionError(path)
    def dispatch(self, path, tag, inputs):
        self.dispatched.append((path, tag, inputs))
        if path.endswith('release.yml'):
            run = dict(self.run, id=46, workflow_id=13, path=path, head_branch=tag, event='workflow_dispatch')
            self.package_runs = [run]
        else:
            self.runs = [self.run]
    def asset_bytes(self, asset_id):
        self.downloaded.append(asset_id)
        return self.assets[asset_id]


class GitHubProtocol(unittest.TestCase):
    def test_calls_are_repository_scoped_and_dispatch_uses_workflow_id_and_tag(self):
        api = evidence.GitHub(REPO)
        with patch.object(subprocess, 'check_output', side_effect=[
                json.dumps({'id': 12, 'path': evidence.BUILD_WORKFLOW, 'state': 'active'}), '']) as command:
            api.dispatch(evidence.BUILD_WORKFLOW, 'v0.1.1', {'package_tag': 'v0.1.1'})
        arguments = command.call_args_list
        self.assertEqual(arguments[0].args[0], ['gh', 'api', 'repos/' + REPO + '/actions/workflows/build.yml'])
        self.assertEqual(arguments[1].args[0], ['gh', 'api', 'repos/' + REPO + '/actions/workflows/12/dispatches',
                                               '--method', 'POST', '--input', '-'])
        self.assertEqual(arguments[0].kwargs['timeout'], 60)
        self.assertEqual(arguments[1].kwargs['timeout'], 60)
        self.assertEqual(json.loads(arguments[1].kwargs['input']),
                         {'ref': 'v0.1.1', 'inputs': {'package_tag': 'v0.1.1'}})
        with self.assertRaises(ValueError):
            evidence.GitHub('../other/repo')

    def test_wrong_workflow_path_or_disabled_workflow_rejects_dispatch(self):
        for value in ({'id': 12, 'path': evidence.PACKAGE_WORKFLOW, 'state': 'active'},
                      {'id': 12, 'path': evidence.BUILD_WORKFLOW, 'state': 'disabled_manually'}):
            with patch.object(subprocess, 'check_output', return_value=json.dumps(value)) as command:
                with self.assertRaises(ValueError):
                    evidence.GitHub(REPO).dispatch(evidence.BUILD_WORKFLOW, 'v0.1.1', {})
                self.assertEqual(command.call_count, 1)


class BuildEvidence(unittest.TestCase):
    def test_success_verifies_producer_source_and_all_jobs(self):
        api = API()
        receipt = evidence.verify_ci(api, 45, SHA, '.github/workflows/build.yml', evidence.BUILD_JOBS)
        self.assertEqual(receipt['runId'], 45)
        self.assertEqual(receipt['workflowId'], 12)
        self.assertEqual(receipt['sourceSha'], SHA)
        self.assertEqual(set(receipt['jobs']), set(evidence.BUILD_JOBS))

    def test_wrong_identity_or_required_result_is_rejected(self):
        for field, value in [('head_sha', 'b' * 40), ('workflow_id', 99),
                             ('repository', {'full_name': 'other/repo'}),
                             ('head_repository', {'full_name': 'fork/repo'}),
                             ('path', '.github/workflows/other.yml'), ('status', 'unknown'),
                             ('conclusion', 'failure'), ('conclusion', 'cancelled')]:
            with self.subTest(field=field, value=value):
                api = API()
                api.run[field] = value
                with self.assertRaises(ValueError):
                    evidence.verify_ci(api, 45, SHA, '.github/workflows/build.yml', evidence.BUILD_JOBS)
        for mode in ('missing', 'duplicate', 'failed', 'cancelled', 'unknown'):
            with self.subTest(mode=mode):
                api = API()
                if mode == 'missing':
                    api.jobs.pop()
                elif mode == 'duplicate':
                    api.jobs.append(dict(api.jobs[0]))
                else:
                    api.jobs[0]['conclusion'] = mode
                with self.assertRaises(ValueError):
                    evidence.verify_ci(api, 45, SHA, '.github/workflows/build.yml', evidence.BUILD_JOBS)

    def test_build_reuses_success_without_dispatch(self):
        api = API()
        receipt = evidence.coordinate_build(api, SHA, 'v0.1.1', timeout=0)
        self.assertEqual(receipt['runId'], 45)
        self.assertEqual(api.dispatched, [])

    def test_absent_build_dispatches_only_on_immutable_tag(self):
        api = API()
        api.runs = []
        receipt = evidence.coordinate_build(api, SHA, 'v0.1.1', timeout=1)
        self.assertEqual(receipt['runId'], 45)
        self.assertEqual(api.dispatched, [('.github/workflows/build.yml', 'v0.1.1', {'package_tag': 'v0.1.1'})])

    def test_pending_build_is_observed_and_timeout_does_not_dispatch(self):
        api = API()
        api.run['status'] = 'in_progress'
        api.run['conclusion'] = None
        with self.assertRaises(TimeoutError):
            evidence.coordinate_build(api, SHA, 'v0.1.1', timeout=0)
        self.assertEqual(api.dispatched, [])

    def test_failed_existing_build_is_not_hidden_by_new_dispatch(self):
        api = API()
        api.run['conclusion'] = 'failure'
        with self.assertRaises(ValueError):
            evidence.coordinate_build(api, SHA, 'v0.1.1', timeout=0)
        self.assertEqual(api.dispatched, [])

    def test_package_gate_uses_all_four_exact_named_producers(self):
        api = API()
        api.run.update(workflow_id=13, path=evidence.PACKAGE_WORKFLOW)
        api.jobs = [{'id': i + 1, 'name': name, 'status': 'completed', 'conclusion': 'success'}
                    for i, name in enumerate(evidence.PACKAGE_JOBS)]
        receipt = evidence.verify_ci(api, 45, SHA, evidence.PACKAGE_WORKFLOW, evidence.PACKAGE_JOBS)
        self.assertEqual(set(receipt['jobs']), set(evidence.PACKAGE_JOBS))
        api.jobs[0]['name'] += ' unrelated image'
        with self.assertRaises(ValueError):
            evidence.verify_ci(api, 45, SHA, evidence.PACKAGE_WORKFLOW, evidence.PACKAGE_JOBS)

    def test_current_package_jobs_verify_while_publisher_runs_without_weakening_build_gate(self):
        api = API()
        api.run.update(workflow_id=13, path=evidence.PACKAGE_WORKFLOW,
                       status='in_progress', conclusion=None)
        api.jobs = [{'id': i + 1, 'name': name, 'status': 'completed', 'conclusion': 'success'}
                    for i, name in enumerate(evidence.PACKAGE_JOBS)]
        self.assertEqual(set(evidence.verify_package_jobs(api, 45, SHA)['jobs']), set(evidence.PACKAGE_JOBS))
        with self.assertRaises(ValueError):
            evidence.verify_ci(api, 45, SHA, evidence.PACKAGE_WORKFLOW, evidence.PACKAGE_JOBS)
        api.jobs[0]['conclusion'] = 'failure'
        with self.assertRaises(ValueError):
            evidence.verify_package_jobs(api, 45, SHA)
        api.jobs[0]['conclusion'] = 'success'
        api.run.update(status='completed', conclusion='failure')
        with self.assertRaises(ValueError):
            evidence.verify_package_jobs(api, 45, SHA)

    def test_packages_dispatch_exact_tag_once_and_reuse_original_run(self):
        api = API()
        build = {'runId': 45, 'workflowId': 12}
        receipt = evidence.coordinate_packages(api, SHA, 'v0.1.1', 71, build, timeout=0)
        self.assertEqual(receipt['runId'], 46)
        self.assertEqual(api.dispatched[0][1], 'v0.1.1')
        self.assertEqual(api.dispatched[0][2]['source_sha'], SHA)
        retry = evidence.coordinate_packages(api, SHA, 'v0.1.1', 72, build, timeout=0)
        self.assertTrue(retry['reused'])
        self.assertEqual(retry['runId'], receipt['runId'])
        self.assertEqual(len(api.dispatched), 1)
        api.package_runs[0]['conclusion'] = 'failure'
        with self.assertRaisesRegex(ValueError, 'recovery: 46'):
            evidence.coordinate_packages(api, SHA, 'v0.1.1', 72, build, timeout=0)
        self.assertEqual(len(api.dispatched), 1)


class PinnedSource(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgnightly-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'source'
        self.root.mkdir()
        (self.root / 'CMakeLists.txt').write_text('project(putgd VERSION 0.1.1 LANGUAGES CXX)\n')
        self.git('init', '-q')
        self.git('config', 'user.name', 'Synthetic fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('add', '.')
        subprocess.run(['git', '-C', str(self.root), 'commit', '-qm', 'Fixture'], check=True,
                       env=dict(os.environ, GIT_AUTHOR_DATE='2026-10-12T03:04:05Z',
                                GIT_COMMITTER_DATE='2026-10-12T03:04:05Z'))
        self.sha = self.git('rev-parse', 'HEAD')
        self.path = Path(self.temporary.name) / 'nightly-preparation.json'
    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True).strip()
    def test_push_and_schedule_pin_nightly_not_default_source(self):
        api = API()
        api.commit = self.sha
        push = nightly.pin(api, 71, 1, {'ref': 'refs/heads/nightly', 'after': self.sha}, self.path)
        scheduled = nightly.pin(api, 72, 1, {}, self.path)
        self.assertEqual(push['sourceSha'], self.sha)
        self.assertEqual(scheduled['sourceSha'], self.sha)
        receipt = nightly.prepare(self.root, scheduled)
        self.assertEqual(receipt['tag'], 'v0.1.1-dev.1.20261012030405+g' + self.sha[:12])
        self.assertEqual(receipt['sourceSha'], self.sha)
    def test_retry_preserves_original_after_branch_advance(self):
        api = API()
        api.commit = self.sha
        receipt = nightly.prepare(self.root, nightly.pin(api, 71, 1, {}, self.path))
        self.path.write_text(json.dumps(receipt))
        api.commit = 'b' * 40
        retry = nightly.pin(api, 71, 2, {}, self.path)
        self.assertEqual(nightly.prepare(self.root, retry), receipt)
    def test_missing_or_contradictory_retry_fails(self):
        with self.assertRaises(ValueError):
            nightly.pin(API(), 71, 2, {}, self.path)
        receipt = nightly.prepare(self.root, {'schemaVersion': 1, 'repository': REPO,
                                            'runId': 71, 'sourceSha': self.sha})
        for key, value in [('runId', 99), ('sourceSha', 'b' * 40), ('tag', 'v0.1.1')]:
            modified = dict(receipt, **{key: value})
            self.path.write_text(json.dumps(modified))
            with self.assertRaises(ValueError):
                retry = nightly.pin(API(), 71, 2, {}, self.path)
                nightly.prepare(self.root, retry)
    def test_tag_conflict_never_mutates(self):
        api = API()
        api.commit = 'b' * 40
        with self.assertRaises(ValueError):
            nightly.ensure_tag(api, self.root, 'v0.1.1', self.sha)
        self.assertEqual(api.dispatched, [])
    def test_uncoordinated_main_and_pr_keep_production_compile(self):
        self.assertFalse(nightly.build_mode(self.root, 'push', 'refs/heads/main', ''))
        self.assertFalse(nightly.build_mode(self.root, 'pull_request', 'refs/pull/1/merge', ''))
        self.assertTrue(nightly.build_mode(self.root, 'push', 'refs/heads/nightly', ''))
        with self.assertRaises(ValueError):
            nightly.build_mode(self.root, 'workflow_dispatch', 'refs/heads/main', 'v0.1.1')
        self.git('tag', 'v0.1.1')
        self.assertTrue(nightly.build_mode(self.root, 'workflow_dispatch', 'refs/tags/v0.1.1', 'v0.1.1'))

    def test_package_dispatch_does_not_wait_for_build(self):
        api = API()
        prepared = nightly.prepare(self.root, {'schemaVersion': 1, 'repository': REPO,
                                              'runId': 71, 'sourceSha': self.sha})
        api.run['head_sha'] = self.sha
        release = {'tag': prepared['tag'], 'channel': 'nightly', 'gitCommit': self.sha}
        with patch.object(nightly, 'ensure_tag', return_value=release), \
             patch.object(nightly, 'published_nightly', return_value=None), \
             patch.object(nightly, 'coordinate_build') as build:
            result = nightly.execute(api, self.root, prepared)
        build.assert_not_called()
        self.assertEqual(result['packageDispatch']['runId'], 46)
        self.assertEqual(api.dispatched[0][1], prepared['tag'])
        self.assertEqual(api.dispatched[0][2]['source_sha'], self.sha)
        self.assertEqual(api.dispatched[0][2]['build_run_id'], '')
        self.assertEqual(api.dispatched[0][2]['build_workflow_id'], '')

    def test_release_ci_accepts_pinned_source_without_prewaited_build(self):
        self.git('tag', 'v0.1.1')
        with patch.object(nightly, 'coordinate_build', return_value={'runId': 45}) as build:
            result = nightly.release_ci(API(), self.root, 'v0.1.1', 'refs/tags/v0.1.1', self.sha)
        self.assertEqual(result['runId'], 45)
        self.assertEqual(build.call_args.args[1:], (self.sha, 'v0.1.1'))

    def test_verified_public_noop_stops_before_all_builder_dispatches(self):
        api = API()
        prepared = nightly.prepare(self.root, {'schemaVersion': 1, 'repository': REPO,
                                              'runId': 71, 'sourceSha': self.sha})
        release = {'tag': prepared['tag'], 'channel': 'nightly', 'gitCommit': self.sha}
        with patch.object(nightly, 'ensure_tag', return_value=release), \
             patch.object(nightly, 'published_nightly', return_value={'releaseId': 66}), \
             patch.object(nightly, 'coordinate_build') as build, \
             patch.object(nightly, 'coordinate_packages') as packages:
            result = nightly.execute(api, self.root, prepared)
        self.assertTrue(result['skipped'])
        build.assert_not_called()
        packages.assert_not_called()


class PublicNightly(unittest.TestCase):
    def setUp(self):
        self.api = API()
        self.tag = 'v0.1.1-dev.2.20261012030405+g' + SHA[:12]
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
        self.assertEqual(len(receipt['assetSha256']), 9)
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
