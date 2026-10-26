"""Synthetic policy/ownership contracts; live desktop evidence is separate."""
import importlib.util
from contextlib import contextmanager
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
    @contextmanager
    def disposable_account(self, home='/native/home'):
        # Synthetic passwd/proc identities must work without USER and as root.
        real_stat = Path.stat
        def owned_stat(path, *args, **kwargs):
            value = list(real_stat(path, *args, **kwargs))
            value[4] = 1000
            return os.stat_result(value)
        account = SimpleNamespace(pw_uid=1000, pw_gid=1000, pw_dir=home)
        with patch.object(idle.pwd, 'getpwnam', return_value=account), \
             patch.object(idle.os, 'getuid', return_value=1000), \
             patch.object(Path, 'stat', owned_stat):
            yield

    def test_physical_system_refused_before_configuration_or_bus_access(self):
        with patch.object(idle, 'command', return_value='none'), \
             patch.object(idle, 'desktop_environment') as environment, \
             patch.object(idle, 'apply_gsettings') as settings:
            with self.assertRaisesRegex(idle.PolicyError, 'non-QEMU'):
                idle.preflight('GNOME', 'gnome-shell', 'lab', 'x11', True)
        environment.assert_not_called()
        settings.assert_not_called()

    def test_desktop_environment_discards_inherited_application_homes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            process = root / '123'
            process.mkdir()
            (process / 'comm').write_text('gnome-shell\n')
            (process / 'environ').write_text('DISPLAY=:7\0XAUTHORITY=/native/auth\0')
            with self.disposable_account(), \
                 patch.dict(os.environ, {'XDG_CONFIG_HOME': '/app/config', 'XDG_DATA_HOME': '/app/data',
                                         'WAYLAND_DISPLAY': 'wrong-display'}):
                env = idle.desktop_environment('gnome-shell', 'lab', root)
            self.assertNotIn('XDG_CONFIG_HOME', env)
            self.assertNotIn('XDG_DATA_HOME', env)
            self.assertNotIn('WAYLAND_DISPLAY', env)
            self.assertEqual(env['HOME'], '/native/home')
            self.assertEqual(env['DISPLAY'], ':7')
            self.assertEqual(env['XAUTHORITY'], '/native/auth')

    def test_native_custom_configuration_home_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            process = root / '123'
            process.mkdir()
            (process / 'comm').write_text('plasmashell\n')
            (process / 'environ').write_text('XDG_CONFIG_HOME=/native/config\0WAYLAND_DISPLAY=wayland-0\0')
            with self.disposable_account():
                env = idle.desktop_environment('plasmashell', 'lab', root)
            self.assertEqual(env['XDG_CONFIG_HOME'], '/native/config')

    def test_ambiguous_desktop_shell_refused(self):
        with tempfile.TemporaryDirectory() as directory, self.disposable_account(), \
             patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(idle.DesktopNotReady, 'not ready'):
                idle.desktop_environment('gnome-shell', 'lab', Path(directory))

    def test_provisioning_refuses_physical_before_reading_or_writing_user_home(self):
        with patch.object(idle, 'command', return_value='none'), \
             patch.object(idle.pwd, 'getpwnam') as account:
            with self.assertRaisesRegex(idle.PolicyError, 'non-QEMU'):
                idle.install_autostart('lab')
        account.assert_not_called()

    def test_provisioning_installs_idempotent_future_login_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(idle, 'command', return_value='kvm'), \
                 self.disposable_account(directory):
                idle.install_autostart('lab')
                idle.install_autostart('lab')
            entry = Path(directory) / '.config/autostart/putgd-lab-visible.desktop'
            self.assertIn('--autodetect --apply', entry.read_text())
            self.assertEqual((Path(directory) / '.local/lib/putgd-lab/guest_idle.py').read_bytes(),
                             (ROOT / 'tools/lab/guest_idle.py').read_bytes())

    def test_gnome_and_cinnamon_disable_both_power_supplies(self):
        for desktop in ('GNOME', 'Cinnamon'):
            policy = {(schema, key): value for schema, key, value in idle.settings(desktop)}
            prefix = 'org.gnome' if desktop == 'GNOME' else 'org.cinnamon'
            self.assertEqual(policy[prefix + '.desktop.session', 'idle-delay'], 'uint32 0')
            self.assertEqual(policy[prefix + '.desktop.screensaver', 'lock-enabled'], 'false')
            for supply in ('ac', 'battery'):
                self.assertEqual(policy[prefix + '.settings-daemon.plugins.power',
                                        'sleep-inactive-' + supply + '-type'], "'nothing'")
                self.assertEqual(policy[prefix + '.settings-daemon.plugins.power',
                                        'sleep-inactive-' + supply + '-timeout'], '0')

    def test_readback_drift_fails_instead_of_accepting_successful_write(self):
        with patch.object(idle, 'command', side_effect=['', 'uint32 300']) as command:
            with self.assertRaisesRegex(idle.PolicyError, 'readback mismatch'):
                idle.apply_gsettings('GNOME', True)
        self.assertEqual(command.call_count, 2)

    def test_check_mode_never_repairs_policy_drift(self):
        with patch.object(idle, 'command', return_value='uint32 300') as command:
            with self.assertRaises(idle.PolicyError):
                idle.apply_gsettings('GNOME', False)
        self.assertEqual(command.call_args.args[:2], ('gsettings', 'get'))

    def test_active_screensaver_fails_even_when_locking_is_disabled(self):
        for desktop in ('GNOME', 'Cinnamon'):
            with patch.object(idle, 'command', return_value='b true'):
                with self.assertRaisesRegex(idle.PolicyError, 'screensaver remains active'):
                    idle.screensaver_inactive(desktop)
            with patch.object(idle, 'command', return_value='b false'):
                idle.screensaver_inactive(desktop)

    def test_desktop_services_wait_only_at_initial_application(self):
        with patch.object(idle, 'command', side_effect=['b false', 'b true', 'b true']), \
             patch.object(idle.time, 'sleep') as sleep:
            idle.desktop_services('KDE', True)
            sleep.assert_called_once()
        with patch.object(idle, 'command', return_value='b false'), \
             patch.object(idle.time, 'sleep') as sleep:
            with self.assertRaisesRegex(idle.PolicyError, 'not ready'):
                idle.desktop_services('KDE', False)
            sleep.assert_not_called()
        with patch.object(idle, 'command', return_value='b false'), \
             patch.object(idle.time, 'monotonic', side_effect=[0, 31]):
            with self.assertRaisesRegex(idle.PolicyError, 'not ready'):
                idle.desktop_services('KDE', True)

    def test_cinnamon_screensaver_can_be_demand_activated_for_state_readback(self):
        with patch.object(idle, 'command', return_value='b false') as command:
            idle.desktop_services('Cinnamon', True)
            idle.screensaver_inactive('Cinnamon')
        self.assertEqual(command.call_count, 1)
        self.assertEqual(command.call_args.args[-1], 'GetActive')

    def test_locked_inactive_and_wrong_session_fail(self):
        for locked, active, kind in [('yes', 'yes', 'x11'), ('no', 'no', 'x11'), ('no', 'yes', 'wayland')]:
            values = f'User={os.getuid()}\nSeat=seat0\nType={kind}\nActive={active}\nLockedHint={locked}'
            with patch.object(idle, 'command', side_effect=['7 user seat0', values]):
                with self.assertRaisesRegex(idle.PolicyError, 'locked, inactive'):
                    idle.active_session('x11')

    def test_plasma_checks_every_supply_and_reloads_live_services(self):
        for major in ('5', '6'):
            values = {}
            calls = []
            def command(*args):
                calls.append(args)
                if args[0] == 'plasmashell':
                    return 'plasmashell ' + major + '.7.5'
                if args[0].startswith('kwriteconfig'):
                    values[args[1:-1]] = args[-1]
                    return ''
                if args[0].startswith('kreadconfig'):
                    return values[args[1:-2]]
                return 'b false' if args[-1] == 'GetActive' else ''
            with patch.object(idle, 'command', side_effect=command):
                result = idle.kde_policy(True)
            self.assertTrue(all(any(supply in row['groups'] for row in result)
                                for supply in ('AC', 'Battery', 'LowBattery')))
            self.assertTrue(any(call[-1] == 'reparseConfiguration' for call in calls))
            self.assertTrue(any(call[-1] == 'configure' for call in calls))

    def test_unrelated_unit_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {'XDG_CONFIG_HOME': directory}):
            unit = Path(directory) / 'systemd/user/putgd-lab-visible.service'
            unit.parent.mkdir(parents=True)
            unit.write_text('unrelated owned session work')
            with self.assertRaisesRegex(idle.PolicyError, 'ownership collision'):
                idle.inhibitor(True)
            self.assertEqual(unit.read_text(), 'unrelated owned session work')

    def test_active_unit_without_actual_inhibitor_fails(self):
        with patch.object(idle, 'command', side_effect=['active', 'enabled', json.dumps({'data': [[]]})]):
            with self.assertRaisesRegex(idle.PolicyError, 'not acquired'):
                idle.inhibitor(False)

    def test_initial_inhibitor_acquisition_waits_boundedly(self):
        empty = json.dumps({'data': [[]]})
        acquired = json.dumps({'data': [[['sleep:idle', 'putgd-lab-guest',
                                         'visible-acceptance', 'block', os.getuid(), 123]]]})
        for rows, clock, succeeds in [([empty, acquired], [0, .1], True),
                                      ([empty, empty], [0, .1, 5.1], False)]:
            with tempfile.TemporaryDirectory() as directory, \
                 patch.dict(os.environ, {'XDG_CONFIG_HOME': directory}), \
                 patch.object(idle, 'command', side_effect=['', '', 'active', 'enabled'] + rows), \
                 patch.object(idle.time, 'monotonic', side_effect=clock), \
                 patch.object(idle.time, 'sleep'):
                if succeeds:
                    self.assertEqual(idle.inhibitor(True)['pid'], 123)
                else:
                    with self.assertRaisesRegex(idle.PolicyError, 'not acquired'):
                        idle.inhibitor(True)

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

    def test_early_owned_guest_refusals_retain_receipts(self):
        for error in ('Guest preflight requires a running VM',
                      'Guest preflight cannot authenticate the owned QEMU/SSH endpoint'):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as directory, \
                 patch.object(lab, 'verify_owned_guest', side_effect=lab.LabError(error)), \
                 patch.object(lab, 'ssh_args') as ssh:
                with self.assertRaisesRegex(lab.LabError, 'retained guest-preflight-'):
                    lab.guest_preflight(directory)
                receipt = json.loads(next(Path(directory).glob('guest-preflight-*.json')).read_text())
                self.assertEqual(receipt['error'], error)
                ssh.assert_not_called()

    def test_bad_profile_or_missing_helper_retain_receipts(self):
        for target in ('load_profile', 'hash_file'):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory, \
                 patch.object(lab, 'verify_owned_guest', return_value={'id': 'owned', 'profile': 'cinnamon-x11'}), \
                 patch.object(lab, target, side_effect=OSError('missing input')), \
                 patch.object(lab, 'ssh_args') as ssh:
                with self.assertRaisesRegex(lab.LabError, 'missing input'):
                    lab.guest_preflight(directory)
                receipt = json.loads(next(Path(directory).glob('guest-preflight-*.json')).read_text())
                self.assertEqual(receipt['runId'], 'owned')
                ssh.assert_not_called()

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

    def test_failed_policy_receipt_retained_and_no_visual_or_app_work(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(lab, 'verify_owned_guest', return_value={'id': 'owned', 'profile': 'cinnamon-x11', 'guest_user': 'lab'}), \
             patch.object(lab, 'ssh_args', return_value=['ssh', 'owned']), \
             patch.object(lab, 'command', return_value=SimpleNamespace(returncode=1, stdout='{"ok":false,"error":"locked"}')) as command, \
             patch.object(lab, 'guest_outputs') as outputs:
            with self.assertRaisesRegex(lab.LabError, 'locked'):
                lab.guest_preflight(directory, stage='before-memory')
            receipt = json.loads(next(Path(directory).glob('guest-preflight-*.json')).read_text())
            self.assertFalse(receipt['ok'])
            self.assertEqual(receipt['stage'], 'before-memory')
            outputs.assert_not_called()
            self.assertIn('--desktop Cinnamon', command.call_args.args[0][-1])

    def test_start_cannot_launch_product_after_preflight_refusal(self):
        with patch.object(lab, 'acceptance_preflight', side_effect=lab.LabError('locked')), \
             patch.object(lab, 'guest_session') as session:
            with self.assertRaisesRegex(lab.LabError, 'locked'):
                lab.start('owned')
            session.assert_not_called()

    def test_failed_launch_stops_even_if_guest_sync_or_receipt_fails(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(lab, 'ssh', side_effect=lab.LabError('guest unreachable')), \
             patch.object(lab, 'reset') as reset:
            lab.stop_failed_launch(directory)
            reset.assert_called_once_with(directory)
            self.assertEqual(json.loads((Path(directory)/'failure-shutdown.json').read_text())['guestSync'], 'failed')
        with patch.object(lab, 'ssh'), patch.object(lab, 'write_json', side_effect=OSError('disk full')), \
             patch.object(lab, 'reset') as reset:
            with self.assertRaises(OSError):
                lab.stop_failed_launch('owned')
            reset.assert_called_once_with('owned')

    def test_initial_blank_frames_are_retained_until_visible(self):
        attempts = []
        client = SimpleNamespace(width=2, height=1, close=lambda: None)
        with patch.object(lab, 'screenshot', side_effect=[{'path': 'blank'}, {'path': 'visible'}]), \
             patch.object(lab, 'ppm', side_effect=[(2, 1, b'\0'*6), (2, 1, b'\0\xff'*3)]), \
             patch.object(lab.rfb, 'RFB', return_value=client), patch.object(lab.time, 'sleep'):
            result = lab.visible_guest_output('owned', 'token', 0, 5900, True, attempts)
        self.assertEqual(result['path'], 'visible')
        self.assertEqual([x['visible'] for x in attempts], [False, True])

    def test_blank_boundary_and_expired_initial_wait_fail(self):
        for apply, clock in [(False, [0, .1]), (True, [0, 16])]:
            attempts = []
            with patch.object(lab, 'screenshot', return_value={'path': 'blank'}), \
                 patch.object(lab, 'ppm', return_value=(2, 1, b'\0'*6)), \
                 patch.object(lab.time, 'monotonic', side_effect=clock), \
                 patch.object(lab.rfb, 'RFB') as vnc, patch.object(lab.time, 'sleep') as sleep:
                with self.assertRaisesRegex(lab.LabError, 'witness is blank'):
                    lab.visible_guest_output('owned', 'token', 0, 5900, apply, attempts)
                vnc.assert_not_called()
                sleep.assert_not_called()
            self.assertEqual(len(attempts), 1)

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
