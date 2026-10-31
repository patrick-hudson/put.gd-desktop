"""Owned-overlay reboot contracts; no real VM is started by these tests."""
import sys
from pathlib import Path
from unittest import TestCase, main
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab import guest_reboot


class GuestReboot(TestCase):
    def test_rejects_an_unowned_guest_before_any_mutation(self):
        with patch.object(guest_reboot.lab, 'verify_owned_guest', side_effect=RuntimeError('unowned')), \
             patch.object(guest_reboot.lab, 'guest_session') as session, \
             patch.object(guest_reboot.lab, 'QMP') as qmp:
            with self.assertRaisesRegex(RuntimeError, 'unowned'):
                guest_reboot.reboot('/run/foreign')
            session.assert_not_called()
            qmp.assert_not_called()

    def test_noop_session_request_uses_synced_authenticated_qmp_reset(self):
        old = {'boot': 'old', 'shell': 'shell-old'}
        fresh = {'boot': 'new', 'shell': 'shell-new'}
        info = {'profile': 'gnome-wayland'}
        with patch.object(guest_reboot.lab, 'verify_owned_guest', return_value=info) as owned, \
             patch.object(guest_reboot.lab, 'load_profile', return_value={'session_process': 'gnome-shell'}), \
             patch.object(guest_reboot, 'identity', side_effect=[old, old, fresh]), \
             patch.object(guest_reboot, 'ready', return_value=True), \
             patch.object(guest_reboot.lab, 'guest_session') as session, \
             patch.object(guest_reboot.lab, 'QMP') as qmp, \
             patch.object(guest_reboot.time, 'sleep'):
            result = guest_reboot.reboot('/owned/run', session_timeout=0)
            self.assertEqual(result['method'], 'owned-qmp-reset')
            self.assertEqual(result['after'], fresh)
            self.assertGreaterEqual(owned.call_count, 3)
            qmp.return_value.__enter__.return_value.execute.assert_called_once_with('system_reset')
            self.assertIn('sync', session.call_args_list[-1].args[1])

    def test_changed_shell_without_changed_boot_is_not_a_reboot(self):
        self.assertFalse(guest_reboot.restarted({'boot': 'a', 'shell': 'x'}, {'boot': 'a', 'shell': 'y'}))
        self.assertFalse(guest_reboot.restarted({'boot': 'a', 'shell': 'x'}, {'boot': 'b', 'shell': ''}))

    def test_does_not_reset_a_new_boot_while_staged_witness_is_starting(self):
        old = {'boot': 'old', 'shell': 'shell-old'}
        fresh = {'boot': 'new', 'shell': 'shell-new'}
        with patch.object(guest_reboot.lab, 'verify_owned_guest', return_value={'profile': 'gnome-wayland'}), \
             patch.object(guest_reboot.lab, 'load_profile', return_value={'session_process': 'gnome-shell'}), \
             patch.object(guest_reboot, 'identity', side_effect=[old, fresh, fresh]), \
             patch.object(guest_reboot, 'ready', side_effect=[guest_reboot.lab.LabError('not ready'), True]), \
             patch.object(guest_reboot.lab, 'guest_session'), \
             patch.object(guest_reboot.lab, 'QMP') as qmp, \
             patch.object(guest_reboot.time, 'sleep'):
            result = guest_reboot.reboot('/owned/run', session_timeout=0)
            self.assertEqual(result['method'], 'desktop-session')
            qmp.assert_not_called()


if __name__ == '__main__':
    main()
