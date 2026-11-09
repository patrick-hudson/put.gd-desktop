"""Safety/shape contracts only. Real compositor evidence is collected separately."""
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/lab'))
import native_acceptance as driver
import native_keychain


class Safety(unittest.TestCase):
    def test_login_check_rejects_prior_shell_and_survivor(self):
        old = {'Id': 'old', 'TimestampMonotonic': '1000000', 'bootId': 'boot', 'shell': {'pid': 1}}
        before = {'prepared': True, 'oldSession': old}
        new = dict(old, Id='new', TimestampMonotonic='2000000', shell={'pid': 2})
        hz = os.sysconf('SC_CLK_TCK')
        self.assertTrue(driver.check_login(before, new, [{'startTicks': 3 * hz}])['newSessionVerified'])
        with self.assertRaises(driver.Refused): driver.check_login(before, old, [{'startTicks': 3 * hz}])
        with self.assertRaises(driver.Refused): driver.check_login(before, new, [{'startTicks': hz}])
        with self.assertRaises(driver.Refused): driver.check_login(before, new, [])
        self.assertTrue(driver.check_login(before, new, [], expected='absent')['newSessionVerified'])
        with self.assertRaises(driver.Refused): driver.check_login(before, new, [{'startTicks': 3 * hz}], expected='absent')

    def test_post_login_ssh_start_is_presence_only_not_autostart_proof(self):
        old = {'Id': 'old', 'TimestampMonotonic': '1000000', 'bootId': 'boot', 'shell': {'pid': 1}}
        before = {'prepared': True, 'oldSession': old, 'stopped': {'exe': '/usr/bin/putgd'}}
        new = dict(old, Id='new', TimestampMonotonic='2000000', shell={'pid': 2})
        # An SSH/user-scope start after login has the same executable, runtime
        # and generation properties as an autostarted process. Inventory alone
        # cannot tell them apart and must never certify desktop autostart.
        ssh_started = {'pid': 3, 'exe': '/usr/bin/putgd', 'startTicks': 3 * os.sysconf('SC_CLK_TCK')}
        present = driver.check_login(before, new, [ssh_started])
        absent = driver.check_login(before, new, [], expected='absent')
        for expected, result in (('present', present), ('absent', absent)):
            self.assertTrue(result['newSessionVerified'])
            self.assertTrue(result['postLoginStateVerified'])
            self.assertEqual(result['expected'], expected)
            self.assertFalse(result['autostartProven'])

    def test_keychain_rejects_bad_receipt_before_unlocking_changed_default(self):
        reference = 'putgd-acceptance-12345678-1234-1234-1234-123456789abc'
        # The new default is also an approved, locked lab wallet. It must not
        # receive Unlock/Prompt or item calls on behalf of an older receipt.
        for phase in ('check', 'cleanup'):
            for before in (None, {'reference': 'another-app', 'defaultCollection': '/lab/new'},
                           {'reference': reference, 'defaultCollection': '/lab/original'}):
                calls = []
                def call_sync(service, path, interface, method, values, *rest):
                    calls.append(method)
                    if method == 'ReadAlias': result = ('/lab/new',)
                    elif method == 'GetConnectionUnixProcessID': result = (123,)
                    elif method == 'Get' and values[1] == 'Locked': result = (True,)
                    elif method == 'Get' and values[1] == 'Label': result = ('putgdlab',)
                    else: raise AssertionError('invalid receipt reached keychain action: ' + method)
                    return SimpleNamespace(unpack=lambda: result)
                gio = SimpleNamespace(bus_get_sync=lambda *a: SimpleNamespace(call_sync=call_sync),
                                      BusType=SimpleNamespace(SESSION=0), DBusCallFlags=SimpleNamespace(NONE=0))
                glib = SimpleNamespace(Variant=lambda signature, value: value, VariantType=lambda value: value,
                                       Error=type('BackendError', (Exception,), {}))
                gi = SimpleNamespace(require_version=lambda *a: None)
                with self.subTest(phase=phase, before=before), \
                     mock.patch.dict(sys.modules, {'gi': gi, 'gi.repository': SimpleNamespace(Gio=gio, GLib=glib)}), \
                     mock.patch.dict(os.environ), mock.patch.object(native_keychain.os, 'readlink', return_value='/usr/bin/ksecretd'):
                    with self.assertRaisesRegex(driver.Refused, 'receipt|changed since store'):
                        native_keychain.lifecycle(phase, {'DBUS_SESSION_BUS_ADDRESS': 'owned-test-bus', 'XDG_RUNTIME_DIR': '/tmp'},
                                                  before, unlock_lab=True)
                    self.assertTrue(set(calls).isdisjoint({'Unlock', 'Prompt', 'SearchItems', 'OpenSession', 'CreateItem', 'GetSecret', 'Delete'}))

    def test_keychain_cleanup_reference_is_scoped(self):
        self.assertTrue(native_keychain.valid_reference('putgd-acceptance-12345678-1234-1234-1234-123456789abc'))
        for ref in ('putgd/proton/lab-f44', 'login', 'putgd-acceptance-/../../default', 'putgd-acceptance-' + '-' * 36):
            self.assertFalse(native_keychain.valid_reference(ref))

    def test_keychain_provider_metadata_does_not_replace_ownership_checks(self):
        ref = 'putgd-acceptance-12345678-1234-1234-1234-123456789abc'
        attributes = {'application': 'putgd-native-acceptance', 'reference': ref, 'xdg:schema': 'org.freedesktop.Secret.Generic'}
        self.assertTrue(native_keychain.owns_probe(attributes, ref))
        self.assertFalse(native_keychain.owns_probe(dict(attributes, reference='another-reference'), ref))
        self.assertFalse(native_keychain.owns_probe(dict(attributes, application='another-app'), ref))

    def test_failed_store_removes_only_new_owned_item_and_preserves_original_error(self):
        removed = []
        recovery = {'reference': 'putgd-acceptance-12345678-1234-1234-1234-123456789abc', 'defaultCollection': '/lab'}
        with self.assertRaises(driver.Refused) as caught:
            with native_keychain.failed_store_guard(recovery, removed.append) as owned:
                owned.update(item='/lab/new-probe', attempted=True)
                raise driver.Refused('synthetic read verification failed')
        self.assertEqual(removed, ['/lab/new-probe'])
        self.assertEqual(str(caught.exception), 'synthetic read verification failed')
        self.assertEqual(caught.exception.recovery, dict(recovery, cleanup='removed'))

    def test_failed_store_retains_exact_recovery_reference_when_cleanup_refused(self):
        def refused(item): raise driver.Refused('ownership changed')
        with self.assertRaises(driver.Refused) as caught:
            with native_keychain.failed_store_guard({'reference': 'owned-reference'}, refused) as owned:
                owned.update(item='/lab/new-probe', attempted=True)
                raise driver.Refused('original store failure')
        self.assertEqual(str(caught.exception), 'original store failure')
        self.assertEqual(caught.exception.recovery, {'reference': 'owned-reference', 'cleanup': 'needs-exact-cleanup'})

    def test_ambiguous_create_failure_preserves_reference_without_guessing_item(self):
        cleanup = mock.Mock()
        with self.assertRaises(driver.Refused) as caught:
            with native_keychain.failed_store_guard({'reference': 'owned-reference'}, cleanup) as owned:
                owned['attempted'] = True
                raise driver.Refused('CreateItem transport failed')
        cleanup.assert_not_called()
        self.assertEqual(caught.exception.recovery['cleanup'], 'needs-exact-cleanup')


if __name__ == '__main__':
    unittest.main()
