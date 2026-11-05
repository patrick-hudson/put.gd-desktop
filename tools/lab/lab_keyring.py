#!/usr/bin/env python3
"""Make a blank-password lab keyring the default Secret Service collection (stdlib + PyGObject).

Runs in a desktop session (a builder over SSH, or a disposable lab guest through
proton_lab.py lend). gnome-keyring gets a collection created through its
CreateWithMasterPassword call with an empty master password; KWallet (ksecretd)
gets a wallet created the way kwallet-pam creates one, from the empty password's
hash. Either is stored unencrypted, so it is open after every boot and autologin
with no prompt. Existing keyrings and wallets stay as they are; only the default
alias moves. Patrick approved blank lab keyrings on the builders on 2026-10-23.

  lab_keyring.py           # create if needed, set as default, print JSON
  lab_keyring.py --status  # the default collection's provider, label and lock state
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def secret_service(run=subprocess.run):
    """The default Secret Service collection's label and lock state (D-Bus properties only)."""
    def call(*arguments):
        result = run(['busctl', '--user', *arguments], capture_output=True, text=True, timeout=20)
        return result.stdout.strip() if result.returncode == 0 else None
    destination = os.environ.get('SECRET_SERVICE_BUS_NAME', 'org.freedesktop.secrets')
    owner = call('status', destination)
    provider = next((line.split('=', 1)[1] for line in (owner or '').splitlines() if line.startswith('Comm=')), None)
    alias = call('call', destination, '/org/freedesktop/secrets', 'org.freedesktop.Secret.Service',
                 'ReadAlias', 's', 'default')
    path = alias.split('"')[1] if alias and '"' in alias else '/'
    if path == '/':
        return {'provider': provider, 'default': None, 'locked': None}
    label = call('get-property', destination, path, 'org.freedesktop.Secret.Collection', 'Label')
    locked = call('get-property', destination, path, 'org.freedesktop.Secret.Collection', 'Locked')
    return {'provider': provider, 'default': path, 'label': label.split('"')[1] if label and '"' in label else None,
            'locked': None if locked is None else locked.endswith('true')}


LAB_KEYRING = 'putgd lab'


def lab_keyring():
    """gnome-keyring only: create a collection with an empty master password (stored unencrypted, so it
    is open after every boot and autologin, with no prompt) and make it the default alias. Existing
    keyrings stay as they are. Patrick approved blank lab keyrings on 2026-10-23."""
    import gi  # PyGObject, present with GNOME, Cinnamon and Plasma
    gi.require_version('Gio', '2.0')
    from gi.repository import Gio, GLib
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def call(path, interface, method, parameters, reply):
        return bus.call_sync(os.environ.get('SECRET_SERVICE_BUS_NAME', 'org.freedesktop.secrets'), path, interface, method, parameters,
                             GLib.VariantType(reply), Gio.DBusCallFlags.NONE, 20000, None).unpack()

    service = '/org/freedesktop/secrets'
    current = call(service, 'org.freedesktop.Secret.Service', 'ReadAlias', GLib.Variant('(s)', ('default',)), '(o)')[0]
    if secret_service().get('provider') == 'ksecretd':
        path = kwallet_lab_wallet(call, GLib)
        call(service, 'org.freedesktop.Secret.Service', 'SetAlias', GLib.Variant('(so)', ('default', path)), '()')
        restarted = refresh_ksecretd_alias(bus, call, Gio, GLib)
        return {'collection': path, 'previousDefault': None if current == '/' else current, 'restarted': restarted}
    for path in call(service, 'org.freedesktop.DBus.Properties', 'Get', GLib.Variant(
            '(ss)', ('org.freedesktop.Secret.Service', 'Collections')), '(v)')[0]:
        label = call(path, 'org.freedesktop.DBus.Properties', 'Get', GLib.Variant(
            '(ss)', ('org.freedesktop.Secret.Collection', 'Label')), '(v)')[0]
        if label == LAB_KEYRING:
            break
    else:
        # The session belongs to this connection; the empty secret is the (blank) master password.
        session = call(service, 'org.freedesktop.Secret.Service', 'OpenSession',
                       GLib.Variant('(sv)', ('plain', GLib.Variant('s', ''))), '(vo)')[1]
        path = call(service, 'org.gnome.keyring.InternalUnsupportedGuiltRiddenInterface', 'CreateWithMasterPassword',
                    GLib.Variant('(a{sv}(oayays))', ({'org.freedesktop.Secret.Collection.Label':
                                                      GLib.Variant('s', LAB_KEYRING)},
                                                     (session, b'', b'', 'text/plain'))), '(o)')[0]
    call(service, 'org.freedesktop.Secret.Service', 'SetAlias', GLib.Variant('(so)', ('default', path)), '()')
    return {'collection': path, 'previousDefault': None if current == '/' else current}


def refresh_ksecretd_alias(bus, call, Gio, GLib):
    """ksecretd builds its aliases/default object when it starts: SetAlias moves ReadAlias and the
    default wallet, but libsecret writes through aliases/default, which keeps the old wallet until
    ksecretd restarts. Stop exactly the process that owns the name, then activate it again."""
    def alias_label():
        return call('/org/freedesktop/secrets/aliases/default', 'org.freedesktop.DBus.Properties', 'Get',
                    GLib.Variant('(ss)', ('org.freedesktop.Secret.Collection', 'Label')), '(v)')[0]
    if alias_label() == KWALLET_LAB:
        return False
    pid = bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                        'GetConnectionUnixProcessID', GLib.Variant('(s)', ('org.freedesktop.secrets',)),
                        GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, 20000, None).unpack()[0]
    if Path(f'/proc/{pid}/comm').read_text().strip() != 'ksecretd':
        raise RuntimeError('org.freedesktop.secrets is not owned by ksecretd')
    os.kill(pid, 15)
    deadline = time.monotonic() + 20
    while Path(f'/proc/{pid}').exists():
        if time.monotonic() > deadline:
            raise RuntimeError('ksecretd did not stop')
        time.sleep(.2)
    # org.kde.secretservicecompat activates ksecretd; org.freedesktop.secrets alone is not activatable.
    bus.call_sync('org.kde.secretservicecompat', '/', 'org.freedesktop.DBus.Peer', 'Ping', None, None,
                  Gio.DBusCallFlags.NONE, 20000, None)
    if alias_label() != KWALLET_LAB:
        raise RuntimeError('aliases/default still names another wallet after restarting ksecretd')
    return True


KWALLET_LAB = 'putgdlab'


def kwallet_lab_wallet(call, GLib):
    """KWallet (ksecretd): create the blank-password wallet the way kwallet-pam creates kdewallet, from
    a PBKDF2-SHA512 hash (50000 rounds, 56 bytes) of the empty password and a new 56-byte salt. kwalletd
    opens a wallet whose password is empty without asking, so it is open after every boot."""
    import hashlib
    wallets = call('/ksecretd', 'org.kde.KWallet', 'wallets', None, '(as)')[0]
    if KWALLET_LAB not in wallets:
        folder = Path.home() / '.local/share/kwalletd'
        folder.mkdir(mode=0o700, parents=True, exist_ok=True)
        salt_path = folder / f'{KWALLET_LAB}.salt'
        if not salt_path.exists():
            with open(os.open(salt_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as stream:
                stream.write(os.urandom(56))
        digest = hashlib.pbkdf2_hmac('sha512', b'', salt_path.read_bytes(), 50000, dklen=56)
        call('/ksecretd', 'org.kde.KWallet', 'pamOpen', GLib.Variant('(sayi)', (KWALLET_LAB, digest, 0)), '()')
        deadline = time.monotonic() + 20
        while KWALLET_LAB not in call('/ksecretd', 'org.kde.KWallet', 'wallets', None, '(as)')[0]:
            if time.monotonic() > deadline:
                raise RuntimeError('KWallet did not create the lab wallet')
            time.sleep(.5)
    return '/org/freedesktop/secrets/collection/' + KWALLET_LAB


if __name__ == '__main__':
    print(json.dumps(secret_service() if sys.argv[1:] == ['--status'] else lab_keyring()))
