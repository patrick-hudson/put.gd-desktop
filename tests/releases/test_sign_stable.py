"""The workstation's RPM signing names the gpg it really has (Debian and Ubuntu's rpm expects gpg2)."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
import sign_stable

ASSET, FILE = 'putgd-1-fedora44-x86_64.rpm', 'putgd-1-1.fc44.x86_64.rpm'
CONTENT = b'synthetic rpm'


class SignRpm(unittest.TestCase):
    def sign(self, directory, program):
        commands = []
        def run(command, **kwargs):
            commands.append(command)
            if command[0] == 'gh':
                (Path(directory) / ASSET).write_bytes(CONTENT)
            return None
        with patch.object(sign_stable.subprocess, 'run', side_effect=run), \
                patch.object(sign_stable.shutil, 'which', return_value=program):
            data = sign_stable.sign_rpm(ASSET, FILE, hashlib.sha256(CONTENT).hexdigest(), 'v1', 'FPR', directory)
        return data, commands

    def test_rpmsign_is_told_which_gpg_to_run(self):
        with tempfile.TemporaryDirectory() as directory:
            data, commands = self.sign(directory, '/usr/bin/gpg')
            self.assertEqual(data, CONTENT)
            self.assertEqual(commands[-1], ['rpmsign', '--define', '__gpg /usr/bin/gpg', '--define', '_gpg_name FPR',
                                            '--addsign', str(Path(directory) / FILE)])

    def test_missing_gpg_stops_before_signing(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(SystemExit, 'gpg is not installed'):
                self.sign(directory, None)


if __name__ == '__main__':
    unittest.main()
