"""The pre-push gitleaks hook (#1959): same pin as CI, outgoing commits only, fails closed."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HOOKS = ROOT / 'tools/git-hooks'
sys.path.insert(0, str(HOOKS))
spec = importlib.util.spec_from_file_location('hooklib', HOOKS / 'hooklib.py')
hooklib = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hooklib)


class Pin(unittest.TestCase):
    def test_hook_scanner_is_the_contracts_job_scanner(self):
        workflow = (ROOT / '.github/workflows/ci.yml').read_text()
        self.assertIn(hooklib.GITLEAKS_URL, workflow)
        self.assertIn(f'echo "{hooklib.GITLEAKS_SHA256}  $archive"', workflow)
        self.assertIn(f'minVersion = "v{hooklib.GITLEAKS_VERSION}"', (ROOT / '.gitleaks.toml').read_text())


@unittest.skipUnless(os.environ.get('GITLEAKS_BIN'), 'set GITLEAKS_BIN to the pinned 8.30.1 scanner')
class PrePush(unittest.TestCase):
    def setUp(self):
        self.binary = str(Path(os.environ['GITLEAKS_BIN']).resolve())
        self.scratch = tempfile.TemporaryDirectory(prefix='pg-hook-')
        root = Path(self.scratch.name)
        self.work, self.remote, previous = root / 'work', root / 'remote.git', root / 'previous'
        previous.mkdir()
        # The hooks directory being replaced: its hooks must keep running.
        for name in ('pre-push', 'commit-msg', 'post-commit'):
            hook = previous / name
            hook.write_text(f'#!/bin/sh\necho ran >> "{root}/{name}.ran"\n')
            hook.chmod(0o755)
        global_config = root / 'gitconfig'
        # Forced colour must not hide patches from the scanner.
        global_config.write_text(f'[core]\n\thooksPath = {previous}\n[color]\n\tui = always\n')
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL=str(global_config), GIT_CONFIG_NOSYSTEM='1',
                        GITLEAKS_BIN=self.binary, GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
                        GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid')
        self.env.pop(hooklib.SKIP_VARIABLE, None)
        self.root = root
        subprocess.run(['git', 'init', '-q', '--bare', str(self.remote)], check=True, env=self.env)
        self.git('init', '-q', '-b', 'work', str(self.work), cwd=root)
        # As install.py does: a copy outside the tree, here on a branch with no tools/git-hooks.
        copy = subprocess.run([sys.executable, '-c', 'import sys; sys.path.insert(0, sys.argv[1]); import hooklib; '
                               'print(hooklib.enable("--local")[0])', str(HOOKS)], cwd=self.work, env=self.env,
                              capture_output=True, text=True, check=True).stdout.strip()
        self.assertEqual(Path(copy), (self.work / '.git/putgd-hooks').resolve())
        self.git('remote', 'add', 'origin', str(self.remote))
        shutil.copy(ROOT / '.gitleaks.toml', self.work / '.gitleaks.toml')
        self.commit('base.txt', 'scratch fixture')

    def tearDown(self):
        self.scratch.cleanup()

    def git(self, *args, cwd=None, env=None, check=True):
        return subprocess.run(['git', '-c', 'maintenance.auto=false', '-c', 'gc.auto=0', *args],
                              cwd=cwd or self.work, env=env or self.env, capture_output=True, text=True, check=check)

    def commit(self, name, text):
        (self.work / name).write_text(text + '\n')
        self.git('add', '.')
        self.git('commit', '-qm', name)
        return self.git('rev-parse', 'HEAD').stdout.strip()

    def push(self, **extra):
        return self.git('push', '-q', 'origin', 'work', env=dict(self.env, **extra), check=False)

    def on_remote(self, sha):
        return subprocess.run(['git', '-C', str(self.remote), 'cat-file', '-e', sha], env=self.env).returncode == 0

    def test_refuses_flagged_commits_fails_closed_and_scans_only_new_commits(self):
        clean = self.push()
        self.assertEqual(clean.returncode, 0, clean.stderr)
        self.assertIn('over 1 outgoing commit\n', clean.stderr)
        self.assertTrue((self.root / 'pre-push.ran').exists(), 'the replaced pre-push must still run')
        for name in ('commit-msg', 'post-commit'):
            self.assertTrue((self.root / f'{name}.ran').exists(), f'the replaced {name} must still run')

        # Assembled at runtime so this source is not itself a finding.
        planted = self.commit('aws.txt', 'synthetic aws_key = "' + 'AKIA' + 'QWERTYUI' + 'ASDFGHJK' + '"')
        refused = self.push()
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn('found a secret-shaped value', refused.stderr)
        self.assertIn('aws-access-token: aws.txt:1', refused.stderr)
        self.assertNotIn('QWERTYUI', refused.stderr + refused.stdout, 'findings must be redacted')
        self.assertFalse(self.on_remote(planted))

        # Another remote already having the commit does not excuse it for this one.
        other = self.root / 'other.git'
        subprocess.run(['git', 'init', '-q', '--bare', str(other)], check=True, env=self.env)
        self.git('remote', 'add', 'other', str(other))
        self.git('push', '-q', '--no-verify', 'other', 'work')
        self.assertNotEqual(self.push().returncode, 0)
        # A ref name git allows but a whitespace split would break up.
        odd = self.git('push', '-q', 'origin', 'work:refs/heads/odd\u00a0name', check=False)
        self.assertNotEqual(odd.returncode, 0)
        # A fetch remote whose pushes go elsewhere: its tracking refs say nothing about the destination.
        self.git('remote', 'set-url', '--push', 'other', str(self.remote))
        self.assertNotEqual(self.git('push', '-q', 'other', 'work', check=False).returncode, 0)
        self.assertFalse(self.on_remote(planted))

        missing = self.push(GITLEAKS_BIN=str(self.root / 'absent'))
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn('install.py --fetch-only', missing.stderr)
        self.assertFalse(self.on_remote(planted))

        forced = self.push(**{hooklib.SKIP_VARIABLE: '1'})
        self.assertEqual(forced.returncode, 0, forced.stderr)
        self.assertTrue(self.on_remote(planted))

        # The flagged commit is on the remote now; only the new one is scanned.
        later = self.commit('later.txt', 'more scratch')
        follow_up = self.push()
        self.assertEqual(follow_up.returncode, 0, follow_up.stderr)
        self.assertIn('over 1 outgoing commit\n', follow_up.stderr)
        self.assertTrue(self.on_remote(later))


if __name__ == '__main__':
    unittest.main()
