#!/usr/bin/env python3
"""Stdin-only provisioning for disposable Linux lab keychains. Never print secrets.

The public CLI consumes one bounded JSON request and prints a generic status. A
request may name a new private receipt path for credential references/settings.
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


def exchange(arguments, body, timeout=30):
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
    return _reply(exchange([program], body), provider, request)


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
    return _reply(exchange(arguments, script), provider, request)


class ProtonSessionChain:
    """Keep baseline and rotating snapshots only in one host process's memory."""
    def __init__(self, host_broker, source_reference):
        validate_reference(source_reference)
        self.host_broker = host_broker
        self.source_reference = source_reference
        self.baseline = host_call(host_broker, 'proton', {'op': 'export', 'reference': source_reference})['snapshot']
        self.current = self.baseline
        self.active = None
        self.owned = {}

    def provision(self, run, guest_broker):
        if self.active is not None:
            _fail()
        reference = 'putgd-lab-' + str(uuid.uuid4())
        self.active = reference
        self.owned[reference] = (str(run), guest_broker)
        # Retain the exact nonsecret reference before a possibly ambiguous SSH
        # result so the caller can inspect/clean up a partial provisioning.
        guest_call(run, guest_broker, 'proton', {'op': 'import', 'reference': reference, 'snapshot': self.current})
        return reference

    def collect(self, run, guest_broker, reference):
        if self.active != reference or self.owned.get(reference) != (str(run), guest_broker):
            _fail()
        self.current = guest_call(run, guest_broker, 'proton', {'op': 'export', 'reference': reference})['snapshot']
        self.active = None

    def carry_back(self, new_lab_reference=False):
        if self.active is not None:
            _fail()
        if new_lab_reference:
            reference = 'putgd-lab-' + str(uuid.uuid4())
            request = {'op': 'import', 'reference': reference, 'snapshot': self.current}
        else:
            reference = self.source_reference
            request = {'op': 'replace', 'reference': reference, 'expectedSnapshot': self.baseline, 'snapshot': self.current}
        host_call(self.host_broker, 'proton', request)
        if not new_lab_reference:
            self.baseline = self.current
        return reference

    def cleanup(self, run, guest_broker, reference):
        if self.active == reference or self.owned.get(reference) != (str(run), guest_broker):
            _fail()
        guest_call(run, guest_broker, 'proton', {'op': 'delete', 'reference': reference})
        del self.owned[reference]  # Local keychain only; never remote logout/signout.


def provision(request):
    if not isinstance(request, dict):
        _fail()
    provider = request.get('provider')
    required = {'provider', 'run', 'guestBroker'} | ({'credentialsFile'} if provider == 's3' else {'hostBroker', 'sourceReference'})
    if provider not in ('s3', 'proton') or set(request) - required - {'receipt'} or not required <= set(request):
        _fail()
    reference = 'putgd-lab-' + str(uuid.uuid4())
    result = {'provider': provider, 'credentialReference': {'mode': 'keychain', 'reference': reference}}
    if provider == 's3':
        parsed = parse_s3_credentials(read_private_credentials(request['credentialsFile']))
        body = {'op': 'import', 'reference': reference, 'credentials': parsed['credentials']}
        result['settings'] = parsed['settings']
    else:
        snapshot = host_call(request['hostBroker'], 'proton', {'op': 'export', 'reference': request['sourceReference']})['snapshot']
        body = {'op': 'import', 'reference': reference, 'snapshot': snapshot}
    if 'receipt' in request:
        try:
            # Retain the reference before a potentially ambiguous guest reply.
            # Fresh file creation cannot replace application config/evidence.
            descriptor = os.open(request['receipt'], os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(descriptor, 'w') as output:
                json.dump(result, output)
                output.write('\n')
        except OSError:
            _fail()
    guest_call(request['run'], request['guestBroker'], provider, body)
    return result


def main(source=None):
    try:
        raw = (source or sys.stdin.buffer).read(MAX_INPUT + 1)
        if len(raw) > MAX_INPUT:
            _fail()
        request = json.loads(raw)
        provision(request)
        print('{"ok":true,"status":"provisioned"}')
        return 0
    except Exception:
        # Neither exception text, process stdout/stderr nor the input is public.
        print('{"ok":false,"status":"provisioning_failed"}')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
