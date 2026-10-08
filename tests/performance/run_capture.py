#!/usr/bin/env python3
"""Run the physical hotkey benchmark with an isolated, local-only resident."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def run(args):
    if os.environ.get('XDG_SESSION_TYPE') != 'x11' or 'Cinnamon' not in os.environ.get('XDG_CURRENT_DESKTOP', ''):
        raise RuntimeError('Requires the physical Cinnamon X11 session')
    if subprocess.run(['systemd-detect-virt'], capture_output=True, text=True).stdout.strip() != 'none':
        raise RuntimeError('Virtual machines cannot establish the physical budget')
    output = Path(args.output).resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    app, cli, driver = [str(Path(path).resolve()) for path in (args.app, args.cli, args.driver)]
    parameters = {'schemaVersion': 1, 'scope': 'synthetic covering windows; local-only; all selections cancelled',
        'desktop': os.environ['XDG_CURRENT_DESKTOP'], 'session': 'x11', 'kernel': os.uname().release,
        'shortcut': 'Ctrl+Shift+F12', 'warmupInvocations': 5, 'sampleCount': 30,
        'timeoutSeconds': 240, 'binarySha256': {name: hashlib.sha256(Path(path).read_bytes()).hexdigest()
            for name, path in [('app', app), ('cli', cli), ('driver', driver)]}}
    (output / 'parameters.json').write_text(json.dumps(parameters, indent=2) + '\n')
    # Keep the Unix socket path short even when the evidence directory is deep.
    with tempfile.TemporaryDirectory(prefix='putgd-capture-') as private, (output / 'resident.log').open('wb') as log:
        environment = os.environ.copy()
        for key, name in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'),
                          ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')]:
            path = Path(private) / name
            path.mkdir(mode=0o700)
            environment[key] = str(path)
        resident = None
        def request(method, params=None):
            value = json.loads(subprocess.check_output([cli, method, json.dumps(params or {})], env=environment, timeout=10))
            if not value['ok']:
                raise RuntimeError(value['error']['code'])
            return value['result']
        def start(platform):
            nonlocal resident
            environment['QT_QPA_PLATFORM'] = platform
            process = subprocess.Popen([app, '--background'], env=environment, stdout=log, stderr=log)
            resident = process
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('Resident exited before readiness')
                try:
                    if request('diagnostics')['journalReady']:
                        return process
                except (subprocess.CalledProcessError, RuntimeError):
                    pass
                time.sleep(.2)
            process.terminate()
            process.wait(timeout=20)
            raise RuntimeError('Resident readiness timeout')
        try:
            resident = start('offscreen')
            document = request('settings.developmentTemplate')['document']
            document['profiles'] = []
            document['actions'] = {action: {'destination': 'local', 'profileId': None,
                'shortcut': 'Ctrl+Shift+F12' if action == 'region' else ''} for action in document['actions']}
            document['archive']['root'] = str(Path(private) / 'archive')
            document['preferences'] = {'notifications': False, 'launchAtLogin': False}
            metadata = request('settings.get')['metadata']
            request('settings.patch', {'patch': document, 'expectedRevision': metadata['observedRevision']})
            resident.terminate()
            resident.wait(timeout=20)
            resident = start('xcb')
            time.sleep(1)
            capabilities = request('capabilities')
            if not capabilities['source']['shortcuts']['bindings'].get('region', {}).get('bound'):
                raise RuntimeError('Physical benchmark region shortcut did not bind')
            (output / 'capabilities.json').write_text(json.dumps(capabilities, indent=2) + '\n')
            with (output / 'driver.log').open('wb') as driver_log:
                completed = subprocess.run([driver, '--resident-pid', str(resident.pid), '--output', str(output / 'results.json')],
                    env=environment, stdout=driver_log, stderr=subprocess.STDOUT, timeout=240)
            outcomes = []
            for entry in request('operation.recent')['operations']:
                item = request('operation.inspect', {'id': entry['id']})
                outcomes.append({key: item.get(key) for key in ('id', 'state', 'error', 'copyStatus')})
            (output / 'operations.json').write_text(json.dumps(outcomes, indent=2) + '\n')
            return completed.returncode
        finally:
            if resident is not None and resident.poll() is None:
                resident.terminate()
                resident.wait(timeout=20)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ('app', 'cli', 'driver', 'output'):
        parser.add_argument('--' + argument, required=True)
    raise SystemExit(run(parser.parse_args()))
