#!/usr/bin/env python3
"""Stdin-only provisioning for disposable Linux lab keychains. Never print secrets.

The public CLI consumes one bounded JSON request and prints a generic status. A
request must name a new private receipt path for credential references/settings.
For rotating Proton sessions, use ProtonSessionChain in one host process: export,
provision one guest, collect its refreshed snapshot, then provision the next.
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import stat
import subprocess
import sys
import time
import uuid
from urllib.parse import urlsplit

MAX_INPUT = 262144
MAX_REPLY = 262144
MAX_SNAPSHOT = 65536
MAX_CREDENTIAL_FILE = 65536
MAX_RECEIPT = 16384
# The Qt relay can spend 40 seconds in two S3 jobs, or about 32 seconds
# coordinating Proton startup/lock/child cleanup. Keep its outer pipe alive.
BROKER_TIMEOUT = 60
LAB_REFERENCE = re.compile(r'putgd-lab-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z')
HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location('putgd_credentials_lab', HERE / 'lab.py')
lab = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lab)


class ProvisionError(RuntimeError):
    """Only stable, secret-free error categories cross this boundary."""


def _fail():
    raise ProvisionError('credential_provisioning_failed')


def _text(value, maximum, allow_empty=False):
    if not isinstance(value, str) or len(value) > maximum or (not value and not allow_empty) or any(ord(c) < 32 for c in value):
        _fail()
    return value


def validate_reference(value, lab_only=False):
    _text(value, 256)
    if lab_only and not LAB_REFERENCE.fullmatch(value):
        _fail()
    return value


def validate_snapshot(raw):
    if not isinstance(raw, str) or len(raw.encode('utf-8')) > MAX_SNAPSHOT:
        _fail()
    try:
        value = json.loads(raw)
    except (ValueError, UnicodeError):
        _fail()
    if not isinstance(value, dict) or not isinstance(value.get('session'), dict):
        _fail()
    session = value['session']
    for field in (session.get('uid'), session.get('accessToken'), value.get('userKeyPassword')):
        if not isinstance(field, str) or not field:
            _fail()
    for owner, key in ((session, 'refreshToken'), (value, 'cachePassword')):
        if owner.get(key) and not isinstance(owner[key], str):
            _fail()
    if 'telemetryEnabled' in value and type(value['telemetryEnabled']) is not bool:
        _fail()
    return raw  # Preserve the official snapshot verbatim, including extension fields.


def _credentials(value):
    if not isinstance(value, dict) or set(value) != {'accessKey', 'secretKey', 'sessionToken'}:
        _fail()
    _text(value['accessKey'], 4096)
    _text(value['secretKey'], 4096)
    _text(value['sessionToken'], 16384, allow_empty=True)
    return value


def parse_s3_credentials(text):
    """Read explicitly named colon-style fields; never infer unlabeled values."""
    if not isinstance(text, str) or len(text.encode('utf-8')) > MAX_CREDENTIAL_FILE or '\0' in text:
        _fail()
    labels = {'access key id': 'accessKey', 'secret access key': 'secretKey',
              'session token': 'sessionToken', 'bucket name': 'bucket', 'bucketname': 'bucket',
              'bucket': 'bucket', 'region': 'region', 'endpoint': 'endpoint'}
    lines = text.splitlines()
    fields = {}
    index = 0
    while index < len(lines):
        line = lines[index].strip()
        index += 1
        if ':' not in line:
            continue
        label, value = line.split(':', 1)
        key = labels.get(' '.join(label.casefold().split()))
        if key is None:
            continue
        if key in fields:
            _fail()
        value = value.strip()
        if not value:
            if index == len(lines) or not lines[index].strip():
                _fail()
            value = lines[index].strip()
            next_label = ' '.join(value.split(':', 1)[0].casefold().split())
            if ':' in value and next_label in labels:
                _fail()
            index += 1
        fields[key] = value
    credentials = {key: fields.get(key, '') for key in ('accessKey', 'secretKey', 'sessionToken')}
    _credentials(credentials)
    settings = {key: value for key, value in fields.items() if key not in credentials}
    for value in settings.values():
        _text(value, 4096)
    if 'endpoint' in settings:
        try:
            endpoint = urlsplit(settings['endpoint'])
            # Production S3 endpoints prohibit user info, query and fragment.
            # Reject these before a nominally nonsecret settings receipt exists.
            if endpoint.username is not None or endpoint.password is not None or endpoint.query or endpoint.fragment:
                _fail()
        except ValueError:
            _fail()
    return {'credentials': credentials, 'settings': settings}


def read_private_credentials(path):
    """Runtime-only read of the user-authorized 0600 input; never log it."""
    descriptor = None
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o600:
            _fail()
        with os.fdopen(descriptor, 'rb') as source:
            descriptor = None
            raw = source.read(MAX_CREDENTIAL_FILE + 1)
        if len(raw) > MAX_CREDENTIAL_FILE:
            _fail()
        return raw.decode('utf-8')
    except (OSError, UnicodeError):
        _fail()
    finally:
        if descriptor is not None:
            os.close(descriptor)


def validate_broker_request(provider, request):
    if not isinstance(request, dict):
        _fail()
    op = request.get('op')
    fields = {'import': {'op', 'reference', 'credentials' if provider == 's3' else 'snapshot'},
              'delete': {'op', 'reference'}}
    if provider == 'proton':
        fields.update(export={'op', 'reference'}, replace={'op', 'reference', 'expectedSnapshot', 'snapshot'})
    elif provider != 's3':
        _fail()
    if op not in fields or set(request) != fields[op]:
        _fail()
    validate_reference(request['reference'], lab_only=op in ('import', 'delete'))
    if op == 'import' and provider == 's3':
        _credentials(request['credentials'])
    for key in ('snapshot', 'expectedSnapshot'):
        if key in request:
            validate_snapshot(request[key])
    return request


def exchange(arguments, body, timeout=BROKER_TIMEOUT):
    """Bound both pipe directions; stderr is discarded and never interpolated."""
    if len(body) > MAX_INPUT:
        _fail()
    process = None
    try:
        process = subprocess.Popen(arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL)
        os.set_blocking(process.stdin.fileno(), False)
        os.set_blocking(process.stdout.fileno(), False)
        output = bytearray()
        written = 0
        deadline = time.monotonic() + timeout
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdin, selectors.EVENT_WRITE)
            selector.register(process.stdout, selectors.EVENT_READ)
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    _fail()
                for key, _ in selector.select(remaining):
                    if key.fileobj is process.stdin:
                        written += os.write(process.stdin.fileno(), body[written:written + 65536])
                        if written == len(body):
                            selector.unregister(process.stdin)
                            process.stdin.close()
                    else:
                        chunk = os.read(process.stdout.fileno(), 65536)
                        if not chunk:
                            selector.unregister(process.stdout)
                        else:
                            output.extend(chunk)
                            if len(output) > MAX_REPLY:
                                _fail()
        if process.wait(timeout=max(0.01, deadline - time.monotonic())) != 0:
            _fail()
        return bytes(output)
    except (OSError, subprocess.SubprocessError):
        _fail()
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait()
            for stream in (process.stdin, process.stdout):
                if stream is not None:
                    stream.close()


def _envelope(broker, provider, request):
    validate_broker_request(provider, request)
    if provider == 'proton':
        if not isinstance(broker, dict) or set(broker) != {'qt', 'proton'}:
            _fail()
        program = broker['qt']
        inner = {'provider': provider, 'broker': broker['proton'], **request}
        _absolute_program(broker['proton'])
    else:
        program = broker
        inner = {'provider': provider, **request}
    _absolute_program(program)
    body = json.dumps(inner, separators=(',', ':'), ensure_ascii=True).encode('ascii') + b'\n'
    if len(body) > MAX_INPUT:
        _fail()
    return program, body


def _absolute_program(value):
    _text(value, 4096)
    if not value.startswith('/') or Path(value).name in ('.', '..'):
        _fail()


def _reply(raw, provider, request):
    try:
        value = json.loads(raw)
    except (ValueError, UnicodeError):
        _fail()
    expected = {'ok', 'snapshot'} if provider == 'proton' and request['op'] == 'export' else {'ok'}
    if not isinstance(value, dict) or set(value) != expected or value['ok'] is not True:
        _fail()
    if 'snapshot' in value:
        validate_snapshot(value['snapshot'])
    return value


def host_call(broker, provider, request):
    program, body = _envelope(broker, provider, request)
    return _reply(exchange([program], body, timeout=BROKER_TIMEOUT), provider, request)


def guest_call(run, broker, provider, request):
    """Use the existing strict lab SSH identity and the live desktop DBus bus."""
    program, body = _envelope(broker, provider, request)
    run = Path(run).resolve()
    try:
        info = lab.state(run)
        key = run / 'identity'
        metadata = key.lstat()
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != os.getuid() or stat.S_IMODE(metadata.st_mode) != 0o600:
            _fail()
        # Provision an existing disposable overlay only; never an image/base.
        if Path(info['overlay']).resolve() != run / 'disk.qcow2' or not (run / 'disk.qcow2').is_file():
            _fail()
        profile = lab.load_profile(info['profile'])
        session = profile['session_process']
        if session not in ('cinnamon', 'gnome-shell', 'plasmashell'):
            _fail()
        user = info['guest_user']
        if not re.fullmatch(r'[a-z_][a-z0-9_-]*[$]?', user):
            _fail()
        arguments = lab.ssh_args(run) + ['sh', '-s']
    except (OSError, ValueError, KeyError, lab.LabError):
        _fail()
    nonce = 'PUTGD_SECRET_' + uuid.uuid4().hex
    # Compact ASCII JSON has no literal newlines; it cannot terminate this
    # single-quoted heredoc or expand shell substitutions in credential values.
    script = f'''set +x
set -eu
pid=$(pgrep -u {shlex.quote(user)} -x {shlex.quote(session)} | head -n 1)
[ -n "$pid" ]
for name in DISPLAY XAUTHORITY DBUS_SESSION_BUS_ADDRESS XDG_SESSION_TYPE XDG_CURRENT_DESKTOP WAYLAND_DISPLAY; do
 value=$(tr '\\0' '\\n' < /proc/$pid/environ | sed -n "s/^$name=//p")
 [ -z "$value" ] || export "$name=$value"
done
export QT_QPA_PLATFORM={shlex.quote(profile['qt_platform'])}
export XDG_RUNTIME_DIR=/run/user/$(id -u)
exec {shlex.quote(program)} <<'{nonce}'
'''.encode('ascii') + body + nonce.encode('ascii') + b'\n'
    return _reply(exchange(arguments, script, timeout=BROKER_TIMEOUT), provider, request)


def _run_name(run):
    try:
        _text(os.fspath(run), 4096)
        return str(Path(run).resolve())
    except (OSError, TypeError, ValueError):
        _fail()


def _receipt_directory(path):
    """Pin a directory without accepting symlinks anywhere in its path."""
    _text(os.fspath(path), 4096)
    path = Path(os.path.abspath(path))
    for parent in reversed(path.parents):
        if not stat.S_ISDIR(parent.lstat().st_mode):
            _fail()
    return path, os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)


def _receipt_shape(value):
    required = {'schema', 'provider', 'target', 'run', 'kind', 'credentialReference', 'state', 'cleanupAllowed'}
    if not isinstance(value, dict) or not required <= set(value) or set(value) - required - {'settings'}:
        _fail()
    if value['schema'] != 1 or type(value['schema']) is not int or type(value['cleanupAllowed']) is not bool:
        _fail()
    kind = value['kind']
    kinds = {'one_time': 'guest', 'chain_guest': 'guest', 'host_lab': 'host', 'source_replace': 'host'}
    if kind not in kinds or value['target'] != kinds[kind] or value['provider'] not in ('s3', 'proton'):
        _fail()
    if kind != 'one_time' and value['provider'] != 'proton':
        _fail()
    if value['target'] == 'guest':
        if _run_name(value['run']) != value['run']:
            _fail()
    elif value['run'] is not None:
        _fail()
    reference = value['credentialReference']
    if not isinstance(reference, dict) or set(reference) != {'mode', 'reference'} or reference['mode'] != 'keychain':
        _fail()
    validate_reference(reference['reference'], lab_only=kind != 'source_replace')
    states = {'replace_pending', 'replace_confirmed', 'replace_not_applied'} if kind == 'source_replace' else {
        'import_pending', 'import_confirmed', 'collected', 'cleanup_pending', 'cleanup_confirmed'}
    if value['state'] not in states or (kind == 'source_replace' and value['cleanupAllowed']):
        _fail()
    if 'settings' in value:
        if kind != 'one_time' or value['provider'] != 's3' or not isinstance(value['settings'], dict):
            _fail()
        if set(value['settings']) - {'bucket', 'region', 'endpoint'}:
            _fail()
        for setting in value['settings'].values():
            _text(setting, 4096)
    return value


def _receipt_value(provider, target, run, reference, kind, state, cleanup_allowed):
    return _receipt_shape({'schema': 1, 'provider': provider, 'target': target, 'run': run,
        'kind': kind, 'credentialReference': {'mode': 'keychain', 'reference': reference},
        'state': state, 'cleanupAllowed': cleanup_allowed})


def _receipt_bytes(value):
    body = (json.dumps(_receipt_shape(value), separators=(',', ':'), ensure_ascii=True) + '\n').encode('ascii')
    if len(body) > MAX_RECEIPT:
        _fail()
    return body


def _private_receipt(info):
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1:
        _fail()


def _write_receipt(descriptor, body):
    with os.fdopen(descriptor, 'wb') as output:
        _private_receipt(os.fstat(output.fileno()))
        output.write(body)
        output.flush()
        os.fsync(output.fileno())


def _create_receipt(path, value):
    body = _receipt_bytes(value)
    directory = None
    descriptor = None
    try:
        path, directory = _receipt_directory(path)
        descriptor = os.open(path.name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=directory)
        owned_descriptor, descriptor = descriptor, None
        _write_receipt(owned_descriptor, body)
        os.fsync(directory)
        return str(path)
    except (OSError, TypeError, ValueError):
        _fail()
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if directory is not None:
            os.close(directory)


def _read_receipt(directory, name):
    descriptor = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    with os.fdopen(descriptor, 'rb') as source:
        info = os.fstat(source.fileno())
        _private_receipt(info)
        raw = source.read(MAX_RECEIPT + 1)
    if len(raw) > MAX_RECEIPT:
        _fail()
    return _receipt_shape(json.loads(raw)), info


def _load_receipt(path):
    directory = None
    try:
        path, directory = _receipt_directory(path)
        value, _ = _read_receipt(directory, path.name)
        return str(path), value
    except (OSError, TypeError, ValueError, UnicodeError):
        _fail()
    finally:
        if directory is not None:
            os.close(directory)


def _update_receipt(path, expected, **changes):
    """Atomically replace only the exact protected receipt, then sync its directory."""
    value = {**expected, **changes}
    body = _receipt_bytes(value)
    directory = None
    temporary = None
    try:
        path, directory = _receipt_directory(path)
        existing, info = _read_receipt(directory, path.name)
        if existing == value:
            os.fsync(directory)
            return value
        if existing != expected:
            _fail()
        temporary_name = '.' + path.name + '.' + uuid.uuid4().hex
        descriptor = os.open(temporary_name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=directory)
        temporary = temporary_name
        _write_receipt(descriptor, body)
        current = os.stat(path.name, dir_fd=directory, follow_symlinks=False)
        _private_receipt(current)
        if (current.st_dev, current.st_ino) != (info.st_dev, info.st_ino):
            _fail()
        os.replace(temporary, path.name, src_dir_fd=directory, dst_dir_fd=directory)
        temporary = None
        os.fsync(directory)
        return value
    except (OSError, TypeError, ValueError, UnicodeError):
        _fail()
    finally:
        if temporary is not None and directory is not None:
            try:
                os.unlink(temporary, dir_fd=directory)
            except OSError:
                pass
        if directory is not None:
            os.close(directory)


def cleanup(request):
    """Delete an exact receipt-owned LAB entry; an absent entry is successful."""
    if not isinstance(request, dict):
        _fail()
    provider = request.get('provider')
    guest = 'run' in request
    fields = {'op', 'provider', 'reference', 'receipt'} | ({'run', 'guestBroker'} if guest else {'hostBroker'})
    if set(request) != fields or request['op'] != 'cleanup' or provider not in ('s3', 'proton'):
        _fail()
    reference = validate_reference(request['reference'], lab_only=True)
    path, value = _load_receipt(request['receipt'])
    if value['provider'] != provider or value['credentialReference']['reference'] != reference:
        _fail()
    if value['target'] != ('guest' if guest else 'host') or value['run'] != (_run_name(request['run']) if guest else None):
        _fail()
    if value['kind'] == 'source_replace' or not value['cleanupAllowed']:
        _fail()
    if value['state'] == 'cleanup_confirmed':
        return value
    value = _update_receipt(path, value, state='cleanup_pending')
    body = {'op': 'delete', 'reference': reference}
    if guest:
        guest_call(request['run'], request['guestBroker'], provider, body)
    else:
        host_call(request['hostBroker'], provider, body)
    return _update_receipt(path, value, state='cleanup_confirmed')


class ProtonSessionChain:
    """Keep baseline and rotating snapshots only in one host process's memory."""
    def __init__(self, host_broker, source_reference):
        validate_reference(source_reference)
        self.host_broker = host_broker
        self.source_reference = source_reference
        self.baseline = host_call(host_broker, 'proton', {'op': 'export', 'reference': source_reference})['snapshot']
        self.current = self.baseline
        self.current_saved = True
        self.active = None
        self.owned = {}
        self.host_owned = {}
        self.host_receipts = {}
        self.pending_source = None

    def provision(self, run, guest_broker, receipt=None, reference=None):
        """reference reuses an earlier, cleaned-up guest name, so that guest's put.gd can finish
        operations recorded under it; the import still refuses an existing entry."""
        if self.active is not None or self.pending_source is not None:
            _fail()
        reference = 'putgd-lab-' + str(uuid.uuid4()) if reference is None else validate_reference(reference, lab_only=True)
        run = _run_name(run)
        value = _receipt_value('proton', 'guest', run, reference, 'chain_guest', 'import_pending', True)
        path = _create_receipt(receipt, value)
        owned = {'run': run, 'broker': guest_broker, 'receipt': path, 'value': value}
        self.active = reference
        self.owned[reference] = owned
        guest_call(run, guest_broker, 'proton', {'op': 'import', 'reference': reference, 'snapshot': self.current})
        owned['value'] = _update_receipt(path, value, state='import_confirmed', cleanupAllowed=False)
        return reference

    def _owned_guest(self, run, guest_broker, reference):
        owned = self.owned.get(reference)
        if owned is None or owned['run'] != _run_name(run) or owned['broker'] != guest_broker:
            _fail()
        return owned

    def collect(self, run, guest_broker, reference):
        owned = self._owned_guest(run, guest_broker, reference)
        if self.active != reference or owned['value']['state'] != 'import_confirmed':
            _fail()
        snapshot = guest_call(run, guest_broker, 'proton', {'op': 'export', 'reference': reference})['snapshot']
        owned['value'] = _update_receipt(owned['receipt'], owned['value'], state='collected', cleanupAllowed=False)
        self.current = snapshot
        self.current_saved = False
        self.active = None

    def abandon(self, run, guest_broker, reference):
        """Only an unconfirmed import can be abandoned; SDK work never started."""
        owned = self._owned_guest(run, guest_broker, reference)
        if self.active != reference or owned['value']['state'] != 'import_pending':
            _fail()
        self._permit_unreturned_import_cleanup(owned)
        cleanup({'op': 'cleanup', 'provider': 'proton', 'run': owned['run'], 'guestBroker': guest_broker,
                 'reference': reference, 'receipt': owned['receipt']})
        del self.owned[reference]
        self.active = None

    def _permit_unreturned_import_cleanup(self, owned):
        # The broker may have replied before receipt durability failed. No SDK
        # work was permitted because the provision/carry_back call did not return.
        path, actual = _load_receipt(owned['receipt'])
        expected = owned['value']
        if actual != {**expected, 'state': actual['state'], 'cleanupAllowed': actual['cleanupAllowed']}:
            _fail()
        if actual['state'] not in ('import_pending', 'import_confirmed', 'cleanup_pending', 'cleanup_confirmed'):
            _fail()
        if not actual['cleanupAllowed']:
            _update_receipt(path, actual, cleanupAllowed=True)

    def recover(self, receipt, reference, run=None, guest_broker=None):
        """Resume a confirmed copied session in a fresh, quiescent controller."""
        if self.active is not None or self.pending_source is not None or not self.current_saved or self.owned or self.host_receipts:
            _fail()
        validate_reference(reference, lab_only=True)
        path, value = _load_receipt(receipt)
        if value['provider'] != 'proton' or value['credentialReference']['reference'] != reference:
            _fail()
        if value['kind'] == 'chain_guest':
            if value['run'] != _run_name(run) or value['state'] not in ('import_confirmed', 'collected'):
                _fail()
            snapshot = guest_call(run, guest_broker, 'proton', {'op': 'export', 'reference': reference})['snapshot']
            value = _update_receipt(path, value, state='collected', cleanupAllowed=False)
            self.owned[reference] = {'run': value['run'], 'broker': guest_broker, 'receipt': path, 'value': value}
        elif value['kind'] == 'host_lab' and value['state'] == 'import_confirmed' and run is None and guest_broker is None:
            snapshot = host_call(self.host_broker, 'proton', {'op': 'export', 'reference': reference})['snapshot']
            value = _update_receipt(path, value, cleanupAllowed=False)
            self.host_owned[reference] = 'import_confirmed'
            self.host_receipts[reference] = {'receipt': path, 'value': value}
        else:
            _fail()
        self.current = snapshot
        self.current_saved = False

    def _confirm_saved(self, source_saved):
        if source_saved:
            for owned in self.host_receipts.values():
                if owned['value']['state'] == 'import_confirmed':
                    owned['value'] = _update_receipt(owned['receipt'], owned['value'], cleanupAllowed=True)
        for owned in self.owned.values():
            if owned['value']['state'] == 'collected':
                owned['value'] = _update_receipt(owned['receipt'], owned['value'], cleanupAllowed=True)
        self.current_saved = True

    def carry_back(self, receipt=None, new_lab_reference=False):
        if self.pending_source is not None or (self.active is not None and self.owned[self.active]['value']['state'] != 'import_pending'):
            _fail()
        if new_lab_reference:
            reference = 'putgd-lab-' + str(uuid.uuid4())
            request = {'op': 'import', 'reference': reference, 'snapshot': self.current}
            value = _receipt_value('proton', 'host', None, reference, 'host_lab', 'import_pending', True)
        else:
            reference = self.source_reference
            request = {'op': 'replace', 'reference': reference, 'expectedSnapshot': self.baseline, 'snapshot': self.current}
            value = _receipt_value('proton', 'host', None, reference, 'source_replace', 'replace_pending', False)
        path = _create_receipt(receipt, value)
        if new_lab_reference:
            self.host_owned[reference] = 'import_pending'
            self.host_receipts[reference] = {'receipt': path, 'value': value}
        else:
            self.pending_source = {'receipt': path, 'value': value, 'baseline': self.baseline, 'snapshot': self.current}
            self.current_saved = False
        host_call(self.host_broker, 'proton', request)
        if new_lab_reference:
            value = _update_receipt(path, value, state='import_confirmed', cleanupAllowed=self.baseline == self.current)
            self.host_receipts[reference]['value'] = value
            self.host_owned[reference] = 'import_confirmed'
        else:
            self.baseline = self.current
            _update_receipt(path, value, state='replace_confirmed')
        self._confirm_saved(source_saved=not new_lab_reference)
        if not new_lab_reference:
            self.pending_source = None
        return reference

    def reconcile_carry_back(self, receipt):
        """Resolve one ambiguous source CAS with a private locked exact export."""
        pending = self.pending_source
        if pending is None or self.current != pending['snapshot']:
            _fail()
        path, value = _load_receipt(receipt)
        if path != pending['receipt'] or value['state'] not in ('replace_pending', 'replace_confirmed'):
            _fail()
        if value != {**pending['value'], 'state': value['state']}:
            _fail()
        observed = host_call(self.host_broker, 'proton', {'op': 'export', 'reference': self.source_reference})['snapshot']
        if observed == pending['snapshot']:
            self.baseline = observed
            _update_receipt(path, value, state='replace_confirmed')
            self._confirm_saved(source_saved=True)
            self.pending_source = None
            return True
        if observed == pending['baseline']:
            _update_receipt(path, value, state='replace_not_applied')
            self.baseline = observed
            self.pending_source = None
            return False
        _fail()  # Preserve the guest, pending receipt and in-memory current on conflict.

    def cleanup(self, run, guest_broker, reference):
        owned = self._owned_guest(run, guest_broker, reference)
        if self.active == reference or self.pending_source is not None or not self.current_saved or owned['value']['state'] != 'collected':
            _fail()
        cleanup({'op': 'cleanup', 'provider': 'proton', 'run': owned['run'], 'guestBroker': guest_broker,
                 'reference': reference, 'receipt': owned['receipt']})
        del self.owned[reference]  # Local keychain only; never remote logout/signout.

    def cleanup_host(self, reference):
        owned = self.host_receipts.get(reference)
        if owned is None or self.pending_source is not None:
            _fail()
        if owned['value']['state'] == 'import_pending':
            self._permit_unreturned_import_cleanup(owned)
        cleanup({'op': 'cleanup', 'provider': 'proton', 'hostBroker': self.host_broker,
                 'reference': reference, 'receipt': owned['receipt']})
        del self.host_receipts[reference]
        del self.host_owned[reference]


def provision(request):
    if not isinstance(request, dict):
        _fail()
    provider = request.get('provider')
    required = {'provider', 'run', 'guestBroker', 'receipt'} | ({'credentialsFile'} if provider == 's3' else {'hostBroker', 'sourceReference'})
    if provider not in ('s3', 'proton') or set(request) - required - {'op'} or not required <= set(request) or request.get('op', 'provision') != 'provision':
        _fail()
    reference = 'putgd-lab-' + str(uuid.uuid4())
    result = _receipt_value(provider, 'guest', _run_name(request['run']), reference, 'one_time', 'import_pending', True)
    if provider == 's3':
        parsed = parse_s3_credentials(read_private_credentials(request['credentialsFile']))
        body = {'op': 'import', 'reference': reference, 'credentials': parsed['credentials']}
        result['settings'] = parsed['settings']
    else:
        snapshot = host_call(request['hostBroker'], 'proton', {'op': 'export', 'reference': request['sourceReference']})['snapshot']
        body = {'op': 'import', 'reference': reference, 'snapshot': snapshot}
    path = _create_receipt(request['receipt'], result)
    guest_call(request['run'], request['guestBroker'], provider, body)
    return _update_receipt(path, result, state='import_confirmed')


def main(source=None):
    action = 'provision'
    try:
        raw = (source or sys.stdin.buffer).read(MAX_INPUT + 1)
        if len(raw) > MAX_INPUT:
            _fail()
        request = json.loads(raw)
        if isinstance(request, dict) and request.get('op') == 'cleanup':
            action = 'cleanup'
            cleanup(request)
        else:
            provision(request)
        print(json.dumps({'ok': True, 'status': 'cleaned' if action == 'cleanup' else 'provisioned'}, separators=(',', ':')))
        return 0
    except Exception:
        # Neither exception text, process stdout/stderr nor the input is public.
        print(json.dumps({'ok': False, 'status': 'cleanup_failed' if action == 'cleanup' else 'provisioning_failed'}, separators=(',', ':')))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
