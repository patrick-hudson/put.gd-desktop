"""Release notes list the pull requests merged since the previous release (#188)."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
import quiet_git  # noqa: F401  (disables git background maintenance, #99)
import changelog
import publish

ENV = dict(os.environ, GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
           GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid')
REPO = 'fixture/putgd'
URL = 'https://github.com/' + REPO

# The lookup the generator would get from GitHub: labels and closing issues.
METADATA = {
    3: {'labels': [], 'closes': [11]},
    4: {'labels': ['bug'], 'closes': [12, 13]},
    5: {'labels': [], 'closes': []},
    6: {'labels': [], 'closes': []},
    7: {'labels': [], 'closes': [14]},
    8: {'labels': [], 'closes': []},
    10: {'labels': [], 'closes': []},
    20: {'labels': [], 'closes': []},
}

NIGHTLY = f"""## What's changed

### Fixes

- Upload the clipboard again ([#4]({URL}/pull/4)), closes [#12]({URL}/issues/12), [#13]({URL}/issues/13)

### Recording

- Keep the bar on its screen ([#5]({URL}/pull/5))

### Uploads

- Abort retired multipart uploads ([#7]({URL}/pull/7)), closes [#14]({URL}/issues/14)

### Lab

- Add the m74 Cinnamon server ([#6]({URL}/pull/6))

### Tests and CI

- Complete overlapping uploads by name ([#8]({URL}/pull/8))

## Downloads and sources

Full comparison: {URL}/compare/v0.1.1-nightly.4...v0.1.1-nightly.12
"""


class Changelog(unittest.TestCase):
    """A fixture history: nightly with numbered PR merges, promoted once to main.

        v0.1.0 (stable, main)
          #2 docs, #3 feat          -> v0.1.1-nightly.4
          #4 fix, #5 recording, #6 lab, #7 s3, #8 test -> v0.1.1-nightly.12
        #10 release promotion (nightly -> main) -> v0.1.1-rc.1
        #20 docs on nightly after the candidate
    """

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgchangelog-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.git('init', '-q', '-b', 'main')
        self.commit('base')
        self.git('tag', 'v0.1.0')
        self.git('tag', 'v0.1.0-dev.1.20261001000000+gabcdef123456')  # not a release grammar tag
        self.git('checkout', '-q', '-b', 'nightly')
        self.pull(2, 'docs: explain the release flow')
        self.pull(3, 'feat: reorder destinations in Settings (#11)')
        self.git('tag', 'v0.1.1-nightly.4')
        self.pull(4, 'fix: upload the clipboard again (#12, #13)')
        self.pull(5, 'Recording: keep the bar on its screen')
        self.pull(6, 'lab: add the m74 Cinnamon server')
        self.pull(7, 'feat(s3): abort retired multipart uploads (#14)')
        self.pull(8, 'test(operations): complete overlapping uploads by name')
        self.git('tag', 'v0.1.1-nightly.12')
        self.git('checkout', '-q', 'main')
        self.merge(10, 'nightly', 'release: v0.1.1', 'nightly')
        self.git('tag', 'v0.1.1-rc.1')
        self.git('checkout', '-q', 'nightly')
        self.pull(20, 'docs: candidate notes')
        self.git('tag', 'v0.1.1-nightly.15')

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True, env=ENV).strip()

    def commit(self, message):
        with (self.root / 'history.txt').open('a') as stream:
            stream.write(message + '\n')
        self.git('add', 'history.txt')
        self.git('commit', '-qm', message)

    def merge(self, number, branch, title, head=None):
        self.git('merge', '-q', '--no-ff', branch, '-m', f'Merge pull request #{number} from fixture/{head or branch}',
                 '-m', title)

    def pull(self, number, title):
        branch = f'claude/change-{number}'
        self.git('checkout', '-q', '-b', branch)
        self.commit(title)
        self.git('checkout', '-q', 'nightly')
        self.merge(number, branch, title)

    def section(self, tag, metadata=METADATA):
        seen = []

        def lookup(repository, numbers):
            seen.append((repository, numbers))
            return {number: metadata[number] for number in numbers if number in metadata}
        text = changelog.section(self.root, tag, REPO, lookup=lookup)
        self.assertEqual([repository for repository, _ in seen], [REPO])
        return text, seen[0][1]

    def test_nightly_lists_pull_requests_since_the_previous_nightly(self):
        text, looked_up = self.section('v0.1.1-nightly.12')
        self.assertEqual(looked_up, [4, 5, 6, 7, 8])
        self.assertEqual(text, NIGHTLY)
        # Deterministic: the same history and lookup give the same bytes.
        self.assertEqual(self.section('v0.1.1-nightly.12')[0], text)

    def test_candidate_lists_everything_since_the_previous_stable_and_folds_noise(self):
        text, looked_up = self.section('v0.1.1-rc.1')
        # The promotion pull request (#10, head nightly) is plumbing, not a change.
        self.assertEqual(looked_up, [2, 3, 4, 5, 6, 7, 8])
        self.assertTrue(text.startswith("## What's changed\n"))
        self.assertIn(f'### Features\n\n- Reorder destinations in Settings ([#3]({URL}/pull/3)), '
                      f'closes [#11]({URL}/issues/11)\n', text)
        for heading in ('### Documentation', '### Lab', '### Tests and CI'):
            self.assertNotIn(heading, text)
        self.assertIn(f'Also 3 documentation, lab and test changes: [#2]({URL}/pull/2), '
                      f'[#6]({URL}/pull/6), [#8]({URL}/pull/8).\n', text)
        self.assertTrue(text.endswith(f'Full comparison: {URL}/compare/v0.1.0...v0.1.1-rc.1\n'))
        # #20 merged after the candidate, so it isn't in it.
        self.assertNotIn('#20', text)

    def test_stable_on_the_candidate_commit_also_starts_from_the_previous_stable(self):
        self.git('tag', 'v0.1.1', 'v0.1.1-rc.1')
        self.assertEqual(changelog.previous_tag(self.root, 'v0.1.1'), 'v0.1.0')
        self.assertEqual(changelog.previous_tag(self.root, 'v0.1.1-nightly.15'), 'v0.1.1-nightly.12')
        text, _ = self.section('v0.1.1-nightly.15')
        self.assertIn(f'### Documentation\n\n- Candidate notes ([#20]({URL}/pull/20))\n', text)

    def test_lookup_reads_labels_and_closing_issues_in_chunks(self):
        queries = []

        def graphql(command, **kwargs):
            self.assertEqual(command[:3], ['gh', 'api', 'graphql'])
            self.assertIn('owner=fixture', command)
            self.assertIn('name=putgd', command)
            numbers = [int(n) for n in re.findall(r'pullRequest\(number: ([0-9]+)\)', command[4])]
            queries.append(numbers)
            pulls = {f'p{n}': {'labels': {'nodes': [{'name': 'bug'}]},
                               'closingIssuesReferences': {'nodes': [{'number': n + 100}, {'number': n + 50}]}}
                     for n in numbers}
            return json.dumps({'data': {'repository': pulls}})
        with patch.object(changelog.subprocess, 'check_output', side_effect=graphql):
            found = changelog.lookup_pulls(REPO, [1, 2, 3], chunk=2)
        self.assertEqual(queries, [[1, 2], [3]])
        self.assertEqual(found[3], {'labels': ['bug'], 'closes': [53, 103]})
        with patch.object(changelog.subprocess, 'check_output', side_effect=subprocess.CalledProcessError(1, 'gh')), \
             patch.object(sys, 'stderr'):
            self.assertEqual(changelog.lookup_pulls(REPO, [1, 2, 3], chunk=2), {})

    def test_calendar_nightly_follows_the_last_pre_calendar_nightly(self):
        # #373: every calendar version sorts after every 0.Y.Z one.
        self.pull(21, 'fix: first calendar change')
        self.git('tag', 'v26.1022.17-nightly')
        self.assertEqual(changelog.previous_tag(self.root, 'v26.1022.17-nightly'), 'v0.1.1-nightly.15')
        self.pull(22, 'fix: second calendar change')
        self.git('tag', 'v26.1023.19-nightly')
        self.assertEqual(changelog.previous_tag(self.root, 'v26.1023.19-nightly'), 'v26.1022.17-nightly')
        text, numbers = self.section('v26.1023.19-nightly')
        self.assertEqual(numbers, [22])
        self.assertIn('## Downloads and sources\n\nFull comparison: ' + URL + '/compare/v26.1022.17-nightly...', text)

    def test_unpublished_releases_never_start_the_changes(self):
        # A stable tag whose acceptance failed, or a nightly whose publication failed, was never released.
        self.git('checkout', '-q', 'main')
        self.merge(30, 'nightly', 'release: promote nightly', 'nightly')
        self.git('tag', 'v26.1022.20')
        self.commit('hotfix')
        self.git('tag', 'v26.1023.22')
        self.assertEqual(changelog.previous_tag(self.root, 'v26.1023.22'), 'v26.1022.20')
        self.assertEqual(changelog.previous_tag(self.root, 'v26.1023.22', {'v0.1.0', 'v26.1023.22'}), 'v0.1.0')
        self.assertEqual(changelog.previous_stable(self.root, 'v26.1023.22', set()), None)
        released = []
        text = changelog.section(self.root, 'v26.1023.22', REPO, lookup=None,
                                 released=lambda repository: released.append(repository) or {'v0.1.0'})
        self.assertEqual(released, [REPO])
        self.assertIn('## Downloads and sources\n\nFull comparison: ' + URL + '/compare/v0.1.0...', text)

    def test_publish_keeps_staging_when_the_history_is_unavailable(self):
        with patch.object(changelog, 'section', side_effect=subprocess.CalledProcessError(128, 'git')), \
             patch.object(sys, 'stderr') as stderr:
            text = publish.changes_section('v0.1.1-nightly.12', {'repository': REPO})
        self.assertIn('could not be listed', text)
        self.assertTrue(text.startswith("## What's changed\n"))
        self.assertTrue(text.endswith('## Downloads and sources\n\n'))
        self.assertTrue(stderr.write.called)


if __name__ == '__main__':
    unittest.main()
