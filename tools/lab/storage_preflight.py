#!/usr/bin/env python3
"""Read-only host/guest capacity gate for existing Linux lab runs (stdlib only)."""
import argparse
import datetime
import json
import math
import os
from pathlib import Path
import shutil
import stat
import subprocess


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
    script = '''import json,os
paths={"home":os.path.expanduser("~"),"tmp":"/tmp"}
result={}
for label,path in paths.items():
 s=os.statvfs(path)
 result[label]={"availableBytes":s.f_bavail*s.f_frsize,"totalBytes":s.f_blocks*s.f_frsize,"availableInodes":s.f_favail}
print(json.dumps(result))
'''
    result = subprocess.run(arguments, input=script, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise ValueError('Guest capacity query failed (exit %d); raw SSH output withheld' % result.returncode)
    filesystems = json.loads(result.stdout)
    if not isinstance(filesystems, dict) or set(filesystems) != {'home', 'tmp'}:
        raise ValueError('Unexpected guest capacity response')
    for value in filesystems.values():
        if not isinstance(value, dict) or set(value) != {'availableBytes', 'totalBytes', 'availableInodes'}:
            raise ValueError('Unexpected guest filesystem metrics')
        if any(type(metric) is not int or metric < 0 for metric in value.values()):
            raise ValueError('Guest filesystem metrics must be non-negative integers')
    return filesystems


def capacity_passes(filesystems, reserve_gib, build_gib):
    return all(value['availableBytes'] >=
               (reserve_gib + (build_gib if label == 'home' else 0)) * 2**30 and
               value['availableInodes'] > 1024 for label, value in filesystems.items())


def host_capacity_passes(free_bytes, free_inodes, reserve_gib, build_gib, guest_count):
    return free_bytes >= (reserve_gib + build_gib * guest_count) * 2**30 and free_inodes > 1024


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
    host_free = shutil.disk_usage(args.host_path).free
    host_inodes = os.statvfs(args.host_path).f_favail
    host_device = args.host_path.stat().st_dev
    host_passed = host_capacity_passes(host_free, host_inodes, args.host_reserve_gib,
                                       args.expected_build_gib, len(args.run))
    rows = []
    for run in args.run:
        row = {'run': str(run.resolve())}
        try:
            disk = run / 'disk.qcow2'
            if (disk if disk.exists() else run).stat().st_dev != host_device:
                raise ValueError('Run each host filesystem separately with --host-path on that filesystem')
            row['filesystems'] = measure_guest(run.resolve())
            row['passed'] = capacity_passes(row['filesystems'], args.guest_reserve_gib,
                                            args.expected_build_gib)
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            row.update(passed=False, errorType=type(exc).__name__)
        rows.append(row)
    receipt = {'schemaVersion': 1, 'timestampUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'hostAvailableBytes': host_free, 'hostPassed': host_passed,
               'hostAvailableInodes': host_inodes,
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
