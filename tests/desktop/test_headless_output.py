"""Safety gates for live HDMI trials; no DRM device or desktop is contacted."""
import datetime
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/lab'))
import headless_output as pilot


class Edid(unittest.TestCase):
    def test_qhd_timings_checksums_and_hdmi_clock_cap(self):
        for hz, clock in [(30, 12075), (60, 24150)]:
            value = pilot.edid(hz)
            self.assertEqual(len(value), 256)
            self.assertEqual(value[126], 1)
            self.assertEqual(sum(value[:128]) % 256, 0)
            self.assertEqual(sum(value[128:]) % 256, 0)
            self.assertEqual(int.from_bytes(value[54:56], 'little'), clock)
            self.assertEqual(value[130], 17)
            self.assertEqual(value[134:142], bytes.fromhex('67030c001000003c'))
            self.assertEqual(value[142:145], bytes.fromhex('e20040'))
            self.assertEqual(value[56] + ((value[58] >> 4) << 8), 2560)
            self.assertEqual(value[59] + ((value[61] >> 4) << 8), 1440)

    def test_rejects_corruption_and_extension_truncation(self):
        original = pilot.edid()
        for value in (original[:128], original[:-1], b'x' * 128,
                      original[:55] + bytes([original[55] ^ 1]) + original[56:]):
            with self.assertRaises(pilot.Refused):
                pilot.decode_edid(value)


class HardwareGuards(unittest.TestCase):
    def test_actual_d69_alias_pair_is_refused_but_independent_hdmi_is_allowed(self):
        rows = pilot.connector_rows('''Connectors:
99 0 disconnected DP-1 0x0 0 98
109 98 connected HDMI-A-1 500x280 1 98
115 0 disconnected DP-2 0x0 0 114
121 0 disconnected HDMI-A-2 0x0 0 114
''')
        with self.assertRaisesRegex(pilot.Refused, 'shares an encoder'):
            pilot.check_aliases(rows, 'DP-1', ['HDMI-A-1'])
        pilot.check_aliases(rows, 'HDMI-A-2', ['HDMI-A-1'])
        with self.assertRaises(pilot.Refused):
            pilot.check_aliases(rows, 'HDMI-A-9', ['HDMI-A-1'])

    def test_new_evidence_path_never_reuses_existing_directory_or_symlink(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            pilot.private_new(root / 'new')
            for p in (root / 'new', root / 'link'):
                if p.name == 'link':
                    p.symlink_to(root / 'absent')
                with self.assertRaises(pilot.Refused):
                    pilot.private_new(p)

    def test_lease_rejects_wrong_session_expired_and_stale(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(pilot.Path, 'home', return_value=Path(folder)):
            p = Path(folder) / 'putgd-lab/locks/desktop.lock'
            p.parent.mkdir(parents=True)
            now = datetime.datetime.now(datetime.timezone.utc)
            stamp = lambda t: t.strftime('%Y-%m-%dT%H:%M:%SZ')
            for session, expiry, heartbeat in [('other', 60, 0), ('ours', -1, 0), ('ours', 60, -901)]:
                p.write_text(f'session: {session}\nexpires: {stamp(now + datetime.timedelta(seconds=expiry))}\nheartbeat: {stamp(now + datetime.timedelta(seconds=heartbeat))}\n')
                with self.assertRaises(pilot.Refused):
                    pilot.lease('ours')

    def test_no_kernel_write_if_rollback_timer_cannot_be_armed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            value = root / 'head.edid'; value.write_bytes(pilot.edid())
            args = SimpleNamespace(card='card0', connector='HDMI-A-2', keep='HDMI-A-1',
                                   edid=value, run=root / 'trial', session='ours', seconds=30)
            state = {'debug': '/sys/kernel/debug/dri/0000:00:02.0',
                     'outputs': {'HDMI-A-1': {'enabled': 'enabled'},
                                 'HDMI-A-2': {'enabled': 'disabled', 'status': 'disconnected'}},
                     'connectors': {'HDMI-A-1': {'encoders': [98]}, 'HDMI-A-2': {'encoders': [114]}}}
            expiry = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(minutes=5)
            with mock.patch.object(pilot, 'lease', return_value={'expires': expiry.strftime('%Y-%m-%dT%H:%M:%SZ')}), \
                    mock.patch.object(pilot, 'snapshot', return_value=state), \
                    mock.patch.object(pilot, 'command', side_effect=OSError('no user-systemd')), \
                    mock.patch.object(pilot, 'root_write') as write:
                with self.assertRaises(OSError):
                    pilot.enable(args)
                write.assert_not_called()
                self.assertTrue((root / 'trial/before.json').exists())


if __name__ == '__main__':
    unittest.main()
