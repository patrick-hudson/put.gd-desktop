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
                    'html_url': 'https://github.com/owner/repo/actions/runs/123'}
        names = ['contracts', 'linux (24.04)', 'linux (26.04)', 'portable (windows-2022)',
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
                             ('path', '.github/workflows/other.yml'), ('repository', {'full_name': 'fork/repo'})]:
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


if __name__ == '__main__':
    unittest.main()
