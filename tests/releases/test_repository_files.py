"""Both channels' pkg.put.gd repository files, as tagged packages install them (#293, #386)."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
REPO = ROOT / 'packaging/linux/repo'
DEB_LABELS = ('ubuntu24.04', 'ubuntu26.04', 'debian13')

# The DEB conffile's bytes. A package that ships other bytes makes dpkg prompt
# anyone who copied the other channel's file into place by hand (#386), so a
# change here needs a migration in the package, not just a new hash. The
# nightly values are those of the published v26.1022.968-nightly DEBs.
FROZEN_SOURCES = {
    ('ubuntu24.04', 'nightly'): '83d1c8e34b4724ba11307fbd161d674d0a3b0d4b0664a78fefc1629a30e5fb81',
    ('ubuntu26.04', 'nightly'): '5d5b79d0154fed9099c42beea5183e41876ba36920bed6ed45a747523d5cc852',
    ('debian13', 'nightly'): '11b30f1c9733b307cb1028e8a05d956a251d15a797173dcffd1e2bc9f165416f',
    ('ubuntu24.04', 'stable'): 'a78e80d841bb1741291080a7936f87d5dd6bd03fbf9155af82fc90a3d181f9bf',
    ('ubuntu26.04', 'stable'): '9d2faa5453a56f93432b6ba42245c8dd9c26d1d2f57e6f2cabb76129b7986cfc',
    ('debian13', 'stable'): 'cad647bf7e015b3456820732b3c7c378b8fa9e1abfc436394ab78c2b842e11e1',
}


def render(label, repository='https://pkg.put.gd'):
    out = Path(tempfile.mkdtemp())
    subprocess.run(['cmake', f'-DLABEL={label}', f'-DREPOSITORY={repository}', f'-DREPO_DIR={REPO}', f'-DOUTPUT={out}',
                    '-P', str(ROOT / 'cmake/PackageRepositoryFiles.cmake')], check=True, capture_output=True)
    files = {path.name: path.read_text() for path in out.iterdir()}
    shutil.rmtree(out)
    return files


def key_lines(channel):
    return [line for line in (REPO / f'keys/putgd-{channel}.asc').read_text().strip().split('\n') if line]


@unittest.skipUnless(shutil.which('cmake'), 'needs cmake')
class RepositoryFilesTest(unittest.TestCase):
    def test_each_deb_label_gets_both_channels_with_their_own_key(self):
        for label in DEB_LABELS:
            files = render(label)
            self.assertEqual(sorted(files), ['putgd-nightly.sources', 'putgd-stable.sources'])
            for channel, other in (('nightly', 'stable'), ('stable', 'nightly')):
                text = files[f'putgd-{channel}.sources']
                self.assertIn(f'URIs: https://pkg.put.gd/apt/{label}\n', text)
                self.assertIn(f'Suites: {channel}\n', text)
                signed = text[text.index('Signed-By:\n') + len('Signed-By:\n'):].split('\n')
                block = [line[1:] for line in signed if line.startswith(' ') and line != ' .']
                self.assertEqual(block, key_lines(channel))
                self.assertNotIn(key_lines(other)[2], text)

    def test_the_rpm_gets_both_channels_each_naming_its_own_key_file(self):
        files = render('fedora44')
        self.assertEqual(sorted(files), ['putgd-nightly.repo', 'putgd-stable.repo'])
        for channel in ('nightly', 'stable'):
            text = files[f'putgd-{channel}.repo']
            self.assertIn(f'[putgd-{channel}]\n', text)
            self.assertIn(f'baseurl=https://pkg.put.gd/rpm/fedora/44/x86_64/{channel}\n', text)
            self.assertIn(f'gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-putgd-{channel}\n', text)
            self.assertIn('gpgcheck=1\nrepo_gpgcheck=0\n', text)

    def test_lab_builds_point_both_channels_at_the_lab(self):
        files = render('ubuntu24.04', 'https://pkg.put.gd/lab')
        self.assertIn('URIs: https://pkg.put.gd/lab/apt/ubuntu24.04\nSuites: stable\n', files['putgd-stable.sources'])

    def test_deb_conffile_bytes_are_frozen(self):
        for (label, channel), digest in FROZEN_SOURCES.items():
            text = render(label)[f'putgd-{channel}.sources']
            self.assertEqual(hashlib.sha256(text.encode()).hexdigest(), digest, f'{label} {channel}')

    def test_other_labels_have_no_repository(self):
        with self.assertRaises(subprocess.CalledProcessError):
            render('appimage')


if __name__ == '__main__':
    unittest.main()
