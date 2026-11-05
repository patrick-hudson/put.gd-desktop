"""Non-destructive Secret Service lifecycle probe using a unique synthetic item.

 Existing collections are never reset, deleted, or made default. --unlock-lab
 may unlock only the existing approved blank lab collection. This proves
the backend only; a product setup/upload/restart is separate QtKeychain evidence.
PyGObject is loaded only on the desktop host, never needed by the safety tests.
"""
import os
from contextlib import contextmanager, nullcontext
from pathlib import Path
import re
import sys
import time
import uuid


def valid_reference(value):
    if not isinstance(value, str) or not value.startswith('putgd-acceptance-'):
        return False
    suffix = value[len('putgd-acceptance-'):]
    try:
        return str(uuid.UUID(suffix)) == suffix
    except ValueError:
        return False


def owns_probe(attributes, reference):
    # Providers may add metadata (gnome-keyring adds xdg:schema). Required
    # ownership attributes must still match exactly; additional metadata is
    # neither a credential nor authority to read/delete another reference.
    return (valid_reference(reference) and isinstance(attributes, dict)
            and attributes.get('application') == 'putgd-native-acceptance'
            and attributes.get('reference') == reference)


@contextmanager
def failed_store_guard(recovery, cleanup):
    """Keep the original failure and the exact recovery reference on all paths."""
    from native_acceptance import Refused
    owned = {'item': None, 'attempted': False}
    try:
        yield owned
    except Exception as error:
        status = 'needs-exact-cleanup' if owned['attempted'] else 'not-created'
        if owned['item'] is not None:
            try:
                cleanup(owned['item'])
                status = 'removed'
            except Exception:
                status = 'needs-exact-cleanup'
        message = str(error) if isinstance(error, Refused) else type(error).__name__
        raise Refused(message, dict(recovery, cleanup=status)) from None


def lifecycle(phase, env, before=None, unlock_lab=False, recovery_sink=lambda value: None):
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
    try:
        connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    except GLib.Error:
        raise Refused('Cannot connect to the current session Secret Service bus') from None
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
    try:
        provider_pid = connection.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'GetConnectionUnixProcessID', GLib.Variant('(s)', ('org.freedesktop.secrets',)),
            GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, 15000, None).unpack()[0]
        provider = Path(os.readlink(f'/proc/{provider_pid}/exe')).name
    except (GLib.Error, OSError):
        raise Refused('Cannot verify the current Secret Service provider') from None
    result = {'backend': 'org.freedesktop.secrets', 'provider': provider, 'defaultCollection': None if collection == '/' else collection,
              'scope': 'real Secret Service backend; not a product QtKeychain roundtrip', 'phase': phase}
    if collection == '/':
        result.update(available=False, prerequisite='No default collection; provision separately with existing authorization')
        require(phase == 'status', result['prerequisite'])
        return result
    # Validate the receipt BEFORE any Unlock/Prompt. Even another approved lab
    # wallet must stay untouched if the default alias changed since storage.
    if phase not in ('status', 'store'):
        require(before is not None and valid_reference(before.get('reference')), 'Need an owned private store receipt from this driver')
        reference = before['reference']
        require(before.get('defaultCollection') == collection, 'Default collection changed since store; refusing another keyring')
    locked = prop(collection, 'org.freedesktop.Secret.Collection', 'Locked')
    result.update(available=True, locked=locked)
    if phase == 'status':
        return result
    if locked:
        label = prop(collection, 'org.freedesktop.Secret.Collection', 'Label')
        require(unlock_lab and label in ('putgdlab', 'putgd lab'),
                'Default keychain is locked; inspect login input/state and use --unlock-lab only for the approved existing lab collection')
        require((provider, label) in (('ksecretd', 'putgdlab'), ('gnome-keyring-daemon', 'putgd lab')),
                'Lab provider/label does not match the approved existing collection')
        # These existing blank-password lab collections were explicitly approved.
        # A Secret Service prompt may be necessary even for a blank lab wallet.
        # Request it visibly, never type a password, and dismiss our own prompt
        # on refusal. No default alias or other collection is changed.
        unlocked_collections, prompt = call(service, 'org.freedesktop.Secret.Service', 'Unlock', '(ao)', ([collection],), '(aoo)')
        if prompt != '/':
            call(prompt, 'org.freedesktop.Secret.Prompt', 'Prompt', '(s)', ('',))
            deadline = time.monotonic() + 5
            while prop(collection, 'org.freedesktop.Secret.Collection', 'Locked') and time.monotonic() < deadline:
                time.sleep(.1)
            if prop(collection, 'org.freedesktop.Secret.Collection', 'Locked'):
                call(prompt, 'org.freedesktop.Secret.Prompt', 'Dismiss')
                raise Refused('Lab keychain requires operator-visible unlock; synthetic roundtrip not proven')
        require(not prop(collection, 'org.freedesktop.Secret.Collection', 'Locked'), 'Lab keychain remains locked')
        result.update(lockedBefore=True, locked=False, unlockedExistingLabCollection=True)
    if phase == 'store':
        reference = 'putgd-acceptance-' + str(uuid.uuid4())
    attrs = {'application': 'putgd-native-acceptance', 'reference': reference}
    unlocked, locked_items = call(service, 'org.freedesktop.Secret.Service', 'SearchItems', '(a{ss})', (attrs,), '(aoao)')
    require(not locked_items, 'Probe item is locked; no lifecycle pass')
    if phase == 'cleanup' and not unlocked:
        return dict(result, reference=reference, removed=True, alreadyAbsent=True)
    if phase == 'store':
        require(not unlocked, 'Synthetic reference already exists; refusing replacement')
    else:
        require(len(unlocked) == 1 and unlocked[0].startswith(collection + '/'), 'Expected one owned probe item in the original collection')
        require(owns_probe(prop(unlocked[0], 'org.freedesktop.Secret.Item', 'Attributes'), reference),
                'Probe ownership attributes changed; refusing read/delete')
    session = call(service, 'org.freedesktop.Secret.Service', 'OpenSession', '(sv)',
                   ('plain', GLib.Variant('s', '')), '(vo)')[1]
    synthetic = ('synthetic-backend-probe:' + reference).encode()
    recovery = dict(result, reference=reference)
    def remove_owned(item):
        require(item.startswith(collection + '/') and owns_probe(prop(item, 'org.freedesktop.Secret.Item', 'Attributes'), reference),
                'Cannot verify newly created probe ownership for failure cleanup')
        prompt = call(item, 'org.freedesktop.Secret.Item', 'Delete', reply='(o)')[0]
        require(prompt == '/', 'Failure cleanup requested a prompt')
        remaining = call(service, 'org.freedesktop.Secret.Service', 'SearchItems', '(a{ss})', (attrs,), '(aoao)')
        require(not any(remaining), 'New synthetic item remains after failure cleanup')
    guard = failed_store_guard(recovery, remove_owned) if phase == 'store' else nullcontext({'item': None})
    with guard as owned:
        try:
            if phase == 'store':
                # Persist the unique nonsecret reference BEFORE CreateItem. Even a
                # later receipt-I/O failure leaves enough information for safe cleanup.
                recovery_sink(dict(recovery, cleanup='not-yet-created'))
                props = {'org.freedesktop.Secret.Item.Label': GLib.Variant('s', 'put.gd acceptance synthetic probe'),
                         'org.freedesktop.Secret.Item.Attributes': GLib.Variant('a{ss}', attrs)}
                owned['attempted'] = True
                item, prompt = call(collection, 'org.freedesktop.Secret.Collection', 'CreateItem', '(a{sv}(oayays)b)',
                                    (props, (session, b'', synthetic, 'text/plain'), False), '(oo)')
                owned['item'] = item
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
            require(call(service, 'org.freedesktop.Secret.Service', 'ReadAlias', '(s)', ('default',), '(o)')[0] == collection,
                    'Default collection changed during the probe; no persistence claim')
        finally:
            unwinding = sys.exc_info()[0] is not None
            try:
                call(session, 'org.freedesktop.Secret.Session', 'Close')
            except Refused:
                recovery['sessionCloseFailed'] = True
                if not unwinding:
                    raise
    return result
