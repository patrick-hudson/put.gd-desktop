"""A transient systemd user scope for one lab job, so cleanup can take every process the job started.

Every process a job starts stays in its scope's cgroup, whatever session, process group or parent it ends up with (rr's tracees, a
resident's helpers, an orphan). Stopping the scope signals every process in it (SIGTERM, then SIGKILL after the unit's stop timeout)
and waits until the cgroup is empty. It needs a systemd user manager reachable over the user bus (XDG_RUNTIME_DIR); available() says
whether there is one.
"""
import os
from pathlib import Path
import secrets
import subprocess


def available():
    try:
        return subprocess.run(['systemd-run', '--user', '--scope', '--quiet', '--collect', '--', 'true'], stdin=subprocess.DEVNULL,
                              capture_output=True, timeout=30).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


class Scope:
    def __init__(self, label):
        # Unique per job and run: a name of this script's PID and a random token, never reused for another job.
        self.unit = 'putgd-lab-%s-%d-%s.scope' % (label, os.getpid(), secrets.token_hex(4))

    def command(self, argv):
        """argv run inside this scope (systemd-run registers the scope and then execs argv in place, so the PID is argv's)."""
        return ['systemd-run', '--user', '--scope', '--quiet', '--collect', '--unit=%s' % self.unit[:-len('.scope')], '--'] + list(argv)

    def _show(self, prop):
        done = subprocess.run(['systemctl', '--user', 'show', '-p', prop, '--value', self.unit], capture_output=True, text=True)
        return done.stdout.strip()

    def pids(self):
        """The processes in the scope's cgroup now (none when the scope is gone). Read from cgroup.procs, so membership, not a PID
        lookup, says they are this job's: NCurrentTasks is empty where task accounting is off."""
        group = self._show('ControlGroup')
        if not group:
            return []
        try:
            return [int(pid) for pid in Path('/sys/fs/cgroup%s/cgroup.procs' % group).read_text().split()]
        except (OSError, ValueError):
            return []

    def tasks(self):
        return len(self.pids())

    def running(self, marker):
        """True while a process in the scope has `marker` in its command line (say, the script that cleans up after itself)."""
        for pid in self.pids():
            try:
                if marker in Path('/proc/%d/cmdline' % pid).read_bytes():
                    return True
            except OSError:
                pass
        return False

    def active(self):
        return self._show('ActiveState') in ('active', 'activating', 'deactivating', 'reloading')

    def stop(self):
        """Stop the scope whenever it is still there: every process in it gets SIGTERM, and SIGKILL after the stop timeout; systemctl
        returns once the cgroup is empty. Returns how many processes were in it."""
        left = self.tasks()
        if self.active() or left:
            subprocess.run(['systemctl', '--user', 'stop', self.unit], capture_output=True)
        return left
