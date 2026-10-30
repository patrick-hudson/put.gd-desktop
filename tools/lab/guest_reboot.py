"""Reboot an authenticated disposable overlay and prove its desktop restarted.

An active-session request gets a bounded chance to reboot gracefully. If the
session manager ignores it, sync the owned guest before a host-side QMP reset.
No sudo password or desktop credentials are required; physical hosts are refused.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lab


REQUESTS = {
    'gnome-shell': 'gnome-session-quit --reboot --no-prompt',
    'plasmashell': 'qdbus6 org.kde.Shutdown /Shutdown org.kde.Shutdown.logoutAndReboot || qdbus org.kde.Shutdown /Shutdown org.kde.Shutdown.logoutAndReboot',
    'cinnamon': 'cinnamon-session-quit --reboot --no-prompt',
}


def identity(run, process):
    reply = lab.ssh(run, 'cat /proc/sys/kernel/random/boot_id\nps -o lstart= -C ' + shlex.quote(process) + ' | head -1\n', timeout=10)
    lines = reply.strip().splitlines()
    if not lines:
        raise lab.LabError('Guest boot identity unavailable')
    return {'boot': lines[0].strip(), 'shell': lines[1].strip() if len(lines) > 1 else ''}


def restarted(before, after):
    return bool(after.get('boot') and after['boot'] != before['boot'] and after.get('shell') and after['shell'] != before['shell'])


def ready(run, process):
    if process != 'gnome-shell':
        return True
    # A staged extension must actually answer from the new Shell, not merely
    # exist on disk or occur in the enabled-extensions setting (#929).
    script = '''set -eu
if [ -d "$HOME/.local/share/gnome-shell/extensions/putgd-lab-witness@put.gd" ]; then
 gnome-extensions enable putgd-lab-witness@put.gd
 gdbus call --session --dest org.gnome.Shell --object-path /org/putgd/LabWitness --method org.putgd.LabWitness.Windows >/dev/null
fi
'''
    lab.guest_session(run, script, timeout=10)
    return True


def reboot(run, *, session_timeout=30, timeout=180):
    info = lab.verify_owned_guest(run)
    process = lab.load_profile(info['profile'])['session_process']
    request = REQUESTS[process]
    before = identity(run, process)
    if not before['shell']:
        raise lab.LabError('Guest has no active desktop to reboot')
    lab.verify_owned_guest(run)
    lab.guest_session(run, 'sync\nnohup sh -c ' + shlex.quote('sleep 1; ' + request) + ' >/dev/null 2>&1 < /dev/null &\n', timeout=15)
    method, reset = 'desktop-session', False
    started = time.monotonic()
    while time.monotonic() - started < timeout:
        after = None
        try:
            after = identity(run, process)
            if restarted(before, after) and ready(run, process):
                lab.verify_owned_guest(run)
                return {'method': method, 'before': before, 'after': after, 'run': str(run), 'witnessReady': True}
        except (lab.LabError, OSError, subprocess.SubprocessError):
            pass  # Expected while the guest is rebooting or Shell is starting.
        if (not reset and after and after['boot'] == before['boot'] and after.get('shell')
                and time.monotonic() - started >= session_timeout):
            # Ownership is re-proven immediately before each mutation. Sync
            # staged code and the run's SSH key before resetting the VM.
            with lab.overlay_cleanup.lifecycle_lock():
                lab.verify_owned_guest(run)
                lab.guest_session(run, 'sync\n', timeout=15)
                lab.verify_owned_guest(run)
                with lab.QMP(Path(run) / 'qmp.sock') as qmp:
                    qmp.execute('system_reset')
            reset, method = True, 'owned-qmp-reset'
        time.sleep(2)
    raise lab.LabError('Guest reboot did not prove a new boot, desktop and staged witness within the deadline')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run')
    parser.add_argument('--session-timeout', type=float, default=30)
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    if not 0 <= args.session_timeout < args.timeout <= 600:
        parser.error('require 0 <= session-timeout < timeout <= 600')
    print(json.dumps(reboot(args.run, session_timeout=args.session_timeout, timeout=args.timeout)))


if __name__ == '__main__':
    main()
