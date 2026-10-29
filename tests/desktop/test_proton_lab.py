"""Proton lab lending contracts with synthetic values; no builder, VM, keychain or Proton service."""
import datetime
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('proton_lab', ROOT / 'tools/lab/proton_lab.py')
proton = importlib.util.module_from_spec(spec)
spec.loader.exec_module(proton)

START = datetime.datetime(2026, 10, 23, 22, 0, tzinfo=datetime.timezone.utc)
BASELINE, ROTATED = 'synthetic-baseline', 'synthetic-rotated'
BORROWED = 'putgd-lab-4bf87900-9e10-4cb0-8f32-0ce7f7f56f44'


class Chain:
    """Records the ProtonSessionChain calls a lend makes; holds only synthetic strings."""

    def __init__(self, calls, fail=None, reconcile=True):
        self.calls, self.fail, self.reconcile = calls, fail or {}, reconcile
        self.baseline = self.current = BASELINE
        self.active = self.pending_source = None

    def __call__(self, host_broker, reference):
        self.calls.append(('export', reference))
        return self

    def step(self, name):
        if self.fail.get(name):
            self.fail[name] -= 1
            raise proton.bridge.ProvisionError('credential_provisioning_failed')

    def provision(self, run, broker, receipt):
        self.calls.append(('provision', Path(receipt).name))
        Path(receipt).open('x').close()
        self.active = BORROWED
        self.step('provision')
        self.active = None
        return BORROWED

    def abandon(self, run, broker, reference):
        self.calls.append(('abandon', reference))
        self.active = None

    def collect(self, run, broker, reference):
        self.calls.append(('collect', reference))
        self.current = ROTATED

    def carry_back(self, receipt):
        self.calls.append(('carry_back', Path(receipt).name))
        Path(receipt).open('x').close()  # receipts are created exclusively, like the bridge's
        self.pending_source = {'receipt': receipt}
        self.step('carry_back')
        self.baseline, self.pending_source = self.current, None

    def reconcile_carry_back(self, receipt):
        self.calls.append(('reconcile', Path(receipt).name))
        self.pending_source = None
        if self.reconcile is None:
            raise proton.bridge.ProvisionError('credential_provisioning_failed')
        if self.reconcile:
            self.baseline = self.current
        return self.reconcile

    def cleanup(self, run, broker, reference):
        self.calls.append(('cleanup', reference))

    def recover(self, receipt, reference, run, broker):
        self.calls.append(('recover', reference))
        self.current = ROTATED


class Guest:
    def __init__(self, calls, run):
        self.calls, self.run, self.directory = calls, Path(run), None

    def place_brokers(self, source):
        self.calls.append(('place_brokers',))
        self.directory = '/home/lab/putgd-lab-bridge'
        return {'qt': self.directory + '/putgd-lab-secret-bridge', 'proton': self.directory + '/putgd-lab-proton-secrets'}

    def prepare_keyring(self):
        self.calls.append(('prepare_keyring',))
        return {'default': '/org/freedesktop/secrets/collection/putgd_5flab', 'locked': False}

    def stop_app(self):
        self.calls.append(('stop_app',))

    def remove_brokers(self):
        self.calls.append(('remove_brokers',))


class LendTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        base = Path(folder.name)
        self.layout = proton.Layout(base / 'proton', base / 'locks')
        self.layout.prepare()
        for directory in (self.layout.host_bridge, self.layout.guest_bridge):
            directory.mkdir(mode=0o700)
            for name in proton.BROKERS:
                (directory / name).write_text('#!/bin/sh\n')
                (directory / name).chmod(0o700)
        proton.write_registry(self.layout, {'schema': 1, 'host': 'd69', 'reference': 'putgd/proton/lab-d69',
                                            'signedIn': '2026-10-23T21:00:00Z'})
        self.run_dir = base / 'run'
        self.calls = []
        self.environments = []
        patcher = mock.patch.object(proton, 'lab_app_processes', lambda layout: [])
        patcher.start()
        self.addCleanup(patcher.stop)

    def clock(self):
        return START

    def lock(self):
        return proton.proton_lock(self.layout, 'd69', self.clock)

    def runner(self, exit_code=0):
        def run(command, env):
            self.calls.append(('test', tuple(command)))
            self.environments.append(env)
            return exit_code
        return run

    def lend(self, chain=None, exit_code=0, session='local_lender'):
        return proton.lend(self.layout, self.run_dir, session, 'lender', 'lend to d69 kde run',
                           ['python3', 'smoke.py'], chain_factory=chain or Chain(self.calls),
                           guest_factory=lambda run: Guest(self.calls, run), runner=self.runner(exit_code),
                           clock=self.clock, heartbeat=False)


class LendOrder(LendTest):
    def test_lends_tests_collects_carries_back_and_cleans_up_in_order_under_the_lock(self):
        result = self.lend()
        self.assertEqual([call[0] for call in self.calls], [
            'place_brokers', 'prepare_keyring', 'export', 'provision', 'test', 'stop_app', 'collect', 'carry_back', 'cleanup',
            'remove_brokers'])
        self.assertEqual(self.calls[2], ('export', 'putgd/proton/lab-d69'))
        self.assertEqual((result['testExit'], result['rotated'], result['reference']), (0, True, BORROWED))
        environment = self.environments[0]
        self.assertEqual(environment['PUTGD_LAB_PROTON_REFERENCE'], BORROWED)
        self.assertEqual(environment['PUTGD_LAB_RUN'], str(self.run_dir.resolve()))
        self.assertEqual(self.lock().status()['state'], 'free')
        self.assertEqual(self.lock().status()['last']['event'], 'release')
        progress = json.loads((Path(result['receipts']) / 'lend.json').read_text())
        self.assertEqual((progress['step'], progress['reference'], progress['source']),
                         ('cleaned', BORROWED, 'putgd/proton/lab-d69'))

    def test_receipts_and_progress_hold_no_session_material(self):
        result = self.lend()
        folder = Path(result['receipts'])
        self.assertEqual(folder.stat().st_mode & 0o777, 0o700)
        for path in folder.iterdir():
            self.assertNotIn(BASELINE, path.read_text())
            self.assertNotIn(ROTATED, path.read_text())
        self.assertNotIn(ROTATED, json.dumps(result))

    def test_the_registry_records_the_last_lend_without_session_material(self):
        result = self.lend()
        last = proton.read_registry(self.layout)['lastLend']
        self.assertEqual((last['run'], last['by'], last['testExit'], last['rotated'], last['carriedBack']),
                         (str(self.run_dir.resolve()), 'lender', 0, True, 'carry-back-1.json'))
        self.assertEqual(last['receipts'], result['receipts'])
        text = self.layout.registry.read_text()
        self.assertNotIn(BASELINE, text)
        self.assertNotIn(ROTATED, text)

    def test_a_failed_test_still_collects_and_carries_back(self):
        result = self.lend(exit_code=3)
        self.assertEqual(result['testExit'], 3)
        self.assertIn(('carry_back', 'carry-back-1.json'), self.calls)
        self.assertEqual(self.lock().status()['state'], 'free')

    def test_reuses_a_lock_this_session_already_holds_and_leaves_it_held(self):
        self.lock().acquire('lender', 'local_lender', 'campaign', 30)
        self.lend()
        state = self.lock().status()
        self.assertEqual((state['state'], state['lock']['session']), ('held', 'local_lender'))

    def test_refuses_while_another_session_holds_the_session(self):
        self.lock().acquire('other', 'local_other', 'host app check', 30)
        with self.assertRaises(proton.LabFailure) as refused:
            self.lend()
        self.assertIn('held by other (local_other)', str(refused.exception))
        self.assertNotIn('export', [call[0] for call in self.calls])

    def test_refuses_while_the_lab_profile_app_runs(self):
        with mock.patch.object(proton, 'lab_app_processes', lambda layout: [4242]):
            with self.assertRaises(proton.LabFailure):
                self.lend()
        self.assertEqual(self.calls, [])
        self.assertEqual(self.lock().status()['state'], 'free')

    def test_needs_a_verified_sign_in(self):
        proton.write_registry(self.layout, {'schema': 1, 'host': 'd69', 'reference': 'putgd/proton/lab-d69',
                                            'signedIn': None})
        with self.assertRaises(proton.LabFailure):
            self.lend()


class LendFailures(LendTest):
    def test_a_failed_import_is_abandoned_and_the_lock_released(self):
        with self.assertRaises(proton.LabFailure):
            self.lend(Chain(self.calls, fail={'provision': 1}))
        self.assertEqual([call[0] for call in self.calls], ['place_brokers', 'prepare_keyring', 'export', 'provision',
                                                          'abandon'])
        self.assertEqual(self.lock().status()['state'], 'free')

    def test_an_ambiguous_carry_back_that_applied_is_confirmed_by_reconciliation(self):
        self.lend(Chain(self.calls, fail={'carry_back': 1}, reconcile=True))
        names = [call for call in self.calls if call[0] in ('carry_back', 'reconcile', 'cleanup')]
        self.assertEqual(names, [('carry_back', 'carry-back-1.json'), ('reconcile', 'carry-back-1.json'),
                                 ('cleanup', BORROWED)])

    def test_a_carry_back_that_did_not_apply_is_retried_with_a_fresh_receipt(self):
        self.lend(Chain(self.calls, fail={'carry_back': 1}, reconcile=False))
        names = [call for call in self.calls if call[0] in ('carry_back', 'reconcile', 'cleanup')]
        self.assertEqual(names, [('carry_back', 'carry-back-1.json'), ('reconcile', 'carry-back-1.json'),
                                 ('carry_back', 'carry-back-2.json'), ('cleanup', BORROWED)])

    def test_a_conflict_keeps_the_guest_copy_and_leaves_the_lock_blocked(self):
        with self.assertRaises(proton.bridge.ProvisionError):
            self.lend(Chain(self.calls, fail={'carry_back': 1}, reconcile=None))
        self.assertNotIn('cleanup', [call[0] for call in self.calls])
        self.assertNotIn('remove_brokers', [call[0] for call in self.calls])
        state = self.lock().status()
        self.assertEqual((state['state'], state['lock']['session']), ('blocked', 'local_lender'))
        self.assertIn('run proton_lab.py recover', state['blocked'])

    def test_a_crashing_test_command_is_still_collected(self):
        def missing(command, env):
            raise FileNotFoundError(command[0])
        result = proton.lend(self.layout, self.run_dir, 'local_lender', 'lender', 'p', ['absent'],
                             chain_factory=Chain(self.calls), guest_factory=lambda run: Guest(self.calls, run),
                             runner=missing, clock=self.clock, heartbeat=False)
        self.assertEqual(result['testExit'], 127)
        self.assertIn(('cleanup', BORROWED), self.calls)


class GuestReference(LendTest):
    def test_lend_can_reuse_an_earlier_guest_name_to_finish_its_operations(self):
        given = {}
        chain = Chain(self.calls)
        original = chain.provision
        def provision(run, broker, receipt, reference=None):
            given['reference'] = reference
            return original(run, broker, receipt)
        chain.provision = provision
        proton.lend(self.layout, self.run_dir, 'local_lender', 'lender', 'revoke the earlier link', ['smoke'],
                    chain_factory=chain, guest_factory=lambda run: Guest(self.calls, run), runner=self.runner(),
                    clock=self.clock, heartbeat=False, guest_reference=BORROWED)
        self.assertEqual(given['reference'], BORROWED)
        self.assertIn(('cleanup', BORROWED), self.calls)


class Recovery(LendTest):
    def interrupted(self):
        with self.assertRaises(proton.bridge.ProvisionError):
            self.lend(Chain(self.calls, fail={'carry_back': 1}, reconcile=None))
        folder = next(self.layout.receipts.iterdir())
        (folder / 'provision.json').write_text(json.dumps({'state': 'collected', 'cleanupAllowed': False}))
        self.calls.clear()
        return folder

    def test_recover_carries_the_guest_copy_back_with_a_fresh_receipt_then_cleans_up(self):
        folder = self.interrupted()
        loaded = lambda path: (str(path), json.loads(Path(path).read_text()))
        with mock.patch.object(proton.bridge, '_load_receipt', loaded):
            result = proton.recover(self.layout, folder, 'local_lender', 'lender', chain_factory=Chain(self.calls),
                                    guest_factory=lambda run: Guest(self.calls, run), clock=self.clock)
        self.assertEqual([call[0] for call in self.calls], ['stop_app', 'export', 'recover', 'carry_back', 'cleanup',
                                                          'remove_brokers'])
        self.assertIn(('carry_back', 'carry-back-2.json'), self.calls)
        self.assertTrue(result['recovered'])
        self.assertEqual(self.lock().status()['state'], 'free')  # the failed lend's lock, now released

    def test_recover_leaves_a_lock_that_was_not_left_blocked_by_a_lend(self):
        folder = self.interrupted()
        self.lock().heartbeat('local_lender')  # the holder cleared the blocked reason by hand
        loaded = lambda path: (str(path), json.loads(Path(path).read_text()))
        with mock.patch.object(proton.bridge, '_load_receipt', loaded):
            proton.recover(self.layout, folder, 'local_lender', 'lender', chain_factory=Chain(self.calls),
                           guest_factory=lambda run: Guest(self.calls, run), clock=self.clock)
        self.assertEqual(self.lock().status()['state'], 'held')

    def test_a_pending_import_is_only_cleaned_up(self):
        folder = self.interrupted()
        (folder / 'provision.json').write_text(json.dumps({'state': 'import_pending', 'cleanupAllowed': True}))
        loaded = lambda path: (str(path), json.loads(Path(path).read_text()))
        cleaned = []
        with mock.patch.object(proton.bridge, '_load_receipt', loaded), \
                mock.patch.object(proton.bridge, 'cleanup', cleaned.append):
            result = proton.recover(self.layout, folder, 'local_lender', 'lender', chain_factory=Chain(self.calls),
                                    guest_factory=lambda run: Guest(self.calls, run), clock=self.clock)
        self.assertEqual(result['carriedBack'], None)
        self.assertEqual([request['reference'] for request in cleaned], [BORROWED])
        self.assertNotIn('export', [call[0] for call in self.calls])


class Resident:
    """A headless lab resident whose setup jobs end with synthetic outcomes."""

    def __init__(self, outcomes, folder='folder-uid'):
        self.outcomes, self.patches = outcomes, []
        self.settings = {'accountRootUid': '', 'folderUid': folder, 'folderName': 'putgd-lab-d69-check'}

    def setup(self, kind, params):
        return self.outcomes.get(kind, {'state': 'completed', 'result': {}})

    def patch_profile(self, changes):
        self.patches.append(changes)
        self.settings.update(changes)

    def profile(self):
        return {}, {'settings': self.settings}


class Checks(LendTest):
    def check(self, outcomes, **options):
        resident = Resident(outcomes)
        with mock.patch.object(proton, 'headless', lambda layout, registry, keychain: _context(resident)):
            return proton.check(self.layout, 'local_checker', 'checker', clock=self.clock, heartbeat=False,
                                **options), resident

    def test_a_passing_check_records_the_account_and_releases_the_lock(self):
        outcome, resident = self.check({'inspectAccount': {'state': 'completed', 'result': {
            'accountRootUid': 'synthetic-root', 'accountEmail': 'lab@example.invalid'}}})
        self.assertEqual((outcome['state'], outcome['passed']), ('ok', True))
        registry = proton.read_registry(self.layout)
        self.assertEqual((registry['session'], registry['account']), ('ok', 'lab@example.invalid'))
        self.assertEqual(resident.patches, [{'accountRootUid': 'synthetic-root'}])
        self.assertEqual(self.lock().status()['state'], 'free')

    def test_a_lost_session_is_reported_with_the_sign_in_again_action(self):
        failed = {'inspectAccount': {'state': 'failed', 'error': {'code': 'authentication_required'}}}
        with self.assertRaises(proton.SessionFailure) as raised:
            self.check(failed)
        self.assertIn('signs in again on this builder only', str(raised.exception))
        registry = proton.read_registry(self.layout)
        self.assertEqual((registry['session'], registry['lastCheck']['passed']), ('signed_out', False))
        status = proton.status(self.layout)['session']
        self.assertEqual(status['state'], 'signed_out')
        self.assertIn('signs in again', status['action'])
        self.assertEqual(self.lock().status()['state'], 'free')

    def test_a_locked_keyring_is_not_reported_as_a_lost_session(self):
        failed = {'testDestination': {'state': 'failed', 'error': {'code': 'keychain_unavailable'}}}
        with self.assertRaises(proton.SessionFailure):
            self.check(failed)
        self.assertEqual(proton.read_registry(self.layout)['session'], 'keychain_unavailable')
        self.assertEqual(proton.diagnose('rate_limited')['state'], 'service_error')

    def test_keepalive_skips_while_the_session_is_lent(self):
        self.lock().acquire('lender', 'local_lender', 'lend', 30)
        self.assertIn('held by lender', proton.keepalive(self.layout, clock=self.clock)['skipped'])
        self.lock().release('local_lender', verified_clean=True)
        with mock.patch.object(proton, 'headless', lambda layout, registry, keychain: _context(Resident({}))):
            later = lambda: START + datetime.timedelta(minutes=5)
            self.assertEqual(proton.keepalive(self.layout, clock=later)['state'], 'ok')
        self.assertEqual(proton.proton_lock(self.layout, 'd69', later).status()['last']['owner'], 'proton-keepalive')


def _context(value):
    import contextlib
    return contextlib.nullcontext(value)


class CommandLine(LendTest):
    def test_lend_takes_its_options_anywhere_and_the_test_command_after_the_separator(self):
        seen = {}
        def lend(layout, run, session, owner, purpose, command, guest_reference=None):
            seen.update(run=run, session=session, owner=owner, purpose=purpose, command=command)
            return {}
        with mock.patch.object(proton, 'lend', lend), mock.patch('sys.stdout'):
            self.assertEqual(proton.main(['lend', '/runs/r1', '--session', 'local_a', '--owner', 'o', '--purpose',
                                          'p', '--', 'python3', 'smoke.py', '--flag']), 0)
        self.assertEqual(seen, {'run': '/runs/r1', 'session': 'local_a', 'owner': 'o', 'purpose': 'p',
                                'command': ['python3', 'smoke.py', '--flag']})


class Registry(unittest.TestCase):
    def test_reference_is_per_builder_and_the_registry_is_private_nonsecret_json(self):
        self.assertEqual(proton.reference_for('m74'), 'putgd/proton/lab-m74')
        with self.assertRaises(proton.LabFailure):
            proton.reference_for('workstation')
        with tempfile.TemporaryDirectory() as folder:
            layout = proton.Layout(Path(folder) / 'proton', Path(folder) / 'locks')
            self.assertIsNone(proton.read_registry(layout))
            proton.write_registry(layout, {'schema': 1, 'host': 'f44', 'reference': 'putgd/proton/lab-f44'})
            self.assertEqual(proton.read_registry(layout)['reference'], 'putgd/proton/lab-f44')
            self.assertEqual(layout.registry, Path(folder) / 'locks' / 'proton-registry.json')
            self.assertEqual(layout.locks.stat().st_mode & 0o777, 0o700)
            layout.registry.write_text('{"schema": 2}')
            with self.assertRaises(proton.LabFailure):
                proton.read_registry(layout)

    def test_profile_environment_is_the_documented_lab_profile(self):
        layout = proton.Layout(Path('/home/patrick/putgd-lab/proton'), Path('/home/patrick/putgd-lab/locks'))
        self.assertEqual(layout.environment(), {'XDG_CONFIG_HOME': '/home/patrick/putgd-lab/proton/config',
                                                'XDG_DATA_HOME': '/home/patrick/putgd-lab/proton/data',
                                                'XDG_STATE_HOME': '/home/patrick/putgd-lab/proton/state'})

    def test_brokers_must_be_owned_and_not_writable_by_others(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            for name in proton.BROKERS:
                (directory / name).write_text('#!/bin/sh\n')
                (directory / name).chmod(0o700)
            self.assertEqual(proton.brokers(directory)['qt'], str(directory / 'putgd-lab-secret-bridge'))
            (directory / proton.BROKERS[1]).chmod(0o722)
            with self.assertRaises(proton.LabFailure):
                proton.brokers(directory)
            (directory / proton.BROKERS[1]).unlink()
            (directory / proton.BROKERS[1]).symlink_to(directory / proton.BROKERS[0])
            with self.assertRaises(proton.LabFailure):
                proton.brokers(directory)

    def test_register_guests_records_the_bridge_and_goldens_by_path_and_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            layout = proton.Layout(base / 'proton', base / 'locks')
            layout.prepare()
            proton.write_registry(layout, {'schema': 1, 'host': 'm74', 'reference': 'putgd/proton/lab-m74'})
            layout.guest_bridge.mkdir(mode=0o700)
            for name in proton.BROKERS:
                (layout.guest_bridge / name).write_text('#!/bin/sh\n')
                (layout.guest_bridge / name).chmod(0o700)
            golden, pins, key = base / 'image.qcow2', base / 'known_hosts', base / 'identity'
            golden.write_bytes(b'synthetic golden')
            pins.write_text('[127.0.0.1]:22220 ssh-ed25519 AAAA\n')
            key.write_text('synthetic key')
            guests = proton.register_guests(layout, key, [('gnome-wayland', golden, pins)], 'lender')
            self.assertEqual(proton.read_registry(layout)['guests'], guests)
            self.assertEqual(guests['bootstrapKey'], str(key))
            self.assertEqual(sorted(guests['bridge']), sorted(proton.BROKERS))
            self.assertEqual(guests['goldens'], [{'path': str(golden), 'bytes': 16, 'profile': 'gnome-wayland',
                                                  'sha256': proton.lab.hash_file(golden), 'knownHosts': str(pins)}])
            self.assertNotIn('synthetic key', layout.registry.read_text())
            with self.assertRaises(proton.LabFailure):
                proton.register_guests(layout, key, [('gnome-wayland', base / 'missing.qcow2', pins)])
            (layout.guest_bridge / proton.BROKERS[0]).chmod(0o722)
            with self.assertRaises(proton.LabFailure):
                proton.register_guests(layout, key, [('gnome-wayland', golden, pins)])

    def test_processes_are_matched_by_exact_environment_value(self):
        pids = proton.processes_with('PUTGD_SYNTHETIC_UNUSED', 'value-that-no-process-has')
        self.assertEqual(pids, [])


smoke_spec = importlib.util.spec_from_file_location('proton_lend_smoke', ROOT / 'tools/lab/proton_lend_smoke.py')
smoke = importlib.util.module_from_spec(smoke_spec)
smoke_spec.loader.exec_module(smoke)


class SmokeCheck(unittest.TestCase):
    def smoke(self, scope):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        test = smoke.Smoke(folder.name, BORROWED)
        test.setup = mock.Mock(return_value={'testScope': scope})
        return test

    def test_check_account_and_folder_runs_test_connection_and_records_its_scope(self):
        test = self.smoke('account-folder')
        with mock.patch('sys.stdout'):
            test.check()
        test.setup.assert_called_once_with('testDestination')
        self.assertEqual(test.result['testScope'], 'account-folder')
        self.assertEqual(test.result['steps'][-1]['step'], 'checked')

    def test_an_account_only_check_does_not_pass(self):
        test = self.smoke('account')
        with mock.patch('sys.stdout'), self.assertRaises(smoke.lab.LabError):
            test.check()


if __name__ == '__main__':
    unittest.main()
