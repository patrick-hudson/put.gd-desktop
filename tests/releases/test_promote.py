"""Promotion pull requests carry nightly's CHANGELOG.md section as [Unreleased] (#217, #373).

The stable version, the merge commit's UTC date and count, exists only after the
merge; --finalize names the section once that release is published.
"""
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

# Every fixture commit is made 2026-10-20 12:00 UTC, so versions are 26.1020.N.
ENV = dict(os.environ, GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
           GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid',
           GIT_AUTHOR_DATE='2026-10-20T12:00:00Z', GIT_COMMITTER_DATE='2026-10-20T12:00:00Z')
REPO = 'fixture/putgd'
URL = 'https://github.com/' + REPO
TODAY = datetime.date(2026, 10, 21)
METADATA = {3: {'labels': [], 'closes': [11]}, 4: {'labels': ['bug'], 'closes': [12]},
            5: {'labels': [], 'closes': []}, 6: {'labels': [], 'closes': []}}

CHANGES = f"""### Added

- Reorder destinations in Settings ([#3]({URL}/pull/3)), closes [#11]({URL}/issues/11)

### Fixed

- History: keep the selection ([#4]({URL}/pull/4)), closes [#12]({URL}/issues/12)

### Changed

- Keep the bar on its screen ([#5]({URL}/pull/5))

Also 1 documentation changes: [#6]({URL}/pull/6).
"""
SECTION = '## [Unreleased]\n\n' + CHANGES


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
        self.commit('project(putgd LANGUAGES CXX)\n', 'base')
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

    def plan(self, metadata=METADATA, released=None):
        return promote.plan(self.root, repository=REPO, released=released,
                            lookup=lambda repository, numbers: {n: metadata[n] for n in numbers if n in metadata})

    def test_title_section_and_body_come_from_nightly_pull_requests(self):
        self.nightly()
        promotion = self.plan()
        # Nightly HEAD: one base commit and four pull requests of two commits each.
        self.assertEqual(promotion['title'], 'release: promote nightly 26.1020.9')
        self.assertEqual(promotion['section'], SECTION)
        # No stable release yet: the whole history, linked to nightly's commits.
        self.assertEqual(promotion['changelog'], promote.HEADER + '\n' + SECTION + '\n' +
                         f'[Unreleased]: {URL}/commits/nightly\n')
        body = promotion['body']
        self.assertIn('## Changes\n\n### Added\n\n- Reorder destinations', body)
        self.assertNotIn('## [Unreleased]', body)
        self.assertIn('tags the stable release `vYY.MDD.N`', body)
        self.assertIn('promote.py --finalize vYY.MDD.N', body)
        # Labels win over the title prefix; without the lookup the prefix decides.
        offline = promote.plan(self.root, repository=REPO, lookup=None)['section']
        self.assertIn('### Changed\n\n- History: keep the selection ([#4]', offline)

    def test_section_starts_at_the_previous_published_stable_release(self):
        self.nightly()
        self.git('tag', 'v26.1001.1', 'HEAD~2')
        self.git('push', '-q', 'origin', 'v26.1001.1')
        promotion = self.plan(released=lambda repository: {'v26.1001.1'})
        self.assertNotIn('[#3]', promotion['section'])
        self.assertNotIn('[#4]', promotion['section'])
        self.assertIn('[#5]', promotion['section'])
        self.assertTrue(promotion['changelog'].endswith(f'[Unreleased]: {URL}/compare/v26.1001.1...nightly\n'))
        # A stable tag whose acceptance failed was never released: the section reaches past it.
        unpublished = self.plan(released=lambda repository: set())
        self.assertIn('[#3]', unpublished['section'])
        self.assertTrue(unpublished['changelog'].endswith(f'[Unreleased]: {URL}/commits/nightly\n'))

    def test_new_sections_and_links_go_first_below_unreleased(self):
        existing = (promote.HEADER + '\n## [Unreleased]\n\n- Draft\n\n## [26.1001.1] - 2026-10-01\n\n### Fixed\n\n'
                    f'- Old fix\n\n[Unreleased]: {URL}/compare/v26.1001.1...nightly\n[26.1001.1]: {URL}/releases/tag/v26.1001.1\n')
        text = promote.with_section(existing, '26.1101.960', '## [26.1101.960] - 2026-11-02\n\n### Added\n\n- New\n',
                                    f'[26.1101.960]: {URL}/compare/v26.1001.1...v26.1101.960')
        self.assertLess(text.index('## [Unreleased]'), text.index('## [26.1101.960]'))
        self.assertLess(text.index('## [26.1101.960]'), text.index('## [26.1001.1]'))
        self.assertIn('- New\n\n## [26.1001.1]', text)
        self.assertTrue(text.endswith(f'[Unreleased]: {URL}/compare/v26.1001.1...nightly\n'
                                      f'[26.1101.960]: {URL}/compare/v26.1001.1...v26.1101.960\n'
                                      f'[26.1001.1]: {URL}/releases/tag/v26.1001.1\n'))
        self.assertIsNone(promote.with_section(text, '26.1101.960', '## [26.1101.960] - x\n', '[26.1101.960]: y'))
        # An [Unreleased] section being curated is never replaced.
        self.assertIsNone(promote.with_section(text, None, '## [Unreleased]\n\n- Other\n', '[Unreleased]: z'))
        # A new [Unreleased] section goes first, above every released version.
        released = text.replace('## [Unreleased]\n\n- Draft\n\n', '').replace(f'[Unreleased]: {URL}/compare/v26.1001.1...nightly\n', '')
        unreleased = promote.with_section(released, None, '## [Unreleased]\n\n- Next\n', '[Unreleased]: z')
        self.assertLess(unreleased.index('## [Unreleased]'), unreleased.index('## [26.1101.960]'))
        self.assertLess(unreleased.index('[Unreleased]: z'), unreleased.index('[26.1101.960]: '))

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
        branch = 'changelog/promote-26.1020.9'
        self.assertEqual(self.git('--git-dir', str(self.origin), 'show', f'{branch}:CHANGELOG.md') + '\n',
                         promotion['changelog'])
        self.assertEqual(self.git('--git-dir', str(self.origin), 'rev-parse', f'{branch}^'),
                         self.git('rev-parse', 'origin/nightly'))
        self.assertEqual(self.git('--git-dir', str(self.origin), 'diff', '--name-only', 'nightly', branch),
                         'CHANGELOG.md')
        self.assertEqual(self.git('status', '--porcelain'), status)
        self.assertFalse((self.root / 'CHANGELOG.md').exists())
        (changelog_pr, _), (promotion_pr, body) = calls
        self.assertEqual(changelog_pr[:7], ['gh', 'pr', 'create', '--base', 'nightly', '--head', branch])
        self.assertEqual(changelog_pr[8], 'changelog: next stable release')
        self.assertEqual(promotion_pr[:7], ['gh', 'pr', 'create', '--base', 'main', '--head', 'nightly'])
        self.assertEqual(promotion_pr[8], 'release: promote nightly 26.1020.9')
        self.assertIn(f'- [ ] {URL}/pull/99 is merged', body)
        self.assertIn('### Fixed\n\n- History: keep the selection', body)

    def test_open_skips_the_changelog_pull_request_once_nightly_has_the_section(self):
        self.nightly()
        self.merge_changelog(self.plan()['changelog'])
        promotion = self.plan()
        self.assertIsNone(promotion['changelog'])
        calls = []
        with patch.object(promote, 'gh', side_effect=lambda root, *args, input: calls.append(['gh', *args]) or 'url'), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertIsNone(promote.open_pulls(self.root, promotion))
        self.assertEqual([command[4] for command in calls], ['main'])

    def test_nothing_to_promote_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'nothing to promote'):
            self.plan()

    def merge_changelog(self, text):
        (self.root / 'CHANGELOG.md').write_text(text)
        self.git('add', 'CHANGELOG.md')
        self.git('commit', '-qm', 'changelog: next stable release')
        self.git('push', '-q', 'origin', 'nightly')

    def release(self):
        """Promote nightly into main and tag the merge commit's stable version, as release.yml does."""
        self.git('checkout', '-q', 'main')
        self.git('merge', '-q', '--no-ff', 'nightly', '-m', 'Merge pull request #7 from fixture/nightly',
                 '-m', 'release: promote nightly')
        tag = 'v' + promote.nightly_version(self.root, 'HEAD')
        self.git('tag', tag)
        self.git('push', '-q', 'origin', 'main', tag)
        self.git('checkout', '-q', 'nightly')
        return tag

    def finalize(self, tag, released=None):
        return promote.finalize(self.root, tag, repository=REPO, today=TODAY, released=released or (lambda r: {tag}),
                                lookup=lambda repository, numbers: {n: METADATA[n] for n in numbers if n in METADATA})

    def test_finalize_names_the_unreleased_section_after_the_published_release(self):
        self.nightly()
        self.merge_changelog(self.plan()['changelog'])
        tag = self.release()
        # Main's merge commit counts the promoted nightly commits and itself.
        self.assertEqual(tag, 'v26.1020.11')
        change = self.finalize(tag)
        self.assertEqual(change['title'], 'changelog: v26.1020.11')
        self.assertEqual(change['changelog'], promote.HEADER + '\n## [26.1020.11] - 2026-10-21\n\n' + CHANGES + '\n'
                         + f'[26.1020.11]: {URL}/releases/tag/v26.1020.11\n')
        calls = []
        with patch.object(promote, 'gh', side_effect=lambda root, *args, input: calls.append(['gh', *args]) or 'url'), \
                contextlib.redirect_stdout(io.StringIO()):
            promote.open_finalize(self.root, change)
        self.assertEqual(calls[0][:9], ['gh', 'pr', 'create', '--base', 'nightly', '--head', 'changelog/v26.1020.11',
                                        '--title', 'changelog: v26.1020.11'])
        self.assertEqual(self.git('--git-dir', str(self.origin), 'show', 'changelog/v26.1020.11:CHANGELOG.md') + '\n',
                         change['changelog'])
        # Once nightly has the named section, there is nothing left to do.
        self.git('pull', '-q', '--ff-only', 'origin', 'changelog/v26.1020.11')
        self.git('push', '-q', 'origin', 'nightly')
        self.assertIsNone(self.finalize(tag)['changelog'])

    def test_finalize_writes_a_section_when_nightly_has_none(self):
        self.nightly()
        tag = self.release()
        change = self.finalize(tag)
        self.assertEqual(change['changelog'], promote.HEADER + '\n## [26.1020.10] - 2026-10-21\n\n' + CHANGES + '\n'
                         + f'[26.1020.10]: {URL}/releases/tag/v26.1020.10\n')

    def test_finalize_needs_a_published_calendar_stable_release(self):
        self.nightly()
        tag = self.release()
        with self.assertRaisesRegex(ValueError, 'is not published'):
            self.finalize(tag, released=lambda repository: set())
        for other in ('v26.1020.10-nightly', 'v0.1.1'):
            with self.subTest(tag=other), self.assertRaisesRegex(ValueError, 'not a calendar stable release'):
                self.finalize(other)

    def test_repository_comes_from_the_github_remote(self):
        for url in ('https://github.com/patrick-hudson/put.gd-desktop.git', 'git@github.com:patrick-hudson/put.gd-desktop',
                    'ssh://git@github.com/patrick-hudson/put.gd-desktop.git/'):
            self.assertEqual(promote.repository_of(url), 'patrick-hudson/put.gd-desktop')
        with self.assertRaisesRegex(ValueError, '--repository'):
            promote.repository_of('/srv/git/putgd.git')


if __name__ == '__main__':
    unittest.main()
