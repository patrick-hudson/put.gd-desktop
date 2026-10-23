#!/usr/bin/env python3
"""A builder's put.gd Proton lab profile, its session registry and lending to VMs (stdlib only).

Run this on the builder itself, from a checkout there; it never contacts the
workstation. Each builder holds one put.gd Proton session that Patrick signed in
once, in the lab profile under ~/putgd-lab/proton/ (its own XDG_CONFIG_HOME,
XDG_DATA_HOME and XDG_STATE_HOME). The session lives in the builder's default
secret-service collection under `ch.proton.drive/putgd` and the reference in
~/putgd-lab/locks/proton-registry.json. Every refresh rotates its refresh token,
so one user at a time holds the builder's Proton lock
(`desktop_lock.py ... --resource proton`): the lab profile's app, a check, or a
VM borrowing the session through `lend`. The rules are in docs/proton-drive.md
("Lab sessions") and docs/remote-builders.md.

  proton_lab.py status
  proton_lab.py install-app APPIMAGE
  proton_lab.py seed HOST
  proton_lab.py open --session ID           # under the desktop lock: Settings for the sign-in
  proton_lab.py close --session ID
  proton_lab.py check --session ID --owner NAME [--new-folder]
  proton_lab.py lend RUN --session ID --owner NAME --purpose TEXT -- COMMAND [ARG ...]
  proton_lab.py recover RECEIPTS --session ID --owner NAME

No command prints, logs, hashes or writes session material. Snapshots move only
through tools/lab/provision_live_credentials.py's private pipes, inside one
process (`lend`, `recover`), and never sign out or revoke the remote session.
"""
import argparse
import contextlib
import datetime
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = Path(__file__).resolve().parent


def _load(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


desktop_lock = _load('putgd_desktop_lock', 'desktop_lock.py')
bridge = _load('putgd_live_credentials', 'provision_live_credentials.py')
lab = bridge.lab

ROOT = Path.home() / 'putgd-lab/proton'
LOCKS = Path.home() / 'putgd-lab/locks'
REGISTRY = 'proton-registry.json'
PROFILE_ID = 'proton-lab'
SESSION_PROCESSES = ('gnome-shell', 'plasmashell', 'cinnamon')
SESSION_VARIABLES = ('DISPLAY', 'XAUTHORITY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE',
                     'XDG_CURRENT_DESKTOP', 'WAYLAND_DISPLAY', 'XDG_CONFIG_DIRS', 'XDG_DATA_DIRS',
                     'XDG_RUNTIME_DIR', 'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION')
BROKERS = ('putgd-lab-secret-bridge', 'putgd-lab-proton-secrets')
LOCK_MINUTES = 30
HEARTBEAT_SECONDS = 300
SETUP_DEADLINE = 300
OUTSTANDING = 'lent copy outstanding, run proton_lab.py recover'


class LabFailure(RuntimeError):
    """A refused or failed step; the message is safe to print."""


def now_text():
    return desktop_lock.stamp(desktop_lock.now_utc())


class Layout:
    """The lab profile's fixed paths on this builder."""

    def __init__(self, root=ROOT, locks=LOCKS):
        self.root, self.locks = Path(root), Path(locks)
        self.config, self.data, self.state = self.root / 'config', self.root / 'data', self.root / 'state'
        self.apps, self.logs, self.receipts = self.root / 'app', self.root / 'logs', self.root / 'receipts'
        self.host_bridge, self.guest_bridge = self.root / 'bridge', self.root / 'guest-bridge'
        self.open_record = self.root / 'open.json'
        self.registry = self.locks / REGISTRY

    def private_directory(self, path):
        path.mkdir(mode=0o700, parents=True, exist_ok=True)
        info = path.lstat()
        if not path.is_dir() or path.is_symlink() or info.st_uid != os.getuid():
            raise LabFailure(f'{path} is not a private directory owned by this user')
        path.chmod(0o700)
        return path

    def prepare(self):
        for path in (self.root, self.config, self.data, self.state, self.logs, self.receipts):
            self.private_directory(path)

    def environment(self):
        return {'XDG_CONFIG_HOME': str(self.config), 'XDG_DATA_HOME': str(self.data),
                'XDG_STATE_HOME': str(self.state)}


def read_registry(layout):
    try:
        value = json.loads(layout.registry.read_text())
    except FileNotFoundError:
        return None
    except ValueError as error:
        raise LabFailure(f'{layout.registry} is unreadable: {error}') from None
    if not isinstance(value, dict) or value.get('schema') != 1:
        raise LabFailure(f'{layout.registry} has an unknown schema')
    return value


def write_registry(layout, value):
    """Nonsecret facts only: host, keychain reference, profile paths, app, check results."""
    layout.private_directory(layout.locks)
    temporary = layout.registry.with_name(f'.{REGISTRY}.{os.getpid()}')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    os.replace(temporary, layout.registry)
    return value


def require_registry(layout, signed_in=True):
    registry = read_registry(layout)
    if registry is None:
        raise LabFailure(f'No {layout.registry}; run seed first')
    if signed_in and not registry.get('signedIn'):
        raise LabFailure('The lab profile has no verified sign-in yet; Patrick signs in, then run check')
    return registry


def reference_for(host):
    if host not in desktop_lock.HOSTS:
        raise LabFailure('Unknown builder ' + host)
    return f'putgd/proton/lab-{host}'


def proton_lock(layout, host, clock=desktop_lock.now_utc):
    return desktop_lock.Desktop(layout.locks, host, clock, resource='proton')


@contextlib.contextmanager
def holding(lock, session, owner, purpose, minutes=LOCK_MINUTES, heartbeat=True, finishing=False):
    """Take the builder's Proton lock, or reuse it when this session already holds it.

    Yields a dict whose `outstanding` the caller sets while a copy of the session is
    lent out. A lock taken here is released afterwards, unless something failed while
    a copy was outstanding: then it stays held and blocked until `recover` finishes, and
    `finishing` (recover's own call) releases a lock left blocked that way."""
    state = lock.status()
    acquired = False
    progress = {'outstanding': False}
    if state['state'] == 'free':
        lock.acquire(owner, session, purpose, minutes)
        acquired = True
    elif state.get('lock', {}).get('session') != session:
        holder = state.get('lock') or {}
        raise LabFailure(f'The {lock.host} Proton session is held by {holder.get("owner")} '
                         f'({holder.get("session")}); tell your coordinator and try again later')
    else:
        acquired = finishing and OUTSTANDING in (state.get('blocked') or '')
        lock.heartbeat(session)  # clears a blocked reason
        lock.extend(session, minutes)
    stop = threading.Event()
    beat = None
    if heartbeat:
        def beating():
            while not stop.wait(HEARTBEAT_SECONDS):
                try:
                    lock.extend(session, minutes)
                except desktop_lock.LockError:
                    return
        beat = threading.Thread(target=beating, daemon=True)
        beat.start()
    try:
        yield progress
    except BaseException as error:
        stop.set()
        if progress['outstanding']:
            with contextlib.suppress(desktop_lock.LockError):
                lock.heartbeat(session, blocked=(OUTSTANDING + ': ' +
                                                 (str(error) or type(error).__name__))[:300])
            raise
        if acquired:
            with contextlib.suppress(desktop_lock.LockError):
                lock.release(session, verified_clean=True)
        raise
    finally:
        stop.set()
        if beat is not None:
            beat.join()
    if acquired:
        lock.release(session, verified_clean=True)


# The lab profile's app ----------------------------------------------------------------------

def app_root(registry):
    app = (registry or {}).get('app') or {}
    root = Path(app.get('root', ''))
    if not app.get('root') or not (root / 'AppRun').is_file():
        raise LabFailure('No extracted put.gd AppImage; run install-app first')
    return root


def app_environment(root, base):
    environment = dict(base)
    library = f'{root}/usr/lib:{root}/usr/lib/putgd'
    environment['LD_LIBRARY_PATH'] = library + (':' + base['LD_LIBRARY_PATH'] if base.get('LD_LIBRARY_PATH') else '')
    environment['QT_PLUGIN_PATH'] = f'{root}/usr/plugins:{root}/usr/lib/qt6/plugins'
    return environment


def processes_with(name, value):
    """PIDs of this user's processes whose environment has NAME=VALUE (exact)."""
    wanted = f'{name}={value}'.encode()
    found = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit() or int(entry.name) == os.getpid():
            continue
        try:
            if entry.stat().st_uid != os.getuid():
                continue
            if wanted in (entry / 'environ').read_bytes().split(b'\0'):
                found.append(int(entry.name))
        except OSError:
            continue
    return sorted(found)


def lab_app_processes(layout):
    return processes_with('XDG_CONFIG_HOME', str(layout.config))


class Resident:
    """One put.gd resident of the lab profile, reached through its own session socket."""

    def __init__(self, layout, registry, environment, runtime):
        self.layout, self.root = layout, app_root(registry)
        self.environment, self.runtime = environment, runtime

    def cli(self, method, params=None, timeout=120):
        result = subprocess.run([str(self.root / 'usr/bin/putgd-cli'), method, json.dumps(params or {})],
                                env=app_environment(self.root, self.environment), capture_output=True,
                                text=True, timeout=timeout)
        try:
            reply = json.loads(result.stdout)
        except ValueError:
            raise LabFailure(f'putgd-cli {method} gave no JSON reply (exit {result.returncode})') from None
        if not reply.get('ok'):
            error = reply.get('error') or {}
            raise LabFailure(f'putgd-cli {method}: {error.get("code", "failed")}: {error.get("message", "")}')
        return reply.get('result')

    def answering(self):
        try:
            self.cli('capabilities', timeout=15)
            return True
        except (LabFailure, subprocess.TimeoutExpired):
            return False

    def wait(self, timeout=60):
        deadline = time.monotonic() + timeout
        while not self.answering():
            if time.monotonic() > deadline:
                raise LabFailure('The lab resident did not answer in time; see ' + str(self.layout.logs))
            time.sleep(1)

    def start(self, log_name):
        log = self.layout.logs / f'{log_name}-{now_text().replace(":", "-")}.log'
        with open(log, 'x') as stream:
            subprocess.Popen([str(self.root / 'AppRun'), '--background'], env=self.environment, stdin=subprocess.DEVNULL,
                             stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        self.wait()
        return log

    def quit(self, pids, timeout=30):
        """ui.quit, then SIGTERM only the exact PIDs that are still the lab profile's."""
        with contextlib.suppress(LabFailure, subprocess.TimeoutExpired):
            self.cli('ui.quit', timeout=15)
        deadline = time.monotonic() + timeout
        left = list(pids)
        while time.monotonic() < deadline:
            left = [pid for pid in pids if pid in lab_app_processes(self.layout)]
            if not left:
                return []
            time.sleep(.5)
        for pid in left:
            with contextlib.suppress(ProcessLookupError):
                os.kill(pid, signal.SIGTERM)
        time.sleep(2)
        return [pid for pid in left if pid in lab_app_processes(self.layout)]

    def profile(self):
        settings = self.cli('settings.get')
        for profile in settings['document']['profiles']:
            if profile['id'] == PROFILE_ID:
                return settings, profile
        raise LabFailure(f'The lab profile has no {PROFILE_ID} destination; run seed')

    def patch_profile(self, settings_patch):
        settings, profile = self.profile()
        profiles = settings['document']['profiles']
        merged = dict(profile, settings=dict(profile['settings'], **settings_patch))
        self.cli('settings.patch', {'patch': {'profiles': [merged if item['id'] == PROFILE_ID else item
                                                           for item in profiles]},
                                    'expectedRevision': settings['metadata']['observedRevision']})

    def setup(self, kind, params):
        settings, _ = self.profile()
        job = self.cli('setup.' + kind, dict(params, profileId=PROFILE_ID,
                                             expectedRevision=settings['metadata']['observedRevision']))
        deadline = time.monotonic() + SETUP_DEADLINE
        while True:
            state = self.cli('setup.inspect', {'id': job['setupId']})
            if state.get('state') != 'running':
                return state
            if time.monotonic() > deadline:
                raise LabFailure(f'setup.{kind} did not finish in {SETUP_DEADLINE} s')
            time.sleep(1)


def minimal_environment():
    keep = ('HOME', 'USER', 'LOGNAME', 'PATH', 'LANG', 'LC_ALL', 'SHELL', 'TZ')
    return {name: os.environ[name] for name in keep if name in os.environ}


@contextlib.contextmanager
def headless(layout, registry, keychain):
    """A lab resident with no display and a private runtime directory (and so its own session
    socket). With `keychain` it joins the user's real session bus, where the secret service is."""
    runtime = Path(tempfile.mkdtemp(prefix='pgp', dir='/tmp'))
    environment = dict(minimal_environment(), **layout.environment(), XDG_RUNTIME_DIR=str(runtime),
                       QT_QPA_PLATFORM='offscreen')
    if keychain:
        environment['DBUS_SESSION_BUS_ADDRESS'] = f'unix:path=/run/user/{os.getuid()}/bus'
    resident = Resident(layout, registry, environment, runtime)
    pids = []
    try:
        resident.start('headless')
        pids = processes_with('XDG_RUNTIME_DIR', str(runtime))
        yield resident
    finally:
        pids = sorted(set(pids) | set(processes_with('XDG_RUNTIME_DIR', str(runtime))))
        left = resident.quit(pids)
        if left:
            raise LabFailure('Lab resident PIDs did not exit: ' + ' '.join(map(str, left)))
        shutil.rmtree(runtime)


def desktop_environment():
    """The live desktop session's display, bus and runtime variables."""
    for name in SESSION_PROCESSES:
        found = subprocess.run(['pgrep', '-u', str(os.getuid()), '-n', '-x', name], capture_output=True, text=True)
        if found.returncode == 0 and found.stdout.strip():
            raw = Path(f'/proc/{found.stdout.split()[0]}/environ').read_bytes()
            values = dict(entry.decode(errors='replace').partition('=')[::2] for entry in raw.split(b'\0') if entry)
            return {name: values[name] for name in SESSION_VARIABLES if values.get(name)}
    raise LabFailure('No GNOME, Plasma or Cinnamon session is running on this builder')


# Commands -----------------------------------------------------------------------------------

def install_app(layout, appimage):
    """Extract an AppImage into the lab profile (no FUSE, no system package)."""
    source = Path(appimage).resolve()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]*\.AppImage', source.name) or not source.is_file():
        raise LabFailure('Pass an existing .AppImage file')
    layout.prepare()
    layout.private_directory(layout.apps)
    digest = lab.hash_file(source)
    target = layout.apps / digest[:16]
    if not (target / 'squashfs-root/AppRun').is_file():
        staging = Path(tempfile.mkdtemp(prefix='.extract-', dir=layout.apps))
        copy = staging / source.name
        shutil.copyfile(source, copy)
        copy.chmod(0o700)
        subprocess.run([str(copy), '--appimage-extract'], cwd=staging, check=True, stdout=subprocess.DEVNULL)
        copy.unlink()
        if target.exists():
            raise LabFailure(f'{target} exists without an extracted AppRun; inspect it')
        staging.rename(target)
    root = target / 'squashfs-root'
    version = json.loads(subprocess.run([str(root / 'AppRun'), '--version-json'], capture_output=True, text=True,
                                        check=True).stdout)
    registry = read_registry(layout) or {'schema': 1}
    registry['app'] = {'appimage': source.name, 'sha256': digest, 'root': str(root), 'version': version,
                       'installed': now_text()}
    write_registry(layout, registry)
    return registry['app']


def seed(layout, host):
    """Create the profile folders and its Proton Drive destination (no account yet)."""
    layout.prepare()
    registry = read_registry(layout) or {'schema': 1}
    app_root(registry)
    reference = reference_for(host)
    if registry.get('reference') not in (None, reference):
        raise LabFailure('The registry already names another reference; never replace a signed-in session')
    if lab_app_processes(layout):
        raise LabFailure('The lab profile is running; close it first')
    with headless(layout, registry, keychain=False) as resident:
        settings = resident.cli('settings.get')
        profiles = settings['document']['profiles']
        if not any(profile['id'] == PROFILE_ID for profile in profiles):
            profile = {'id': PROFILE_ID, 'label': f'Proton lab ({host})', 'provider': 'proton-drive',
                       'settings': {'accountRootUid': '', 'folderUid': '', 'shareByDefault': False,
                                    'linkExpirationDays': 0, 'sharePasswordReference': None,
                                    'requestTimeoutMs': 120000},
                       'credentials': {'mode': 'keychain', 'reference': reference}}
            resident.cli('settings.patch', {'patch': {'profiles': profiles + [profile]},
                                            'expectedRevision': settings['metadata']['observedRevision']})
        _, profile = resident.profile()
        if profile['credentials'] != {'mode': 'keychain', 'reference': reference}:
            raise LabFailure(f'{PROFILE_ID} does not use {reference}')
        config_path = resident.cli('settings.get')['metadata']['path']
    registry.update(host=host, reference=reference, service='ch.proton.drive/putgd', profileId=PROFILE_ID,
                    profile=dict(layout.environment(), config=config_path), signedIn=registry.get('signedIn'))
    return write_registry(layout, registry)


def held_desktop(layout, host, session):
    state = desktop_lock.Desktop(layout.locks, host).status()
    if state['state'] == 'free' or state.get('lock', {}).get('session') != session:
        raise LabFailure(f'Take the {host} desktop lock first (desktop_lock.py acquire {host} ...)')


def open_settings(layout, session):
    """Start the lab profile's app in the desktop session and show Settings."""
    registry = require_registry(layout, signed_in=False)
    held_desktop(layout, registry['host'], session)
    if lab_app_processes(layout):
        raise LabFailure('The lab profile is already running')
    environment = dict(minimal_environment(), **desktop_environment(), **layout.environment())
    resident = Resident(layout, registry, environment, environment.get('XDG_RUNTIME_DIR'))
    if resident.answering():
        raise LabFailure('Another put.gd already answers on this desktop session; quit it first')
    log = resident.start('desktop')
    resident.cli('ui.show', {'view': 'settings'})
    pids = lab_app_processes(layout)
    layout.open_record.write_text(json.dumps({'session': session, 'pids': pids, 'log': str(log),
                                              'opened': now_text()}) + '\n')
    return {'pids': pids, 'log': str(log)}


def close_settings(layout, session):
    registry = require_registry(layout, signed_in=False)
    record = json.loads(layout.open_record.read_text()) if layout.open_record.exists() else {'pids': []}
    environment = dict(minimal_environment(), **desktop_environment(), **layout.environment())
    resident = Resident(layout, registry, environment, environment.get('XDG_RUNTIME_DIR'))
    left = resident.quit(sorted(set(record['pids']) | set(lab_app_processes(layout))))
    if left:
        raise LabFailure('Lab profile PIDs did not exit: ' + ' '.join(map(str, left)))
    layout.open_record.unlink(missing_ok=True)
    return {'closed': record['pids']}


def check(layout, session, owner, new_folder=False, clock=desktop_lock.now_utc):
    """Non-destructive proof that the builder's session works: inspect the account, optionally
    create a new isolated synthetic folder for it, then run Test connection (read-only)."""
    registry = require_registry(layout, signed_in=False)
    host = registry['host']
    if lab_app_processes(layout):
        raise LabFailure('The lab profile is open on the desktop; close it (close) before a check')
    with holding(proton_lock(layout, host, clock), session, owner, 'proton_lab.py check'):
        with headless(layout, registry, keychain=True) as resident:
            account = resident.setup('inspectAccount', {})
            if account.get('state') != 'completed':
                raise LabFailure('Account inspection failed: ' + json.dumps(account.get('error')))
            result = account.get('result') or {}
            changes = {}
            if result.get('accountRootUid'):
                changes['accountRootUid'] = result['accountRootUid']
            if new_folder:
                name = f'putgd-lab-{host}-check-{now_text().replace(":", "")}'
                created = resident.setup('createFolder', {'input': {'name': name}})
                if created.get('state') != 'completed':
                    raise LabFailure('Creating the check folder failed: ' + json.dumps(created.get('error')))
                folder = created.get('result') or {}
                changes.update(folderUid=folder['uid'], folderName=folder.get('name', name))
            if changes:
                resident.patch_profile(changes)
            tested = resident.setup('testDestination', {})
            _, profile = resident.profile()
    passed = tested.get('state') == 'completed'
    registry = require_registry(layout, signed_in=False)
    registry['lastCheck'] = {'at': now_text(), 'passed': passed, 'by': owner,
                             'folderName': profile['settings'].get('folderName'),
                             'testScope': (tested.get('result') or {}).get('testScope'),
                             'error': (tested.get('error') or {}).get('code'),
                             'app': (registry.get('app') or {}).get('version', {}).get('version')}
    if passed and not registry.get('signedIn'):
        registry['signedIn'] = registry['lastCheck']['at']
    if result.get('accountEmail'):
        registry['account'] = result['accountEmail']
    write_registry(layout, registry)
    if not passed:
        raise LabFailure('Test connection failed: ' + str(registry['lastCheck']['error']))
    return registry['lastCheck']


def brokers(directory):
    endpoint = {'qt': str(directory / BROKERS[0]), 'proton': str(directory / BROKERS[1])}
    for path in map(Path, endpoint.values()):
        info = path.lstat() if path.exists() or path.is_symlink() else None
        if info is None or path.is_symlink() or info.st_uid != os.getuid() or info.st_mode & 0o022 \
                or not os.access(path, os.X_OK):
            raise LabFailure(f'{path} must be an owned executable that only you can write; build the bridge '
                             '(tools/lab/secret_bridge/README.md)')
    return endpoint


class Guest:
    """The disposable VM run on this builder that borrows the session."""

    def __init__(self, run):
        self.run = Path(run).resolve()
        try:
            self.info = lab.state(self.run)
        except (OSError, ValueError) as error:
            raise LabFailure(f'{self.run} is not a lab run on this builder: {error}') from None
        self.directory = None

    def place_brokers(self, source):
        local = brokers(source)
        home = lab.ssh(self.run, 'printf %s "$HOME"').strip()
        if not home.startswith('/'):
            raise LabFailure('The guest home is not absolute')
        self.directory = home + '/putgd-lab-bridge'
        lab.ssh(self.run, f'set -eu\nrm -rf -- {self.directory}\nmkdir -m 700 -- {self.directory}\n')
        for path in local.values():
            lab.put(self.run, path, self.directory + '/' + Path(path).name)
        lab.ssh(self.run, f'set -eu\nchmod 700 -- {self.directory}/{BROKERS[0]} {self.directory}/{BROKERS[1]}\n')
        return {'qt': f'{self.directory}/{BROKERS[0]}', 'proton': f'{self.directory}/{BROKERS[1]}'}

    def stop_app(self, timeout=60):
        """Quit the guest's put.gd so nothing refreshes the borrowed copy after collection."""
        with contextlib.suppress(lab.LabError, ValueError, subprocess.TimeoutExpired):
            lab.cli(self.run, 'ui.quit')
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            left = lab.ssh(self.run, 'pgrep -u "$(id -u)" "^putgd" || true').split()
            if not left:
                return
            time.sleep(1)
        raise LabFailure('put.gd is still running in the guest; stop it before the session is collected')

    def remove_brokers(self):
        if self.directory:
            lab.ssh(self.run, f'rm -rf -- {self.directory}\n')


def receipts_folder(layout, run):
    layout.private_directory(layout.receipts)
    name = now_text().replace(':', '-') + '-' + re.sub(r'[^A-Za-z0-9._-]', '_', Path(run).name)[:40]
    folder = layout.receipts / name
    folder.mkdir(mode=0o700)
    return folder


def note(folder, **fields):
    """The lend's nonsecret progress (references, steps, outcomes), for recovery."""
    path = folder / 'lend.json'
    value = json.loads(path.read_text()) if path.exists() else {}
    value.update(fields, updated=now_text())
    temporary = folder / '.lend.json.tmp'
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    os.replace(temporary, path)
    return value


def carry_back(chain, folder, attempt=1):
    """Exact CAS onto the builder's entry; reconcile an ambiguous reply before any retry."""
    receipt = folder / f'carry-back-{attempt}.json'
    try:
        chain.carry_back(receipt)
        return receipt
    except bridge.ProvisionError:
        if chain.pending_source is None:
            raise
        if chain.reconcile_carry_back(receipt):
            return receipt
        if attempt >= 3:
            raise
        return carry_back(chain, folder, attempt + 1)


def lend(layout, run, session, owner, purpose, command, chain_factory=None, guest_factory=Guest,
         runner=subprocess.call, clock=desktop_lock.now_utc, heartbeat=True):
    """Lend the builder's session to one VM run for COMMAND, then carry its refreshed state back."""
    registry = require_registry(layout)
    reference = registry['reference']
    chain_factory = chain_factory or bridge.ProtonSessionChain
    if not command:
        raise LabFailure('Give the test command after --')
    guest = guest_factory(run)
    if lab_app_processes(layout):
        raise LabFailure('The lab profile is running on this builder; close it before lending its session')
    folder = receipts_folder(layout, guest.run)
    note(folder, run=str(guest.run), source=reference, host=registry['host'], session=session, step='start')
    result = {'receipts': str(folder)}
    with holding(proton_lock(layout, registry['host'], clock), session, owner, purpose,
                 heartbeat=heartbeat) as progress:
        host_broker = brokers(layout.host_bridge)
        guest_broker = guest.place_brokers(layout.guest_bridge)
        chain = chain_factory(host_broker, reference)
        note(folder, step='exported', guestBroker=guest_broker)
        progress['outstanding'] = True
        try:
            borrowed = chain.provision(str(guest.run), guest_broker, folder / 'provision.json')
        except bridge.ProvisionError:
            if chain.active is not None:
                note(folder, reference=chain.active)  # an ambiguous import keeps its exact cleanup name
                chain.abandon(str(guest.run), guest_broker, chain.active)
            progress['outstanding'] = False
            note(folder, step='provision_failed_abandoned')
            raise LabFailure('Provisioning the guest failed; its pending entry was removed') from None
        note(folder, step='provisioned', reference=borrowed)
        environment = dict(os.environ, PUTGD_LAB_RUN=str(guest.run), PUTGD_LAB_PROTON_REFERENCE=borrowed,
                           PUTGD_LAB_PROTON_RECEIPTS=str(folder))
        try:
            result['testExit'] = runner(list(command), env=environment)
        except OSError:
            result['testExit'] = 127  # collect and carry back all the same
        note(folder, step='tested', testExit=result['testExit'])
        guest.stop_app()
        chain.collect(str(guest.run), guest_broker, borrowed)
        result['rotated'] = chain.current != chain.baseline  # a comparison only; nothing is kept
        note(folder, step='collected', rotated=result['rotated'])
        result['carriedBack'] = str(carry_back(chain, folder))
        note(folder, step='carried_back')
        chain.cleanup(str(guest.run), guest_broker, borrowed)
        progress['outstanding'] = False
        note(folder, step='cleaned')
        guest.remove_brokers()
    result['reference'] = borrowed
    return result


def recover(layout, folder, session, owner, chain_factory=None, guest_factory=Guest, clock=desktop_lock.now_utc):
    """Finish an interrupted lend from its receipts: carry the guest's copy back, then clean it up.
    The VM must still be running; its copy may be the only current one."""
    registry = require_registry(layout)
    folder = Path(folder).resolve()
    lent = json.loads((folder / 'lend.json').read_text())
    if lent.get('source') != registry['reference']:
        raise LabFailure('Those receipts belong to another source entry')
    chain_factory = chain_factory or bridge.ProtonSessionChain
    guest = guest_factory(lent['run'])
    guest_broker, reference, run = lent.get('guestBroker'), lent.get('reference'), str(guest.run)
    if lab_app_processes(layout):
        raise LabFailure('The lab profile is running on this builder; close it first')
    with holding(proton_lock(layout, registry['host'], clock), session, owner, 'proton_lab.py recover',
                 finishing=True) as progress:
        if not reference:
            note(folder, step='recovered_nothing_lent')
            return {'recovered': False}
        progress['outstanding'] = True
        provision = folder / 'provision.json'
        _, receipt = bridge._load_receipt(provision)
        if receipt['state'] == 'import_pending' or (receipt['state'] in ('cleanup_pending', 'cleanup_confirmed')
                                                     and receipt['cleanupAllowed']):
            # Nothing used an unconfirmed import, and a started cleanup follows a confirmed carry-back.
            bridge.cleanup({'op': 'cleanup', 'provider': 'proton', 'run': run, 'guestBroker': guest_broker,
                            'reference': reference, 'receipt': str(provision)})
            carried = None
        else:
            guest.stop_app()
            chain = chain_factory(brokers(layout.host_bridge), registry['reference'])
            chain.recover(str(provision), reference, run, guest_broker)
            carried = str(carry_back(chain, folder, 1 + len(list(folder.glob('carry-back-*.json')))))
            chain.cleanup(run, guest_broker, reference)
        progress['outstanding'] = False
        note(folder, step='recovered', carriedBack=carried)
        guest.directory = guest_broker['qt'].rsplit('/', 1)[0]
        guest.remove_brokers()
        return {'recovered': True, 'carriedBack': carried}


def status(layout):
    registry = read_registry(layout)
    lock = proton_lock(layout, registry['host']).status() if registry and registry.get('host') else None
    return {'registry': registry, 'lock': lock, 'running': lab_app_processes(layout),
            'hostBridge': all((layout.host_bridge / name).is_file() for name in BROKERS),
            'guestBridge': all((layout.guest_bridge / name).is_file() for name in BROKERS)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='action', required=True)
    commands.add_parser('status', help='registry, Proton lock, running lab app and bridge')
    installing = commands.add_parser('install-app', help='extract a put.gd AppImage into the lab profile')
    installing.add_argument('appimage')
    seeding = commands.add_parser('seed', help='create the lab profile and its Proton Drive destination')
    seeding.add_argument('host', choices=sorted(desktop_lock.HOSTS))
    opening = commands.add_parser('open', help='show the lab profile Settings on the desktop (desktop lock)')
    closing = commands.add_parser('close', help='quit the lab profile app on the desktop')
    checking = commands.add_parser('check', help='inspect the account and run Test connection (Proton lock)')
    checking.add_argument('--new-folder', action='store_true', help='first create a new isolated check folder')
    lending = commands.add_parser('lend', help='lend the session to one VM run for COMMAND (Proton lock)')
    lending.add_argument('run')
    lending.add_argument('--purpose', required=True)
    lending.add_argument('command', nargs=argparse.REMAINDER)
    recovering = commands.add_parser('recover', help="finish an interrupted lend from its receipts folder")
    recovering.add_argument('receipts')
    for sub in (opening, closing, checking, lending, recovering):
        sub.add_argument('--session', required=True, help='your session id')
    for sub in (checking, lending, recovering):
        sub.add_argument('--owner', required=True, help='short session name')
    args = parser.parse_args(argv)
    layout = Layout()
    try:
        if args.action == 'status':
            reply = status(layout)
        elif args.action == 'install-app':
            reply = install_app(layout, args.appimage)
        elif args.action == 'seed':
            reply = seed(layout, args.host)
        elif args.action == 'open':
            reply = open_settings(layout, args.session)
        elif args.action == 'close':
            reply = close_settings(layout, args.session)
        elif args.action == 'check':
            reply = check(layout, args.session, args.owner, args.new_folder)
        elif args.action == 'lend':
            command = args.command[1:] if args.command[:1] == ['--'] else args.command
            reply = lend(layout, args.run, args.session, args.owner, args.purpose, command)
        else:
            reply = recover(layout, args.receipts, args.session, args.owner)
    except (LabFailure, desktop_lock.LockError, lab.LabError) as error:
        print(json.dumps({'ok': False, 'action': args.action, 'message': str(error)}))
        return 1
    except bridge.ProvisionError:
        # The bridge's errors are generic on purpose; the receipts folder says how far it got.
        print(json.dumps({'ok': False, 'action': args.action, 'message': 'credential bridge step failed; '
                          'the Proton lock stays held and blocked: run recover with the receipts folder'}))
        return 1
    print(json.dumps(dict(ok=True, action=args.action, result=reply), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
