#!/usr/bin/env python3
"""Initialization locator only. Does not establish product or tray memory savings."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
EXPECTED_HARNESS = {
    'bench_ui_process.py': 'a28c94b075036ea65f8eca8e068d480cff46ee959d4680593f5c4d55cce3fc40',
    'bench_idle.py': '47e65e026ebd3a2a596224036ef594cf30770a49f4ff1dbc462dbf0f6c95e652',
}


def load_helpers(name):
    path = REPO / 'tests/performance' / name
    if hashlib.sha256(path.read_bytes()).hexdigest() != EXPECTED_HARNESS[name]:
        raise RuntimeError('observer_hash_mismatch')
    spec = importlib.util.spec_from_file_location(name.replace('.', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return path, module


def run(args):
    ui_path, ui = load_helpers('bench_ui_process.py')
    idle_path, idle = load_helpers('bench_idle.py')
    for path in (ui_path, idle_path):
        if ui.sha256(path) != EXPECTED_HARNESS[path.name]:
            raise RuntimeError('observer_hash_mismatch')
    if os.environ.get('XDG_SESSION_TYPE') not in ('x11', 'wayland'):
        raise RuntimeError('native_graphical_session_required')
    if not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        raise RuntimeError('native_display_required')
    qpa = os.environ.get('QT_QPA_PLATFORM', '')
    if qpa and qpa.split(':', 1)[0] not in ('xcb', 'wayland', 'wayland-egl'):
        raise RuntimeError('non_native_platform_rejected')
    output = Path(args.output).resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    cache = (HERE / 'build/CMakeCache.txt').read_text().splitlines()
    compiler_value = next(line.split('=', 1)[1] for line in cache
                          if line.startswith('CMAKE_CXX_COMPILER:FILEPATH='))
    toolchain = {}
    for label, command in [('compiler', compiler_value), ('cmake', 'cmake'), ('ninja', 'ninja')]:
        executable = Path(shutil.which(command) or command).resolve(strict=True)
        completed = subprocess.run([str(executable), '--version'], capture_output=True,
                                   text=True, timeout=10, check=True)
        toolchain[label] = {'path': str(executable), 'sha256': ui.sha256(executable),
                            'versionCommand': [str(executable), '--version'],
                            'version': completed.stdout.splitlines()[:3]}
    receipt = {
        'schemaVersion': 1,
        'purpose': 'Native initialization cost locator; no product, tray, capture or IPC parity claim',
        'startedUtc': datetime.now(timezone.utc).isoformat(), 'status': 'running',
        'command': ['python3', str(HERE / 'run.py'), '--output', str(output),
                    '--idle-seconds', str(args.idle_seconds)] + (['--session-bus'] if args.session_bus else []),
        'idleSeconds': args.idle_seconds, 'sessionBusRequested': args.session_bus,
        'sampleStages': ['constructed'] + (['session-bus-connected'] if args.session_bus else []) + ['settled'],
        'sourceHashes': {p.name: ui.sha256(p) for p in
                        [HERE / n for n in ['CMakeLists.txt', 'CMakePresets.json', 'locator.cpp', 'run.py']]},
        'observerHashes': EXPECTED_HARNESS, 'toolchain': toolchain,
        'cmakeCacheSha256': ui.sha256(HERE / 'build/CMakeCache.txt'),
        'environmentUnmodified': True,
        'nativeEnvironment': {k: os.environ.get(k, '') for k in
                              ['XDG_CURRENT_DESKTOP', 'XDG_SESSION_TYPE', 'QT_QPA_PLATFORM',
                               'QT_QPA_PLATFORMTHEME', 'QT_STYLE_OVERRIDE']},
        'order': ['core', 'gui', 'widgets', 'widgets', 'gui', 'core'], 'runs': [],
        'limitations': [
            'All three binaries link Qt DBus, including constructor-only observations.',
            'Gui/Widgets constructors may themselves use the session bus through native integration.',
            'The optional explicit session-bus connection is scheduled one second after construction.',
            'No tray/menu, clipboard, shortcuts, capture, services or product IPC is implemented.',
            'Five-second observations are locating evidence, not 300-second cold budgets or savings.',
            'Memory snapshots occur after the stdout phase marker; they do not stop the event loop.',
            'Dependency hashes are collected after all observations, outside sampled intervals.',
        ],
    }
    ui.write_json(output / 'receipt.json', receipt)
    dependencies = set()
    for index, kind in enumerate(receipt['order'], 1):
        label = f'{index:02d}-{kind}'
        binary = (HERE / 'build' / f'init-{kind}').resolve(strict=True)
        argv = [str(binary), '--idle-seconds', str(args.idle_seconds)]
        if args.session_bus:
            argv.append('--session-bus')
        row = {'label': label, 'argv': argv, 'binarySha256': ui.sha256(binary), 'samples': []}
        receipt['runs'].append(row)
        process = None
        try:
            with (output / f'{label}.stdout.jsonl').open('x') as raw, \
                    (output / f'{label}.stderr').open('xb') as errors:
                process = subprocess.Popen(argv, cwd=HERE, stdout=subprocess.PIPE,
                                           stderr=errors, text=True)

                def marker(expected):
                    ready, _, _ = select.select([process.stdout], [], [], 4)
                    if not ready:
                        raise RuntimeError('phase_marker_timeout')
                    line = process.stdout.readline()
                    raw.write(line)
                    raw.flush()
                    value = json.loads(line)
                    if value['stage'] != expected:
                        raise RuntimeError('phase_marker_mismatch')
                    return value

                constructed = marker('constructed')
                expected_type = {'core': 'QCoreApplication', 'gui': 'QGuiApplication',
                                 'widgets': 'QApplication'}[kind]
                if constructed['applicationType'] != expected_type:
                    raise RuntimeError('probe_application_type_mismatch')
                observed_start = time.monotonic()
                root_identity = ui.identity(process.pid)
                if Path(root_identity['executable']) != binary:
                    raise RuntimeError('probe_executable_mismatch')
                row['identity'] = root_identity
                row['constructorMarker'] = constructed

                def sample(stage):
                    if process.poll() is not None or not ui.same_identity(root_identity, ui.identity(process.pid)):
                        raise RuntimeError('probe_identity_changed_or_exited')
                    if ui.children(process.pid):
                        raise RuntimeError('unexpected_descendant')
                    row['samples'].append({'stage': stage,
                                           'elapsedAfterConstructorMarkerSeconds': time.monotonic() - observed_start,
                                           'memory': ui.memory(process.pid)})

                sample('constructed')
                if args.session_bus:
                    row['busMarker'] = marker('session-bus-connected')
                    sample('session-bus-connected')
                remaining = args.idle_seconds - 0.5 - (time.monotonic() - observed_start)
                if remaining <= 0:
                    raise RuntimeError('settled_observation_window_missed')
                time.sleep(remaining)
                sample('settled')
                row['groupedFootprint'] = idle.footprint(process.pid)
                for line in Path(f'/proc/{process.pid}/maps').read_text().splitlines():
                    parts = line.split(maxsplit=5)
                    if len(parts) == 6 and parts[5].startswith('/') and '.so' in parts[5]:
                        dependencies.add(parts[5])
                row['finalMarker'] = marker('idle-finished')
                row['exitCode'] = process.wait(timeout=5)
                if row['exitCode']:
                    raise RuntimeError('probe_nonzero_exit')
                if ui.sha256(binary) != row['binarySha256']:
                    raise RuntimeError('probe_binary_changed')
                row['status'] = 'complete'
        except Exception as error:
            row['status'] = 'failed-preserved'
            row['failureType'] = type(error).__name__
            receipt['status'] = 'failed-preserved-no-automatic-retry'
            raise
        finally:
            if process is not None:
                if process.poll() is None:
                    # Failure cleanup of this exact child only; not lifecycle proof.
                    process.terminate()
                    process.wait(timeout=5)
                process.stdout.close()
                row['exitCode'] = process.returncode
            ui.write_json(output / 'receipt.json', receipt)
    receipt['dependencyHashes'] = {p: ui.sha256(Path(p)) for p in sorted(dependencies)}
    receipt['system'] = ui.system_metadata()
    receipt['status'] = 'complete'
    receipt['completedUtc'] = datetime.now(timezone.utc).isoformat()
    ui.write_json(output / 'receipt.json', receipt)
    print(json.dumps({'status': 'complete', 'receipt': str(output / 'receipt.json')}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--idle-seconds', type=int, default=5)
    parser.add_argument('--session-bus', action='store_true')
    arguments = parser.parse_args()
    if not 2 <= arguments.idle_seconds <= 60:
        parser.error('--idle-seconds must be from 2 to 60')
    run(arguments)
