"""Shared code for the repository's git hooks (#1959).

The pre-push hook scans exactly the commits a push would publish with the
gitleaks version and configuration the CI contracts job uses, so a token-shaped
test literal is refused before it reaches any branch CI scans. The other hook
files here only forward to the hooks directory this one replaced, so a global
rule such as a commit-msg check keeps running.
"""
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

# Keep in step with the contracts job in .github/workflows/ci.yml;
# tests/ci/test_git_hooks.py fails when the two differ.
GITLEAKS_VERSION = '8.30.1'
GITLEAKS_URL = ('https://github.com/gitleaks/gitleaks/releases/download/'
                f'v{GITLEAKS_VERSION}/gitleaks_{GITLEAKS_VERSION}_linux_x64.tar.gz')
GITLEAKS_SHA256 = '551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb'

HOOKS = Path(__file__).resolve().parent
ROOT = HOOKS.parents[1]
SKIP_VARIABLE = 'PUTGD_SKIP_GITLEAKS'
INSTALL = 'python3 tools/git-hooks/install.py'
HOOK_FILES = ('hooklib.py', 'pre-push', 'commit-msg', 'pre-commit', 'prepare-commit-msg')


def cache_binary():
    base = os.environ.get('XDG_CACHE_HOME') or str(Path.home() / '.cache')
    return Path(base) / 'putgd-tools' / f'gitleaks-{GITLEAKS_VERSION}' / 'gitleaks'


def binary_version(binary):
    try:
        result = subprocess.run([str(binary), 'version'], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def find_binary():
    """Return (path, problem). GITLEAKS_BIN overrides the tool cache; either must be the pin."""
    chosen = os.environ.get('GITLEAKS_BIN')
    binary = Path(chosen) if chosen else cache_binary()
    if not binary.is_file():
        return None, f'gitleaks {GITLEAKS_VERSION} is not installed at {binary}'
    version = binary_version(binary)
    if version != GITLEAKS_VERSION:
        return None, f'{binary} reports version {version!r}, not the pinned {GITLEAKS_VERSION}'
    return binary, None


def fetch(stream=sys.stdout):
    """Install the pinned gitleaks into the user tool cache, verifying the archive checksum."""
    target = cache_binary()
    if binary_version(target) == GITLEAKS_VERSION:
        print(f'gitleaks {GITLEAKS_VERSION} already at {target}', file=stream)
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent, prefix='.fetch-') as scratch:
        archive = Path(scratch) / 'gitleaks.tar.gz'
        subprocess.run([sys.executable, str(ROOT / 'tools/releases/http_retry.py'), GITLEAKS_URL, str(archive)],
                       check=True)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != GITLEAKS_SHA256:
            raise SystemExit(f'gitleaks archive checksum {digest} does not match the pin {GITLEAKS_SHA256}')
        with tarfile.open(archive) as bundle:
            member = bundle.getmember('gitleaks')
            if not member.isfile():
                raise SystemExit('gitleaks archive entry is not a regular file')
            staged = Path(scratch) / 'gitleaks'
            with bundle.extractfile(member) as source:
                staged.write_bytes(source.read())
        staged.chmod(0o755)
        if binary_version(staged) != GITLEAKS_VERSION:
            raise SystemExit('the downloaded gitleaks does not report the pinned version')
        os.replace(staged, target)
    print(f'installed gitleaks {GITLEAKS_VERSION} at {target}', file=stream)
    return target


def git(*args, check=True):
    result = subprocess.run(['git', *args], capture_output=True, text=True)
    if check and result.returncode != 0:
        raise SystemExit(f'git {" ".join(args)} failed: {result.stderr.strip()}')
    return result


def common_dir():
    value = git('rev-parse', '--path-format=absolute', '--git-common-dir', check=False).stdout.strip()
    return Path(value) if value else None


FORWARDER = """#!/bin/sh
# Run the same hook from the directory core.hooksPath replaced; see pre-push for the lookup.
d=$(git rev-parse --show-toplevel 2>/dev/null)/tools/git-hooks
[ -f "$d/hooklib.py" ] || d=$(dirname "$0")
exec python3 "$d/hooklib.py" forward "$(basename "$0")" "$@"
"""
PREVIOUS_KEY = 'putgd.previousHooksPath'


def ours():
    common = common_dir()
    return {path.resolve() for path in (HOOKS, common / 'putgd-hooks' if common else HOOKS)}


def enable(scope):
    """Point core.hooksPath at a copy of the hooks in <git common dir>/putgd-hooks, which no
    branch switch removes, after recording the hooks directory it replaces."""
    current = git('config', '--type=path', '--get', 'core.hooksPath', check=False).stdout.strip()
    if current and Path(current).expanduser().resolve() not in ours():
        git('config', scope, PREVIOUS_KEY, current)
    elif current and not git('config', '--get', PREVIOUS_KEY, check=False).stdout.strip():
        # Already pointing here with nothing recorded: the replaced path can only be a wider scope's.
        for wider in ('--global', '--system'):
            value = git('config', wider, '--includes', '--type=path', '--get', 'core.hooksPath',
                        check=False).stdout.strip()
            if value and Path(value).expanduser().resolve() not in ours():
                git('config', scope, PREVIOUS_KEY, value)
                break
    target = common_dir() / 'putgd-hooks'
    target.mkdir(exist_ok=True)
    previous = previous_hooks_dir()
    # A forwarder for every hook the replaced directory runs, so none of them stops running.
    extra = sorted(hook.name for hook in previous.iterdir()
                   if hook.is_file() and os.access(hook, os.X_OK) and '.' not in hook.name
                   and hook.name not in HOOK_FILES) if previous and previous.is_dir() else []
    for name in (*HOOK_FILES, *extra):
        staged = target / f'.{name}.new'
        if name in HOOK_FILES:
            shutil.copyfile(HOOKS / name, staged)
        else:
            staged.write_text(FORWARDER)
        staged.chmod(0o644 if name.endswith('.py') else 0o755)
        os.replace(staged, target / name)
    git('config', scope, 'core.hooksPath', str(target))
    effective = git('config', '--type=path', '--get', 'core.hooksPath', check=False).stdout.removesuffix('\n')
    if effective != str(target):
        raise SystemExit(f'core.hooksPath is still {effective!r} after setting it in {scope[2:]} config: '
                         'a later include or scope overrides it. Remove that setting and run this again.')
    return target, previous


def previous_hooks_dir():
    """The hooks directory enable() replaced, else the repository's own hooks directory."""
    value = git('config', '--type=path', '--get', PREVIOUS_KEY, check=False).stdout.strip()
    if value:
        return Path(value).expanduser()
    common = common_dir()
    return common / 'hooks' if common else None


def run_previous(name, args, stdin=None):
    directory = previous_hooks_dir()
    if directory is None:
        return 0
    try:
        if directory.resolve() in ours():
            return 0
    except OSError:
        return 0
    hook = directory / name
    if not (hook.is_file() and os.access(hook, os.X_OK)):
        return 0
    return subprocess.run([str(hook), *args], input=stdin).returncode


def outgoing(remote, data):
    """Revision arguments naming the commits this push adds: the pushed tips, minus what the
    destination remote's tracking refs and current tips already have. None when a line of
    git's update list is malformed."""
    tips, known = [], []
    for line in data.split(b'\n'):
        if not line:
            continue
        fields = line.rsplit(b' ', 3)  # the last three fields never contain a space
        if len(fields) != 4:
            return None
        local_sha, remote_sha = fields[1].decode('ascii', 'replace'), fields[3].decode('ascii', 'replace')
        if not all(c in '0123456789abcdef' for c in local_sha + remote_sha):
            return None
        if set(local_sha) == {'0'}:
            continue  # branch or tag deletion
        tips.append(local_sha)
        if set(remote_sha) != {'0'} and git('cat-file', '-e', f'{remote_sha}^{{commit}}', check=False).returncode == 0:
            known.append(remote_sha)
    # Tracking refs describe the push destination only when it is the repository they are fetched from.
    fetch_url = git('remote', 'get-url', remote, check=False).stdout.removesuffix('\n') if remote else ''
    push_urls = git('remote', 'get-url', '--push', '--all', remote, check=False).stdout.split('\n')[:-1] if remote else []
    tracked = bool(fetch_url) and push_urls == [fetch_url]
    return tips, ([f'--remotes={remote}'] if tracked else []) + known


def pre_push(args, stdin):
    if os.environ.get(SKIP_VARIABLE) == '1':
        print(f'pre-push: {SKIP_VARIABLE}=1, skipping the gitleaks scan of this push. CI still scans every '
              'branch; a finding on this push turns contracts red for every pull request.', file=sys.stderr)
        return run_previous('pre-push', args, stdin)
    found = outgoing(args[0] if args else '', stdin)
    if found is None:
        print('pre-push: refused, git passed an update line this hook cannot read.', file=sys.stderr)
        return 1
    tips, exclude = found
    if tips:
        count = int(git('rev-list', '--count', *tips, '--not', *exclude, '--').stdout.strip() or 0)
        if count:
            status = scan(tips, exclude, count)
            if status:
                return status
    return run_previous('pre-push', args, stdin)


def scan(tips, exclude, count):
    binary, problem = find_binary()
    if problem:
        print(f'pre-push: refused, {problem}.\n'
              f'  Install it with: {INSTALL} --fetch-only\n'
              f'  Emergency override (CI still scans): {SKIP_VARIABLE}=1 git push ...', file=sys.stderr)
        return 1
    top = Path(git('rev-parse', '--show-toplevel').stdout.strip())
    config = top / '.gitleaks.toml'
    if not config.is_file():
        print(f'pre-push: refused, {config} is missing.', file=sys.stderr)
        return 1
    # gitleaks parses `git log -p`; these undo user settings that would reshape or hide the patch.
    log_opts = ' '.join(['--no-color', '--no-ext-diff', '--no-textconv', '--no-show-signature', '--default-prefix', '--root',
                         *tips, '--not', *exclude])
    plural = '' if count == 1 else 's'
    print(f'pre-push: gitleaks {GITLEAKS_VERSION} over {count} outgoing commit{plural}', file=sys.stderr)
    with tempfile.TemporaryDirectory(prefix='putgd-gitleaks-') as scratch:
        report = Path(scratch) / 'report.json'
        result = subprocess.run([str(binary), 'git', str(top), '--config', str(config), f'--log-opts={log_opts}',
                                 '--redact', '--no-banner', '--exit-code', '1', '--report-format', 'json',
                                 '--report-path', str(report)], stdout=sys.stderr,
                                # Git's default, so a user's lower threshold cannot turn text fixtures binary.
                                env=dict(os.environ, GIT_CONFIG_COUNT='1', GIT_CONFIG_KEY_0='core.bigFileThreshold',
                                         GIT_CONFIG_VALUE_0='512m'))
        findings = []
        if result.returncode == 1:
            try:
                findings = json.loads(report.read_text())
            except (OSError, ValueError):
                findings = []
    if result.returncode == 0:
        return 0
    if result.returncode == 1:
        # Only the location: a finding's match or line could carry a neighbouring secret.
        for finding in findings:
            print(f"  {finding.get('RuleID')}: {finding.get('File')}:{finding.get('StartLine')} "
                  f"in {str(finding.get('Commit'))[:12]}", file=sys.stderr)
        print('pre-push: refused, gitleaks found a secret-shaped value in an outgoing commit.\n'
              '  Rewrite the commit that adds it; a later fix-up commit leaves it in the history CI scans.\n'
              '  Build synthetic fixture values from pieces, or use obviously fake words.', file=sys.stderr)
    else:
        print(f'pre-push: refused, gitleaks exited with {result.returncode}.', file=sys.stderr)
    return 1


def main(argv):
    if len(argv) >= 2 and argv[0] == 'forward':
        # The forwarded hook inherits stdin, so a hook that reads none never waits on it.
        return run_previous(argv[1], argv[2:])
    if argv and argv[0] == 'pre-push':
        return pre_push(argv[1:], sys.stdin.buffer.read())
    print('usage: hooklib.py pre-push|forward <hook> [args...]', file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
