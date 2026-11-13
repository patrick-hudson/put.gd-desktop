"""The put.gd resident of a lab run: talking to it, starting it and stopping it.

record_run.py and fluidity_run.py both start a build's resident with a private
configuration and drive it through putgd-cli. A "driver" here is the object holding
the run: .run (its directory), .env (the resident's environment), .socket (the
resident's session socket path) and .resident (the process, once started).
"""
import json
import os
import signal
import subprocess
import time


class RunError(RuntimeError):
    pass


def cli(driver, build, method, params=None, timeout=60):
    result = subprocess.run([build.cli, method, json.dumps(params or {})], env=driver.env, capture_output=True,
                            text=True, timeout=timeout)
    try:
        reply = json.loads(result.stdout)
    except ValueError:
        raise RunError('%s: no JSON reply (exit %d): %s' % (method, result.returncode, result.stderr.strip()[-300:]))
    if not reply.get('ok'):
        error = reply.get('error') or {}
        raise RunError('%s failed: %s %s' % (method, error.get('code'), error.get('message')))
    return reply.get('result') or {}


def start_resident(driver, build, label=None, env=None, no_notifications=False):
    """Starts the build's resident and returns its recording capabilities.

    env: the resident's environment when it differs from the driver's. label: names
    the resident's log, default the build's. no_notifications: turns the run's own put.gd
    notifications off in its private configuration.
    """
    if driver.socket.exists():
        raise RunError('%s exists: another put.gd resident owns this session' % driver.socket)
    driver.resident = subprocess.Popen(build.launcher, env=env or driver.env, stdin=subprocess.DEVNULL,
                                       stdout=open(driver.run / 'logs' / ('resident-%s.log' % (label or build.label)), 'a'),
                                       stderr=subprocess.STDOUT, start_new_session=True)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            caps = cli(driver, build, 'recording.capabilities', timeout=40)
            break
        except (RunError, subprocess.TimeoutExpired):
            time.sleep(0.5)
    else:
        raise RunError('resident did not answer')
    # A fresh configuration has no output; keep local copies inside the run, never in the user's Documents.
    settings = cli(driver, build, 'settings.get')
    archive = (settings.get('document') or {}).get('archive') or {}
    root = str(driver.run / 'archive')
    if not archive.get('keepUploads') or archive.get('root') != root:
        cli(driver, build, 'settings.patch', {'patch': {'archive': {'keepUploads': True, 'root': root}},
                                              'expectedRevision': (settings.get('metadata') or {}).get('observedRevision')})
    if no_notifications and ((settings.get('document') or {}).get('preferences') or {}).get('notifications'):
        # A resident that warns at every start (a missing GNOME extension) would put its banner over the cover.
        settings = cli(driver, build, 'settings.get')
        cli(driver, build, 'settings.patch', {'patch': {'preferences': {'notifications': False}},
                                              'expectedRevision': (settings.get('metadata') or {}).get('observedRevision')})
    return caps


def stop_resident(driver, build):
    if driver.resident is None:
        return
    try:
        cli(driver, build, 'ui.quit', timeout=15)
    except (RunError, subprocess.TimeoutExpired):
        pass
    try:
        driver.resident.wait(20)
    except subprocess.TimeoutExpired:
        os.killpg(driver.resident.pid, signal.SIGTERM)
        driver.resident.wait(10)
    deadline = time.monotonic() + 10
    while driver.socket.exists() and time.monotonic() < deadline:
        time.sleep(0.2)
    driver.resident = None
