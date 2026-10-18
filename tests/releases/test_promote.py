"""Promotion pull requests name nightly's version and list main's missing changes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
import promote

ENV = dict(os.environ, GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
           GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid')


class Promotion(unittest.TestCase):
    def setUp(self):
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

    def test_title_and_changes_come_from_nightly(self):
        self.git('checkout', '-q', '-b', 'nightly', 'origin/nightly')
        self.commit('project(putgd VERSION 0.1.2 LANGUAGES CXX)\n# a\n', 'feat: first change')
        self.commit('project(putgd VERSION 0.1.2 LANGUAGES CXX)\n# b\n', 'fix: second change')
        self.git('push', '-q', 'origin', 'nightly')
        title, text = promote.plan(self.root)
        self.assertEqual(title, 'release: v0.1.2')
        self.assertIn('- fix: second change (', text)
        self.assertIn('- feat: first change (', text)
        self.assertLess(text.index('second change'), text.index('first change'))
        self.assertIn('v0.1.2-rc.N', text)

    def test_nothing_to_promote_or_released_version_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'nothing to promote'):
            promote.plan(self.root)
        self.git('tag', 'v0.1.2')
        self.git('push', '-q', 'origin', 'v0.1.2')
        with self.assertRaisesRegex(ValueError, 'already released'):
            promote.plan(self.root)


if __name__ == '__main__':
    unittest.main()
