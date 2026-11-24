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



class StagedRpms(unittest.TestCase):
    """The stage must name exactly the tag's RPM assets, one per Fedora release, with GitHub's checksums."""
    TAG = 'v26.1201.1'
    SUMS = {f'putgd-26.1201.1-{label}-x86_64.rpm': digest for label, digest in (('fedora44', 'a' * 64), ('fedora45', 'b' * 64))}

    def manifest(self, rpms, schema=3):
        base = {'schemaVersion': schema, 'check': {'packages': []}, 'sign': {}}
        return {**base, 'rpms': rpms} if schema >= 3 else {**base, 'rpm': rpms.get('fedora44')}

    def rpms(self, labels=('fedora44', 'fedora45')):
        return {label: {'asset': f'putgd-26.1201.1-{label}-x86_64.rpm', 'sha256': self.SUMS[f'putgd-26.1201.1-{label}-x86_64.rpm'],
                        'file': f'putgd-26.1201.1-1.fc{label[-2:]}.x86_64.rpm'} for label in labels}

    def test_every_released_rpm_is_staged(self):
        sign_stable.check_staged(self.manifest(self.rpms()), {}, self.TAG, self.SUMS)
        with self.assertRaisesRegex(SystemExit, 'RPM assets'):
            sign_stable.check_staged(self.manifest(self.rpms(('fedora44',))), {}, self.TAG, self.SUMS)
        wrong = self.rpms()
        wrong['fedora45']['sha256'] = 'c' * 64
        with self.assertRaisesRegex(SystemExit, 'RPM assets'):
            sign_stable.check_staged(self.manifest(wrong), {}, self.TAG, self.SUMS)

    def test_a_release_without_fedora45_and_an_older_stage_still_sign(self):
        only44 = {name: digest for name, digest in self.SUMS.items() if 'fedora44' in name}
        sign_stable.check_staged(self.manifest(self.rpms(('fedora44',))), {}, self.TAG, only44)
        sign_stable.check_staged(self.manifest(self.rpms(('fedora44',)), schema=2), {}, self.TAG, only44)
        sign_stable.check_staged(self.manifest({}), {}, None, {})
        with self.assertRaisesRegex(SystemExit, 'no fedora44'):
            sign_stable.check_staged(self.manifest({}), {}, self.TAG, {})


if __name__ == '__main__':
    unittest.main()
