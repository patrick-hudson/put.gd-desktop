"""Non-destructive Secret Service lifecycle probe using a unique synthetic item.

No existing collection is unlocked, reset, deleted, or made default. This proves
the backend only; a product setup/upload/restart is separate QtKeychain evidence.
PyGObject is loaded only on the desktop host, never needed by the safety tests.
"""
import os
import re
import uuid


def valid_reference(value):
    return isinstance(value, str) and re.fullmatch(r'putgd-acceptance-[0-9a-f-]{36}', value) is not None


def lifecycle(phase, env, before=None):
    from native_acceptance import Refused, require
    # Gio uses the process bus, not a subprocess environment.
    for key in ('DBUS_SESSION_BUS_ADDRESS', 'XDG_RUNTIME_DIR'):
        os.environ[key] = env[key]
    try:
        import gi
        gi.require_version('Gio', '2.0')
        from gi.repository import Gio, GLib
    except ImportError:
        raise Refused('PyGObject is missing; use the distro python3-gi/python3-gobject package') from None
    connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    service = '/org/freedesktop/secrets'

    def call(path, interface, method, signature=None, values=(), reply=None):
        try:
            return connection.call_sync('org.freedesktop.secrets', path, interface, method,
                                        GLib.Variant(signature, values) if signature else None,
                                        GLib.VariantType(reply) if reply else None,
                                        Gio.DBusCallFlags.NONE, 15000, None).unpack()
        except GLib.Error:
            raise Refused('Secret Service refused the synthetic probe (no backend diagnostic retained)') from None

    def prop(path, interface, name):
        return call(path, 'org.freedesktop.DBus.Properties', 'Get', '(ss)', (interface, name), '(v)')[0]

    collection = call(service, 'org.freedesktop.Secret.Service', 'ReadAlias', '(s)', ('default',), '(o)')[0]
    result = {'backend': 'org.freedesktop.secrets', 'defaultCollection': None if collection == '/' else collection,
              'scope': 'real Secret Service backend; not a product QtKeychain roundtrip', 'phase': phase}
    if collection == '/':
        result.update(available=False, prerequisite='No default collection; provision separately with existing authorization')
        require(phase == 'status', result['prerequisite'])
        return result
    locked = prop(collection, 'org.freedesktop.Secret.Collection', 'Locked')
    result.update(available=True, locked=locked)
    if phase == 'status':
        return result
    require(not locked, 'Default keychain is locked; unlock through its visible console; no lifecycle pass')
    if phase == 'store':
        reference = 'putgd-acceptance-' + str(uuid.uuid4())
    else:
        require(before is not None and valid_reference(before.get('reference')), 'Need an owned private store receipt from this driver')
        reference = before['reference']
        require(before.get('defaultCollection') == collection, 'Default collection changed since store; refusing another keyring')
    attrs = {'application': 'putgd-native-acceptance', 'reference': reference}
    unlocked, locked_items = call(service, 'org.freedesktop.Secret.Service', 'SearchItems', '(a{ss})', (attrs,), '(aoao)')
    require(not locked_items, 'Probe item is locked; no lifecycle pass')
    if phase == 'store':
        require(not unlocked, 'Synthetic reference already exists; refusing replacement')
    else:
        require(len(unlocked) == 1 and unlocked[0].startswith(collection + '/'), 'Expected one owned probe item in the original collection')
        require(prop(unlocked[0], 'org.freedesktop.Secret.Item', 'Attributes') == attrs, 'Probe attributes changed; refusing read/delete')
    session = call(service, 'org.freedesktop.Secret.Service', 'OpenSession', '(sv)',
                   ('plain', GLib.Variant('s', '')), '(vo)')[1]
    synthetic = ('synthetic-backend-probe:' + reference).encode()
    try:
        if phase == 'store':
            props = {'org.freedesktop.Secret.Item.Label': GLib.Variant('s', 'put.gd acceptance synthetic probe'),
                     'org.freedesktop.Secret.Item.Attributes': GLib.Variant('a{ss}', attrs)}
            item, prompt = call(collection, 'org.freedesktop.Secret.Collection', 'CreateItem', '(a{sv}(oayays)b)',
                                (props, (session, b'', synthetic, 'text/plain'), False), '(oo)')
            require(prompt == '/' and item.startswith(collection + '/'), 'Keychain requested a prompt; no lifecycle pass')
        else:
            item = unlocked[0]
        if phase in ('store', 'check'):
            secret = call(item, 'org.freedesktop.Secret.Item', 'GetSecret', '(o)', (session,), '((oayays))')[0]
            require(bytes(secret[2]) == synthetic, 'Synthetic keychain value did not roundtrip')
            result.update(reference=reference, roundtrip=True)
        elif phase == 'cleanup':
            prompt = call(item, 'org.freedesktop.Secret.Item', 'Delete', reply='(o)')[0]
            require(prompt == '/', 'Synthetic cleanup requested a prompt')
            remaining = call(service, 'org.freedesktop.Secret.Service', 'SearchItems', '(a{ss})', (attrs,), '(aoao)')
            require(not any(remaining), 'Synthetic item remains after cleanup')
            result.update(reference=reference, removed=True)
    finally:
        call(session, 'org.freedesktop.Secret.Session', 'Close')
    return result
