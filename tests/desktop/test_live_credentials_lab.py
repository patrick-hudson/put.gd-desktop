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
        with patch.object(bridge, 'host_call', side_effect=host), patch.object(bridge, 'guest_call', side_effect=guest):
            chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
            first = chain.provision('/guest-a', '/unused')
            with self.assertRaises(bridge.ProvisionError): chain.provision('/guest-b', '/unused')
            chain.collect('/guest-a', '/unused', first)
            second = chain.provision('/guest-b', '/unused')
            self.assertNotEqual(first, second)
            self.assertEqual(events[-1][1]['snapshot'], refreshed)
            chain.collect('/guest-b', '/unused', second)
            chain.carry_back()
            self.assertEqual(events[-1][1], {'op': 'replace', 'reference': 'existing-host-reference',
                'expectedSnapshot': SNAPSHOT, 'snapshot': refreshed})
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
        with patch.object(bridge, 'host_call', side_effect=host):
            chain = bridge.ProtonSessionChain('/unused', 'existing-host-reference')
            with self.assertRaises(bridge.ProvisionError): chain.carry_back(new_lab_reference=True)
            self.assertEqual(len(chain.host_owned), 1)
            reference, status = next(iter(chain.host_owned.items()))
            bridge.validate_reference(reference, lab_only=True)
            self.assertEqual(status, 'import_pending')


if __name__ == '__main__':
    unittest.main()
