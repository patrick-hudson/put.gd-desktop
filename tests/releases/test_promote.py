"""Promotion pull requests name nightly's version and carry its CHANGELOG.md section (#217)."""
import contextlib
import datetime
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
import quiet_git  # noqa: F401  (disables git background maintenance, #99)
import promote

ENV = dict(os.environ, GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
           GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid')
REPO = 'fixture/putgd'
URL = 'https://github.com/' + REPO
TODAY = datetime.date(2026, 10, 20)
METADATA = {3: {'labels': [], 'closes': [11]}, 4: {'labels': ['bug'], 'closes': [12]},
            5: {'labels': [], 'closes': []}, 6: {'labels': [], 'closes': []}}

SECTION = f"""## [0.1.2] - 2026-10-20

### Added

- Reorder destinations in Settings ([#3]({URL}/pull/3)), closes [#11]({URL}/issues/11)

### Fixed

- History: keep the selection ([#4]({URL}/pull/4)), closes [#12]({URL}/issues/12)

### Changed

- Keep the bar on its screen ([#5]({URL}/pull/5))

Also 1 documentation changes: [#6]({URL}/pull/6).
"""


class Promotion(unittest.TestCase):
    def setUp(self):
        # promote.py commits the changelog with the operator's own git identity.
        identity = patch.dict(os.environ, {key: ENV[key] for key in ENV if key.startswith('GIT_')})
        identity.start()
        self.addCleanup(identity.stop)
        self.temporary = tempfile.TemporaryDirectory(prefix='pgpromote-')
        self.addCleanup(self.temporary.cleanup)
        top = Path(self.temporary.name)
        self.origin, self.root = top / 'origin.git', top / 'work'
        subprocess.run(['git', 'init', '-q', '--bare', str(self.origin)], check=True)
        subprocess.run(['git', 'clone', '-q', str(self.origin), str(self.root)], check=True,
                       capture_output=True)
        self.commit('project(putgd VERSION 0.1.2 LANGUAGES CXX)\n', 'base')
        self.git('branch', '-M', 'main')
        self.git('push', '-q', 'origin', 'main', 'main:nightly')

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True, env=ENV).strip()

    def commit(self, cmake, message):
        (self.root / 'CMakeLists.txt').write_text(cmake)
        self.git('add', '.')
        self.git('commit', '-qm', message)

    def pull(self, number, title):
        branch = f'claude/change-{number}'
        self.git('checkout', '-q', '-b', branch)
        with (self.root / 'history.txt').open('a') as stream:
            stream.write(title + '\n')
        self.git('add', 'history.txt')
        self.git('commit', '-qm', title)
        self.git('checkout', '-q', 'nightly')
        self.git('merge', '-q', '--no-ff', branch, '-m', f'Merge pull request #{number} from fixture/{branch}',
                 '-m', title)

    def nightly(self):
        self.git('checkout', '-q', '-b', 'nightly', 'origin/nightly')
        self.pull(3, 'feat: reorder destinations in Settings (#11)')
        self.pull(4, 'History: keep the selection (#12)')
        self.pull(5, 'Recording: keep the bar on its screen')
        self.pull(6, 'docs: explain the release flow')
        self.git('push', '-q', 'origin', 'nightly')

    def plan(self, metadata=METADATA):
        return promote.plan(self.root, repository=REPO, today=TODAY,
                            lookup=lambda repository, numbers: {n: metadata[n] for n in numbers if n in metadata})

    def test_title_section_and_body_come_from_nightly_pull_requests(self):
        self.nightly()
        promotion = self.plan()
        self.assertEqual(promotion['title'], 'release: v0.1.2')
        self.assertEqual(promotion['section'], SECTION)
        # No stable release yet: the whole history, linked to the release itself.
        self.assertEqual(promotion['changelog'], promote.HEADER + '\n' + SECTION + '\n' +
                         f'[0.1.2]: {URL}/releases/tag/v0.1.2\n')
        body = promotion['body']
        self.assertIn('## Changes\n\n### Added\n\n- Reorder destinations', body)
        self.assertNotIn('## [0.1.2]', body)
        self.assertIn('v0.1.2-rc.N', body)
        # Labels win over the title prefix; without the lookup the prefix decides.
        offline = promote.plan(self.root, repository=REPO, today=TODAY, lookup=None)['section']
        self.assertIn('### Changed\n\n- History: keep the selection ([#4]', offline)

    def test_section_starts_at_the_previous_stable_release(self):
        self.nightly()
        self.git('tag', 'v0.1.1', 'HEAD~2')
        self.git('push', '-q', 'origin', 'v0.1.1')
        promotion = self.plan()
        self.assertNotIn('[#3]', promotion['section'])
        self.assertNotIn('[#4]', promotion['section'])
        self.assertIn('[#5]', promotion['section'])
        self.assertTrue(promotion['changelog'].endswith(f'[0.1.2]: {URL}/compare/v0.1.1...v0.1.2\n'))

    def test_new_sections_and_links_go_first_below_unreleased(self):
        existing = (promote.HEADER + '\n## [Unreleased]\n\n- Draft\n\n## [0.1.1] - 2026-10-01\n\n### Fixed\n\n'
                    f'- Old fix\n\n[Unreleased]: {URL}/compare/v0.1.1...HEAD\n[0.1.1]: {URL}/releases/tag/v0.1.1\n')
        text = promote.with_section(existing, '0.1.2', '## [0.1.2] - 2026-10-20\n\n### Added\n\n- New\n',
                                    f'[0.1.2]: {URL}/compare/v0.1.1...v0.1.2')
        self.assertLess(text.index('## [Unreleased]'), text.index('## [0.1.2]'))
        self.assertLess(text.index('## [0.1.2]'), text.index('## [0.1.1]'))
        self.assertIn('- New\n\n## [0.1.1]', text)
        self.assertTrue(text.endswith(f'[Unreleased]: {URL}/compare/v0.1.1...HEAD\n'
                                      f'[0.1.2]: {URL}/compare/v0.1.1...v0.1.2\n[0.1.1]: {URL}/releases/tag/v0.1.1\n'))
        self.assertIsNone(promote.with_section(text, '0.1.2', '## [0.1.2] - x\n', '[0.1.2]: y'))

    def test_open_adds_the_changelog_through_a_nightly_pull_request_first(self):
        self.nightly()
        promotion = self.plan()
        calls = []

        def gh(root, *args, input):
            calls.append((['gh', *args], input))
            return f'{URL}/pull/{99 if args[3] == "nightly" else 100}'
        status = self.git('status', '--porcelain')
        with patch.object(promote, 'gh', side_effect=gh), contextlib.redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(promote.open_pulls(self.root, promotion), f'{URL}/pull/99')
        self.assertEqual(printed.getvalue(), f'{URL}/pull/100\n')
        # The section is on its own branch, one commit on top of nightly; nothing else changed.
        self.assertEqual(self.git('--git-dir', str(self.origin), 'show', 'changelog/v0.1.2:CHANGELOG.md') + '\n',
                         promotion['changelog'])
        self.assertEqual(self.git('--git-dir', str(self.origin), 'rev-parse', 'changelog/v0.1.2^'),
                         self.git('rev-parse', 'origin/nightly'))
        self.assertEqual(self.git('--git-dir', str(self.origin), 'diff', '--name-only', 'nightly', 'changelog/v0.1.2'),
                         'CHANGELOG.md')
        self.assertEqual(self.git('status', '--porcelain'), status)
        self.assertFalse((self.root / 'CHANGELOG.md').exists())
        (changelog_pr, _), (promotion_pr, body) = calls
        self.assertEqual(changelog_pr[:7], ['gh', 'pr', 'create', '--base', 'nightly', '--head', 'changelog/v0.1.2'])
        self.assertEqual(promotion_pr[:7], ['gh', 'pr', 'create', '--base', 'main', '--head', 'nightly'])
        self.assertIn(f'- [ ] {URL}/pull/99 is merged', body)
        self.assertIn('### Fixed\n\n- History: keep the selection', body)

    def test_open_skips_the_changelog_pull_request_once_nightly_has_the_section(self):
        self.nightly()
        (self.root / 'CHANGELOG.md').write_text(self.plan()['changelog'])
        self.git('add', 'CHANGELOG.md')
        self.git('commit', '-qm', 'changelog: v0.1.2')
        self.git('push', '-q', 'origin', 'nightly')
        promotion = self.plan()
        self.assertIsNone(promotion['changelog'])
        calls = []
        with patch.object(promote, 'gh', side_effect=lambda root, *args, input: calls.append(['gh', *args]) or 'url'), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertIsNone(promote.open_pulls(self.root, promotion))
        self.assertEqual([command[4] for command in calls], ['main'])

    def test_nothing_to_promote_or_released_version_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'nothing to promote'):
            self.plan()
        self.git('tag', 'v0.1.2')
        self.git('push', '-q', 'origin', 'v0.1.2')
        with self.assertRaisesRegex(ValueError, 'already released'):
            self.plan()

    def test_repository_comes_from_the_github_remote(self):
        for url in ('https://github.com/patrick-hudson/put.gd-desktop.git', 'git@github.com:patrick-hudson/put.gd-desktop',
                    'ssh://git@github.com/patrick-hudson/put.gd-desktop.git/'):
            self.assertEqual(promote.repository_of(url), 'patrick-hudson/put.gd-desktop')
        with self.assertRaisesRegex(ValueError, '--repository'):
            promote.repository_of('/srv/git/putgd.git')


if __name__ == '__main__':
    unittest.main()
