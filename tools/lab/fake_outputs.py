#!/usr/bin/env python3
"""Extra monitors on a physical builder without hardware: force spare DRM connectors on with an EDID (stdlib only, Linux).

The lab's multi-monitor checks (selector surfaces per output, recording one
output of several) need more outputs than the builders have monitors. The DRM
core lets root force a disconnected connector on and feed it an EDID from the
firmware search path, at runtime and without a reboot: the compositor sees a
hotplug and lays the new output out like a real one. This uses the booted
monitor's own EDID by default, so the extra outputs match it.

  fake_outputs.py add CONNECTOR [CONNECTOR ...] [--edid FILE] [--card card1]
      Copies the EDID (default: the first connected connector's) under
      ~/putgd-lab/firmware/edid/, points firmware_class.path at it, sets
      drm.edid_firmware for the connectors and forces their status on. Prints
      each connector's status and modes afterwards.
  fake_outputs.py remove CONNECTOR [CONNECTOR ...] [--card card1]
      Sets the connectors back to detect and clears the overrides.
  fake_outputs.py status [--card card1]
      Connector status, enabled flag and first mode, plus the current overrides.

Needs passwordless sudo for the sysfs writes (the builders grant it). Run it
while holding the desktop's lock: outputs appear and vanish under the user's
session. Remove the outputs before releasing the lock.
"""
import argparse
import glob
import os
from pathlib import Path
import subprocess
import sys
import time

FIRMWARE = Path.home() / 'putgd-lab/firmware'
EDID_PARAM = '/sys/module/drm/parameters/edid_firmware'
PATH_PARAM = '/sys/module/firmware_class/parameters/path'


def sudo_write(path, value):
    subprocess.run(['sudo', '-n', 'sh', '-c', 'printf %s "$1" > "$2"', 'sh', value, path], check=True)


def connectors(card):
    return sorted(os.path.basename(p)[len(card) + 1:] for p in glob.glob('/sys/class/drm/%s-*' % card) if not p.endswith('Writeback-1'))


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return ''


def status(args):
    for name in connectors(args.card):
        base = '/sys/class/drm/%s-%s' % (args.card, name)
        modes = read(base + '/modes').split('\n')
        print('%-10s %-13s %-9s %s' % (name, read(base + '/status'), read(base + '/enabled'), modes[0] if modes and modes[0] else '-'))
    print('edid_firmware=[%s] firmware path=[%s]' % (read(EDID_PARAM), read(PATH_PARAM)))
    return 0


def add(args):
    edid = Path(args.edid) if args.edid else None
    if edid is None:
        for name in connectors(args.card):
            base = '/sys/class/drm/%s-%s' % (args.card, name)
            if read(base + '/status') == 'connected' and os.path.getsize(base + '/edid') >= 128:
                edid = Path(base + '/edid')
                break
        if edid is None:
            print('no connected connector with an EDID to copy; pass --edid', file=sys.stderr)
            return 2
    target = FIRMWARE / 'edid' / 'lab.bin'
    target.parent.mkdir(parents=True, exist_ok=True)
    data = edid.read_bytes()
    if len(data) < 128 or data[:8] != bytes.fromhex('00ffffffffffff00') or sum(data[:128]) % 256:
        print('%s is not a valid EDID' % edid, file=sys.stderr)
        return 2
    target.write_bytes(data)
    os.chmod(target, 0o644)
    sudo_write(PATH_PARAM, str(FIRMWARE))
    sudo_write(EDID_PARAM, ','.join('%s:edid/lab.bin' % c for c in args.connector))
    for name in args.connector:
        sudo_write('/sys/class/drm/%s-%s/status' % (args.card, name), 'on')
    time.sleep(2)
    return status(args)


def remove(args):
    for name in args.connector:
        sudo_write('/sys/class/drm/%s-%s/status' % (args.card, name), 'detect')
    sudo_write(EDID_PARAM, '')
    sudo_write(PATH_PARAM, '')
    time.sleep(2)
    return status(args)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    commands = parser.add_subparsers(dest='command', required=True)
    for name, func in (('add', add), ('remove', remove), ('status', status)):
        p = commands.add_parser(name)
        p.add_argument('--card', default='card1')
        if name != 'status':
            p.add_argument('connector', nargs='+')
        if name == 'add':
            p.add_argument('--edid')
        p.set_defaults(func=func)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
