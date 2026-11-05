import base64
import contextlib
import datetime
import fcntl
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from unittest.mock import Mock, patch
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
    def test_first_publish_creates_clone_and_pins_links(self):
        before = self.main_head()
        source = self.source(files={'README.md': '# Sample [record]\n', 'shots/a b.PNG': b'png', 'log.txt': 'x\n'})
        result = self.publish_json(source)
        head = self.main_head()
        self.assertNotEqual(head, before)
        self.assertEqual(real_git('rev-parse', 'refs/heads/main~1', cwd=self.bare), before)
        self.assertTrue((self.clone / '.git').is_dir())
        self.assertEqual(self.remote_file('records/2026-11-05-sample/log.txt'), 'x')
        self.assertEqual(result['status'], 'published')
        self.assertEqual(result['commit'], head)
        slug = 'https://github.com/patrick-hudson/put.gd-evidence'
        self.assertEqual(result['tree_url'], f'{slug}/tree/{head}/records/2026-11-05-sample')
        self.assertEqual(result['readme_url'], f'{slug}/blob/{head}/records/2026-11-05-sample/README.md')
        self.assertEqual(result['markdown'], f'[Sample \\[record\\]]({slug}/tree/{head}/records/2026-11-05-sample)')
        self.assertEqual(result['images'], [{
            'path': 'shots/a b.PNG',
            'raw_url': f'https://raw.githubusercontent.com/patrick-hudson/put.gd-evidence/{head}'
                       '/records/2026-11-05-sample/shots/a%20b.PNG'}])
        self.assertEqual(self.message(), '2026-11-05-sample: Sample [record]')

    def test_human_output_lists_every_link(self):
        output = self.tool('publish', str(self.source(files={'README.md': '# T\n', 'a.png': b'x'}))).stdout
        head = self.main_head()
        self.assertIn(f'Published records/2026-11-05-sample at {head}', output)
        for label in ('Tree:', 'README:', 'Markdown:', 'Image:'):
            self.assertIn(label, output)
        self.assertIn(f'raw.githubusercontent.com/patrick-hudson/put.gd-evidence/{head}/', output)

    def test_date_prefix_uses_utc_today(self):
        before = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%d')
        result = self.publish_json(self.source('recording-kde'))
        after = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%d')
        self.assertIn(result['name'], {f'{before}-recording-kde', f'{after}-recording-kde'})
        self.assertTrue(self.remote_paths(result['name']))

    def test_name_option_overrides_the_basename(self):
        result = self.publish_json(self.source('whatever'), '--name', '2026-11-05-chosen')
        self.assertEqual(result['name'], '2026-11-05-chosen')

    def test_invalid_name_spells_out_the_rule(self):
        before = self.main_head()
        for name in ('Bad_Name', '2026-11-05-UPPER', '2026-11-05--leading'):
            with self.subTest(name=name):
                result = self.tool('publish', str(self.source(name)), expect=1)
                self.assertIn('YYYY-MM-DD-', result.stderr)
                self.assertIn('lowercase letters', result.stderr)
        self.assertEqual(self.main_head(), before)
        self.assertFalse(self.clone.exists())

    def test_check_failure_blocks_the_commit_and_relays_messages(self):
        before = self.main_head()
        result = self.tool('publish', str(self.source(files={'README.md': 'no title\n'})), expect=1)
        self.assertIn('README.md needs a "# " title', result.stderr)
        self.assertIn('Fix the source directory', result.stderr)
        self.assertEqual(self.main_head(), before)
        self.assertEqual(real_git('status', '--porcelain', cwd=self.clone), '')
        self.assertEqual(real_git('rev-parse', 'HEAD', cwd=self.clone), before)

    def test_add_to_existing_record(self):
        source = self.source()
        self.publish_json(source)
        (source / 'more.txt').write_text('more\n')
        result = self.publish_json(source)
        self.assertEqual(result['added'], ['records/2026-11-05-sample/more.txt'])
        self.assertEqual(result['unchanged'], 2)
        self.assertNotIn('Evidence-Amend', self.message())
        self.assertEqual(self.remote_file('records/2026-11-05-sample/more.txt'), 'more')

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

    def test_follow_up_directory_extends_a_record(self):
        self.publish_json(self.source())
        published = self.remote_paths('2026-11-05-sample')
        follow_up = self.root / 'follow-up' / '2026-11-05-sample'
        follow_up.mkdir(parents=True)
        (follow_up / 'rerun/result.json').parent.mkdir()
        (follow_up / 'rerun/result.json').write_text('{}\n')
        result = self.publish_json(follow_up)
        self.assertEqual(result['added'], ['records/2026-11-05-sample/rerun/result.json'])
        self.assertEqual(self.remote_paths('2026-11-05-sample'),
                         published | {'records/2026-11-05-sample/rerun/result.json'})
        self.assertNotIn('Evidence-Amend', self.message())

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

    def test_multiline_amend_reason_refused(self):
        result = self.tool('publish', str(self.source()), '--amend', 'one\nForged: trailer', expect=1)
        self.assertIn('one-line reason', result.stderr)

    def test_executable_bit_is_preserved_and_a_flip_needs_amend(self):
        source = self.source(files={'README.md': '# T\n', 'run.sh': '#!/bin/sh\n', 'plain.sh': '#!/bin/sh\n'})
        (source / 'run.sh').chmod(0o755)
        self.publish_json(source)
        listing = real_git('ls-tree', '-r', 'refs/heads/main', '--', 'records/2026-11-05-sample', cwd=self.bare)
        modes = {line.split('\t')[1].rsplit('/', 1)[1]: line.split()[0] for line in listing.splitlines()}
        self.assertEqual(modes['run.sh'], '100755')
        self.assertEqual(modes['plain.sh'], '100644')
        (source / 'plain.sh').chmod(0o755)
        self.assertIn('executable bit', self.tool('publish', str(source), expect=1).stderr)

    def test_identical_republish_is_success_with_existing_links(self):
        source = self.source()
        first = self.publish_json(source)
        head = self.main_head()
        again = self.publish_json(source)
        self.assertEqual(self.main_head(), head)
        self.assertEqual(again['status'], 'unchanged')
        self.assertEqual(again['commit'], head)
        self.assertEqual(again['tree_url'], first['tree_url'])
        self.assertIn('Nothing to publish', self.tool('publish', str(source)).stdout)

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

    def test_empty_source_and_missing_source_refused(self):
        empty = self.root / 'src/2026-11-05-empty'
        empty.mkdir(parents=True)
        self.assertIn('no files', self.tool('publish', str(empty), expect=1).stderr)
        self.assertIn('not a directory', self.tool('publish', str(self.root / 'absent'), expect=1).stderr)

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

    def test_source_inside_the_clone_refused(self):
        self.publish_json(self.source())
        inside = self.clone / 'records/2026-11-05-sample'
        self.assertIn('inside the cache clone', self.tool('publish', str(inside), expect=1).stderr)
        self.assertTrue((inside / 'README.md').is_file())

    def test_existing_single_file_record_name_refused(self):
        result = self.tool('publish', str(self.source('2026-01-02-single.png')), expect=1)
        self.assertIn('single-file record', result.stderr)

    def test_records_index_is_never_touched(self):
        source = self.source(files={'README.md': '# T\n'})
        self.publish_json(source)
        changed = real_git('diff', '--name-only', 'refs/heads/main~1', 'refs/heads/main', cwd=self.bare)
        self.assertEqual(changed, 'records/2026-11-05-sample/README.md')

    def test_dry_run_pushes_nothing_and_leaves_the_clone_clean(self):
        before = self.main_head()
        source = self.source()
        output = self.tool('publish', str(source), '--dry-run').stdout
        self.assertIn('Dry run', output)
        self.assertIn('nothing was committed or pushed', output)
        self.assertEqual(self.main_head(), before)
        self.assertEqual(real_git('rev-parse', 'HEAD', cwd=self.clone), before)
        self.assertEqual(real_git('status', '--porcelain', cwd=self.clone), '')
        self.assertFalse((self.clone / 'records/2026-11-05-sample').exists())
        self.assertNotIn('commit', self._git_verbs())
        self.assertNotIn('push', self._git_verbs())
        result = self.publish_json(source, '--dry-run')
        self.assertEqual(result['status'], 'dry-run')
        self.assertIsNone(result['commit'])
        self.assertEqual(len(result['added']), 2)
        self.assertNotIn('tree_url', result)

    def test_dry_run_still_runs_the_checks(self):
        result = self.tool('publish', str(self.source(files={'README.md': 'none\n'})), '--dry-run', expect=1)
        self.assertIn('README.md needs', result.stderr)

    def _git_verbs(self):
        return {line.split()[0] for line in self.argv_log.read_text().splitlines() if line.split()}

    def test_source_trailer_names_the_invoking_checkout(self):
        checkout = self.root / 'desktop'
        (checkout / 'tools/maintenance').mkdir(parents=True)
        shutil.copy(TOOL, checkout / 'tools/maintenance/evidence.py')
        real_git('init', '-q', '-b', 'main', cwd=checkout)
        real_git('config', 'user.name', 'Checkout Identity', cwd=checkout)
        real_git('config', 'user.email', 'checkout@example.invalid', cwd=checkout)
        real_git('add', '-A', cwd=checkout)
        real_git('commit', '-q', '-m', 'Desktop', cwd=checkout)
        sha = real_git('rev-parse', 'HEAD', cwd=checkout)
        self.tool('publish', str(self.source()), '--amend', 'first', cwd=checkout,
                  script=checkout / 'tools/maintenance/evidence.py')
        message = self.message()
        self.assertEqual(message.splitlines()[0], '2026-11-05-sample: Sample record')
        self.assertIn(f'Source: put.gd-desktop@{sha}', message)
        self.assertIn('Evidence-Amend: first', message)
        trailers = real_git('interpret-trailers', '--parse', stdin=message)
        self.assertIn('Source:', trailers)
        self.assertEqual(real_git('log', '-1', '--format=%an <%ae>', 'refs/heads/main', cwd=self.bare),
                         'Checkout Identity <checkout@example.invalid>')

    def test_no_source_trailer_outside_a_checkout(self):
        self.tool('publish', str(self.source()))
        self.assertNotIn('Source:', self.message())
        self.assertEqual(real_git('log', '-1', '--format=%an', 'refs/heads/main', cwd=self.bare), 'Evidence Test')

    def test_missing_identity_fails_with_the_fix(self):
        (self.home / '.gitconfig').write_text('')
        before = self.main_head()
        result = self.tool('publish', str(self.source()), expect=1)
        self.assertIn('git config --global user.name', result.stderr)
        self.assertEqual(self.main_head(), before)

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

    def test_without_gitleaks_one_line_says_ci_will_scan(self):
        clone = Mock(path=self.root, remote='r')
        stderr = io.StringIO()
        with patch.object(module.shutil, 'which', return_value=None), contextlib.redirect_stderr(stderr):
            module.scan_secrets(clone, self.root)
        self.assertEqual(len(stderr.getvalue().splitlines()), 1)
        self.assertIn("CI will scan the push", stderr.getvalue())


class RaceTests(EvidenceBase):
    def test_remote_advance_before_push_rebases_and_retries(self):
        self.race_count.write_text('1\n')
        result = self.publish_json(self.source())
        head = self.main_head()
        self.assertEqual(result['commit'], head)
        self.assertEqual(self.race_count.read_text().strip(), '0')
        parent = real_git('log', '-1', '--format=%s', 'refs/heads/main~1', cwd=self.bare)
        self.assertEqual(parent, 'index 1')
        self.assertEqual(self.remote_file('records/2026-11-05-sample/log.txt'), 'evidence')
        self.assertEqual(self.message(), '2026-11-05-sample: Sample record')

    def test_retries_keep_the_amend_trailer(self):
        source = self.source()
        self.publish_json(source)
        (source / 'log.txt').write_text('changed\n')
        self.race_count.write_text('2\n')
        self.publish_json(source, '--amend', 'fix')
        self.assertIn('Evidence-Amend: fix', self.message())

    def test_gives_up_after_five_retries_keeping_the_commit(self):
        self.race_count.write_text('6\n')
        result = self.tool('publish', str(self.source()), expect=1)
        self.assertIn('5 retries', result.stderr)
        self.assertIn('run the same publish command again', result.stderr)
        self.assertEqual(sum(1 for line in self.argv_log.read_text().splitlines() if line.startswith('push ')), 6)
        self.assertNotIn('Tree:', result.stdout)
        self.assertEqual(self.tool('publish', str(self.source('2026-11-05-next'))).returncode, 0)

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

    def test_lock_timeout_explains_what_to_do(self):
        settings = {'PUTGD_EVIDENCE_DIR': str(self.clone), 'PUTGD_EVIDENCE_REMOTE': f'file://{self.bare}'}
        with patch.dict(os.environ, settings), \
                patch.object(module, 'LOCK_TIMEOUT', 0), contextlib.redirect_stderr(io.StringIO()):
            clone = module.Clone()
            self.clone.parent.mkdir(parents=True)
            with open(self.clone.with_name('evidence.lock'), 'a') as holder:
                fcntl.flock(holder, fcntl.LOCK_EX)
                with self.assertRaisesRegex(module.EvidenceError, 'Run again when it finishes'):
                    with clone.locked():
                        self.fail('The lock must not be granted')


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

    def test_documented_retry_command_lands_a_kept_commit(self):
        hook = self.reject_pushes()
        self.tool('publish', str(self.source()), expect=1)
        hook.unlink()
        branch = 'unpushed/' + real_git('rev-parse', 'main', cwd=self.clone)[:12]
        self.tool('publish', str(self.source('2026-11-05-another')))
        real_git('cherry-pick', branch, cwd=self.clone)
        real_git('push', 'origin', 'HEAD:main', cwd=self.clone)
        self.assertEqual(self.remote_file('records/2026-11-05-sample/log.txt'), 'evidence')

    def test_dirty_clone_is_resynced_and_reported(self):
        self.publish_json(self.source())
        head = self.main_head()
        (self.clone / 'README.md').write_text('tampered\n')
        (self.clone / 'stray.txt').write_text('stray\n')
        result = self.tool('publish', str(self.source('2026-11-05-second')))
        self.assertIn('uncommitted paths', result.stderr)
        self.assertEqual(self.remote_file('README.md'), '# put.gd evidence')
        self.assertFalse((self.clone / 'stray.txt').exists())
        self.assertEqual(real_git('rev-parse', 'HEAD~1', cwd=self.clone), head)

    def test_interrupted_rebase_and_stale_index_lock_recover(self):
        self.publish_json(self.source())
        (self.clone / '.git/index.lock').write_text('')
        (self.clone / '.git/rebase-merge').mkdir()
        (self.clone / '.git/rebase-merge/head-name').write_text('refs/heads/main\n')
        result = self.tool('publish', str(self.source('2026-11-05-second')))
        self.assertIn('Published', result.stdout)
        self.assertFalse((self.clone / '.git/rebase-merge').exists())

    def test_interrupted_clone_creation_recovers(self):
        self.clone.mkdir(parents=True)
        real_git('init', '-q', cwd=self.clone)
        self.assertEqual(self.publish_json(self.source())['status'], 'published')

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

    def test_clone_directory_inside_another_repository_is_refused(self):
        outer = self.root / 'outer'
        real_git('init', '-q', str(outer))
        nested = outer / 'nested'
        nested.mkdir()
        (nested / '.git').mkdir()
        result = self.tool('link', SEEDED, env=self.env(PUTGD_EVIDENCE_DIR=str(nested)), expect=1)
        self.assertIn('not the top of a git clone', result.stderr)

    def test_unreachable_remote_says_what_to_do(self):
        env = self.env(PUTGD_EVIDENCE_REMOTE=f'file://{self.root}/absent.git')
        result = self.tool('link', SEEDED, env=env, expect=1)
        self.assertIn('credential helper', result.stderr)
        self.assertIn('GH_TOKEN', result.stderr)


class LinkAndFetchTests(EvidenceBase):
    def test_link_record_is_pinned_to_remote_main(self):
        head = self.main_head()
        result = json.loads(self.tool('link', SEEDED, '--json').stdout)
        self.assertEqual(result['commit'], head)
        self.assertEqual(result['markdown'],
                         f'[Seeded record](https://github.com/patrick-hudson/put.gd-evidence/tree/{head}'
                         f'/records/{SEEDED})')
        self.assertEqual([image['path'] for image in result['images']], ['shots/first.png'])
        self.assertIn(f'Record {SEEDED} at {head}', self.tool('link', SEEDED).stdout)

    def test_link_follows_remote_main(self):
        self.tool('link', SEEDED)
        self.publish_json(self.source())
        head = self.main_head()
        self.assertEqual(json.loads(self.tool('link', SEEDED, '--json').stdout)['commit'], head)

    def test_link_paths(self):
        head = self.main_head()
        result = json.loads(self.tool('link', SEEDED, 'shots/first.png', 'shots', 'run.sh', '--json').stdout)
        base = 'https://github.com/patrick-hudson/put.gd-evidence'
        self.assertEqual([(item['type'], item['url']) for item in result['paths']], [
            ('blob', f'{base}/blob/{head}/records/{SEEDED}/shots/first.png'),
            ('tree', f'{base}/tree/{head}/records/{SEEDED}/shots'),
            ('blob', f'{base}/blob/{head}/records/{SEEDED}/run.sh')])
        self.assertEqual(len(result['images']), 1)
        narrowed = json.loads(self.tool('link', SEEDED, 'run.sh', '--json').stdout)
        self.assertEqual(narrowed['images'], [])

    def test_link_reaches_imported_shapes(self):
        head = self.main_head()
        single = json.loads(self.tool('link', '2026-01-02-single.png', '--json').stdout)
        self.assertIn(f'/blob/{head}/records/2026-01-02-single.png', single['tree_url'])
        self.assertIsNone(single['readme_url'])
        self.assertEqual(single['images'][0]['raw_url'],
                         f'https://raw.githubusercontent.com/patrick-hudson/put.gd-evidence/{head}'
                         '/records/2026-01-02-single.png')
        unprefixed = json.loads(self.tool('link', 'old-unprefixed', '--json').stdout)
        self.assertIsNone(unprefixed['readme_url'])
        self.assertIn('[old-unprefixed]', unprefixed['markdown'])
        self.assertNotIn('README', self.tool('link', 'old-unprefixed').stdout)

    def test_link_missing_record_or_path_fails_clearly(self):
        self.assertIn('No record named 2026-09-09-absent', self.tool('link', '2026-09-09-absent', expect=1).stderr)
        missing = self.tool('link', SEEDED, 'shots/first.png', 'nope.txt', expect=1)
        self.assertIn('has no', missing.stderr)
        self.assertIn('nope.txt', missing.stderr)
        self.assertEqual(missing.stdout, '')
        self.assertIn('Invalid path', self.tool('link', SEEDED, '../other', expect=1).stderr)
        self.assertIn('Invalid record name', self.tool('link', '../etc', expect=1).stderr)

    def test_link_does_not_disturb_a_kept_commit(self):
        hook = self.bare / 'hooks/pre-receive'
        hook.write_text('#!/bin/sh\nexit 1\n')
        hook.chmod(0o755)
        self.tool('publish', str(self.source()), expect=1)
        local = real_git('rev-parse', 'main', cwd=self.clone)
        self.tool('link', SEEDED)
        self.assertEqual(real_git('rev-parse', 'main', cwd=self.clone), local)

    def test_fetch_materializes_the_record(self):
        destination = self.root / 'copies/seeded'
        result = self.tool('fetch', SEEDED, '--dest', str(destination))
        self.assertEqual(result.stdout.strip(), str(destination))
        self.assertEqual((destination / 'README.md').read_text(), '# Seeded record\n')
        self.assertEqual((destination / 'shots/first.png').read_bytes(), b'\x89PNG seeded')
        self.assertTrue(stat.S_IMODE((destination / 'run.sh').stat().st_mode) & 0o100)
        self.assertFalse(stat.S_IMODE((destination / 'README.md').stat().st_mode) & 0o100)

    def test_fetch_default_destination_is_outside_the_clone_and_cwd(self):
        path = Path(self.tool('fetch', SEEDED).stdout.strip())
        self.assertEqual(path, self.root / 'cache/evidence-records' / SEEDED)
        self.assertTrue((path / 'README.md').is_file())
        self.assertEqual(list(self.outside.iterdir()), [])

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

    def test_fetch_missing_record_creates_nothing(self):
        destination = self.root / 'absent-copy'
        result = self.tool('fetch', '2026-09-09-absent', '--dest', str(destination), expect=1)
        self.assertIn('No record named', result.stderr)
        self.assertFalse(destination.exists())

    def test_fetch_single_file_record(self):
        destination = self.root / 'single'
        self.tool('fetch', '2026-01-02-single.png', '--dest', str(destination))
        self.assertEqual((destination / '2026-01-02-single.png').read_bytes(), b'\x89PNG single')

    def test_fetch_then_add_then_publish_round_trip(self):
        destination = self.root / 'work' / SEEDED
        self.tool('fetch', SEEDED, '--dest', str(destination))
        (destination / 'added.txt').write_text('new\n')
        result = self.publish_json(destination)
        self.assertEqual(result['added'], [f'records/{SEEDED}/added.txt'])
        self.assertEqual(result['changed'], [])


class PartialCloneTests(EvidenceBase):
    def missing_objects(self):
        listing = real_git('rev-list', '--objects', '--missing=print', 'origin/main', cwd=self.clone)
        return {line[1:] for line in listing.splitlines() if line.startswith('?')}

    def git_dir_bytes(self):
        return sum(path.stat().st_size for path in (self.clone / '.git').rglob('*') if path.is_file())

    def test_publish_downloads_only_what_it_handles(self):
        big = real_git('rev-parse', 'refs/heads/main:records/2026-01-03-other/big.bin', cwd=self.bare)
        self.publish_json(self.source())
        self.assertIn(big, self.missing_objects())
        self.assertFalse((self.clone / 'records/2026-01-03-other').exists())
        self.assertFalse((self.clone / f'records/{SEEDED}').exists())
        self.assertFalse((self.clone / 'records/README.md').exists())
        self.assertFalse((self.clone / 'records/2026-01-02-single.png').exists())
        self.assertTrue((self.clone / 'records/2026-11-05-sample/log.txt').is_file())
        self.assertTrue((self.clone / 'tools/check.py').is_file())
        self.assertTrue((self.clone / 'README.md').is_file())
        self.assertLess(self.git_dir_bytes(), BIG_BLOB_BYTES // 5)

    def test_race_rebase_keeps_other_blobs_out(self):
        big = real_git('rev-parse', 'refs/heads/main:records/2026-01-03-other/big.bin', cwd=self.bare)
        self.race_count.write_text('1\n')
        self.publish_json(self.source())
        self.assertIn(big, self.missing_objects())
        self.assertLess(self.git_dir_bytes(), BIG_BLOB_BYTES // 5)

    def test_comparing_an_existing_record_downloads_nothing_when_refused(self):
        self.tool('link', 'old-unprefixed')
        big = real_git('rev-parse', 'refs/heads/main:records/2026-01-03-other/big.bin', cwd=self.bare)
        source = self.source('2026-01-03-other', files={'README.md': '# Other record, edited\n'})
        self.assertIn('Different bytes', self.tool('publish', str(source), expect=1).stderr)
        self.assertIn(big, self.missing_objects())

    def test_handling_another_record_narrows_the_checkout(self):
        self.publish_json(self.source())
        self.tool('fetch', SEEDED, '--dest', str(self.root / 'copy'))
        self.assertFalse((self.clone / 'records/2026-11-05-sample').exists())
        self.assertTrue((self.clone / f'records/{SEEDED}/README.md').is_file())


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

    def test_github_token_also_works(self):
        env = self.env(PUTGD_EVIDENCE_REMOTE=self.REMOTE, GITHUB_TOKEN=TOKEN)
        self.tool('link', SEEDED, env=env)
        self.assertIn('extraHeader', (self.root / 'argv.log.config').read_text())

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
        with patch.dict(os.environ, {'GH_TOKEN': TOKEN, 'GIT_CONFIG_COUNT': '2'}), patch.object(module, 'secrets', []):
            env = module.git_env('https://github.com/owner/repo.git')
            self.assertEqual(env['GIT_CONFIG_COUNT'], '3')
            self.assertEqual(env['GIT_CONFIG_KEY_2'], 'http.https://github.com/.extraHeader')
            self.assertEqual(env['GIT_CONFIG_VALUE_2'], 'Authorization: Basic ' + self.credential)
            self.assertNotIn('GH_TOKEN', env)
            for remote in ('git@github.com:owner/repo.git', 'ssh://git@github.com/owner/repo.git',
                           'http://example.invalid/x.git'):
                self.assertEqual(module.git_env(remote)['GIT_CONFIG_COUNT'], '2')

    def test_remote_with_embedded_credentials_refused(self):
        env = self.env(PUTGD_EVIDENCE_REMOTE=f'https://x-access-token:{TOKEN}@github.com/o/r.git')
        result = self.tool('link', SEEDED, env=env, expect=1)
        self.assertIn('must not embed credentials', result.stderr)
        self.assert_clean(result.stdout, result.stderr)
        self.assertFalse(self.clone.exists())


class NameTests(unittest.TestCase):
    def test_slug_parsing_falls_back_to_the_evidence_repo(self):
        for remote, slug in (('https://github.com/owner/repo.git', 'owner/repo'),
                             ('git@github.com:owner/repo.git', 'owner/repo'),
                             ('ssh://git@github.com/owner/repo', 'owner/repo'),
                             ('file:///tmp/remote.git', module.DEFAULT_SLUG)):
            self.assertEqual(module.github_slug(remote), slug)

    def test_title_needs_a_real_heading(self):
        self.assertEqual(module.title_of('intro\n# Real title  \n# Second\n'), 'Real title')
        self.assertIsNone(module.title_of('## Not a title\n#nospace\n'))

    def test_every_name_rule_example(self):
        for name in ('2026-11-05-a', '2026-11-05-a.b-c9'):
            self.assertTrue(re.match(module.NAME_RULE, name))
        for name in ('2026-11-05-', '2026-11-05-.a', 'a', '2026-11-5-a', '2026-11-05-A'):
            self.assertFalse(re.match(module.NAME_RULE, name))


if __name__ == '__main__':
    unittest.main()
