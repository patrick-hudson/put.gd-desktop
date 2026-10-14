"""Synthetic host contracts only; no desktop, keychain or service proof."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('live_credentials', ROOT / 'tools/lab/provision_live_credentials.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)

SNAPSHOT = json.dumps({'session': {'uid': 'synthetic-uid', 'accessToken': 'synthetic-access',
                                'refreshToken': 'synthetic-refresh'}, 'userKeyPassword': 'synthetic-password'})
REF = 'putgd-lab-4bf87900-9e10-4cb0-8f32-0ce7f7f56f44'


class LiveCredentialContracts(unittest.TestCase):
    def test_colon_labels_inline_and_next_line_without_guessing(self):
        value = bridge.parse_s3_credentials('Access key ID:\nsynthetic-access\nSecret access key: synthetic-secret\n'
            'Bucket name: synthetic-bucket\nRegion:\nsynthetic-region\nEndpoint: https://example.invalid\n')
        self.assertEqual(value['credentials'], {'accessKey': 'synthetic-access', 'secretKey': 'synthetic-secret', 'sessionToken': ''})
        self.assertEqual(value['settings'], {'bucket': 'synthetic-bucket', 'region': 'synthetic-region', 'endpoint': 'https://example.invalid'})
        self.assertEqual(bridge.parse_s3_credentials('Access key ID: a\nSecret access key: b\nbucketName: c\n'
            'Endpoint:\nhttps://example.invalid\n')['settings'], {'bucket': 'c', 'endpoint': 'https://example.invalid'})
        for invalid in ('Access key ID: a\nAccess key ID: b\nSecret access key: c',
                        'Access key ID:\nSecret access key: c',
                        'a\nb\nc', 'Access key ID: a\nSecret access key: ',
                        'Access key ID: a\nSecret access key: b\nEndpoint: https://synthetic-private@example.invalid',
                        'Access key ID: a\nSecret access key: b\nEndpoint: https://example.invalid?synthetic-private'):
            with self.subTest(invalid=invalid), self.assertRaises(bridge.ProvisionError):
                bridge.parse_s3_credentials(invalid)

    def test_protected_file_rejects_symlink_and_public_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'synthetic'
            source.write_text('Access key ID: a\nSecret access key: b\n')
            source.chmod(0o600)
            self.assertIn('Access key ID', bridge.read_private_credentials(source))
            link = Path(directory) / 'link'
            link.symlink_to(source)
            with self.assertRaises(bridge.ProvisionError): bridge.read_private_credentials(link)
            source.chmod(0o644)
            with self.assertRaises(bridge.ProvisionError): bridge.read_private_credentials(source)

    def test_snapshot_is_opaque_validated_and_never_normalized(self):
        self.assertEqual(bridge.validate_snapshot(SNAPSHOT), SNAPSHOT)
        unusual = json.dumps({'session': {'uid': 'u', 'accessToken': 'a', 'refreshToken': None},
                              'userKeyPassword': 'synthetic\nopaque', 'cachePassword': ''})
        self.assertEqual(bridge.validate_snapshot(unusual), unusual)
        for value in ('{}', '{"session":null,"userKeyPassword":"p"}',
                      '{"session":{"uid":"u","accessToken":"a"},"userKeyPassword":1}',
                      '{"session":{"uid":"u","accessToken":"a","refreshToken":1},"userKeyPassword":"p"}',
                      SNAPSHOT + '\0', 'x' * (bridge.MAX_SNAPSHOT + 1)):
            with self.subTest(value=value[:20]), self.assertRaises(bridge.ProvisionError):
                bridge.validate_snapshot(value)

    def test_mutating_reference_scope_and_guard_are_explicit(self):
        bridge.validate_broker_request('proton', {'op': 'import', 'reference': REF, 'snapshot': SNAPSHOT})
        bridge.validate_broker_request('proton', {'op': 'replace', 'reference': 'existing-host-reference',
            'expectedSnapshot': SNAPSHOT, 'snapshot': SNAPSHOT})
        for provider, request in (
                ('proton', {'op': 'import', 'reference': 'existing-host-reference', 'snapshot': SNAPSHOT}),
                ('proton', {'op': 'delete', 'reference': 'existing-host-reference'}),
                ('proton', {'op': 'replace', 'reference': REF, 'snapshot': SNAPSHOT}),
                ('proton', {'op': 'export', 'reference': 'x\nattack'}),
                ('s3', {'op': 'import', 'reference': REF, 'credentials': {'accessKey': 'a', 'secretKey': 'b', 'extra': 'c'}}),
                ('proton', {'op': 'export', 'reference': REF, 'service': 'unrelated'})):
            with self.subTest(request=request), self.assertRaises(bridge.ProvisionError):
                bridge.validate_broker_request(provider, request)

    def test_ssh_uses_pinned_identity_and_secret_stdin_only(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / 'identity').write_text('synthetic identity')
            (run / 'identity').chmod(0o600)
            (run / 'state.json').write_text(json.dumps({'profile': 'cinnamon-x11', 'guest_user': 'fixture',
                'known_hosts': str(run / 'known_hosts'), 'ssh_port': 22222, 'overlay': str(run / 'disk.qcow2')}))
            (run / 'disk.qcow2').write_text('synthetic disposable overlay')
            request = {'op': 'import', 'reference': REF, 'snapshot': SNAPSHOT}
            broker = {'qt': '/home/fixture/bridge', 'proton': '/home/fixture/proton'}
            with patch.object(bridge, 'exchange', return_value=b'{"ok":true}') as exchange:
                self.assertEqual(bridge.guest_call(run, broker, 'proton', request), {'ok': True})
            arguments, body = exchange.call_args.args[:2]
            self.assertEqual(exchange.call_args.kwargs, {'timeout': bridge.BROKER_TIMEOUT})
            self.assertIn('StrictHostKeyChecking=yes', arguments)
            self.assertIn(str(run / 'identity'), arguments)
            self.assertNotIn('synthetic-password', ' '.join(arguments))
            self.assertIn(b'synthetic-password', body)
            self.assertIn(b"<<'PUTGD_SECRET_", body)
            (run / 'identity').chmod(0o644)
            with self.assertRaises(bridge.ProvisionError): bridge.guest_call(run, broker, 'proton', request)

    def test_transport_withholds_stderr_and_bounds_output(self):
        value = bridge.exchange([sys.executable, '-c',
            'import sys; data=sys.stdin.buffer.read(); sys.stderr.buffer.write(data); print("{\\"ok\\":true}")'], b'synthetic-private')
        self.assertEqual(json.loads(value), {'ok': True})
        for code in ('import sys; sys.stdout.write("x"*262145)',
                     'import sys; sys.stderr.write("synthetic-private"); sys.exit(1)'):
            with self.assertRaises(bridge.ProvisionError) as error:
                bridge.exchange([sys.executable, '-c', code], b'synthetic-private')
            self.assertNotIn('synthetic-private', str(error.exception))

    def test_outer_broker_deadline_allows_internal_stages_and_remains_bounded(self):
        self.assertGreater(bridge.BROKER_TIMEOUT, 40)
        with patch.object(bridge, 'exchange', return_value=b'{"ok":true}') as exchange:
            bridge.host_call('/unused', 's3', {'op': 'delete', 'reference': REF})
        self.assertEqual(exchange.call_args.kwargs, {'timeout': bridge.BROKER_TIMEOUT})
        with self.assertRaises(bridge.ProvisionError):
            bridge.exchange([sys.executable, '-c', 'import time; time.sleep(2)'], b'synthetic-private', timeout=0.02)

    def test_hostile_credentials_remain_literal_heredoc_data(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / 'identity').write_text('synthetic identity')
            (run / 'identity').chmod(0o600)
            (run / 'disk.qcow2').write_text('synthetic overlay')
            (run / 'state.json').write_text(json.dumps({'profile': 'cinnamon-x11', 'guest_user': 'fixture',
                'known_hosts': str(run / 'known_hosts'), 'ssh_port': 22222, 'overlay': str(run / 'disk.qcow2')}))
            hostile = "$(touch /tmp/never-executed) `touch /tmp/never-executed` ' & ;"
            request = {'op': 'import', 'reference': REF,
                       'credentials': {'accessKey': 'a', 'secretKey': hostile, 'sessionToken': ''}}
            with patch.object(bridge, 'exchange', return_value=b'{"ok":true}') as exchange:
                bridge.guest_call(run, '/home/fixture/bridge', 's3', request)
            arguments, body = exchange.call_args.args[:2]
            self.assertNotIn(hostile, ' '.join(arguments))
            marker = body.decode().split("<<'")[1].split("'")[0]
            payload = body.decode().split("<<'" + marker + "'\n", 1)[1].rsplit('\n' + marker + '\n', 1)[0]
            self.assertEqual(json.loads(payload)['credentials']['secretKey'], hostile)

    def test_sequential_session_chain_carries_refresh_with_expected_baseline(self):
        refreshed = SNAPSHOT.replace('synthetic-access', 'synthetic-refreshed')
        events = []
        def host(_broker, provider, request):
            events.append(('host', request.copy()))
            return {'ok': True, 'snapshot': SNAPSHOT} if request['op'] == 'export' else {'ok': True}
        def guest(run, _broker, provider, request):
            events.append((str(run), request.copy()))
            return {'ok': True, 'snapshot': refreshed} if request['op'] == 'export' else {'ok': True}
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(bridge, 'host_call', side_effect=host), patch.object(bridge, 'guest_call', side_effect=guest):
            chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
            first = chain.provision('/guest-a', '/unused', Path(directory) / 'a.json')
            with self.assertRaises(bridge.ProvisionError): chain.provision('/guest-b', '/unused', Path(directory) / 'blocked.json')
            chain.collect('/guest-a', '/unused', first)
            chain.carry_back(Path(directory) / 'save-a.json')
            chain.cleanup('/guest-a', '/unused', first)
            second = chain.provision('/guest-b', '/unused', Path(directory) / 'b.json')
            self.assertNotEqual(first, second)
            self.assertEqual(events[-1][1]['snapshot'], refreshed)
            chain.collect('/guest-b', '/unused', second)
            self.assertFalse(chain.current_saved)  # A fresh collect always needs confirmation.
            chain.carry_back(Path(directory) / 'save-b.json')
            self.assertEqual(events[-1][1], {'op': 'replace', 'reference': 'existing-host-reference',
                'expectedSnapshot': refreshed, 'snapshot': refreshed})
            chain.cleanup('/guest-b', '/unused', second)
            self.assertEqual(events[-1][1]['op'], 'delete')
            self.assertNotIn('logout', str(events))

    def test_public_wrapper_never_echoes_exceptions_or_broker_output(self):
        request = {'provider': 'proton', 'run': '/unused', 'guestBroker': '/unused',
                   'hostBroker': '/unused', 'sourceReference': 'host-ref'}
        output = io.StringIO()
        with patch.object(bridge, 'provision', side_effect=RuntimeError('synthetic-private-output')), \
                contextlib.redirect_stdout(output):
            self.assertEqual(bridge.main(io.BytesIO(json.dumps(request).encode())), 1)
        self.assertEqual(json.loads(output.getvalue()), {'ok': False, 'status': 'provisioning_failed'})
        self.assertNotIn('synthetic-private', output.getvalue())

    def test_receipt_preserves_reference_before_an_ambiguous_guest_reply(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / 'reference.json'
            request = {'provider': 's3', 'run': '/unused', 'guestBroker': '/unused',
                       'credentialsFile': '/unused', 'receipt': str(receipt)}
            with patch.object(bridge, 'read_private_credentials', return_value='Access key ID: a\nSecret access key: b'), \
                    patch.object(bridge, 'guest_call', side_effect=bridge.ProvisionError('withheld')):
                with self.assertRaises(bridge.ProvisionError): bridge.provision(request)
            reference = json.loads(receipt.read_text())['credentialReference']['reference']
            bridge.validate_reference(reference, lab_only=True)
            self.assertEqual(receipt.stat().st_mode & 0o777, 0o600)
            with patch.object(bridge, 'read_private_credentials', return_value='Access key ID: a\nSecret access key: b'), \
                    patch.object(bridge, 'guest_call') as guest:
                with self.assertRaises(bridge.ProvisionError): bridge.provision(request)
                guest.assert_not_called()


    def test_one_time_provisioning_requires_a_reference_receipt_before_import(self):
        request = {'provider': 's3', 'run': '/unused', 'guestBroker': '/unused',
                   'credentialsFile': '/unused'}
        with patch.object(bridge, 'read_private_credentials', return_value='Access key ID: a\nSecret access key: b'), \
                patch.object(bridge, 'guest_call', return_value={'ok': True}) as guest:
            with self.assertRaises(bridge.ProvisionError): bridge.provision(request)
            guest.assert_not_called()

    def test_ambiguous_host_import_retains_exact_recovery_reference(self):
        def host(_broker, provider, request):
            if request['op'] == 'export': return {'ok': True, 'snapshot': SNAPSHOT}
            raise bridge.ProvisionError('withheld')
        with tempfile.TemporaryDirectory() as directory, patch.object(bridge, 'host_call', side_effect=host):
            chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
            with self.assertRaises(bridge.ProvisionError): chain.carry_back(Path(directory) / 'host.json', new_lab_reference=True)
            self.assertEqual(len(chain.host_owned), 1)
            reference, status = next(iter(chain.host_owned.items()))
            bridge.validate_reference(reference, lab_only=True)
            self.assertEqual(status, 'import_pending')

    def test_pending_guest_can_be_abandoned_without_stranding_prior_refresh(self):
        refreshed = SNAPSHOT.replace('synthetic-access', 'synthetic-refreshed')
        requests = []
        with tempfile.TemporaryDirectory() as directory:
            def host(_broker, _provider, request):
                requests.append(request.copy())
                return {'ok': True, 'snapshot': SNAPSHOT} if request['op'] == 'export' else {'ok': True}
            def guest(run, _broker, _provider, request):
                if request['op'] == 'import' and str(run) == '/guest-b':
                    raise bridge.ProvisionError('withheld')
                return {'ok': True, 'snapshot': refreshed} if request['op'] == 'export' else {'ok': True}
            with patch.object(bridge, 'host_call', side_effect=host), patch.object(bridge, 'guest_call', side_effect=guest):
                chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
                first = chain.provision('/guest-a', '/unused', Path(directory) / 'a.json')
                chain.collect('/guest-a', '/unused', first)
                with self.assertRaises(bridge.ProvisionError):
                    chain.provision('/guest-b', '/unused', Path(directory) / 'b.json')
                pending = chain.active
                with self.assertRaises(bridge.ProvisionError): chain.collect('/guest-b', '/unused', pending)
                chain.abandon('/guest-b', '/unused', pending)
                self.assertIsNone(chain.active)
                self.assertEqual(chain.current, refreshed)
                chain.carry_back(Path(directory) / 'save.json')
                self.assertEqual(requests[-1], {'op': 'replace', 'reference': 'existing-host-reference',
                    'expectedSnapshot': SNAPSHOT, 'snapshot': refreshed})
                chain.cleanup('/guest-a', '/unused', first)
                self.assertEqual(json.loads((Path(directory) / 'b.json').read_text())['state'], 'cleanup_confirmed')

    def test_chain_receipts_exist_before_ambiguous_guest_and_host_imports(self):
        with tempfile.TemporaryDirectory() as directory:
            guest_receipt = Path(directory) / 'guest.json'
            host_receipt = Path(directory) / 'host.json'
            def host(_broker, _provider, request):
                if request['op'] == 'export': return {'ok': True, 'snapshot': SNAPSHOT}
                data = json.loads(host_receipt.read_text())
                self.assertEqual(data['credentialReference']['reference'], request['reference'])
                self.assertEqual(data['state'], 'import_pending')
                self.assertNotIn('synthetic-access', host_receipt.read_text())
                raise bridge.ProvisionError('withheld')
            def guest(run, _broker, _provider, request):
                data = json.loads(guest_receipt.read_text())
                self.assertEqual(data['run'], str(run))
                self.assertEqual(data['credentialReference']['reference'], request['reference'])
                self.assertEqual(data['state'], 'import_pending')
                self.assertEqual(guest_receipt.stat().st_mode & 0o777, 0o600)
                self.assertNotIn('synthetic-access', guest_receipt.read_text())
                raise bridge.ProvisionError('withheld')
            with patch.object(bridge, 'host_call', side_effect=host), patch.object(bridge, 'guest_call', side_effect=guest):
                chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
                with self.assertRaises(bridge.ProvisionError): chain.provision('/guest', '/unused', guest_receipt)
                # No SDK work can have started after an unconfirmed import.
                with self.assertRaises(bridge.ProvisionError): chain.carry_back(host_receipt, new_lab_reference=True)
                self.assertEqual(json.loads(host_receipt.read_text())['state'], 'import_pending')

    def test_cleanup_retains_guest_until_each_collect_is_carried_back(self):
        refreshed = SNAPSHOT.replace('synthetic-access', 'synthetic-refreshed')
        with tempfile.TemporaryDirectory() as directory:
            def host(_broker, _provider, request):
                if request['op'] == 'export': return {'ok': True, 'snapshot': SNAPSHOT}
                raise bridge.ProvisionError('withheld')
            with patch.object(bridge, 'host_call', side_effect=host), \
                    patch.object(bridge, 'guest_call', side_effect=lambda *_: {'ok': True, 'snapshot': refreshed}) as guest:
                chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
                reference = chain.provision('/guest', '/unused', Path(directory) / 'guest.json')
                with self.assertRaises(bridge.ProvisionError): chain.abandon('/guest', '/unused', reference)
                chain.collect('/guest', '/unused', reference)
                with self.assertRaises(bridge.ProvisionError): chain.cleanup('/guest', '/unused', reference)
                with self.assertRaises(bridge.ProvisionError): chain.carry_back(Path(directory) / 'save.json')
                with self.assertRaises(bridge.ProvisionError): chain.cleanup('/guest', '/unused', reference)
                self.assertFalse(chain.current_saved)
                self.assertIn(reference, chain.owned)
                self.assertFalse(any(call.args[-1]['op'] == 'delete' for call in guest.call_args_list))

    def test_public_cleanup_verifies_exact_receipt_and_rejects_unsafe_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / 'copy.json'
            provision = {'provider': 's3', 'run': '/guest', 'guestBroker': '/unused',
                         'credentialsFile': '/unused', 'receipt': str(receipt)}
            with patch.object(bridge, 'read_private_credentials', return_value='Access key ID: a\nSecret access key: b'), \
                    patch.object(bridge, 'guest_call', side_effect=bridge.ProvisionError('withheld')):
                with self.assertRaises(bridge.ProvisionError): bridge.provision(provision)
            reference = json.loads(receipt.read_text())['credentialReference']['reference']
            request = {'op': 'cleanup', 'provider': 's3', 'run': '/guest', 'guestBroker': '/unused',
                       'reference': reference, 'receipt': str(receipt)}
            with patch.object(bridge, 'guest_call', return_value={'ok': True}) as guest:
                for change in ({'provider': 'proton'}, {'run': '/other-guest'},
                               {'reference': 'existing-host-reference'}, {'reference': REF}):
                    with self.subTest(change=change), self.assertRaises(bridge.ProvisionError):
                        bridge.cleanup({**request, **change})
                receipt.chmod(0o644)
                with self.assertRaises(bridge.ProvisionError): bridge.cleanup(request)
                receipt.chmod(0o600)
                link = Path(directory) / 'link.json'
                link.symlink_to(receipt)
                with self.assertRaises(bridge.ProvisionError): bridge.cleanup({**request, 'receipt': str(link)})
                guest.assert_not_called()
                # The lower broker confirms deletion even when the entry is absent.
                bridge.cleanup(request)
                bridge.cleanup(request)
                self.assertEqual(guest.call_args.args[-1], {'op': 'delete', 'reference': reference})
                self.assertEqual(json.loads(receipt.read_text())['state'], 'cleanup_confirmed')

    def test_required_fresh_receipts_reject_reuse_and_symlink_before_mutations(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(bridge, 'host_call', return_value={'ok': True, 'snapshot': SNAPSHOT}) as host, \
                patch.object(bridge, 'guest_call', return_value={'ok': True}) as guest:
            chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
            original = Path(directory) / 'existing.json'
            original.write_text('synthetic retained input')
            original.chmod(0o600)
            link = Path(directory) / 'link.json'
            link.symlink_to(original)
            parent_link = Path(directory) / 'parent-link'
            parent_link.symlink_to(Path(directory), target_is_directory=True)
            for receipt in (None, original, link, parent_link / 'copy.json'):
                with self.subTest(receipt=receipt), self.assertRaises(bridge.ProvisionError):
                    chain.provision('/guest', '/unused', receipt)
                with self.subTest(receipt=receipt), self.assertRaises(bridge.ProvisionError):
                    chain.carry_back(receipt)
                with self.subTest(receipt=receipt), self.assertRaises(bridge.ProvisionError):
                    chain.carry_back(receipt, new_lab_reference=True)
            self.assertEqual(original.read_text(), 'synthetic retained input')
            self.assertIsNone(chain.active)
            self.assertFalse(chain.owned)
            self.assertFalse(chain.host_owned)
            guest.assert_not_called()
            self.assertEqual(host.call_count, 1)  # Constructor's private export only.

    def test_pending_guest_does_not_block_receipted_source_carry_back(self):
        refreshed = SNAPSHOT.replace('synthetic-access', 'synthetic-refreshed')
        with tempfile.TemporaryDirectory() as directory:
            save_receipt = Path(directory) / 'save.json'
            def host(_broker, _provider, request):
                if request['op'] == 'export': return {'ok': True, 'snapshot': SNAPSHOT}
                value = json.loads(save_receipt.read_text())
                self.assertEqual(value['state'], 'replace_pending')
                self.assertFalse(value['cleanupAllowed'])
                self.assertEqual(request['expectedSnapshot'], SNAPSHOT)
                self.assertEqual(request['snapshot'], refreshed)
                return {'ok': True}
            def guest(run, _broker, _provider, request):
                if str(run) == '/guest-b' and request['op'] == 'import': raise bridge.ProvisionError('withheld')
                return {'ok': True, 'snapshot': refreshed} if request['op'] == 'export' else {'ok': True}
            with patch.object(bridge, 'host_call', side_effect=host), patch.object(bridge, 'guest_call', side_effect=guest):
                chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
                reference = chain.provision('/guest-a', '/unused', Path(directory) / 'a.json')
                chain.collect('/guest-a', '/unused', reference)
                with self.assertRaises(bridge.ProvisionError): chain.provision('/guest-b', '/unused', Path(directory) / 'b.json')
                pending = chain.active
                chain.carry_back(save_receipt)
                self.assertEqual(chain.active, pending)
                self.assertTrue(chain.current_saved)
                self.assertEqual(json.loads(save_receipt.read_text())['state'], 'replace_confirmed')
                chain.abandon('/guest-b', '/unused', pending)
                chain.cleanup('/guest-a', '/unused', reference)

    def test_recovery_exports_confirmed_copy_before_carry_back_and_exact_cleanup(self):
        refreshed = SNAPSHOT.replace('synthetic-access', 'synthetic-refreshed')
        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / 'guest.json'
            def host(_broker, _provider, request):
                return {'ok': True, 'snapshot': SNAPSHOT} if request['op'] == 'export' else {'ok': True}
            with patch.object(bridge, 'host_call', side_effect=host), \
                    patch.object(bridge, 'guest_call', side_effect=lambda *_: {'ok': True, 'snapshot': refreshed}) as guest:
                first = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
                reference = first.provision('/guest', '/unused', receipt)
                # A new controller has only metadata and the host baseline.
                recovered = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
                recovered.recover(receipt, reference, '/guest', '/unused')
                self.assertEqual(recovered.current, refreshed)
                self.assertFalse(recovered.current_saved)
                request = {'op': 'cleanup', 'provider': 'proton', 'run': '/guest', 'guestBroker': '/unused',
                           'reference': reference, 'receipt': str(receipt)}
                with self.assertRaises(bridge.ProvisionError): bridge.cleanup(request)
                self.assertFalse(any(call.args[-1]['op'] == 'delete' for call in guest.call_args_list))
                recovered.carry_back(Path(directory) / 'save.json')
                recovered.cleanup('/guest', '/unused', reference)
                self.assertEqual(json.loads(receipt.read_text())['state'], 'cleanup_confirmed')

    def test_public_cleanup_redacts_broker_failure_and_retains_pending_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / 'copy.json'
            request = {'provider': 's3', 'run': '/guest', 'guestBroker': '/unused',
                       'credentialsFile': '/unused', 'receipt': str(receipt)}
            with patch.object(bridge, 'read_private_credentials', return_value='Access key ID: a\nSecret access key: b'), \
                    patch.object(bridge, 'guest_call', return_value={'ok': True}):
                reference = bridge.provision(request)['credentialReference']['reference']
            request = {'op': 'cleanup', 'provider': 's3', 'run': '/guest', 'guestBroker': '/unused',
                       'reference': reference, 'receipt': str(receipt)}
            output = io.StringIO()
            def failed(*_):
                self.assertEqual(json.loads(receipt.read_text())['state'], 'cleanup_pending')
                raise RuntimeError('synthetic-private-broker-output')
            with patch.object(bridge, 'guest_call', side_effect=failed), contextlib.redirect_stdout(output):
                self.assertEqual(bridge.main(io.BytesIO(json.dumps(request).encode())), 1)
            self.assertEqual(json.loads(output.getvalue()), {'ok': False, 'status': 'cleanup_failed'})
            self.assertNotIn('synthetic-private', output.getvalue())
            self.assertEqual(json.loads(receipt.read_text())['state'], 'cleanup_pending')

    def test_confirmed_host_lab_copy_requires_source_save_before_cleanup(self):
        refreshed = SNAPSHOT.replace('synthetic-access', 'synthetic-refreshed')
        with tempfile.TemporaryDirectory() as directory:
            host_receipt = Path(directory) / 'host.json'
            def host(_broker, _provider, request):
                snapshot = refreshed if request['reference'].startswith('putgd-lab-') else SNAPSHOT
                return {'ok': True, 'snapshot': snapshot} if request['op'] == 'export' else {'ok': True}
            with patch.object(bridge, 'host_call', side_effect=host) as host_call, \
                    patch.object(bridge, 'guest_call', side_effect=lambda *_: {'ok': True, 'snapshot': refreshed}):
                chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
                guest_reference = chain.provision('/guest', '/unused', Path(directory) / 'guest.json')
                chain.collect('/guest', '/unused', guest_reference)
                host_reference = chain.carry_back(host_receipt, new_lab_reference=True)
                self.assertTrue(chain.current_saved)
                chain.cleanup('/guest', '/unused', guest_reference)
                with self.assertRaises(bridge.ProvisionError): chain.cleanup_host(host_reference)
                self.assertFalse(any(call.args[-1]['op'] == 'delete' for call in host_call.call_args_list))
                recovered = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
                recovered.recover(host_receipt, host_reference)
                self.assertEqual(recovered.current, refreshed)
                self.assertFalse(recovered.current_saved)
                recovered.carry_back(Path(directory) / 'source-save.json')
                recovered.cleanup_host(host_reference)
                self.assertFalse(recovered.host_owned)
                self.assertEqual(json.loads(host_receipt.read_text())['state'], 'cleanup_confirmed')

    def test_source_receipt_is_never_deletable_even_for_a_lab_named_source(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(bridge, 'host_call', return_value={'ok': True, 'snapshot': SNAPSHOT}) as host:
            receipt = Path(directory) / 'source.json'
            chain = bridge.ProtonSessionChain('/unused', REF)
            chain.carry_back(receipt)
            host.reset_mock()
            with self.assertRaises(bridge.ProvisionError):
                bridge.cleanup({'op': 'cleanup', 'provider': 'proton', 'hostBroker': '/unused',
                                'reference': REF, 'receipt': str(receipt)})
            host.assert_not_called()

    def test_receipt_confirmation_failure_keeps_import_pending_and_abandonable(self):
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(bridge, 'host_call', return_value={'ok': True, 'snapshot': SNAPSHOT}), \
                patch.object(bridge, 'guest_call', return_value={'ok': True}) as guest:
            receipt = Path(directory) / 'guest.json'
            chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
            with patch.object(bridge, '_update_receipt', side_effect=bridge.ProvisionError('withheld')):
                with self.assertRaises(bridge.ProvisionError): chain.provision('/guest', '/unused', receipt)
            pending = chain.active
            self.assertEqual(json.loads(receipt.read_text())['state'], 'import_pending')
            with self.assertRaises(bridge.ProvisionError): chain.collect('/guest', '/unused', pending)
            chain.abandon('/guest', '/unused', pending)
            self.assertIsNone(chain.active)
            self.assertEqual(chain.current, SNAPSHOT)
            self.assertEqual([call.args[-1]['op'] for call in guest.call_args_list], ['import', 'delete'])

    def test_lost_source_ack_is_reconciled_by_private_exact_export_before_cleanup(self):
        refreshed = SNAPSHOT.replace('synthetic-access', 'synthetic-refreshed')
        host_snapshot = SNAPSHOT
        with tempfile.TemporaryDirectory() as directory:
            save_receipt = Path(directory) / 'save.json'
            def host(_broker, _provider, request):
                nonlocal host_snapshot
                if request['op'] == 'export': return {'ok': True, 'snapshot': host_snapshot}
                self.assertEqual(request['expectedSnapshot'], SNAPSHOT)
                host_snapshot = request['snapshot']
                raise bridge.ProvisionError('withheld')  # CAS applied; acknowledgement lost.
            with patch.object(bridge, 'host_call', side_effect=host) as host_call, \
                    patch.object(bridge, 'guest_call', side_effect=lambda *_: {'ok': True, 'snapshot': refreshed}) as guest:
                chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
                reference = chain.provision('/guest', '/unused', Path(directory) / 'guest.json')
                chain.collect('/guest', '/unused', reference)
                with self.assertRaises(bridge.ProvisionError): chain.carry_back(save_receipt)
                self.assertFalse(chain.current_saved)
                before = host_call.call_count
                with self.assertRaises(bridge.ProvisionError): chain.carry_back(Path(directory) / 'blind-retry.json')
                self.assertEqual(host_call.call_count, before)
                with self.assertRaises(bridge.ProvisionError): chain.cleanup('/guest', '/unused', reference)
                self.assertFalse(any(call.args[-1]['op'] == 'delete' for call in guest.call_args_list))
                self.assertTrue(chain.reconcile_carry_back(save_receipt))
                self.assertEqual(host_call.call_args.args[-1], {'op': 'export', 'reference': 'existing-host-reference'})
                self.assertEqual(chain.baseline, refreshed)
                self.assertTrue(chain.current_saved)
                self.assertEqual(json.loads(save_receipt.read_text())['state'], 'replace_confirmed')
                self.assertNotIn('synthetic-refreshed', save_receipt.read_text())
                chain.cleanup('/guest', '/unused', reference)

    def test_reconciliation_requires_exact_original_or_current_snapshot(self):
        refreshed = SNAPSHOT.replace('synthetic-access', 'synthetic-refreshed')
        for observed, conflict in ((SNAPSHOT, False), (refreshed + ' ', True),
                                   (SNAPSHOT.replace('synthetic-access', 'synthetic-unrelated'), True)):
            with self.subTest(observed=observed), tempfile.TemporaryDirectory() as directory:
                save_receipt = Path(directory) / 'save.json'
                host_values = [SNAPSHOT, observed]
                def host(_broker, _provider, request):
                    if request['op'] == 'export': return {'ok': True, 'snapshot': host_values.pop(0)}
                    raise bridge.ProvisionError('withheld')
                with patch.object(bridge, 'host_call', side_effect=host), \
                        patch.object(bridge, 'guest_call', side_effect=lambda *_: {'ok': True, 'snapshot': refreshed}):
                    chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
                    reference = chain.provision('/guest', '/unused', Path(directory) / 'guest.json')
                    chain.collect('/guest', '/unused', reference)
                    with self.assertRaises(bridge.ProvisionError): chain.carry_back(save_receipt)
                    if conflict:
                        with self.assertRaises(bridge.ProvisionError): chain.reconcile_carry_back(save_receipt)
                        self.assertIsNotNone(chain.pending_source)
                    else:
                        self.assertFalse(chain.reconcile_carry_back(save_receipt))
                        self.assertIsNone(chain.pending_source)
                        self.assertEqual(json.loads(save_receipt.read_text())['state'], 'replace_not_applied')
                    self.assertFalse(chain.current_saved)
                    with self.assertRaises(bridge.ProvisionError): chain.cleanup('/guest', '/unused', reference)


if __name__ == '__main__':
    unittest.main()
