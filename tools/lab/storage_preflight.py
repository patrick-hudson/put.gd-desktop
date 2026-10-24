#!/usr/bin/env python3
"""Read-only host/guest capacity gate for existing Linux lab runs (Python + GNU stat)."""
import argparse
import datetime
import inspect
import json
import math
import os
from pathlib import Path
import stat
import subprocess


def measure_filesystem(path):
    # Pin the measured object: statfs identification and fstatvfs must describe
    # the same filesystem, even for symlinks or a concurrent mount change.
    path = os.path.realpath(path, strict=True)
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
    try:
        raw = os.fstatvfs(descriptor)
        kind, magic, error = None, None, None
        try:
            identity = subprocess.run(
                ['stat', '-f', '--format=%T:%t', '--', '/proc/self/fd/%d' % descriptor],
                pass_fds=(descriptor,), text=True, capture_output=True, timeout=5,
                env={**os.environ, 'LC_ALL': 'C'})
            if identity.returncode:
                error = 'CommandFailed'
            elif identity.stdout.strip().count(':') != 1:
                error = 'InvalidResponse'
            else:
                kind, magic = identity.stdout.strip().split(':')
        except (OSError, subprocess.TimeoutExpired) as exc:
            # Never persist command arguments, stdout/stderr or exception text.
            # Retain the already measured raw capacity with unknown accounting.
            error = type(exc).__name__
        return {'path': path, 'device': os.fstat(descriptor).st_dev,
                'filesystemType': kind, 'filesystemMagic': magic,
                'identificationErrorType': error,
                'statvfs': {name: getattr(raw, name) for name in dir(raw) if name.startswith('f_')}}
    finally:
        os.close(descriptor)


def classify_filesystem(value):
    if not isinstance(value, dict) or set(value) != {
            'path', 'device', 'filesystemType', 'filesystemMagic', 'identificationErrorType', 'statvfs'}:
        raise ValueError('Unexpected filesystem metrics')
    if (not isinstance(value['path'], str) or not value['path'].startswith('/') or
            type(value['device']) is not int or value['device'] < 0):
        raise ValueError('Invalid measured path or device')
    raw = value['statvfs']
    required = {'f_bsize', 'f_frsize', 'f_blocks', 'f_bfree', 'f_bavail',
                'f_files', 'f_ffree', 'f_favail'}
    if (not isinstance(raw, dict) or not required <= raw.keys() or
            any(type(metric) is not int or metric < 0 for metric in raw.values())):
        raise ValueError('Filesystem metrics must be non-negative integers')
    if (raw['f_bsize'] == 0 or raw['f_frsize'] == 0 or
            not raw['f_bavail'] <= raw['f_bfree'] <= raw['f_blocks'] or
            not raw['f_favail'] <= raw['f_ffree'] <= raw['f_files']):
        raise ValueError('Inconsistent filesystem metrics')
    identity = (value['filesystemType'], value['filesystemMagic'])
    error = value['identificationErrorType']
    if error is not None and (not isinstance(error, str) or len(error) > 64 or
                              not error.isascii() or not error.isidentifier() or identity != (None, None)):
        raise ValueError('Invalid filesystem identification error')
    mode = 'unknown'
    # Linux magic.h and GNU stat's human_fstype. Only Btrfs has a verified
    # unreported, dynamic inode pool here; other/unidentified zeroes fail closed.
    # https://btrfs.readthedocs.io/en/stable/Administration.html
    # Linux fs/statfs.c zeroes kstatfs; fs/btrfs/super.c leaves inode counts
    # unset and makes f_bavail zero when metadata space is exhausted.
    if identity == ('btrfs', '9123683e'):
        if any(raw[name] != 0 for name in ('f_files', 'f_ffree', 'f_favail')):
            raise ValueError('Unexpected Btrfs inode accounting')
        mode = 'dynamic'
    elif identity in (('ext2/ext3', 'ef53'), ('tmpfs', '1021994'), ('xfs', '58465342')):
        mode = 'reported'
    elif not all(item is None or isinstance(item, str) for item in identity):
        raise ValueError('Invalid filesystem identification')
    return {**value, 'availableBytes': raw['f_bavail'] * raw['f_frsize'],
            'totalBytes': raw['f_blocks'] * raw['f_frsize'],
            'availableInodes': raw['f_favail'] if mode == 'reported' else None,
            'inodeAccounting': mode,
            'inodeCheckPassed': mode == 'dynamic' or (mode == 'reported' and raw['f_favail'] > 1024)}


def measure_guest(run):
    info = json.loads((run / 'state.json').read_text())
    key = run / 'identity'
    metadata = key.lstat()
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.getuid() or metadata.st_mode & 0o077:
        raise ValueError('Lab identity must be an owned private regular file')
    arguments = ['ssh', '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes',
                 '-o', 'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=5',
                 '-o', 'UserKnownHostsFile=' + info['known_hosts'],
                 '-i', str(key), '-p', str(info['ssh_port']),
                 info['guest_user'] + '@127.0.0.1', 'python3', '-']
    script = 'import json,os,subprocess\n' + inspect.getsource(measure_filesystem) + '''
paths={"home":os.path.expanduser("~"),"tmp":"/tmp"}
print(json.dumps({label:measure_filesystem(path) for label,path in paths.items()}))
'''
    result = subprocess.run(arguments, input=script, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise ValueError('Guest capacity query failed (exit %d); raw SSH output withheld' % result.returncode)
    filesystems = json.loads(result.stdout)
    if not isinstance(filesystems, dict) or set(filesystems) != {'home', 'tmp'}:
        raise ValueError('Unexpected guest capacity response')
    return {label: classify_filesystem(value) for label, value in filesystems.items()}


def capacity_passes(filesystems, reserve_gib, build_gib):
    return all(value['availableBytes'] >=
               (reserve_gib + (build_gib if label == 'home' else 0)) * 2**30 and
               value['inodeCheckPassed'] for label, value in filesystems.items())


def host_capacity_passes(filesystem, reserve_gib, build_gib, guest_count):
    return (filesystem['availableBytes'] >= (reserve_gib + build_gib * guest_count) * 2**30
            and filesystem['inodeCheckPassed'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, action='append', required=True)
    parser.add_argument('--host-path', type=Path, default=Path.cwd())
    parser.add_argument('--host-reserve-gib', type=float, default=10)
    parser.add_argument('--guest-reserve-gib', type=float, default=2)
    parser.add_argument('--expected-build-gib', type=float, default=0,
                        help='Additional estimated space required by the next guest build')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    budgets = (args.host_reserve_gib, args.guest_reserve_gib, args.expected_build_gib)
    if any(not math.isfinite(value) or value < 0 for value in budgets):
        parser.error('Space budgets must be finite and non-negative')
    if args.host_reserve_gib < 10 or args.guest_reserve_gib < 2:
        parser.error('Minimum reserves are 10 GiB host and 2 GiB guest')
    host, host_error = None, None
    try:
        host = classify_filesystem(measure_filesystem(args.host_path))
        host_error = host['identificationErrorType']
    except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
        host_error = type(exc).__name__
    host_device = host['device'] if host else None
    host_passed = host is not None and host_capacity_passes(
        host, args.host_reserve_gib, args.expected_build_gib, len(args.run))
    rows = []
    for run in args.run:
        row = {'run': str(run.resolve())}
        try:
            if host_device is None:
                raise ValueError('Host filesystem measurement unavailable')
            disk = run / 'disk.qcow2'
            if (disk if disk.exists() else run).stat().st_dev != host_device:
                raise ValueError('Run each host filesystem separately with --host-path on that filesystem')
            row['filesystems'] = measure_guest(run.resolve())
            row['passed'] = capacity_passes(row['filesystems'], args.guest_reserve_gib,
                                            args.expected_build_gib)
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            row.update(passed=False, errorType=type(exc).__name__)
        rows.append(row)
    receipt = {'schemaVersion': 2, 'timestampUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'hostAvailableBytes': host['availableBytes'] if host else None, 'hostPassed': host_passed,
               'hostAvailableInodes': host['availableInodes'] if host else None, 'hostFilesystem': host,
               'hostErrorType': host_error,
               'hostExpectedGrowthBytes': args.expected_build_gib * len(args.run) * 2**30,
               'hostReserveGiB': args.host_reserve_gib, 'guestReserveGiB': args.guest_reserve_gib,
               'expectedBuildGiB': args.expected_build_gib, 'guests': rows,
               'passed': host_passed and all(row['passed'] for row in rows), 'readOnly': True}
    os.umask(0o077)
    with args.output.open('x') as output:
        output.write(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt))
    return 0 if receipt['passed'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
