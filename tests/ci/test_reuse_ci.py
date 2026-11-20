"""Only complete successful source checks on the same trunk commit can be reused."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/ci'))
import reuse_ci


class Reuse(unittest.TestCase):
    def setUp(self):
        self.run = {'id': 123, 'run_attempt': 2, 'head_sha': 'a' * 40, 'head_branch': 'nightly',
                    'event': 'push', 'status': 'completed', 'conclusion': 'success',
                    'path': '.github/workflows/ci.yml', 'repository': {'full_name': 'owner/repo'},
                    'head_repository': {'full_name': 'owner/repo'},
                    'html_url': 'https://github.com/owner/repo/actions/runs/123'}
        names = ['contracts', 'linux (24.04)', 'linux (26.04)',
                 'sanitizers (clang ASan/UBSan)', 'CI']
        self.jobs = {'total_count': len(names), 'jobs': [
            {'id': i, 'name': name, 'conclusion': 'success'} for i, name in enumerate(names, 1)]}
        self.paths = []

    def api(self, path):
        self.paths.append(path)
        return {'workflow_runs': [self.run]} if '/workflows/' in path else self.jobs

    def find(self):
        return reuse_ci.find_run('owner/repo', 'a' * 40, api=self.api)

    def test_exact_commit_and_successful_attempt_are_recorded(self):
        proof = self.find()
        self.assertEqual((proof['gitCommit'], proof['runId'], proof['runAttempt']), ('a' * 40, 123, 2))
        self.assertIn('head_sha=' + 'a' * 40, self.paths[0])
        self.assertIn('/runs/123/attempts/2/jobs?', self.paths[1])

    def test_other_commit_branch_event_workflow_or_failure_does_not_skip_tests(self):
        original = copy.deepcopy(self.run)
        for field, value in [('head_sha', 'b' * 40), ('head_branch', 'topic'), ('event', 'pull_request'),
                             ('status', 'in_progress'), ('conclusion', 'cancelled'),
                             ('path', '.github/workflows/other.yml'), ('repository', {'full_name': 'fork/repo'}),
                             ('head_repository', {'full_name': 'fork/repo'})]:
            with self.subTest(field=field):
                self.run = {**original, field: value}
                self.assertIsNone(self.find())

    def test_skipped_failed_missing_or_paginated_jobs_cannot_establish_reuse(self):
        original = copy.deepcopy(self.jobs)
        for result in ('skipped', 'failure', 'cancelled', None):
            for index in range(len(original['jobs'])):
                self.jobs = copy.deepcopy(original)
                self.jobs['jobs'][index]['conclusion'] = result
                self.assertIsNone(self.find())
        self.jobs = copy.deepcopy(original)
        self.jobs['jobs'].pop()
        self.assertIsNone(self.find())
        self.jobs['total_count'] -= 1
        self.assertIsNone(self.find())

    def test_api_failure_records_full_test_fallback(self):
        with tempfile.TemporaryDirectory(prefix='pg-reuse-') as temp, \
                patch.dict(os.environ, {'GITHUB_REPOSITORY': 'owner/repo'}), \
                patch.object(reuse_ci.subprocess, 'check_output', return_value='a' * 40), \
                patch.object(reuse_ci, 'find_run', side_effect=subprocess.CalledProcessError(1, ['gh'])):
            self.assertIsNone(reuse_ci.reuse(temp))
            proof = json.loads((Path(temp) / 'build/reused-ci.json').read_text())
            self.assertFalse(proof['reused'])
            self.assertIsNone(proof['producer'])


class Packages(unittest.TestCase):
    """A nightly publishes push-CI packages only from a complete, successful nightly push run."""
    commit = 'a' * 40

    def setUp(self):
        self.run = {'id': 77, 'run_attempt': 1, 'head_sha': self.commit, 'head_branch': 'nightly',
                    'event': 'push', 'status': 'completed', 'conclusion': 'success',
                    'path': '.github/workflows/ci.yml', 'repository': {'full_name': 'owner/repo'},
                    'head_repository': {'full_name': 'owner/repo'},
                    'html_url': 'https://github.com/owner/repo/actions/runs/77'}
        names = sorted(reuse_ci.SOURCE_JOBS | reuse_ci.PACKAGE_JOBS) + ['prune caches']
        self.jobs = {'total_count': len(names), 'jobs': [
            {'id': i, 'name': name, 'conclusion': 'success'} for i, name in enumerate(names, 1)]}
        self.artifacts = [{'id': 100 + i, 'name': name, 'expired': False,
                           'workflow_run': {'id': 77, 'head_sha': self.commit}}
                          for i, name in enumerate(reuse_ci.PACKAGE_ARTIFACTS + ('ui-catalog',))]
        self.paths, self.slept, self.now = [], [], 0.0

    def api(self, path):
        self.paths.append(path)
        if '/workflows/' in path:
            return {'workflow_runs': [self.run]}
        if path.endswith('/artifacts?per_page=100'):
            return {'total_count': len(self.artifacts), 'artifacts': self.artifacts}
        if '/attempts/' in path:
            return self.jobs
        return self.run

    def sleep(self, seconds):
        self.slept.append(seconds)
        self.now += seconds

    def find(self, wait=0):
        return reuse_ci.find_package_run('owner/repo', self.commit, wait=wait, api=self.api, sleep=self.sleep,
                                         clock=lambda: self.now, log=lambda *a, **k: None)

    def test_a_complete_nightly_push_run_is_reused_with_its_artifacts(self):
        proof, _ = self.find()
        self.assertEqual((proof['runId'], proof['runAttempt'], proof['gitCommit']), (77, 1, self.commit))
        self.assertEqual(set(proof['artifacts']), set(reuse_ci.PACKAGE_ARTIFACTS))
        self.assertEqual(set(proof['jobs']), reuse_ci.SOURCE_JOBS | reuse_ci.PACKAGE_JOBS)
        # The query asks for this commit's nightly pushes, whatever their state.
        self.assertIn('event=push', self.paths[0])
        self.assertIn('branch=nightly', self.paths[0])
        self.assertIn('head_sha=' + self.commit, self.paths[0])
        self.assertNotIn('status=', self.paths[0])

    def test_tests_and_every_package_job_must_have_succeeded(self):
        self.assertIn('linux (24.04)', reuse_ci.SOURCE_JOBS)
        self.assertIn('packages / sbom', reuse_ci.PACKAGE_JOBS)
        original = copy.deepcopy(self.jobs)
        for index, job in enumerate(original['jobs']):
            if job['name'] == 'prune caches':
                continue
            for result in ('failure', 'skipped', 'cancelled', None):
                self.jobs = copy.deepcopy(original)
                self.jobs['jobs'][index]['conclusion'] = result
                with self.subTest(job=job['name'], result=result):
                    self.assertIsNone(self.find()[0])

    def test_pull_request_branch_fork_or_other_workflow_runs_are_never_reused(self):
        original = copy.deepcopy(self.run)
        for field, value in [('event', 'pull_request'), ('event', 'workflow_dispatch'), ('head_branch', 'topic'),
                             ('head_sha', 'b' * 40), ('path', '.github/workflows/nightly.yml'),
                             ('repository', {'full_name': 'fork/repo'}), ('head_repository', {'full_name': 'fork/repo'}),
                             ('conclusion', 'failure'), ('conclusion', 'cancelled')]:
            with self.subTest(field=field, value=value):
                self.run = {**original, field: value}
                self.assertIsNone(self.find()[0])

    def test_missing_expired_duplicate_or_foreign_artifacts_mean_a_rebuild(self):
        original = copy.deepcopy(self.artifacts)
        changes = [lambda a: a.pop(0), lambda a: a[0].update(expired=True),
                   lambda a: a.append(dict(a[0], id=999)), lambda a: a[1].update(workflow_run={'id': 78, 'head_sha': self.commit}),
                   lambda a: a[2].update(workflow_run={'id': 77, 'head_sha': 'b' * 40})]
        for number, change in enumerate(changes):
            self.artifacts = copy.deepcopy(original)
            change(self.artifacts)
            with self.subTest(change=number):
                proof, reason = self.find()
                self.assertIsNone(proof)
                self.assertIn('unexpired artifacts', reason)

    def test_a_run_in_progress_is_waited_for_and_then_reused(self):
        states = iter([('in_progress', None), ('in_progress', None), ('completed', 'success')])
        real = self.api
        def api(path):
            if '/workflows/' in path:
                status, conclusion = next(states)
                self.run.update(status=status, conclusion=conclusion)
            return real(path)
        self.api = api
        proof, _ = self.find(wait=1500)
        self.assertEqual(proof['runId'], 77)
        self.assertEqual(len(self.slept), 2)

    def test_a_run_cancelled_while_waited_for_or_still_running_at_the_deadline_means_a_rebuild(self):
        self.run.update(status='in_progress', conclusion=None)
        proof, reason = self.find(wait=600)
        self.assertIsNone(proof)
        self.assertIn('still in_progress after 10 minutes', reason)
        self.assertLessEqual(self.now, 600)
        self.run.update(status='completed', conclusion='cancelled')
        self.assertIsNone(self.find(wait=600)[0])
        self.run = {**self.run, 'head_sha': 'b' * 40}
        self.assertEqual(self.find()[1], 'no push-CI run of this commit')

    def test_record_outputs_and_lookup_failure(self):
        with tempfile.TemporaryDirectory(prefix='pg-reuse-') as temp:
            output, values = Path(temp) / 'reused-ci.json', Path(temp) / 'out'
            with patch.dict(os.environ, {'GITHUB_OUTPUT': str(values), 'GITHUB_STEP_SUMMARY': str(Path(temp) / 's')}), \
                    patch.object(reuse_ci, 'github', side_effect=self.api), patch('builtins.print'):
                reuse_ci.packages('owner/repo', self.commit, output, wait=0)
                record = json.loads(output.read_text())
                self.assertEqual((record['reused'], record['packages'], record['producer']['runId']), (True, True, 77))
                self.assertEqual(values.read_text(), 'run_id=77\n')
                with patch.object(reuse_ci, 'find_package_run', side_effect=subprocess.CalledProcessError(1, ['gh'])):
                    self.assertIsNone(reuse_ci.packages('owner/repo', self.commit, output, wait=0))
                self.assertFalse(json.loads(output.read_text())['reused'])
                self.assertTrue(values.read_text().endswith('run_id=\n'))

    def test_a_rehearsal_accepts_a_manual_run_and_marks_its_record(self):
        self.run.update(event='workflow_dispatch', head_branch='ci/topic')
        self.assertIsNone(self.find()[0])
        proof, reason = reuse_ci.rehearse_run('owner/repo', self.commit, 77, api=self.api)
        self.assertTrue(proof['rehearsal'])
        self.assertEqual(proof['event'], 'workflow_dispatch')
        self.assertIn('rehearsal', reason)
        self.assertIsNone(reuse_ci.rehearse_run('owner/repo', 'b' * 40, 77, api=self.api)[0])

    def test_job_and_artifact_names_match_the_workflows(self):
        root = Path(__file__).resolve().parents[2]
        package = (root / '.github/workflows/package.yml').read_text()
        for name in reuse_ci.NATIVE:
            self.assertIn('- name: ' + name + '\n', package)
        for name in ('arch', 'arch-install', 'appimage', 'appimage-sources', 'appimage-pipewire', 'sbom'):
            self.assertIn('\n  ' + name + ':\n', package)
        for name in ('release-appimage', 'appimage-sources', 'release-sbom'):
            self.assertIn('name: ' + name + '\n', package)
        self.assertIn('name: release-native-${{ matrix.name }}', package)
        sys.path.insert(0, str(root / 'tools/releases'))
        import collect
        self.assertEqual(tuple(name for name, _, _ in collect.NATIVE_TARGETS), (*reuse_ci.NATIVE, 'archlinux'))
        # Nothing in a workflow may accept a rehearsal record.
        for path in (root / '.github/workflows').glob('*.yml'):
            self.assertNotIn('--rehearse', path.read_text())


if __name__ == '__main__':
    unittest.main()
