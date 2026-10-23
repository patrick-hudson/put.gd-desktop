#!/usr/bin/env python3
"""Select CI jobs from a pull request's changed paths.

The contracts job always runs. `build` selects the Linux test matrix and the
portable compiles; `packages` selects the native/AppImage package builds;
`sanitizers` selects the clang ASan/UBSan core contract job; `pipewire` selects
the AppImage pipewiresrc check against a PipeWire 1.6 daemon.
Any path not explicitly known to be build-neutral selects `build`, so a new
directory is tested by default. Push, schedule, dispatch and called runs
select everything.
"""
import argparse
import json
import os
from pathlib import PurePosixPath
import subprocess

# Prose read by people and agents only. Packaged or exported docs are listed in
# PACKAGING_FILES below because they reach the curated source archive.
DOCUMENTATION_PREFIXES = ('docs/',)
DOCUMENTATION_FILES = frozenset({'README.md', 'AGENTS.md', 'CLAUDE.md', 'CHANGELOG.md'})

# Python and lab inputs that no compiled target, CTest registration or package
# reads. The contracts job runs their suites on every pull request.
CONTRACT_PREFIXES = ('tools/lab/', 'tools/maintenance/', 'tools/mcp/',
                     'tests/ci/', 'tests/releases/', 'tests/maintenance/')
CONTRACT_SUFFIXES = {'tests/desktop': ('.py', '.yaml'), 'tests/performance': ('.py',)}
# The contracts job's secret scan reads its allowlist from here alone.
CONTRACT_FILES = frozenset({'.gitleaks.toml'})

# Inputs to the package builders, the curated source archive or CI itself.
PACKAGING_PREFIXES = ('packaging/', 'tools/releases/', 'tools/proton/', 'tools/ci/',
                      'cmake/', 'integrations/', '.github/workflows/')
PACKAGING_FILES = frozenset({'CMakeLists.txt', 'CMakePresets.json', 'LICENSE',
                             'THIRD_PARTY_NOTICES.md', '.gitignore',
                             'tools/verify_dependencies.py',
                             'docs/build.md', 'docs/proton-drive.md'})


# The core and application libraries, their C++ tests and the build definitions
# that compile them. The clang ASan/UBSan job runs only for these.
SANITIZER_PREFIXES = ('src/core/', 'src/app/', 'tests/unit/', 'tests/integration/',
                      'tests/fixtures/', 'cmake/', 'tools/ci/')
SANITIZER_FILES = frozenset({'CMakeLists.txt', 'CMakePresets.json', '.github/workflows/ci.yml'})

# The pipewire job links the AppImage's pipewiresrc, with the recorder's
# properties from src/record, to compositor-like screen casts (#144). Any
# packaging change selects it too, with that run's own AppImage. Its scripts
# are read by that job alone, so they don't select `build`.
PIPEWIRE_CHECK_PREFIXES = ('tests/desktop/pipewire/',)
PIPEWIRE_PREFIXES = PIPEWIRE_CHECK_PREFIXES + ('src/record/',)


def _documentation(path):
    return path in DOCUMENTATION_FILES or path.startswith(DOCUMENTATION_PREFIXES)


def _contract_only(path):
    if path in CONTRACT_FILES or path.startswith(CONTRACT_PREFIXES):
        return True
    parent = str(PurePosixPath(path).parent)
    return path.endswith(CONTRACT_SUFFIXES.get(parent, ()))


def _packaging(path):
    return path in PACKAGING_FILES or path.startswith(PACKAGING_PREFIXES)


def _sanitized(path):
    if path in SANITIZER_FILES:
        return True
    # Python suites under tests/unit run in the contracts job, not CTest.
    return path.startswith(SANITIZER_PREFIXES) and not (path.startswith('tests/') and path.endswith('.py'))


def select(paths):
    paths = sorted(set(paths))
    packages = any(_packaging(path) for path in paths)
    build = packages or any(not _documentation(path) and not _contract_only(path)
                            and not path.startswith(PIPEWIRE_CHECK_PREFIXES) for path in paths)
    sanitizers = any(_sanitized(path) for path in paths)
    pipewire = packages or any(path.startswith(PIPEWIRE_PREFIXES) for path in paths)
    return {'build': build, 'packages': packages, 'sanitizers': sanitizers, 'pipewire': pipewire,
            'paths': paths}


def changed_paths(root, base, head):
    output = subprocess.check_output(['git', '-C', str(root), 'diff', '--name-only', '--no-renames',
                                      base, head], text=True)
    return [line for line in output.splitlines() if line]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default='.')
    parser.add_argument('--event', default=os.environ.get('GITHUB_EVENT_NAME', ''))
    parser.add_argument('--base', default='HEAD^1', help='Pull request base (the merge commit\'s first parent)')
    parser.add_argument('--head', default='HEAD')
    args = parser.parse_args()
    if args.event == 'pull_request':
        result = select(changed_paths(args.root, args.base, args.head))
    else:
        result = {'build': True, 'packages': True, 'sanitizers': True, 'pipewire': True, 'paths': None}
    result['event'] = args.event
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
            stream.write(f"build={str(result['build']).lower()}\n")
            stream.write(f"packages={str(result['packages']).lower()}\n")
            stream.write(f"sanitizers={str(result['sanitizers']).lower()}\n")
            stream.write(f"pipewire={str(result['pipewire']).lower()}\n")
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as stream:
            stream.write('| Job group | Selected |\n| --- | --- |\n| contracts | yes |\n')
            stream.write(f"| linux, portable | {'yes' if result['build'] else 'no'} |\n")
            stream.write(f"| packages | {'yes' if result['packages'] else 'no'} |\n")
            stream.write(f"| sanitizers | {'yes' if result['sanitizers'] else 'no'} |\n")
            stream.write(f"| pipewire | {'yes' if result['pipewire'] else 'no'} |\n")
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
