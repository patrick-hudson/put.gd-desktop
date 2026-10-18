#!/usr/bin/env python3
"""Record the registered CTest suite and preflight every socket path it can create.

Writes registered-tests.json, socket-preflight.json and ci-test-context.json into
the build directory before the unprivileged test user runs CTest.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build/dev'))
    parser.add_argument('--tmp', type=Path, required=True, help='Private TMPDIR the tests will use')
    parser.add_argument('--user', required=True, help='Unprivileged account that runs CTest')
    args = parser.parse_args()
    root, temporary = args.build, args.tmp.resolve()
    listing = subprocess.check_output(['ctest', '--test-dir', str(root), '--show-only=json-v1'], text=True)
    (root / 'registered-tests.json').write_text(listing)
    registry = json.loads(listing)
    names = {'test_' + test['name'] for test in registry['tests']}
    names.update(Path(argument).name for test in registry['tests'] for argument in test.get('command', [])
                 if Path(argument).name.startswith('test_'))
    paths = [str(temporary / (name + '-XXXXXX') / 'runtime/putgd/session.sock') for name in sorted(names)]
    paths += [str(temporary / 'putgd mcp socket XXXXXXXX/session socket'),
              '/tmp/.X11-unix/X65535', '/tmp/dbus-XXXXXXXXXX']
    with (root / 'socket-preflight.json').open('w') as output:
        subprocess.run(['python3', 'tools/diagnostics/check_socket_paths.py', '--socket', *paths],
                       stdout=output, check=True)
    uid = int(subprocess.check_output(['runuser', '-u', args.user, '--', 'id', '-u']))
    (root / 'ci-test-context.json').write_text(json.dumps({
        'configuredUser': args.user, 'configuredUid': uid, 'observedUidFile': 'ctest-observed-uid',
        'locale': os.environ.get('LC_ALL', ''), 'temporaryDirectory': str(temporary),
        'registeredTests': len(registry['tests']),
        'socketPreflight': 'Conservative length-preserving normal socket templates; intentional rejection cases unchanged',
        'scope': 'Hosted contract tests; no native desktop or physical memory acceptance'
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
