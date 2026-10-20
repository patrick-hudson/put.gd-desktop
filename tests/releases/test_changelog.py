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

NIGHTLY = f"""## Changes since v0.1.1-nightly.4

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

    def test_change_types_follow_labels_then_the_title_kind(self):
        # CHANGELOG.md (#217) sorts by Keep a Changelog type, not by area.
        self.assertEqual(changelog.change_type('feat(s3): abort retired multipart uploads'), 'Added')
        self.assertEqual(changelog.change_type('fix: upload the clipboard again'), 'Fixed')
        self.assertEqual(changelog.change_type('History: keep the selection'), 'Changed')
        self.assertEqual(changelog.change_type('History: keep the selection', ['bug']), 'Fixed')
        self.assertEqual(changelog.change_type('fix: x', ['enhancement']), 'Added')
        self.assertEqual(changelog.previous_stable(self.root, 'nightly'), 'v0.1.0')

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
        self.assertTrue(text.startswith('## Changes since v0.1.0\n'))
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

    def test_without_the_lookup_titles_supply_the_group_and_issues(self):
        text = changelog.section(self.root, 'v0.1.1-nightly.12', REPO, lookup=lambda *_: {})
        self.assertIn(f'- Upload the clipboard again ([#4]({URL}/pull/4)), closes [#12]({URL}/issues/12), '
                      f'[#13]({URL}/issues/13)\n', text)
        self.assertIn('### Fixes', text)
        self.assertEqual(changelog.section(self.root, 'v0.1.1-nightly.12', REPO, lookup=None), text)

    def test_labels_win_over_title_prefixes(self):
        relabelled = {**METADATA, 6: {'labels': ['recording'], 'closes': []}}
        text, _ = self.section('v0.1.1-nightly.12', relabelled)
        self.assertIn(f'### Recording\n\n- Keep the bar on its screen ([#5]({URL}/pull/5))\n'
                      f'- Add the m74 Cinnamon server ([#6]({URL}/pull/6))\n', text)
        self.assertNotIn('### Lab', text)

    def test_first_release_of_a_channel_lists_the_whole_history(self):
        self.assertIsNone(changelog.previous_tag(self.root, 'v0.1.1-nightly.4'))
        text, looked_up = self.section('v0.1.1-nightly.4')
        self.assertEqual(looked_up, [2, 3])
        self.assertTrue(text.startswith('## Changes in v0.1.1-nightly.4\n'))
        self.assertIn('first release of its kind', text)

    def test_nothing_merged_says_so(self):
        self.git('tag', 'v0.1.1-nightly.16', 'v0.1.1-nightly.15')
        self.git('checkout', '-q', 'nightly')
        self.commit('direct commit without a pull request')
        self.git('tag', 'v0.1.1-nightly.17')
        text, _ = self.section('v0.1.1-nightly.17')
        self.assertIn('No pull requests were merged since then.', text)

    def test_classify_titles(self):
        for title, group in [('feat: x', 'features'), ('fix(proton): x', 'uploads'), ('docs(evidence): x', 'docs'),
                             ('ci: x', 'test'), ('AppImage: x', 'packaging'), ('releases: x', 'packaging'),
                             ('record: x', 'recording'), ('GNOME: x', 'other'), ('Plain words', 'other')]:
            with self.subTest(title=title):
                self.assertEqual(changelog.classify(title), group)
        self.assertEqual(changelog.classify('Plain words', ['enhancement']), 'features')
        self.assertEqual(changelog.classify('feat: x', ['documentation']), 'docs')

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

    def test_publish_keeps_staging_when_the_history_is_unavailable(self):
        with patch.object(changelog, 'section', side_effect=subprocess.CalledProcessError(128, 'git')), \
             patch.object(sys, 'stderr') as stderr:
            text = publish.changes_section('v0.1.1-nightly.12', {'repository': REPO})
        self.assertIn('could not be listed', text)
        self.assertTrue(stderr.write.called)


if __name__ == '__main__':
    unittest.main()
