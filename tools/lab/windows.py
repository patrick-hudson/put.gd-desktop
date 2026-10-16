#!/usr/bin/env python3
"""Visible Windows QEMU lab. Bases include disk, firmware and TPM state."""
import argparse
import datetime
from contextlib import contextmanager, ExitStack
import fcntl
from functools import wraps
import inspect
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import time
import uuid

from lab import LabError, QMP, cleanup_overlay, lifecycle_operation, read_json as load

ROOT = Path(__file__).resolve().parents[2] / 'work/windows'
FIRMWARE = Path('/usr/share/OVMF')


@contextmanager
def bundle_lock(*directories):
    """Serialize cooperating CLI processes without modifying sealed bundles."""
    lock_root = ROOT / '.locks'
    lock_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    with ExitStack() as stack:
        for directory in sorted({str(p.resolve()) for p in directories}):
            name = hashlib.sha256(directory.encode()).hexdigest()
            stream = stack.enter_context((lock_root / name).open('a'))
            try:
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise LabError('VM busy: another lifecycle operation is in progress')
        yield


def serialized(*names):
    def decorate(function):
        signature = inspect.signature(function)
        @wraps(function)
        @lifecycle_operation
        def locked(*args, **kwargs):
            bound = signature.bind(*args, **kwargs)
            with bundle_lock(*(bound.arguments[name] for name in names)):
                return function(*args, **kwargs)
        return locked
    return decorate


def command(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True).stdout


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def owned_pid(directory, name):
    pidfile = directory / f'{name}.pid'
    try:
        pid = int(pidfile.read_text())
    except FileNotFoundError:
        return None
    try:
        args = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
    except FileNotFoundError:
        return None
    if not args[0]:
        return None  # An exited daemon can briefly remain as a zombie.
    marker = str(directory / ('qmp.sock' if name == 'qemu' else 'tpm.sock')).encode()
    executable = Path(os.fsdecode(args[0])).name
    expected = 'qemu-system-x86_64' if name == 'qemu' else 'swtpm'
    if executable != expected or not any(marker in arg for arg in args):
        raise LabError('PID ownership mismatch; refusing to touch another process')
    return pid


def stopped(directory):
    if owned_pid(directory, 'qemu') or owned_pid(directory, 'swtpm'):
        raise LabError('VM and TPM must both be stopped')


def new_directory(path):
    path.mkdir(parents=True, exist_ok=False, mode=0o700)
    if len(str(path / 'qmp.sock').encode()) >= 104:
        raise LabError('VM path too long for a Unix socket')


@serialized('directory')
def prepare(directory, os_name):
    new_directory(directory)
    command('qemu-img', 'create', '-f', 'qcow2', directory / 'disk.qcow2', '80G')
    shutil.copyfile(FIRMWARE / 'OVMF_VARS_4M.ms.fd', directory / 'vars.fd')
    (directory / 'tpm').mkdir(mode=0o700)
    save(directory / 'state.json', {'os': os_name, 'uuid': str(uuid.uuid4()),
                                  'clean_shutdown': False})


@serialized('base', 'directory')
def clone(base, directory):
    stopped(base)
    manifest = load(base / 'manifest.json')
    print('Verifying sealed base disk, firmware and TPM hashes...', flush=True)
    for relative, expected in manifest['sha256'].items():
        if digest(base / relative) != expected:
            raise LabError('Base checksum mismatch: ' + relative)
    new_directory(directory)
    command('qemu-img', 'create', '-f', 'qcow2', '-F', 'qcow2',
            '-b', base / 'disk.qcow2', directory / 'disk.qcow2')
    shutil.copyfile(base / 'vars.fd', directory / 'vars.fd')
    shutil.copytree(base / 'tpm', directory / 'tpm')
    for path in (directory / 'tpm').rglob('*'):
        if path.is_file():
            path.chmod(0o600)
    state = load(base / 'state.json')
    state.update(base=str(base), clean_shutdown=False, run_id=str(uuid.uuid4()))
    save(directory / 'state.json', state)


@serialized('directory')
def launch(directory, display, iso=None, network=False, memory=4096):
    if (directory / 'manifest.json').exists():
        raise LabError('Sealed bases cannot boot; create an overlay instead')
    stopped(directory)
    for program in ('qemu-system-x86_64', 'qemu-img', 'swtpm', 'swtpm_ioctl'):
        if not shutil.which(program):
            raise LabError('Missing distro package executable: ' + program)
    if not os.access('/dev/kvm', os.R_OK | os.W_OK):
        raise LabError('KVM unavailable; run on the host with /dev/kvm access')
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(('127.0.0.1', 5900 + display))
    if iso and not iso.is_file():
        raise LabError('Installation ISO missing')
    state = load(directory / 'state.json')
    state['uuid']
    state['os']
    for name in ('qmp.sock', 'tpm.sock'):
        (directory / name).unlink(missing_ok=True)
    command('swtpm', 'socket', '--tpm2', '--tpmstate', f'dir={directory / "tpm"}',
            '--ctrl', f'type=unixio,path={directory / "tpm.sock"}',
            '--flags', 'not-need-init', '--pid', f'file={directory / "swtpm.pid"}', '--daemon')
    try:
        state.update(clean_shutdown=False, vnc_port=5900 + display, network=network)
        save(directory / 'state.json', state)
        args = ['qemu-system-x86_64', '-enable-kvm', '-machine', 'q35,smm=on',
                '-cpu', 'host', '-smp', '2', '-m', str(memory), '-uuid', state['uuid'],
                '-name', f'{state["os"]}-{directory.name}',
                '-global', 'driver=cfi.pflash01,property=secure,value=on',
                '-drive', f'if=pflash,format=raw,readonly=on,file={FIRMWARE / "OVMF_CODE_4M.ms.fd"}',
                '-drive', f'if=pflash,format=raw,file={directory / "vars.fd"}',
                '-drive', f'file={directory / "disk.qcow2"},format=qcow2,if=ide',
                '-chardev', f'socket,id=chrtpm,path={directory / "tpm.sock"}',
                '-tpmdev', 'emulator,id=tpm0,chardev=chrtpm', '-device', 'tpm-tis,tpmdev=tpm0',
                '-vga', 'std', '-device', 'qemu-xhci', '-device', 'usb-tablet',
                '-vnc', f'127.0.0.1:{display}', '-display', 'none',
                '-qmp', f'unix:{directory / "qmp.sock"},server=on,wait=off',
                '-no-shutdown', '-daemonize', '-pidfile', str(directory / 'qemu.pid')]
        if iso:
            args += ['-cdrom', str(iso), '-boot', 'order=dc,menu=on']
        if network:
            args += ['-netdev', 'user,id=net0', '-device', 'e1000e,netdev=net0']
        else:
            args += ['-nic', 'none']
        command(*args)
    except Exception:
        stop_tpm(directory)
        raise
    print(f'{state["os"]}: VNC 127.0.0.1:{5900 + display}; wall http://127.0.0.1:8765/')


def stop_tpm(directory):
    pid = owned_pid(directory, 'swtpm')
    if pid:
        command('swtpm_ioctl', '--unix', directory / 'tpm.sock', '-s')
        for _ in range(100):
            if not owned_pid(directory, 'swtpm'):
                return
            time.sleep(0.1)
        raise LabError('TPM did not stop; do not copy its state')


@serialized('directory')
def shutdown(directory, force=False):
    if (directory / 'manifest.json').exists():
        raise LabError('Sealed bases cannot be shut down or modified')
    clean = False
    pid = owned_pid(directory, 'qemu')
    if pid:
        if force:
            os.kill(pid, signal.SIGTERM)
        else:
            with QMP(directory / 'qmp.sock') as qmp:
                qmp.execute('system_powerdown')
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline:
                    if qmp.execute('query-status')['status'] == 'shutdown':
                        clean = True
                        break
                    time.sleep(1)
                if not clean:
                    raise LabError('Guest did not shut down; use its visible Start menu then retry')
                qmp.execute('quit')
        for _ in range(100):
            if not owned_pid(directory, 'qemu'):
                break
            time.sleep(0.1)
        if owned_pid(directory, 'qemu'):
            raise LabError('QEMU has not exited')
    stop_tpm(directory)
    state = load(directory / 'state.json')
    # Repeated shutdown must not invalidate a prior clean shutdown.
    if clean or force:
        state['clean_shutdown'] = clean
        save(directory / 'state.json', state)


@serialized('directory')
def seal(directory, notes):
    stopped(directory)
    state = load(directory / 'state.json')
    if state.get('base') or not state.get('clean_shutdown'):
        raise LabError('Seal only an installed base after a recorded clean shutdown')
    if (directory / 'manifest.json').exists():
        raise LabError('Base already sealed')
    print('Checking disk integrity and hashing the clean base...', flush=True)
    command('qemu-img', 'check', directory / 'disk.qcow2')
    paths = [directory / 'disk.qcow2', directory / 'vars.fd', directory / 'state.json']
    paths += sorted(p for p in (directory / 'tpm').rglob('*') if p.is_file())
    if not any(p.parent == directory / 'tpm' for p in paths):
        raise LabError('TPM state missing')
    save(directory / 'manifest.json', {
        'sealed_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'notes': notes, 'sha256': {str(p.relative_to(directory)): digest(p) for p in paths}})
    for path in paths:
        path.chmod(0o400)
    print('Sealed base; use clone for every test. Evaluation expiry is unchanged by cloning.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('--os', choices=['windows11', 'windows10'], required=True)
    p.add_argument('--vm', type=Path, required=True)
    p = sub.add_parser('clone')
    p.add_argument('--base', type=Path, required=True)
    p.add_argument('--vm', type=Path, required=True)
    p = sub.add_parser('start')
    p.add_argument('--vm', type=Path, required=True)
    p.add_argument('--display', type=int, required=True)
    p.add_argument('--iso', type=Path)
    p.add_argument('--network', action='store_true', help='Explicit outbound NAT; no forwarded ports')
    p.add_argument('--memory', type=int, default=4096)
    p = sub.add_parser('shutdown')
    p.add_argument('--vm', type=Path, required=True)
    p.add_argument('--force', action='store_true', help='Abrupt stop; disallows sealing')
    p = sub.add_parser('cleanup')
    p.add_argument('--vm', type=Path, required=True)
    p.add_argument('--evidence-manifest', required=True, help='Verified completion evidence; disk only, preserves witnesses and bundle metadata')
    p.add_argument('--dry-run', action='store_true')
    p = sub.add_parser('seal')
    p.add_argument('--vm', type=Path, required=True)
    p.add_argument('--notes', required=True, help='OS build, activation expiry and provisioning summary; no secrets')
    for name in ('status', 'screenshot', 'key'):
        p = sub.add_parser(name)
        p.add_argument('--vm', type=Path, required=True)
        if name == 'key':
            p.add_argument('keys', nargs='+', help='QEMU qcodes, e.g. ctrl alt delete')
    args = parser.parse_args()
    directory = args.vm.resolve()
    if not directory.is_relative_to(ROOT.resolve()):
        raise LabError('VM directories must be under work/windows')
    if args.action == 'prepare':
        prepare(directory, args.os)
    elif args.action == 'clone':
        base = args.base.resolve()
        if not base.is_relative_to(ROOT.resolve()):
            raise LabError('Base must be under work/windows')
        clone(base, directory)
    elif args.action == 'start':
        if not 0 <= args.display <= 99 or args.memory < 4096:
            raise LabError('Display must be 0..99 and memory at least 4096 MiB')
        launch(directory, args.display, args.iso.resolve() if args.iso else None, args.network, args.memory)
    elif args.action == 'shutdown':
        shutdown(directory, args.force)
    elif args.action == 'cleanup':
        print(json.dumps(cleanup_overlay(args.vm, evidence_manifest=args.evidence_manifest, dry_run=args.dry_run)))
    elif args.action == 'seal':
        seal(directory, args.notes)
    else:
        if not owned_pid(directory, 'qemu'):
            raise LabError('Owned QEMU is not running')
        with QMP(directory / 'qmp.sock') as qmp:
            if args.action == 'status':
                print(json.dumps(qmp.execute('query-status')))
            elif args.action == 'screenshot':
                path = directory / 'witness.ppm'
                qmp.execute('screendump', {'filename': str(path)})
                print(path)
            else:
                qmp.execute('send-key', {'keys': [{'type': 'qcode', 'data': k} for k in args.keys]})


if __name__ == '__main__':
    try:
        main()
    except (LabError, OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
