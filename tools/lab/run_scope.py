"""A transient systemd user scope for one lab job, so cleanup can take every process the job started.

Every process a job starts stays in its scope's cgroup, whatever session, process group or parent it ends up with (rr's tracees, a
resident's helpers, an orphan). Stopping the scope signals every process in it (SIGTERM, then SIGKILL after the unit's stop timeout)
and waits until the cgroup is empty. It needs a systemd user manager reachable over the user bus (XDG_RUNTIME_DIR); available() says
whether there is one.

Scopes are named putgd-lab-<PID of the loop>-<job>-<random>.scope, so the scopes of one loop are `putgd-lab-<its PID>-*` and never
another loop's.
"""
import os
from pathlib import Path
import secrets
import signal
import subprocess
import time


class ScopeError(Exception):
    """A scope could not be started or stopped: the caller reports it and does not claim the job's processes are gone."""


def available():
    try:
        return subprocess.run(['systemd-run', '--user', '--scope', '--quiet', '--collect', '--', 'true'], stdin=subprocess.DEVNULL,
                              capture_output=True, timeout=30).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def pattern():
    """The unit pattern of every scope this process made."""
    return 'putgd-lab-%d-*.scope' % os.getpid()


class Scope:
    def __init__(self, label):
        # Unique per job: this loop's PID, the job and a random token; no two jobs of a loop share a name.
        self.unit = 'putgd-lab-%d-%s-%s.scope' % (os.getpid(), label, secrets.token_hex(4))

    def command(self, argv):
        """argv run inside this scope (systemd-run registers the scope and then execs argv in place, so the PID is argv's)."""
        return ['systemd-run', '--user', '--scope', '--quiet', '--collect', '--unit=%s' % self.unit[:-len('.scope')], '--'] + list(argv)

    def launch(self, argv, wait=10.0, **popen):
        """Start argv in this scope and check the scope came up. Raises ScopeError if systemd-run failed before the scope existed."""
        process = subprocess.Popen(self.command(argv), **popen)
        end = time.monotonic() + wait
        while time.monotonic() < end:
            if self.active():
                return process
            if process.poll() is not None:
                break
            time.sleep(0.05)
        if process.poll() is None and self.active():
            return process
        raise ScopeError('%s did not start (systemd-run exit %s)' % (self.unit, process.poll()))

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
        returns once the cgroup is empty. SIGINT and SIGTERM are held while it runs (delivered right after), so an interrupt cannot cut
        the stop short. Returns how many processes were in it; raises ScopeError if systemctl failed or the scope is still there."""
        blocked = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
        try:
            left = self.tasks()
            if self.active() or left:
                done = subprocess.run(['systemctl', '--user', 'stop', self.unit], capture_output=True, text=True)
                if done.returncode != 0 or self.active() or self.tasks():
                    raise ScopeError('could not stop %s (systemctl exit %d: %s); %d processes are still in it' % (
                        self.unit, done.returncode, done.stderr.strip()[-200:], self.tasks()))
            return left
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, blocked)
