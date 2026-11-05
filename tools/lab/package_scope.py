#!/usr/bin/python3 -I
"""Start one lab install job command in its own delegated system scope.

Staged root-owned in each disposable overlay and granted alone in sudoers. Every
systemd-run argument is fixed here: the caller chooses only the job kind and ID,
and its command runs only after setpriv has dropped back to the sudo caller's
UID, GID and groups. It never runs a caller-selected command as root.
"""
import os
import pwd
import re
import sys

SYSTEMD_RUN = '/usr/bin/systemd-run'
SETPRIV = '/usr/bin/setpriv'
ENV = '/usr/bin/env'
PATH = '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin'


def caller():
    # sudo sets SUDO_UID itself; a caller cannot choose it.
    uid = int(os.environ['SUDO_UID'])
    if os.geteuid() != 0 or uid == 0:
        raise ValueError('run through sudo by the unprivileged lab user')
    return pwd.getpwuid(uid)


def command(kind, job, arguments, user):
    if kind not in ('staging', 'supervisor') or not re.fullmatch(r'install-[0-9a-f]{32}', job) or not arguments:
        raise ValueError('expected staging|supervisor install-<32 hex> COMMAND...')
    return [SYSTEMD_RUN, '--system', '--scope', '--quiet', '--slice=system.slice',
            '--property=Delegate=yes', f'--unit=putgd-{kind}-{job}.scope', '--expand-environment=no', '--',
            SETPRIV, f'--reuid={user.pw_uid}', f'--regid={user.pw_gid}', '--init-groups', '--',
            ENV, '-i', 'HOME=' + user.pw_dir, 'USER=' + user.pw_name, 'LOGNAME=' + user.pw_name,
            'PATH=' + PATH, *arguments]


def main(argv):
    try:
        user = caller()
        if argv[1:] == ['--probe']:
            # Proves sudo really executes this launcher as root, not just that
            # a policy line exists. Creates no unit.
            if not all(os.access(path, os.X_OK) for path in (SYSTEMD_RUN, SETPRIV, ENV)):
                raise ValueError('systemd-run, setpriv or env is missing')
            print('putgd-lab-scope-ready')
            return 0
        arguments = command(argv[1] if len(argv) > 1 else '', argv[2] if len(argv) > 2 else '', argv[3:], user)
    except (KeyError, ValueError) as error:
        raise SystemExit('putgd-lab-scope refused: ' + str(error)) from None
    os.execv(arguments[0], arguments)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
