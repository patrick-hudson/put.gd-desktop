"""Synthetic policy/ownership contracts; live desktop evidence is separate."""
import importlib.util
import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / file)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


idle = module('guest_idle_contract', 'tools/lab/guest_idle.py')
lab = module('lab_idle_contract', 'tools/lab/lab.py')


class GuestPolicyContracts(unittest.TestCase):
    def test_physical_system_refused_before_configuration_or_bus_access(self):
        with patch.object(idle, 'command', return_value='none'), \
             patch.object(idle, 'desktop_environment') as environment, \
             patch.object(idle, 'apply_gsettings') as settings:
            with self.assertRaisesRegex(idle.PolicyError, 'non-QEMU'):
                idle.preflight('GNOME', 'gnome-shell', 'lab', 'x11', True)
        environment.assert_not_called()
        settings.assert_not_called()

    def test_provisioning_refuses_physical_before_reading_or_writing_user_home(self):
        with patch.object(idle, 'command', return_value='none'), \
             patch.object(idle.pwd, 'getpwnam') as account:
            with self.assertRaisesRegex(idle.PolicyError, 'non-QEMU'):
                idle.install_autostart('lab')
        account.assert_not_called()

    def test_readback_drift_fails_instead_of_accepting_successful_write(self):
        with patch.object(idle, 'command', side_effect=['', 'uint32 300']) as command:
            with self.assertRaisesRegex(idle.PolicyError, 'readback mismatch'):
                idle.apply_gsettings('GNOME', True)
        self.assertEqual(command.call_count, 2)

    def test_active_unit_without_actual_inhibitor_fails(self):
        with patch.object(idle, 'command', side_effect=['active', 'enabled', json.dumps({'data': [[]]})]):
            with self.assertRaisesRegex(idle.PolicyError, 'not acquired'):
                idle.inhibitor(False)

    def test_physical_adapter_does_not_mutate_user_policies(self):
        with patch.object(lab, 'state', return_value={'acceleration': 'physical'}), \
             patch.object(lab, 'guest_preflight') as preflight:
            result = lab.acceptance_preflight('physical-run', apply=True)
        self.assertIn('unchanged', result['scope'])
        preflight.assert_not_called()

    def test_unowned_endpoint_fails_before_any_ssh(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'disk.qcow2').write_bytes(b'synthetic overlay')
            info = {'overlay': str(root / 'disk.qcow2'), 'golden': str(root / 'base'),
                    'ssh_port': 22, 'pid': os.getpid()}
            (root / 'state.json').write_text(json.dumps(info))
            with patch.object(lab, 'ssh_args') as ssh:
                with self.assertRaisesRegex(lab.LabError, 'forwarded SSH port'):
                    lab.guest_preflight(root, apply=True)
            ssh.assert_not_called()
            receipt = json.loads(next(root.glob('guest-preflight-*.json')).read_text())
            self.assertFalse(receipt['ok'])
            self.assertIn('forwarded SSH port', receipt['error'])

    def test_unsafe_receipt_directory_has_no_writes_or_remote_access(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / 'destination'
            destination.mkdir()
            link = root / 'link'
            link.symlink_to(destination, target_is_directory=True)
            with patch.object(lab, 'verify_owned_guest') as verify:
                with self.assertRaises(lab.overlay_cleanup.CleanupError):
                    lab.guest_preflight(link)
                destination.chmod(0o777)
                with self.assertRaisesRegex(lab.LabError, 'receipt requires'):
                    lab.guest_preflight(destination)
                verify.assert_not_called()
            self.assertEqual(list(destination.iterdir()), [])

    def test_scenario_boundary_refuses_drift_without_repair_or_app_start(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / 'state.json').write_text(json.dumps({'profile': 'cinnamon-x11'}))
            args = SimpleNamespace(run=directory, scenario='region', clipboard_client='qt', timeout=3, keep=True)
            calls = []
            def preflight(_run, *, apply=False, stage):
                calls.append((stage, apply))
                if not apply:
                    raise lab.LabError('injected policy drift')
                return {'ok': True}
            with patch.object(lab, 'acceptance_preflight', side_effect=preflight), \
                 patch.object(lab, 'guest_session', side_effect=AssertionError('must not launch')), \
                 patch.object(lab, 'screenshot', side_effect=lab.LabError('no witness')):
                result = lab.fixture_run(args)
            self.assertEqual(calls, [('scenario-before', False)])
            self.assertFalse(result['ok'])
            self.assertEqual(result['error'], 'injected policy drift')

    def test_uniform_rgb_and_truncated_frames_are_not_visible(self):
        for pixels in (b'\xff\x00\x00' * 2, b'\xff\x00\xff' * 2, b'\xff\x00\x00'):
            with self.subTest(pixels=pixels), \
                 patch.object(lab, 'screenshot', return_value={'path': 'uniform'}), \
                 patch.object(lab, 'ppm', return_value=(2, 1, pixels)), \
                 patch.object(lab.rfb, 'RFB') as vnc:
                with self.assertRaisesRegex(lab.LabError, 'witness is blank'):
                    lab.visible_guest_output('owned', 'token', 0, 5900, False, [])
                vnc.assert_not_called()


if __name__ == '__main__':
    unittest.main()
