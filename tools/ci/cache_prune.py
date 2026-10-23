#!/usr/bin/env python3
"""Delete GitHub Actions caches that no run can restore any more.

Rolling caches (`ccache-linux-24.04-<run>-<attempt>`, the CTest durations, the
instrumented QtCore and the weekly distro package snapshots) are restored by key prefix, and a prefix restore takes
the newest entry on the run's ref (or on nightly). The Grype database archive is
replaced by each new database. Every older entry of the same
family on the same ref is therefore never read again. Caches saved by closed
pull requests and deleted branches are unreachable too. On main, nightly and
v* tags only superseded family entries are ever deleted. Exact-key caches
(SDK prefixes, the Qt installer's) are left for GitHub's seven-day idle
eviction. See docs/ci.md, "Cache budget".
"""
import argparse
import json
import re
import subprocess
import sys

TRUNK = 'nightly'
# Caches on these refs are deleted only when a newer entry of the same family exists there.
PROTECTED = re.compile(r'^refs/heads/(?:main|nightly)$|^refs/tags/v')
# Group 1 is the family; entries of one family on one ref supersede each other.
FAMILIES = (
    re.compile(r'^((?:ccache|ctest-cost)-.+-)\d+-\d+$'),
    re.compile(r'^(qt-asan-core-[0-9.]+-)'),
    re.compile(r'^(putgd-pkgs-v\d+-.+-[0-9a-f]{64}-)\d{4}-W\d{2}$'),
    # The Grype database archive by its SHA-256 (tools/releases/sbom.py, #420).
    re.compile(r'^(grype-db-v\d+-)[0-9a-f]{64}$'),
)


def family(key):
    for pattern in FAMILIES:
        match = pattern.match(key)
        if match:
            return match[1]
    return None


def plan(caches, open_pulls, branches):
    """Return (cache, reason) for every cache to delete, newest first."""
    if TRUNK not in branches:
        # An empty or truncated branch listing must not read as "every branch was deleted".
        raise ValueError('branch listing does not include ' + TRUNK)
    doomed = []
    newest = {}
    for cache in sorted(caches, key=lambda c: (c['created_at'], c['id']), reverse=True):
        ref = cache['ref']
        pull = re.fullmatch(r'refs/pull/(\d+)/merge', ref)
        if not PROTECTED.match(ref):
            if pull and int(pull[1]) not in open_pulls:
                doomed.append((cache, 'closed pull request'))
                continue
            if ref.startswith('refs/heads/') and ref[len('refs/heads/'):] not in branches:
                doomed.append((cache, 'deleted branch'))
                continue
        name = family(cache['key'])
        if name is None:
            continue
        kept = newest.setdefault((name, ref), cache['key'])
        if kept != cache['key']:
            doomed.append((cache, 'superseded by ' + kept))
    return doomed


def gh_lines(*args):
    result = subprocess.run(['gh', 'api', '--paginate', *args], check=True, text=True,
                            stdout=subprocess.PIPE)
    return [line for line in result.stdout.splitlines() if line]


def delete(repo, cache):
    """Delete one cache by id; one GitHub already evicted counts as deleted."""
    result = subprocess.run(['gh', 'api', '--method', 'DELETE', f"repos/{repo}/actions/caches/{cache['id']}"],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode and 'HTTP 404' not in result.stderr:
        sys.stderr.write(result.stderr)
        raise subprocess.CalledProcessError(result.returncode, result.args)


def mb(size):
    return f'{size / 1e6:,.0f} MB'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    parser.add_argument('--repo', required=True, help='owner/name')
    parser.add_argument('--dry-run', action='store_true', help='list the deletions without deleting')
    args = parser.parse_args()

    caches = [json.loads(line) for line in
              gh_lines(f'repos/{args.repo}/actions/caches?per_page=100', '--jq', '.actions_caches[] | @json')]
    open_pulls = {int(n) for n in gh_lines(f'repos/{args.repo}/pulls?state=open&per_page=100', '--jq', '.[].number')}
    branches = set(gh_lines(f'repos/{args.repo}/branches?per_page=100', '--jq', '.[].name'))
    doomed = plan(caches, open_pulls, branches)

    total = sum(c['size_in_bytes'] for c in caches)
    freed = sum(c['size_in_bytes'] for c, _ in doomed)
    for cache, reason in doomed:
        print(f"{'would delete' if args.dry_run else 'delete'} {cache['key']} on {cache['ref']}, "
              f"{mb(cache['size_in_bytes'])}: {reason}")
        if not args.dry_run:
            delete(args.repo, cache)
    print(f'{len(caches)} caches, {mb(total)}; {len(doomed)} deleted, {mb(freed)}; '
          f'{len(caches) - len(doomed)} left, {mb(total - freed)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
