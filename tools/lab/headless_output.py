#!/usr/bin/env python3
"""Bounded live Intel HDMI EDID trials. Run on the builder, under its desktop lease.

inspect --card card0 --output NEW_DIR
edid --output FILE --refresh 30|60
enable --card card0 --connector HDMI-A-2 --keep HDMI-A-1 --edid FILE
       --run NEW_DIR --session DESKTOP_LOCK_SESSION [--seconds 300]
restore --run DIR

Never changes boot configuration, takes DRM master, or forces a DP/HDMI alias
of an active encoder. A user-systemd timer is armed before the first write.
Restore the compositor's arrangement/configuration separately before handoff;
the receipt certifies kernel state only. Keep every enabled head on the wall.
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time
import uuid


class Refused(RuntimeError):
    pass


def command(args, data=None):
    return subprocess.run(args, input=data, capture_output=True, check=True, timeout=30).stdout


def root_read(path):
    return command(['sudo', '-n', 'cat', str(path)])


def root_write(path, value):
    command(['sudo', '-n', 'tee', str(path)], value)


def stamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def private_new(path):
    path = Path(path).absolute()
    if path.exists() or path.is_symlink():
        raise Refused('Output directory must be new')
    path.mkdir(mode=0o700, parents=False)
    return path


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def decode_edid(value):
    if len(value) not in (128, 256) or value[:8] != bytes.fromhex('00ffffffffffff00'):
        raise Refused('Expected a 128/256-byte EDID')
    if value[126] != len(value) // 128 - 1 or any(sum(value[i:i + 128]) % 256 for i in range(0, len(value), 128)):
        raise Refused('EDID extension count/checksum mismatch')
    return value


def edid(refresh=60):
    """Synthetic digital QHD head with a 1080p fallback and HDMI TMDS limit.

    QHD60 uses 241.5MHz reduced blanking. Without the HDMI VSDB i915 treats
    this as DVI and rejects clocks above 165MHz. QHD30 needs 120.75MHz.
    """
    if refresh not in (30, 60):
        raise Refused('Only the validated 30/60 Hz timing recipes are available')
    b = bytearray(256)
    b[:8] = bytes.fromhex('00ffffffffffff00')
    b[8:10] = bytes.fromhex('31d8')  # LNX, synthetic laboratory identity
    b[10:16] = bytes.fromhex('7c017c010000')
    b[16:25] = bytes([1, 36, 1, 3, 0x80, 60, 34, 120, 0x0a])
    b[25:35] = bytes.fromhex('5ec0a4594a9825205054')
    b[38:54] = bytes.fromhex('d1c0' + '0101' * 7)
    b[54:72] = bytes.fromhex('565e00a0a0a029503020350055502100001a')
    b[54:56] = (24150 if refresh == 60 else 12075).to_bytes(2, 'little')
    b[72:90] = b'\0\0\0\xff\0' + b'putgd-380\n   '
    b[90:108] = bytes.fromhex('000000fd00194b1e5a1e000a202020202020')
    b[108:126] = b'\0\0\0\xfc\0' + b'putgd QHD\n   '
    b[126] = 1
    b[127] = (-sum(b[:127])) & 255
    # CTA-861 extension; HDMI OUI 000c03, physical address 1.0.0.0,
    # no deep colour/audio, max TMDS clock 300MHz (60 * 5MHz).
    b[128:140] = bytes.fromhex('02030c0067030c001000003c')
    b[255] = (-sum(b[128:255])) & 255
    return decode_edid(bytes(b))


def connector_rows(text):
    rows = {}
    for line in text.splitlines():
        m = re.fullmatch(r'(\d+)\s+(\d+)\s+(connected|disconnected|unknown)\s+(\S+)\s+\d+x\d+\s+\d+\s+([\d,]+)\s*', line)
        if m:
            rows[m[4]] = {'id': int(m[1]), 'encoder': int(m[2]), 'status': m[3],
                          'encoders': [int(x) for x in m[5].split(',')]}
    return rows


def check_aliases(rows, target, active):
    if target not in rows or not rows[target]['encoders']:
        raise Refused('Target has no proven encoder mapping')
    for name in active:
        if name not in rows:
            raise Refused('Active connector missing from encoder inventory')
        if set(rows[target]['encoders']) & set(rows[name]['encoders']):
            raise Refused(f'{target} shares an encoder with active {name}')


def lease(session):
    p = Path.home() / 'putgd-lab/locks/desktop.lock'
    fields = dict(line.split(': ', 1) for line in p.read_text().splitlines() if ': ' in line)
    now = datetime.datetime.now(datetime.timezone.utc)
    def date(key):
        return datetime.datetime.strptime(fields[key], '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=datetime.timezone.utc)
    if fields.get('session') != session or date('expires') <= now or (now - date('heartbeat')).total_seconds() >= 900:
        raise Refused('A current matching desktop lease and heartbeat are required')
    return fields


def device(card):
    if not re.fullmatch(r'card\d+', card):
        raise Refused('Invalid DRM card name')
    sysfs = Path('/sys/class/drm') / card
    pci = (sysfs / 'device').resolve().name
    driver = (sysfs / 'device/driver').resolve().name
    if driver != 'i915':
        raise Refused('This pilot supports only the inspected i915 path')
    return sysfs, Path('/sys/kernel/debug/dri') / pci


def snapshot(card):
    sysfs, debug = device(card)
    raw = command(['modetest', '-M', 'i915', '-D', '/dev/dri/' + card, '-c', '-e']).decode()
    outputs = {}
    for p in sorted(sysfs.parent.glob(card + '-*')):
        name = p.name[len(card) + 1:]
        d = debug / name
        outputs[name] = {'status': (p / 'status').read_text().strip(),
                         'enabled': (p / 'enabled').read_text().strip(),
                         'modes': (p / 'modes').read_text().splitlines(),
                         'force': root_read(d / 'force').decode().strip(),
                         'override': base64.b64encode(root_read(d / 'edid_override')).decode()}
    return {'at': stamp(), 'host': socket.gethostname(), 'bootId': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
            'kernel': os.uname().release, 'card': card, 'debug': str(debug),
            'outputs': outputs, 'connectors': connector_rows(raw), 'modetest': raw,
            'displayInfo': root_read(debug / 'i915_display_info').decode(),
            'atomicState': root_read(debug / 'state').decode()}


def notify(card):
    root_write(Path('/sys/class/drm') / card / 'uevent', b'change HOTPLUG=1')


def enable(args):
    if not re.fullmatch(r'HDMI-A-\d+', args.connector):
        raise Refused('Only an independent HDMI connector may be forced by this tool')
    if not 30 <= args.seconds <= 900:
        raise Refused('Rollback must be armed for 30..900 seconds')
    lock = lease(args.session)
    expires = datetime.datetime.strptime(lock['expires'], '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=datetime.timezone.utc)
    if time.time() + args.seconds + 5 > expires.timestamp():
        raise Refused('Rollback must precede lease expiry; extend the lease first')
    value = decode_edid(Path(args.edid).read_bytes())
    before = snapshot(args.card)
    active = [name for name, o in before['outputs'].items() if o['enabled'] == 'enabled']
    if args.keep not in active or args.connector in active:
        raise Refused('Preserved head must be active and trial connector inactive')
    target = before['outputs'].get(args.connector)
    if not target or target['status'] != 'disconnected':
        raise Refused('Trial target must currently be disconnected')
    check_aliases(before['connectors'], args.connector, active)
    run = private_new(args.run)
    unit = 'headless-output-' + uuid.uuid4().hex[:12]
    before.update(target=args.connector, keep=args.keep, lease=lock, timer=unit,
                  expiresEpoch=time.time() + args.seconds, edidSha256=hashlib.sha256(value).hexdigest())
    save(run / 'before.json', before)
    (run / 'trial.edid').write_bytes(value)
    helper = run / 'restore-tool.py'
    shutil.copyfile(__file__, helper)
    # Pin a copy of the restorer so later edits to the checkout cannot change rollback.
    command(['systemd-run', '--user', '--unit=' + unit, '--on-active=' + str(args.seconds) + 's',
             '/usr/bin/python3', str(helper), 'restore', '--run', str(run)])
    save(run / 'armed.json', {'at': stamp(), 'timer': unit, 'helperSha256': hashlib.sha256(helper.read_bytes()).hexdigest()})
    try:
        d = Path(before['debug']) / args.connector
        root_write(d / 'edid_override', value)
        # A changed sysfs force triggers fill_modes, unlike debugfs force alone.
        root_write(d / 'force', b'unspecified')
        root_write(Path('/sys/class/drm') / (args.card + '-' + args.connector) / 'status', b'on-digital')
        notify(args.card)
        after = snapshot(args.card)
        save(run / 'after.json', after)
        current = after['outputs'][args.connector]
        if current['force'] != 'digital' or base64.b64decode(current['override']) != value:
            raise Refused('Trial was not retained; inspect the rollback receipt')
        if any(after['outputs'][name]['enabled'] != 'enabled' for name in active):
            raise Refused('An original head stopped scanning out; trial rolled back')
    except BaseException:
        restore(run)
        raise
    return {'status': 'trial-active-not-accepted', 'run': str(run), 'timer': unit,
            'reminder': 'Verify distinct active CRTCs, compositor outputs, and all heads live on the wall; restore compositor settings separately.'}


def restore(run):
    run = Path(run).resolve()
    if run.stat().st_uid != os.getuid() or run.stat().st_mode & 0o077:
        raise Refused('Restoration directory must be private and owned')
    before = json.loads((run / 'before.json').read_text())
    if before['host'] != socket.gethostname() or before['bootId'] != Path('/proc/sys/kernel/random/boot_id').read_text().strip():
        raise Refused('Host or boot changed; do not replay a runtime snapshot')
    _, debug = device(before['card'])
    if str(debug) != before['debug'] or not re.fullmatch(r'HDMI-A-\d+', before['target']):
        raise Refused('Device identity mismatch')
    original = before['outputs'][before['target']]
    d = debug / before['target']
    value = base64.b64decode(original['override'], validate=True)
    force = {'unspecified': b'detect', 'digital': b'on-digital', 'on': b'on', 'off': b'off'}[original['force']]
    root_write(d / 'edid_override', value or b'reset')
    root_write(d / 'force', b'unspecified' if original['force'] != 'unspecified' else b'off')
    root_write(Path('/sys/class/drm') / (before['card'] + '-' + before['target']) / 'status', force)
    notify(before['card'])
    after = snapshot(before['card'])
    target = after['outputs'][before['target']]
    preserved = all(after['outputs'][name]['force'] == o['force'] and after['outputs'][name]['override'] == o['override']
                    for name, o in before['outputs'].items() if name != before['target'])
    ok = target['force'] == original['force'] and target['override'] == original['override'] and preserved
    receipt = {'at': stamp(), 'kernelRestored': ok, 'otherOverridesPreserved': preserved, 'after': after,
               'compositorRestored': 'Must be verified separately, including its persistent configuration.'}
    save(run / 'restoration.json', receipt)
    if not ok:
        raise Refused('Kernel restoration verification failed; inspect retained receipt')
    command(['systemctl', '--user', 'stop', before['timer'] + '.timer'])
    return receipt


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='action', required=True)
    i = sub.add_parser('inspect'); i.add_argument('--card', required=True); i.add_argument('--output', required=True)
    e = sub.add_parser('edid'); e.add_argument('--output', required=True); e.add_argument('--refresh', type=int, choices=(30, 60), default=60)
    e = sub.add_parser('enable')
    for key in ('card', 'connector', 'keep', 'edid', 'run', 'session'):
        e.add_argument('--' + key, required=True)
    e.add_argument('--seconds', type=int, default=300)
    r = sub.add_parser('restore'); r.add_argument('--run', required=True)
    a = p.parse_args()
    if a.action == 'edid':
        with Path(a.output).open('xb') as f:
            f.write(edid(a.refresh))
        result = {'edid': a.output, 'refresh': a.refresh}
    elif a.action == 'inspect':
        result = snapshot(a.card); save(private_new(a.output) / 'inventory.json', result)
    elif a.action == 'enable':
        result = enable(a)
    else:
        result = restore(a.run)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (Refused, OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
