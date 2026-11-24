import base64
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch
import unittest

TOOL = Path(__file__).resolve().parents[2] / 'tools/maintenance/evidence.py'
spec = importlib.util.spec_from_file_location('evidence', TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

REAL_GIT = shutil.which('git')
TOKEN = 'ghp_synthetic0token0for0evidence0tests'
SEEDED = '2026-01-01-seeded'
BIG_BLOB_BYTES = 2 * 1024 * 1024

CHECK_STUB = r'''#!/usr/bin/env python3
"""Stub of the evidence repository's record validator."""
import pathlib
import re
import sys

assert sys.argv[1] == 'record'
directory = pathlib.Path(sys.argv[2])
name = sys.argv[sys.argv.index('--name') + 1] if '--name' in sys.argv else directory.name
problems = []
if not re.match(r'^\d{4}-\d{2}-\d{2}-[a-z0-9][a-z0-9.-]*$', name):
    problems.append('bad name ' + name)
readme = directory / 'README.md'
if not readme.is_file() or not any(line.startswith('# ') for line in readme.read_text().splitlines()):
    problems.append('README.md needs a "# " title')
for path in directory.rglob('*'):
    if path.is_symlink():
        problems.append('symlink ' + path.name)
    elif path.is_file() and path.stat().st_size > 50 * 1024 * 1024:
        problems.append('too big ' + path.name)
    elif 'credential' in path.name:
        problems.append('credential-like name ' + path.name)
for problem in problems:
    print(problem, file=sys.stderr)
sys.exit(1 if problems else 0)
'''

# Logs each git invocation, and on request lets another publisher win the race to main first.
GIT_WRAPPER = '''#!/bin/sh
printf '%s\\n' "$*" >> "$ARGV_LOG"
env | grep -c '^GIT_CONFIG_VALUE_' >> "$ARGV_LOG.env" || true
env | grep '^GIT_CONFIG_' | sed 's/^GIT_CONFIG_VALUE_[0-9]*=.*/VALUE/' >> "$ARGV_LOG.config" || true
if [ "$1" = push ] && [ -s "$RACE_COUNT" ]; then
    left=$(cat "$RACE_COUNT")
    if [ "$left" -gt 0 ]; then
        echo $((left - 1)) > "$RACE_COUNT"
        (
            cd "$RACE_CHECKOUT" || exit 9
            echo "$left" >> records/README.md
            "$REAL_GIT" add records/README.md
            "$REAL_GIT" commit -q -m "index $left"
            "$REAL_GIT" push -q origin main
        ) || exit 9
    fi
fi
exec "$REAL_GIT" "$@"
'''

GITLEAKS_STUB = '''#!/bin/sh
printf '%s\\n' "$*" >> "$GITLEAKS_LOG"
if grep -rq LEAKME "$2"; then
    echo 'Finding: REDACTED'
    exit 1
fi
'''


def real_git(*args, cwd=None, stdin=''):
    result = subprocess.run([REAL_GIT, *args], cwd=cwd, input=stdin, capture_output=True, text=True,
                            env=clean_git_env())
    if result.returncode:
        raise AssertionError(f'git {args}: {result.stderr}')
    return result.stdout.strip()


def clean_git_env():
    env = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
    env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
               GIT_AUTHOR_NAME='Seed', GIT_AUTHOR_EMAIL='seed@example.invalid',
               GIT_COMMITTER_NAME='Seed', GIT_COMMITTER_EMAIL='seed@example.invalid')
    return env


class EvidenceBase(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='putgd-evidence-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.bare = self.root / 'remote.git'
        self.seed_remote()
        self.clone = self.root / 'cache/evidence'
        self.outside = self.root / 'outside'
        self.outside.mkdir()
        self.home = self.root / 'home'
        self.home.mkdir()
        (self.home / '.gitconfig').write_text('[user]\n\tname = Evidence Test\n\temail = evidence@example.invalid\n')
        self.wrapper_dir = self.root / 'wrapper'
        self.wrapper_dir.mkdir()
        wrapper = self.wrapper_dir / 'git'
        wrapper.write_text(GIT_WRAPPER)
        wrapper.chmod(0o755)
        gitleaks = self.root / 'gitleaks-stub'
        gitleaks.write_text(GITLEAKS_STUB)
        gitleaks.chmod(0o755)
        self.argv_log = self.root / 'argv.log'
        self.gitleaks_log = self.root / 'gitleaks.log'
        self.race_count = self.root / 'race-count'
        self.race_checkout = self.root / 'race-checkout'
        real_git('clone', '-q', f'file://{self.bare}', str(self.race_checkout))
        # A copy of the tool outside any checkout, so runs carry no Source trailer unless a test asks.
        self.script = self.root / 'installed/evidence.py'
        self.script.parent.mkdir()
        shutil.copy(TOOL, self.script)

    def seed_remote(self):
        real_git('init', '-q', '--bare', '-b', 'main', str(self.bare))
        real_git('config', 'uploadpack.allowFilter', 'true', cwd=self.bare)
        work = self.root / 'seed'
        (work / 'tools').mkdir(parents=True)
        (work / 'tools/check.py').write_text(CHECK_STUB)
        (work / '.gitleaks.toml').write_text('title = "stub"\n')
        (work / 'README.md').write_text('# put.gd evidence\n')
        record = work / 'records' / SEEDED
        (record / 'shots').mkdir(parents=True)
        (record / 'README.md').write_text('# Seeded record\n')
        (record / 'shots/first.png').write_bytes(b'\x89PNG seeded')
        (record / 'run.sh').write_text('#!/bin/sh\n')
        (record / 'run.sh').chmod(0o755)
        (work / 'records/README.md').write_text('# Index\n')
        (work / 'records/2026-01-02-single.png').write_bytes(b'\x89PNG single')
        (work / 'records/old-unprefixed').mkdir()
        (work / 'records/old-unprefixed/notes.txt').write_text('no readme here\n')
        other = work / 'records/2026-01-03-other'
        other.mkdir()
        (other / 'README.md').write_text('# Other record\n')
        (other / 'big.bin').write_bytes(os.urandom(BIG_BLOB_BYTES))
        real_git('init', '-q', '-b', 'main', cwd=work)
        real_git('add', '-A', cwd=work)
        real_git('commit', '-q', '-m', 'Seed', cwd=work)
        real_git('remote', 'add', 'origin', str(self.bare), cwd=work)
        real_git('push', '-q', 'origin', 'main', cwd=work)

    def env(self, **extra):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith('GIT_')
               and key not in {'GH_TOKEN', 'GITHUB_TOKEN', 'XDG_CACHE_HOME', 'XDG_CONFIG_HOME'}}
        env.update(HOME=str(self.home), GIT_CONFIG_NOSYSTEM='1',
                   PATH=f'{self.wrapper_dir}{os.pathsep}{os.environ["PATH"]}',
                   ARGV_LOG=str(self.argv_log), REAL_GIT=REAL_GIT, RACE_COUNT=str(self.race_count),
                   RACE_CHECKOUT=str(self.race_checkout), GITLEAKS_LOG=str(self.gitleaks_log),
                   GITLEAKS_BIN=str(self.root / 'gitleaks-stub'), PUTGD_EVIDENCE_DIR=str(self.clone),
                   PUTGD_EVIDENCE_REMOTE=f'file://{self.bare}')
        env.update(extra)
        return env

    def tool(self, *args, expect=0, cwd=None, env=None, script=None):
        result = subprocess.run([sys.executable, str(script or self.script), *args], cwd=cwd or self.outside,
                                env=env or self.env(), input='', capture_output=True, text=True)
        if expect is not None:
            self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return result

    def source(self, name='2026-11-05-sample', files=None):
        path = self.root / 'src' / name
        path.mkdir(parents=True)
        for relative, content in (files or {'README.md': '# Sample record\n', 'log.txt': 'evidence\n'}).items():
            (path / relative).parent.mkdir(parents=True, exist_ok=True)
            data = content if isinstance(content, bytes) else content.encode()
            (path / relative).write_bytes(data)
        return path

    def main_head(self):
        return real_git('rev-parse', 'refs/heads/main', cwd=self.bare)

    def remote_file(self, path, ref='refs/heads/main'):
        return real_git('show', f'{ref}:{path}', cwd=self.bare)

    def remote_paths(self, record):
        listing = real_git('ls-tree', '-r', '--name-only', 'refs/heads/main', '--', f'records/{record}', cwd=self.bare)
        return set(listing.splitlines())

    def message(self, ref='refs/heads/main'):
        return real_git('log', '-1', '--format=%B', ref, cwd=self.bare)

    def publish_json(self, source, *args, **kwargs):
        result = self.tool('publish', str(source), '--json', *args, **kwargs)
        return json.loads(result.stdout)


class PublishTests(EvidenceBase):
    def test_check_failure_blocks_the_commit_and_relays_messages(self):
        before = self.main_head()
        result = self.tool('publish', str(self.source(files={'README.md': 'no title\n'})), expect=1)
        self.assertIn('README.md needs a "# " title', result.stderr)
        self.assertIn('Fix the source directory', result.stderr)
        self.assertEqual(self.main_head(), before)
        self.assertEqual(real_git('status', '--porcelain', cwd=self.clone), '')
        self.assertEqual(real_git('rev-parse', 'HEAD', cwd=self.clone), before)

    def test_changed_file_needs_amend_and_gets_the_trailer(self):
        source = self.source()
        self.publish_json(source)
        head = self.main_head()
        (source / 'log.txt').write_text('different\n')
        refused = self.tool('publish', str(source), expect=1)
        self.assertIn('--amend REASON', refused.stderr)
        self.assertIn('records/2026-11-05-sample/log.txt', refused.stderr)
        self.assertEqual(self.main_head(), head)
        self.publish_json(source, '--amend', 'wrong log attached')
        self.assertEqual(self.remote_file('records/2026-11-05-sample/log.txt'), 'different')
        trailers = real_git('interpret-trailers', '--parse', stdin=self.message())
        self.assertIn('Evidence-Amend: wrong log attached', trailers)

    def test_files_missing_from_the_source_stay_published(self):
        source = self.source()
        self.publish_json(source)
        published = self.remote_paths('2026-11-05-sample')
        head = self.main_head()
        (source / 'log.txt').unlink()
        unchanged = self.publish_json(source)
        self.assertEqual(unchanged['status'], 'unchanged')
        self.assertEqual(self.main_head(), head)
        self.publish_json(source, '--amend', 'never deletes')
        self.assertEqual(self.remote_paths('2026-11-05-sample'), published)

    def test_file_and_directory_swaps_are_refused(self):
        source = self.source(files={'README.md': '# T\n', 'a': 'file\n', 'b/c.txt': 'nested\n'})
        self.publish_json(source)
        published = self.remote_paths('2026-11-05-sample')
        (source / 'a').unlink()
        (source / 'a').mkdir()
        (source / 'a/inner.txt').write_text('inner\n')
        (source / 'b/c.txt').unlink()
        (source / 'b').rmdir()
        (source / 'b').write_text('now a file\n')
        for extra in ((), ('--amend', 'layout')):
            refused = self.tool('publish', str(source), *extra, expect=1)
            self.assertIn('would share a name', refused.stderr)
            self.assertIn('records/2026-11-05-sample/a/inner.txt', refused.stderr)
            self.assertIn('records/2026-11-05-sample/b', refused.stderr)
        self.assertEqual(self.remote_paths('2026-11-05-sample'), published)

    def test_symlinks_and_git_metadata_refused(self):
        before = self.main_head()
        secret = self.root / 'secret.txt'
        secret.write_text('outside\n')
        for name, make in (('2026-11-05-filelink', lambda path: (path / 'link').symlink_to(secret)),
                           ('2026-11-05-dirlink',
                            lambda path: (path / 'link').symlink_to(self.root, target_is_directory=True)),
                           ('2026-11-05-inner', lambda path: (path / 'link').symlink_to('README.md')),
                           ('2026-11-05-dotgit', lambda path: (path / 'sub/.git').mkdir(parents=True))):
            with self.subTest(name=name):
                source = self.source(name)
                make(source)
                result = self.tool('publish', str(source), expect=1)
                self.assertIn('Cannot publish', result.stderr)
                self.assertRegex(result.stderr, 'link|git metadata')
        self.assertEqual(self.main_head(), before)

    def test_bytes_survive_attributes_and_ignore_rules(self):
        (self.home / '.gitconfig').write_text('[user]\n\tname = E\n\temail = e@example.invalid\n'
                                              f'[core]\n\tautocrlf = true\n\texcludesFile = {self.home}/ignored\n')
        (self.home / 'ignored').write_text('*.log\n')
        data = b'line one\nline two\r\nno newline'
        source = self.source(files={'README.md': '# T\n', '.gitattributes': '* text eol=crlf\n',
                                    '.gitignore': '*.txt\n', 'kept.log': data, 'kept.txt': data})
        self.publish_json(source)
        for name in ('kept.log', 'kept.txt'):
            blob = subprocess.run([REAL_GIT, 'cat-file', 'blob', f'refs/heads/main:records/2026-11-05-sample/{name}'],
                                  cwd=self.bare, capture_output=True, check=True).stdout
            self.assertEqual(blob, data, name)

    def test_records_index_is_never_touched(self):
        source = self.source(files={'README.md': '# T\n'})
        self.publish_json(source)
        changed = real_git('diff', '--name-only', 'refs/heads/main~1', 'refs/heads/main', cwd=self.bare)
        self.assertEqual(changed, 'records/2026-11-05-sample/README.md')

    def test_hooks_from_global_config_do_not_run(self):
        hooks = self.home / 'hooks'
        hooks.mkdir()
        for name in ('pre-commit', 'pre-push', 'commit-msg'):
            (hooks / name).write_text('#!/bin/sh\nexit 1\n')
            (hooks / name).chmod(0o755)
        with (self.home / '.gitconfig').open('a') as stream:
            stream.write(f'[core]\n\thooksPath = {hooks}\n[commit]\n\tgpgSign = true\n')
        self.assertEqual(self.publish_json(self.source())['status'], 'published')


class SecretScanTests(EvidenceBase):
    def test_mistyped_gitleaks_bin_stops_the_publish(self):
        before = self.main_head()
        result = self.tool('publish', str(self.source()), expect=1,
                           env=self.env(GITLEAKS_BIN=str(self.root / 'no-such-gitleaks')))
        self.assertIn('GITLEAKS_BIN is set to', result.stderr)
        self.assertEqual(self.main_head(), before)

    def test_findings_stop_the_commit_with_redacted_scan(self):
        before = self.main_head()
        result = self.tool('publish', str(self.source(files={'README.md': '# T\n', 'leak.txt': 'LEAKME\n'})), expect=1)
        self.assertIn('gitleaks found', result.stderr)
        self.assertIn('Finding: REDACTED', result.stderr)
        self.assertEqual(self.main_head(), before)
        arguments = self.gitleaks_log.read_text()
        self.assertIn('--redact', arguments)
        self.assertIn(f'--config {self.clone}/.gitleaks.toml', arguments)
        self.assertIn(f'dir {self.clone}/records/2026-11-05-sample', arguments)

    def test_scanner_failure_stops_too(self):
        broken = self.root / 'broken-gitleaks'
        broken.write_text('#!/bin/sh\necho unknown flag >&2\nexit 126\n')
        broken.chmod(0o755)
        before = self.main_head()
        result = self.tool('publish', str(self.source()), expect=1, env=self.env(GITLEAKS_BIN=str(broken)))
        self.assertIn('could not scan', result.stderr)
        self.assertEqual(self.main_head(), before)


class RaceTests(EvidenceBase):
    def test_conflicting_concurrent_publish_reports_and_keeps_the_commit(self):
        checkout = self.race_checkout
        real_git('config', 'user.name', 'Other', cwd=checkout)
        real_git('config', 'user.email', 'other@example.invalid', cwd=checkout)
        record = checkout / 'records/2026-11-05-sample'
        record.mkdir()
        (record / 'log.txt').write_text('the other publisher\n')
        wrapper = self.wrapper_dir / 'git'
        wrapper.write_text(GIT_WRAPPER.replace('echo "$left" >> records/README.md', ':').replace(
            '"$REAL_GIT" add records/README.md', '"$REAL_GIT" add records/2026-11-05-sample'))
        self.race_count.write_text('1\n')
        result = self.tool('publish', str(self.source()), expect=1)
        self.assertIn('conflicting files', result.stderr)
        self.assertEqual(self.remote_file('records/2026-11-05-sample/log.txt'), 'the other publisher')
        self.assertEqual(real_git('log', '-1', '--format=%s', 'main', cwd=self.clone),
                         '2026-11-05-sample: Sample record')
        self.assertFalse((self.clone / '.git/rebase-merge').exists())

    def test_concurrent_publishers_on_one_clone_both_land(self):
        sources = [self.source(f'2026-11-05-concurrent-{index}') for index in range(4)]
        env = self.env()
        running = [subprocess.Popen([sys.executable, str(self.script), 'publish', str(source)],
                                    cwd=self.outside, env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                   for source in sources]
        for process in running:
            stdout, stderr = process.communicate(timeout=120)
            self.assertEqual(process.returncode, 0, stderr)
            self.assertIn('Published', stdout)
        for source in sources:
            self.assertTrue(self.remote_paths(source.name))


class RecoveryTests(EvidenceBase):
    def reject_pushes(self):
        hook = self.bare / 'hooks/pre-receive'
        hook.write_text('#!/bin/sh\necho "denied by policy" >&2\nexit 1\n')
        hook.chmod(0o755)
        return hook

    def test_failed_push_keeps_the_commit_and_the_retry_says_so(self):
        before = self.main_head()
        hook = self.reject_pushes()
        source = self.source()
        failed = self.tool('publish', str(source), expect=1)
        self.assertIn('denied by policy', failed.stderr)
        self.assertIn('run the same publish command again', failed.stderr)
        self.assertNotIn('Tree:', failed.stdout)
        self.assertEqual(self.main_head(), before)
        local = real_git('rev-parse', 'main', cwd=self.clone)
        self.assertNotEqual(local, before)
        hook.unlink()
        retried = self.tool('publish', str(source))
        self.assertIn(f'kept as branch unpushed/{local[:12]}', retried.stderr)
        self.assertIn('2026-11-05-sample: Sample record', retried.stderr)
        self.assertIn('run publish again', retried.stderr)
        self.assertIn('Published', retried.stdout)
        self.assertEqual(real_git('rev-parse', f'unpushed/{local[:12]}', cwd=self.clone), local)
        self.assertEqual(self.remote_file('records/2026-11-05-sample/log.txt'), 'evidence')

    def test_foreign_directory_and_foreign_remote_are_refused(self):
        self.clone.mkdir(parents=True)
        (self.clone / 'precious.txt').write_text('mine\n')
        result = self.tool('publish', str(self.source()), expect=1)
        self.assertIn('not an evidence clone', result.stderr)
        self.assertTrue((self.clone / 'precious.txt').is_file())
        (self.clone / 'precious.txt').unlink()
        self.publish_json(self.source('2026-11-05-b'))
        other = self.env(PUTGD_EVIDENCE_REMOTE='file:///elsewhere/remote.git')
        self.assertIn('remote set-url', self.tool('link', SEEDED, env=other, expect=1).stderr)


class LinkAndFetchTests(EvidenceBase):
    def test_fetch_never_overwrites_a_non_empty_destination(self):
        destination = self.root / 'occupied'
        destination.mkdir()
        (destination / 'mine.txt').write_text('keep\n')
        result = self.tool('fetch', SEEDED, '--dest', str(destination), expect=1)
        self.assertIn('not empty', result.stderr)
        self.assertEqual([path.name for path in destination.iterdir()], ['mine.txt'])
        target = self.root / 'occupied.txt'
        target.write_text('a file\n')
        self.assertIn('not empty', self.tool('fetch', SEEDED, '--dest', str(target), expect=1).stderr)
        empty = self.root / 'empty'
        empty.mkdir()
        self.tool('fetch', SEEDED, '--dest', str(empty))
        self.assertTrue((empty / 'README.md').is_file())


class PartialCloneTests(EvidenceBase):
    def missing_objects(self):
        listing = real_git('rev-list', '--objects', '--missing=print', 'origin/main', cwd=self.clone)
        return {line[1:] for line in listing.splitlines() if line.startswith('?')}

    def git_dir_bytes(self):
        return sum(path.stat().st_size for path in (self.clone / '.git').rglob('*') if path.is_file())

    def test_race_rebase_keeps_other_blobs_out(self):
        big = real_git('rev-parse', 'refs/heads/main:records/2026-01-03-other/big.bin', cwd=self.bare)
        self.race_count.write_text('1\n')
        self.publish_json(self.source())
        self.assertIn(big, self.missing_objects())
        self.assertLess(self.git_dir_bytes(), BIG_BLOB_BYTES // 5)


class TokenTests(EvidenceBase):
    REMOTE = 'https://evidence.invalid/owner/evidence.git'

    def setUp(self):
        super().setUp()
        with (self.home / '.gitconfig').open('a') as stream:
            stream.write(f'[url "file://{self.bare}"]\n\tinsteadOf = {self.REMOTE}\n')
        self.credential = base64.b64encode(f'x-access-token:{TOKEN}'.encode()).decode()

    def token_env(self, **extra):
        return self.env(PUTGD_EVIDENCE_REMOTE=self.REMOTE, GH_TOKEN=TOKEN, **extra)

    def assert_clean(self, *texts):
        for text in texts:
            self.assertNotIn(TOKEN, text)
            self.assertNotIn(self.credential, text)

    def test_token_reaches_git_only_through_the_environment(self):
        env = self.token_env()
        source = self.source()
        published = self.tool('publish', str(source), env=env)
        linked = self.tool('link', SEEDED, env=env)
        fetched = self.tool('fetch', SEEDED, '--dest', str(self.root / 'copy'), env=env)
        (source / 'log.txt').write_text('changed\n')
        refused = self.tool('publish', str(source), env=env, expect=1)
        self.assert_clean(*[part for result in (published, linked, fetched, refused)
                            for part in (result.stdout, result.stderr)])
        arguments = self.argv_log.read_text()
        self.assertIn('push', arguments)
        self.assert_clean(arguments)
        config = (self.root / 'argv.log.config').read_text()
        self.assert_clean(config)
        self.assertIn('GIT_CONFIG_KEY_0=http.https://evidence.invalid/.extraHeader', config)
        self.assertIn('VALUE', config)

    def test_token_is_in_no_file_of_the_clone(self):
        self.tool('publish', str(self.source()), env=self.token_env())
        for path in list(self.clone.parent.rglob('*')):
            if path.is_file():
                data = path.read_bytes()
                self.assertNotIn(TOKEN.encode(), data, path)
                self.assertNotIn(self.credential.encode(), data, path)

    def test_error_output_cannot_carry_the_token(self):
        hook = self.bare / 'hooks/pre-receive'
        leak = f'remote: fatal https://x-access-token:{TOKEN}@evidence.invalid {self.credential}'
        hook.write_text(f"#!/bin/sh\necho '{leak}' >&2\nexit 1\n")
        hook.chmod(0o755)
        result = self.tool('publish', str(self.source()), env=self.token_env(), expect=1)
        self.assert_clean(result.stdout, result.stderr)
        self.assertIn('Push to', result.stderr)

    def test_scrub_masks_values_and_url_userinfo(self):
        with patch.object(module, 'secrets', [TOKEN]):
            self.assertEqual(module.scrub(f'token {TOKEN} here'), 'token *** here')
        self.assertEqual(module.scrub('see https://user:pass@example.invalid/x'), 'see https://***@example.invalid/x')

    def test_children_outside_git_never_receive_the_token(self):
        dumper = self.root / 'dump-gitleaks'
        dumper.write_text(f'#!/bin/sh\nenv > {self.root}/scanner.env\n')
        dumper.chmod(0o755)
        self.tool('publish', str(self.source()), env=self.token_env(GITLEAKS_BIN=str(dumper), GITHUB_TOKEN=TOKEN))
        self.assert_clean((self.root / 'scanner.env').read_text())

    def test_header_is_scoped_and_only_for_https(self):
        inherited = {'GH_TOKEN': TOKEN, 'GIT_CONFIG_COUNT': '2', 'GIT_CONFIG_GLOBAL': os.devnull,
                     'GIT_CONFIG_KEY_0': 'url.https://elsewhere.invalid/.insteadOf',
                     'GIT_CONFIG_VALUE_0': 'https://github.com/',
                     'GIT_CONFIG_KEY_1': 'http.proxy', 'GIT_CONFIG_VALUE_1': 'http://elsewhere.invalid'}
        with patch.dict(os.environ, inherited), patch.object(module, 'secrets', []):
            env = module.git_env('https://github.com/owner/repo.git')
            self.assertEqual(env['GIT_CONFIG_COUNT'], '1')
            self.assertEqual(env['GIT_CONFIG_KEY_0'], 'http.https://github.com/.extraHeader')
            self.assertEqual(env['GIT_CONFIG_GLOBAL'], os.devnull)
            self.assertEqual(env['GIT_CONFIG_VALUE_0'], 'Authorization: Basic ' + self.credential)
            self.assertNotIn('GIT_CONFIG_KEY_1', env)
            self.assertNotIn('GIT_CONFIG_VALUE_1', env)
            self.assertNotIn('GH_TOKEN', env)
            for remote in ('git@github.com:owner/repo.git', 'ssh://git@github.com/owner/repo.git',
                           'http://example.invalid/x.git'):
                self.assertNotIn('GIT_CONFIG_COUNT', module.git_env(remote))

    def test_remote_with_embedded_credentials_refused(self):
        env = self.env(PUTGD_EVIDENCE_REMOTE=f'https://x-access-token:{TOKEN}@github.com/o/r.git')
        result = self.tool('link', SEEDED, env=env, expect=1)
        self.assertIn('must not embed credentials', result.stderr)
        self.assert_clean(result.stdout, result.stderr)
        self.assertFalse(self.clone.exists())


if __name__ == '__main__':
    unittest.main()
