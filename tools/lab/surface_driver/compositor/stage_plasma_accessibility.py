#!/usr/bin/env python3
"""Enable Plasma's Qt accessibility bridge only in an authenticated overlay."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.lab import lab

SCRIPT = '''python3 - <<'SD_PLASMA_A11Y'
import dbus,json
bus=dbus.SessionBus()
status=dbus.Interface(bus.get_object('org.a11y.Bus','/org/a11y/bus'),'org.freedesktop.DBus.Properties')
before={k:bool(status.Get('org.a11y.Status',k)) for k in ('IsEnabled','ScreenReaderEnabled')}
status.Set('org.a11y.Status','IsEnabled',dbus.Boolean(True))
after={k:bool(status.Get('org.a11y.Status',k)) for k in before}
assert after['IsEnabled'] and before['ScreenReaderEnabled']==after['ScreenReaderEnabled']
print(json.dumps({'before':before,'after':after,'scope':'owned-overlay'}))
SD_PLASMA_A11Y
'''


def stage(run):
    lab.verify_owned_guest(run)
    profile = lab.load_profile(lab.state(run)['profile'])
    if profile['session_process'] != 'plasmashell' or profile['session'] != 'wayland':
        raise lab.LabError('Plasma accessibility staging requires a disposable Plasma Wayland overlay')
    lab.verify_owned_guest(run)
    return lab.guest_session(run, SCRIPT)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run')
    args = parser.parse_args()
    try:
        print(stage(args.run))
    except (lab.LabError, OSError, ValueError) as error:
        parser.exit(2, 'Plasma accessibility staging refused: '+str(error)+'\n')
