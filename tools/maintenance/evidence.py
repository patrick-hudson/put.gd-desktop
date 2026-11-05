#!/usr/bin/env python3
"""Publish test evidence to the put.gd-evidence repository and print pinned links.

Collect evidence in a local directory with a top-level README.md whose title is
a "# " line, then run:

  python3 tools/maintenance/evidence.py publish work/2026-11-05-recording-kde

The directory lands at records/<NAME>/ on the evidence repository's main branch
and the command prints commit-pinned links for docs and pull requests. There is
no pull request, manual clone or index edit.

publish SRC_DIR [--name NAME] [--amend REASON] [--dry-run] [--json]
  NAME defaults to the directory name; without a YYYY-MM-DD- prefix it gets
  today's UTC date. SRC_DIR is the complete content of the record: new files are
  added and identical files skipped. A file with different bytes or a different
  executable bit, or a published file missing from SRC_DIR, is refused unless
  --amend REASON is given. To add to a record, `fetch` it, add the files to that
  copy and publish the copy, so nothing reads as a deletion. With --amend,
  everything missing from SRC_DIR is deleted from the record.
link NAME [PATH ...] [--json]
  Print the links of an existing record, or of files and directories inside it,
  pinned to the current remote main commit.
fetch NAME [--dest DIR]
  Copy an existing record out of the repository and print where it is. The
  default destination is a directory next to the cache clone. A non-empty
  destination is never overwritten.

Environment:
  PUTGD_EVIDENCE_DIR     cache clone, default ${XDG_CACHE_HOME:-~/.cache}/putgd/evidence
  PUTGD_EVIDENCE_REMOTE  remote, default https://github.com/patrick-hudson/put.gd-evidence.git
  GH_TOKEN, GITHUB_TOKEN authenticate https remotes; otherwise git's configured
                         credential helper applies
  GITLEAKS_BIN           gitleaks executable, default gitleaks on PATH

The cache clone is a blobless partial clone with a sparse checkout: it holds
history and trees, and downloads files only for tools/, root files and the record
being handled. A commit that could not be pushed is kept as a branch there and
the next run says how to retry it. One run at a time uses a clone.
"""
import argparse
import base64
import contextlib
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.parse

DEFAULT_REMOTE = 'https://github.com/patrick-hudson/put.gd-evidence.git'
DEFAULT_SLUG = 'patrick-hudson/put.gd-evidence'
NAME_RULE = re.compile(r'^\d{4}-\d{2}-\d{2}-[a-z0-9][a-z0-9.-]*$')
DATE_PREFIX = re.compile(r'^\d{4}-\d{2}-\d{2}-')
# The imported tree predates NAME_RULE; link and fetch still have to reach it.
EXISTING_NAME = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]*$')
IMAGE_SUFFIXES = ('.png', '.jpg', '.jpeg', '.webp', '.gif')
NOT_FAST_FORWARD = re.compile(r'\[rejected\]|non-fast-forward|fetch first|cannot lock ref|failed to update ref')
URL_CREDENTIALS = re.compile(r'(\b[a-z][a-z0-9+.-]*://)[^/@\s]+@', re.IGNORECASE)
PUSH_RETRIES = 5
LOCK_TIMEOUT = 900
# Repository-scoped variables from an enclosing hook or tool would redirect git away from the clone.
DROPPED_ENV = frozenset({'GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY',
                         'GIT_ALTERNATE_OBJECT_DIRECTORIES', 'GIT_COMMON_DIR', 'GIT_PREFIX', 'GIT_NAMESPACE',
                         'GIT_CONFIG', 'GIT_CONFIG_PARAMETERS', 'GIT_CURL_VERBOSE', 'LANGUAGE',
                         'GH_TOKEN', 'GITHUB_TOKEN'})
CLONE_CONFIG = (('core.hooksPath', '/dev/null'), ('core.autocrlf', 'false'), ('core.safecrlf', 'false'),
                ('core.fileMode', 'true'), ('commit.gpgSign', 'false'), ('rebase.autoStash', 'false'),
                ('remote.origin.fetch', '+refs/heads/main:refs/remotes/origin/main'),
                ('remote.origin.promisor', 'true'), ('remote.origin.partialCloneFilter', 'blob:none'),
                ('remote.origin.tagOpt', '--no-tags'))
# Evidence is stored byte for byte, whatever a .gitattributes in a record or the repository says.
ATTRIBUTES = '* -text -ident -filter -working-tree-encoding\n'
secrets = []


class EvidenceError(Exception):
    pass


def scrub(text):
    for secret in secrets:
        text = text.replace(secret, '***')
    return URL_CREDENTIALS.sub(r'\1***@', text)


def note(message):
    print(scrub(message), file=sys.stderr, flush=True)


def tool_env():
    env = {key: value for key, value in os.environ.items()
           if key not in DROPPED_ENV and not key.startswith('GIT_TRACE')}
    env.update(LC_ALL='C', GIT_EDITOR='true', GIT_LITERAL_PATHSPECS='1')
    return env


def git_env(remote):
    env = tool_env()
    token = (os.environ.get('GH_TOKEN') or os.environ.get('GITHUB_TOKEN') or '').strip()
    if not sys.stdin.isatty() or token:
        env['GIT_TERMINAL_PROMPT'] = '0'
    if not token:
        return env
    credential = base64.b64encode(('x-access-token:' + token).encode()).decode()
    secrets.extend((token, credential))
    parsed = urllib.parse.urlsplit(remote)
    # Scoped to the remote's own host, so the token cannot reach anything else git talks to.
    if parsed.scheme == 'https':
        count = os.environ.get('GIT_CONFIG_COUNT', '')
        index = int(count) if count.isdigit() else 0
        env[f'GIT_CONFIG_KEY_{index}'] = f'http.https://{parsed.netloc}/.extraHeader'
        env[f'GIT_CONFIG_VALUE_{index}'] = 'Authorization: Basic ' + credential
        env['GIT_CONFIG_COUNT'] = str(index + 1)
    return env


def run(argv, cwd, env, stdin=''):
    return subprocess.run(argv, cwd=cwd, env=env, input=stdin, capture_output=True,
                          encoding='utf-8', errors='surrogateescape')


def output_of(result):
    return (result.stderr.strip() + '\n' + result.stdout.strip()).strip()


def remote_setting():
    remote = os.environ.get('PUTGD_EVIDENCE_REMOTE') or DEFAULT_REMOTE
    if re.match(r'https?://[^/]*@', remote, re.IGNORECASE):
        raise EvidenceError('PUTGD_EVIDENCE_REMOTE must not embed credentials. Use a credential helper or GH_TOKEN.')
    return remote


def clone_setting():
    configured = os.environ.get('PUTGD_EVIDENCE_DIR')
    if configured:
        return Path(configured).expanduser().resolve()
    cache = os.environ.get('XDG_CACHE_HOME') or '~/.cache'
    return (Path(cache).expanduser() / 'putgd/evidence').resolve()


def invoking_checkout():
    """Return (toplevel, HEAD) of the put.gd-desktop checkout this run belongs to, if any."""
    env = tool_env()
    for start in (Path.cwd(), Path(__file__).resolve().parent):
        top = run(['git', 'rev-parse', '--show-toplevel'], start, env)
        if top.returncode:
            continue
        toplevel = Path(top.stdout.strip())
        if not (toplevel / 'tools/maintenance/evidence.py').is_file():
            continue
        head = run(['git', 'rev-parse', '--verify', '-q', 'HEAD'], toplevel, env)
        if not head.returncode:
            return toplevel, head.stdout.strip()
    return None, None


class Clone:
    def __init__(self):
        self.path = clone_setting()
        self.remote = remote_setting()
        self.env = git_env(self.remote)

    def run(self, *args, stdin=''):
        return run(['git', *args], self.path, self.env, stdin)

    def git(self, *args, stdin='', hint=''):
        result = self.run(*args, stdin=stdin)
        if result.returncode:
            message = f'git {args[0]} failed in {self.path}:\n{output_of(result)}'
            raise EvidenceError(message + ('\n' + hint if hint else ''))
        return result.stdout

    @contextlib.contextmanager
    def locked(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        lock = self.path.with_name(self.path.name + '.lock')
        with open(lock, 'a+') as stream:
            deadline = time.monotonic() + LOCK_TIMEOUT
            waiting = False
            while True:
                try:
                    fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise EvidenceError(f'Another evidence command still holds {lock} after {LOCK_TIMEOUT} s. '
                                            'Run again when it finishes, or set PUTGD_EVIDENCE_DIR to another clone.')
                    if not waiting:
                        note(f'Waiting for another evidence command to release {lock}')
                        waiting = True
                    time.sleep(0.2)
            stream.truncate(0)
            stream.write(f'{os.getpid()}\n')
            stream.flush()
            yield

    def ensure(self):
        if not (self.path / '.git').exists():
            if self.path.exists() and (not self.path.is_dir() or any(self.path.iterdir())):
                raise EvidenceError(f'{self.path} exists and is not an evidence clone. '
                                    'Set PUTGD_EVIDENCE_DIR to an empty or new directory.')
            self.path.mkdir(parents=True, exist_ok=True)
            note(f'Creating the evidence cache clone at {self.path}')
            self.git('init', '-q', '--template=', '-b', 'main')
        top = self.run('rev-parse', '--show-toplevel')
        if top.returncode or Path(top.stdout.strip()).resolve() != self.path:
            raise EvidenceError(f'{self.path} is not the top of a git clone. '
                                'Set PUTGD_EVIDENCE_DIR to an empty or new directory.')
        # Read the raw setting: `remote get-url` would apply the user's url.insteadOf rewrites.
        current = self.run('config', '--local', '--get', 'remote.origin.url')
        if current.returncode:
            self.git('remote', 'add', 'origin', self.remote)
        elif current.stdout.strip() != self.remote:
            raise EvidenceError(f'The clone at {self.path} uses {current.stdout.strip()}, not {self.remote}. '
                                'Set PUTGD_EVIDENCE_DIR to another directory, or if it is the same repository run '
                                f'`git -C {self.path} remote set-url origin <url>`.')
        for key, value in CLONE_CONFIG:
            self.git('config', key, value)
        attributes = self.path / '.git/info/attributes'
        attributes.parent.mkdir(exist_ok=True)
        attributes.write_text(ATTRIBUTES)
        # Sparse patterns come first: one checkout without them downloads the whole repository.
        if self.run('config', '--get', 'core.sparseCheckout').stdout.strip() != 'true':
            self.sparse()

    def fetch(self):
        self.git('fetch', '-q', 'origin',
                 hint=f'Check the network and access to {self.remote}: configure a git credential helper '
                      '(for example `gh auth setup-git`) or set GH_TOKEN, then run again.')

    def sparse(self, name=None):
        patterns = ['/*', '!/*/', '/tools/']
        if name:
            patterns.append(f'/records/{name}')
        self.git('sparse-checkout', 'set', '--no-cone', *patterns)

    def discard(self):
        self.git('reset', '-q', '--hard', 'origin/main')
        self.git('clean', '-fdqx')

    def resync(self):
        """Return the clone to origin/main, keeping and reporting what a failed run left behind."""
        git_dir = self.path / '.git'
        if (git_dir / 'rebase-merge').exists() or (git_dir / 'rebase-apply').exists():
            if self.run('rebase', '--abort').returncode:
                self.run('rebase', '--quit')
        # The lock is held, so an index.lock can only be left over from a killed run.
        (git_dir / 'index.lock').unlink(missing_ok=True)
        tips = {self.run('rev-parse', '--verify', '-q', ref + '^{commit}').stdout.strip()
                for ref in ('HEAD', 'refs/heads/main')}
        for tip in sorted(tips - {''}):
            self.keep_unpushed(tip)
        if self.run('rev-parse', '--verify', '-q', 'HEAD').stdout.strip():
            dirty = self.run('status', '--porcelain', '--no-renames').stdout.splitlines()
            if dirty:
                note(f'Discarding {len(dirty)} uncommitted paths left in {self.path}; they were never pushed. '
                     'Publish from the source directory to put them on the evidence repository.')
        self.git('checkout', '-q', '-f', '-B', 'main', 'origin/main')
        self.git('clean', '-fdqx')

    def keep_unpushed(self, tip):
        subjects = self.git('log', '--format=%h %s', 'origin/main..' + tip).splitlines()
        if not subjects:
            return
        branch = 'unpushed/' + tip[:12]
        self.git('branch', '-f', branch, tip)
        note(f'An earlier run did not push {len(subjects)} commit(s), kept as branch {branch} in {self.path}:\n  '
             + '\n  '.join(subjects) + '\nTo retry, run publish again with the same source directory. If the source '
             f'is gone, run `git -C {self.path} cherry-pick {branch} && git -C {self.path} push origin HEAD:main`.')

    def head(self, ref='origin/main'):
        return self.git('rev-parse', '--verify', ref + '^{commit}').strip()

    def entry_type(self, commit, path):
        """Return 'tree', 'blob' or None without downloading the object."""
        listing = self.git('ls-tree', '-z', commit, '--', path)
        return listing.split('\0')[0].split()[1] if listing else None

    def files(self, commit, path):
        found = {}
        for line in self.git('ls-tree', '-r', '-z', commit, '--', path).split('\0'):
            if line:
                meta, name = line.split('\t', 1)
                mode, _, sha = meta.split()
                found[name] = (mode, sha)
        return found

    def commit_identity(self, checkout):
        scopes = [(checkout, [])] if checkout else [(self.path, ['--global']), (self.path, ['--system'])]
        env = tool_env()
        for key in ('user.name', 'user.email'):
            for cwd, scope in scopes:
                value = run(['git', 'config', *scope, '--get', key], cwd, env).stdout.strip()
                if value:
                    self.git('config', key, value)
                    break
            else:
                raise EvidenceError(f'No git {key} is configured for the evidence commit. Run '
                                    f'`git config --global {key} <value>` and publish again.')

    def push(self):
        kept = (f'The commit stays in {self.path} and nothing was lost; '
                'run the same publish command again to retry.')
        for attempt in range(PUSH_RETRIES + 1):
            result = self.run('push', '-q', '--no-verify', 'origin', 'HEAD:refs/heads/main')
            if not result.returncode:
                return self.head('HEAD')
            if not NOT_FAST_FORWARD.search(result.stderr + result.stdout):
                raise EvidenceError(f'Push to {self.remote} failed:\n{output_of(result)}\n'
                                    'Check push access (credential helper or GH_TOKEN). ' + kept)
            if attempt == PUSH_RETRIES:
                break
            note('Remote main moved; rebasing the record commit and pushing again')
            self.fetch()
            rebased = self.run('rebase', '-q', '--no-autostash', '-X', 'no-renames', 'origin/main')
            if rebased.returncode:
                self.run('rebase', '--abort')
                raise EvidenceError('Someone else published conflicting files to this record:\n'
                                    f'{output_of(rebased)}\n' + kept)
        raise EvidenceError(f'Remote main kept moving through {PUSH_RETRIES} retries. ' + kept)


def record_name(requested, explicit):
    name = requested
    if not DATE_PREFIX.match(name):
        name = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%d-') + name
        if explicit:
            note(f'The name has no date prefix; using {name}')
    if not NAME_RULE.match(name):
        raise EvidenceError(f'Invalid record name {name!r}. A name is YYYY-MM-DD- followed by lowercase letters, '
                            'digits, dots and hyphens, starting with a letter or digit '
                            '(for example 2026-11-05-recording-kde). Choose one with --name.')
    return name


def existing_name(name):
    if not EXISTING_NAME.match(name):
        raise EvidenceError(f'Invalid record name {name!r}. Record names have no slashes and start with '
                            'a letter or digit; pass the directory name under records/.')
    return name


def amend_reason(reason):
    if reason is None:
        return None
    reason = reason.strip()
    if not reason or '\n' in reason:
        raise EvidenceError('--amend needs a one-line reason; it becomes the Evidence-Amend trailer.')
    return reason


def blob_id(path):
    size = os.path.getsize(path)
    digest = hashlib.sha1(b'blob %d\0' % size, usedforsecurity=False)
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def listed(paths, limit=20):
    shown = '\n'.join('  ' + path for path in paths[:limit])
    return shown + (f'\n  and {len(paths) - limit} more' if len(paths) > limit else '')


def scan_source(source):
    """Map each relative path in source to its git (mode, sha), refusing what cannot be published as is."""
    wanted, refused = {}, []
    root = str(source)
    for current, directories, names in os.walk(root, followlinks=False):
        for name in sorted(directories + names):
            path = os.path.join(current, name)
            relative = os.path.relpath(path, root)
            if name.lower() == '.git':
                refused.append(relative + ' (git metadata)')
            elif os.path.islink(path):
                refused.append(relative + ' (symlink)')
            elif not relative.isprintable():
                refused.append(ascii(relative) + ' (control characters or undecodable bytes in the name)')
            elif not os.path.realpath(path).startswith(root + os.sep):
                refused.append(relative + ' (resolves outside the directory)')
            elif name in names and not os.path.isfile(path):
                refused.append(relative + ' (not a regular file)')
            elif name in names:
                wanted[relative] = ('100755' if os.lstat(path).st_mode & 0o100 else '100644', blob_id(path))
        directories[:] = [name for name in directories
                          if name.lower() != '.git' and not os.path.islink(os.path.join(current, name))]
    if refused:
        raise EvidenceError(f'Cannot publish {source}. Remove or replace these and run again:\n'
                            + listed(sorted(refused)))
    if not wanted:
        raise EvidenceError(f'{source} has no files to publish.')
    return wanted


def title_of(text):
    for line in text.splitlines():
        match = re.match(r'# +(\S.*)$', line)
        if match:
            return match.group(1).strip()
    return None


def place_files(source, target, wanted, removed):
    for relative in removed:
        (target / relative).unlink()
        for parent in (target / relative).parents:
            if parent == target or any(parent.iterdir()):
                break
            parent.rmdir()
    for relative, mode in wanted:
        path = target / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / relative, path)
        path.chmod(0o755 if mode == '100755' else 0o644)


def check_record(clone, directory, name):
    checker = clone.path / 'tools/check.py'
    if not checker.is_file():
        raise EvidenceError(f'{clone.remote} has no tools/check.py on main, so the record cannot be validated. '
                            'Nothing was committed; publish once the evidence repository has it.')
    result = run([sys.executable, str(checker), 'record', str(directory), '--name', name], clone.path, tool_env())
    if result.returncode:
        raise EvidenceError(f'The evidence repository rejected this record:\n{output_of(result)}\n'
                            'Nothing was committed. Fix the source directory and run again.')


def scan_secrets(clone, directory):
    candidates = (os.environ.get('GITLEAKS_BIN'), 'gitleaks')
    binary = next((found for found in (shutil.which(item) for item in candidates if item) if found), None)
    if not binary:
        note("gitleaks is not installed here; the evidence repository's CI will scan the push.")
        return
    config = clone.path / '.gitleaks.toml'
    if not config.is_file():
        raise EvidenceError(f'{clone.remote} has no .gitleaks.toml on main, so the record cannot be scanned. '
                            'Nothing was committed; publish once the evidence repository has it.')
    result = run([binary, 'dir', str(directory), '--config', str(config), '--redact', '--no-banner',
                  '--no-color', '--verbose', '--exit-code', '1'], clone.path, tool_env())
    if result.returncode == 1:
        raise EvidenceError(f'gitleaks found possible secrets in the record:\n{output_of(result)}\n'
                            'Nothing was committed. Remove them from the source directory and run again.')
    if result.returncode:
        raise EvidenceError(f'gitleaks could not scan the record (exit {result.returncode}):\n{output_of(result)}\n'
                            'Nothing was committed. Fix the gitleaks install or unset GITLEAKS_BIN, then run again.')


def github_slug(remote):
    match = re.search(r'github\.com[/:]([^/\s]+/[^/\s]+?)(?:\.git)?/?$', remote)
    return match.group(1) if match else DEFAULT_SLUG


def quoted(path):
    return urllib.parse.quote(path.encode('utf-8', 'surrogateescape'), safe='/')


def markdown_link(title, url):
    return '[' + re.sub(r'([\[\]\\])', r'\\\1', title) + '](' + url + ')'


def is_image(path):
    return path.lower().endswith(IMAGE_SUFFIXES)


def record_links(clone, commit, name, paths=()):
    """Build every link of one record at one commit; images narrow to the given paths."""
    slug = github_slug(clone.remote)
    record = 'records/' + name
    web = f'https://github.com/{slug}'
    raw = f'https://raw.githubusercontent.com/{slug}/{commit}/'

    def page(kind, path):
        return f'{web}/{kind}/{commit}/{quoted(path)}'

    kind = clone.entry_type(commit, record)
    files = clone.files(commit, record)
    readme = record + '/README.md'
    title = name
    if readme in files:
        title = title_of(clone.git('cat-file', 'blob', f'{commit}:{readme}')) or name
    result = {'name': name, 'commit': commit, 'tree_url': page(kind, record),
              'readme_url': page('blob', readme) if readme in files else None,
              'markdown': markdown_link(title, page(kind, record))}
    selected = list(files)
    if paths:
        selected, result['paths'] = [], []
        for path in paths:
            full = record + '/' + path
            found = clone.entry_type(commit, full)
            result['paths'].append({'path': path, 'type': found, 'url': page(found, full),
                                    'markdown': markdown_link(f'{name}/{path}', page(found, full))})
            selected.extend(file for file in files if file == full or file.startswith(full + '/'))
    result['images'] = [{'path': file[len(record) + 1:] or file, 'raw_url': raw + quoted(file)}
                        for file in sorted(set(selected)) if is_image(file)]
    return result


def print_result(result, as_json, summary):
    if as_json:
        print(scrub(json.dumps(result)))
        return
    print(scrub(summary))
    if result.get('tree_url'):
        print(scrub('Tree:     ' + result['tree_url']))
        if result['readme_url']:
            print(scrub('README:   ' + result['readme_url']))
        print(scrub('Markdown: ' + result['markdown']))
    for item in result.get('paths', ()):
        print(scrub(f"Path:     {item['url']}\nMarkdown: {item['markdown']}"))
    for image in result.get('images', ()):
        print(scrub('Image:    ' + image['raw_url']))


def commit_message(name, title, source_sha, reason):
    trailers = []
    if source_sha:
        trailers.append('Source: put.gd-desktop@' + source_sha)
    if reason:
        trailers.append('Evidence-Amend: ' + reason)
    message = f'{name}: {title}\n'
    return message + ('\n' + '\n'.join(trailers) + '\n' if trailers else '')


def publish(args):
    source = Path(args.src_dir).expanduser().resolve()
    if not source.is_dir():
        raise EvidenceError(f'{args.src_dir} is not a directory. Pass the directory holding the evidence.')
    name = record_name(args.name or source.name, bool(args.name))
    reason = amend_reason(args.amend)
    clone = Clone()
    if source == clone.path or clone.path in source.parents:
        raise EvidenceError(f'{source} is inside the cache clone, which every run resets. '
                            'Copy the evidence elsewhere first, or use `fetch` to get a working copy.')
    local = scan_source(source)
    record = 'records/' + name
    wanted = {f'{record}/{relative}': value for relative, value in local.items()}
    with clone.locked():
        clone.ensure()
        clone.fetch()
        clone.resync()
        base = clone.head()
        if clone.entry_type(base, record) == 'blob':
            raise EvidenceError(f'{record} is a single-file record from the import. Choose another name with --name.')
        existing = clone.files(base, record)
        added = sorted(path for path in wanted if path not in existing)
        changed = sorted(path for path in wanted if path in existing and existing[path] != wanted[path])
        deleted = sorted(path for path in existing if path not in wanted)
        plan = {'name': name, 'added': added, 'changed': changed, 'deleted': deleted,
                'unchanged': len(wanted) - len(added) - len(changed)}
        if (changed or deleted) and reason is None:
            problems = []
            if changed:
                problems.append('Different bytes or executable bit:\n' + listed(changed))
            if deleted:
                problems.append('Published but missing from the source (would be deleted):\n' + listed(deleted))
            raise EvidenceError(f'{record} is already published and evidence only grows.\n' + '\n'.join(problems)
                                + '\nTo add files, run `fetch ' + name + '`, add them to that copy and publish it. '
                                'To really change the record, pass --amend REASON.')
        counts = f'{len(added)} added, {len(changed)} changed, {len(deleted)} deleted, {plan["unchanged"]} unchanged'
        if not added and not changed and not deleted:
            result = dict(plan, status='unchanged', **record_links(clone, base, name))
            print_result(result, args.json, f'Nothing to publish: {record} already matches {source}.')
            return
        checkout, source_sha = invoking_checkout()
        clone.commit_identity(checkout)
        clone.sparse(name)
        committed = False
        try:
            target = clone.path / record
            place_files(source, target, [(path[len(record) + 1:], wanted[path][0]) for path in added + changed],
                        [path[len(record) + 1:] for path in deleted])
            check_record(clone, target, name)
            scan_secrets(clone, target)
            clone.git('add', '-A', '-f', '--', record)
            staged = {}
            for line in clone.git('ls-files', '-s', '-z', '--', record).split('\0'):
                if line:
                    meta, path = line.split('\t', 1)
                    mode, sha, _ = meta.split()
                    staged[path] = (mode, sha)
            if staged != wanted:
                raise EvidenceError(f'{source} changed while it was being published. Nothing was committed; '
                                    'run again once it is stable.')
            if args.dry_run:
                print_result(dict(plan, status='dry-run', commit=None, base=base), args.json,
                             f'Dry run: {record} would change on top of {base} ({counts}). '
                             'Checks passed; nothing was committed or pushed.')
                return
            readme = target / 'README.md'
            title = title_of(readme.read_text(encoding='utf-8', errors='replace')) if readme.is_file() else None
            if not title:
                raise EvidenceError(f'{source}/README.md has no "# " title line. Add one and run again.')
            clone.git('commit', '-q', '--no-verify', '--cleanup=whitespace', '-F', '-',
                      stdin=commit_message(name, title, source_sha, reason))
            committed = True
        finally:
            if not committed:
                clone.discard()
        pushed = clone.push()
        result = dict(plan, status='published', **record_links(clone, pushed, name))
        print_result(result, args.json, f'Published {record} at {pushed} ({counts}).')


def link(args):
    name = existing_name(args.name)
    paths = []
    for path in args.paths:
        cleaned = path.strip('/')
        if not cleaned or any(part in {'', '.', '..'} for part in cleaned.split('/')):
            raise EvidenceError(f'Invalid path {path!r}. Pass a path relative to the record.')
        paths.append(cleaned)
    clone = Clone()
    with clone.locked():
        clone.ensure()
        clone.fetch()
        commit = clone.head()
        if not clone.entry_type(commit, 'records/' + name):
            raise EvidenceError(f'No record named {name} at {commit}. Check the name in the records/ index.')
        missing = [path for path in paths if not clone.entry_type(commit, f'records/{name}/{path}')]
        if missing:
            raise EvidenceError(f'Record {name} at {commit} has no:\n' + listed(missing)
                                + '\nCheck the paths; they are relative to the record.')
        print_result(record_links(clone, commit, name, paths), args.json, f'Record {name} at {commit}')


def fetch(args):
    name = existing_name(args.name)
    clone = Clone()
    default = clone.path.with_name(clone.path.name + '-records') / name
    destination = Path(args.dest).expanduser().resolve() if args.dest else default
    if destination == clone.path or clone.path in destination.parents:
        raise EvidenceError(f'{destination} is inside the cache clone, which every run resets. Choose another --dest.')
    if destination.exists() and (not destination.is_dir() or any(destination.iterdir())):
        raise EvidenceError(f'{destination} already exists and is not empty, so nothing was copied. '
                            'Use it as it is, or pass another --dest.')
    record = 'records/' + name
    with clone.locked():
        clone.ensure()
        clone.fetch()
        clone.resync()
        commit = clone.head()
        if not clone.entry_type(commit, record):
            raise EvidenceError(f'No record named {name} at {commit}. Check the name in the records/ index.')
        clone.sparse(name)
        destination.mkdir(parents=True, exist_ok=True)
        for path, (mode, _) in clone.files(commit, record).items():
            target = destination / (path[len(record) + 1:] or Path(path).name)
            target.parent.mkdir(parents=True, exist_ok=True)
            if mode == '120000':
                target.symlink_to(os.readlink(clone.path / path))
            else:
                shutil.copyfile(clone.path / path, target)
                target.chmod(0o755 if mode == '100755' else 0o644)
        note(f'Fetched {name} at {commit}')
        print(scrub(str(destination)))


def parser():
    top = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = top.add_subparsers(dest='command', required=True)
    command = commands.add_parser('publish', help='Put a directory on the evidence repository and print pinned links')
    command.add_argument('src_dir', metavar='SRC_DIR')
    command.add_argument('--name', help='record name; defaults to the directory name, date prefixed if needed')
    command.add_argument('--amend', metavar='REASON',
                         help='allow changing or deleting published files, with this reason')
    command.add_argument('--dry-run', action='store_true', help='run every check, then neither commit nor push')
    command.add_argument('--json', action='store_true', help='print the result as one JSON object')
    command.set_defaults(handler=publish)
    command = commands.add_parser('link', help='Print pinned links for an existing record')
    command.add_argument('name', metavar='NAME')
    command.add_argument('paths', metavar='PATH', nargs='*', help='file or directory relative to the record')
    command.add_argument('--json', action='store_true', help='print the result as one JSON object')
    command.set_defaults(handler=link)
    command = commands.add_parser('fetch', help='Copy an existing record out of the repository')
    command.add_argument('name', metavar='NAME')
    command.add_argument('--dest', metavar='DIR', help='destination, which must be new or empty')
    command.set_defaults(handler=fetch)
    return top


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        args.handler(args)
    except (EvidenceError, OSError) as error:
        print('evidence: ' + scrub(str(error)), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
