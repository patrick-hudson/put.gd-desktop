"""Shared code for the repository's git hooks (#1959).

The pre-push hook scans exactly the commits a push would publish with the
gitleaks version and configuration the CI contracts job uses, so a token-shaped
test literal is refused before it reaches any branch CI scans. The other hook
files here only forward to the hooks directory this one replaced, so a global
rule such as a commit-msg check keeps running.
"""
import hashlib
import os
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


def previous_hooks_dir():
    """The hooks directory core.hooksPath=tools/git-hooks replaced: a global or system
    core.hooksPath, else the repository's own hooks directory."""
    for scope in ('--global', '--system'):
        value = git('config', scope, '--type=path', '--get', 'core.hooksPath', check=False).stdout.strip()
        if value:
            return Path(value).expanduser()
    common = git('rev-parse', '--git-common-dir', check=False).stdout.strip()
    return Path(common).resolve() / 'hooks' if common else None


def run_previous(name, args, stdin=None):
    directory = previous_hooks_dir()
    if directory is None:
        return 0
    try:
        if directory.resolve() == HOOKS:
            return 0
    except OSError:
        return 0
    hook = directory / name
    if not (hook.is_file() and os.access(hook, os.X_OK)):
        return 0
    return subprocess.run([str(hook), *args], input=stdin, text=True).returncode


def outgoing(lines):
    """Revision arguments naming the commits this push adds: the pushed tips, minus
    everything any remote-tracking ref already has and the remote's current tips."""
    tips, known = [], []
    for line in lines:
        fields = line.split()
        if len(fields) != 4:
            continue
        _, local_sha, _, remote_sha = fields
        if set(local_sha) == {'0'}:
            continue  # branch or tag deletion
        tips.append(local_sha)
        if set(remote_sha) != {'0'} and git('cat-file', '-e', f'{remote_sha}^{{commit}}', check=False).returncode == 0:
            known.append(remote_sha)
    return tips, ['--remotes', *known]


def pre_push(args, stdin):
    lines = stdin.splitlines()
    if os.environ.get(SKIP_VARIABLE) == '1':
        print(f'pre-push: {SKIP_VARIABLE}=1, skipping the gitleaks scan of this push. CI still scans every '
              'branch; a finding on this push turns contracts red for every pull request.', file=sys.stderr)
        return run_previous('pre-push', args, stdin)
    tips, exclude = outgoing(lines)
    if tips:
        count = int(git('rev-list', '--count', *tips, '--not', *exclude).stdout.strip() or 0)
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
    log_opts = ' '.join([*tips, '--not', *exclude])
    plural = '' if count == 1 else 's'
    print(f'pre-push: gitleaks {GITLEAKS_VERSION} over {count} outgoing commit{plural}', file=sys.stderr)
    result = subprocess.run([str(binary), 'git', str(top), '--config', str(config), f'--log-opts={log_opts}',
                             '--redact', '--verbose', '--no-banner', '--exit-code', '1'], stdout=sys.stderr)
    if result.returncode == 0:
        return 0
    if result.returncode == 1:
        print('pre-push: refused, gitleaks found a secret-shaped value in an outgoing commit (above).\n'
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
        return pre_push(argv[1:], sys.stdin.read())
    print('usage: hooklib.py pre-push|forward <hook> [args...]', file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
