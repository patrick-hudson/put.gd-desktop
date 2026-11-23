#!/usr/bin/env python3
"""Enable the repository's git hooks in this worktree and fetch the pinned gitleaks (#1959).

Run from the top of a checkout: python3 tools/git-hooks/install.py
--fetch-only installs the scanner into the user tool cache without touching git config.
"""
import argparse
import sys

import hooklib


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--fetch-only', action='store_true', help='only install the pinned gitleaks')
    options = parser.parse_args(argv)
    hooklib.fetch()
    if options.fetch_only:
        return 0
    top = hooklib.git('rev-parse', '--show-toplevel').stdout.strip()
    # With extensions.worktreeConfig the setting stays in this worktree; without it, it is the
    # repository's and applies to every worktree of it.
    shared = hooklib.git('config', '--bool', '--get', 'extensions.worktreeConfig', check=False).stdout.strip()
    scope = '--worktree' if shared == 'true' else '--local'
    # The copy outside the tree keeps the hooks running on branches that predate tools/git-hooks;
    # where the checkout has them, the copy hands over to the checkout's own files.
    target = hooklib.install_copy()
    hooklib.git('-C', top, 'config', scope, 'core.hooksPath', str(target))
    previous = hooklib.previous_hooks_dir()
    print(f'core.hooksPath={target} set ({scope[2:]}) in {top}; '
          f'other hooks still forward to {previous}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
