#!/usr/bin/env python3
"""Offline installer contracts. Fakes are download/package tools, never desktop proof."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import shutil
import subprocess
import sys
import tempfile
import termios
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'packaging/install.sh'
PAYLOAD = b'checked package fixture\n'


class Installer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.home = self.root / 'home with spaces'
        self.home.mkdir()
        self.os_release = self.root / 'os-release'
        self.os_release.write_text('ID=ubuntu\nVERSION_ID=24.04\n')
        self.script = self.root / 'install.sh'
        self.script.write_text(SOURCE.read_text().replace('/etc/os-release', str(self.os_release))
                               .replace('/run/ostree-booted', str(self.root / 'ostree-booted')))
        self.log = self.root / 'calls.jsonl'
        self.env = dict(os.environ, HOME=str(self.home), TMPDIR=str(self.root),
                        PATH=str(self.bin), CALLS=str(self.log), ARCH='x86_64', STABLE='302',
                        NIGHTLY='302', CHECKSUM_MODE='ok', SUFFIX='-ubuntu24.04-x86_64.deb')
        for name in ('cat', 'mktemp', 'rm', 'awk', 'sha256sum', 'cp', 'chmod', 'mv', 'mkdir'):
            (self.bin / name).symlink_to(shutil.which(name))
        self.tool('uname', "import os; print(os.environ['ARCH'])")
        for name in ('apt', 'dnf', 'sudo'):
            self.tool(name, "import json, os, sys\nwith open(os.environ['CALLS'], 'a') as f: "
                      "f.write(json.dumps(sys.argv) + '\\n')")
        self.tool('curl', '''import hashlib, json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ['CALLS'], 'a') as f: f.write(json.dumps(['curl'] + args) + '\\n')
url = args[-1]
assert url.startswith('https://app.put.gd/dl/'), url
assert args[args.index('--proto') + 1] == '=https'
if '--write-out' in args:
    print(os.environ[url.rsplit('/', 1)[-1].upper()], end='')
    sys.exit(0)
assert args[args.index('--proto-redir') + 1] == '=https'
out = pathlib.Path(args[args.index('--output') + 1])
payload = b'checked package fixture\\n'
if url.endswith('/checksums'):
    name = 'putgd-26.1031.1' + os.environ['SUFFIX']
    digest = hashlib.sha256(payload).hexdigest()
    mode = os.environ['CHECKSUM_MODE']
    if mode == 'bad': digest = '0' * 64
    if mode == 'path': name = '../' + name
    if mode == 'missing': name = 'unrelated.deb'
    line = digest + '  ' + name + '\\n'
    out.write_text(line * (2 if mode == 'duplicate' else 1))
else:
    if os.environ['CHECKSUM_MODE'] == 'download-failure': sys.exit(22)
    out.write_bytes(payload)
''')

    def tool(self, name, body):
        path = self.bin / name
        path.write_text('#!' + sys.executable + '\n' + body + '\n')
        path.chmod(0o755)

    def run_script(self, *args, ok=True):
        master, slave = pty.openpty()
        def terminal():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        try:
            result = subprocess.run(['/bin/sh', str(self.script), *args], env=self.env,
                                    stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    preexec_fn=terminal, text=True, timeout=15)
        finally:
            os.close(slave)
            os.close(master)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def test_distro_matrix(self):
        for fields, expected in (
            ('ubuntu:24.04', 'ubuntu24.04'), ('linuxmint:22.3', 'ubuntu24.04'),
            ('ubuntu:26.04', 'ubuntu26.04'), ('kubuntu:26.04', 'ubuntu26.04'),
            ('debian:13', 'debian13'), ('debian:13.1', 'debian13'),
            ('fedora:44', 'fedora44'), ('fedora:45', 'appimage'),
            ('arch:rolling', 'appimage'), ('ubuntu:22.04', 'appimage')):
            with self.subTest(fields=fields):
                distro, version = fields.split(':')
                self.os_release.write_text(f'ID={distro}\nVERSION_ID={version}\n')
                out = self.run_script('--dry-run')
                self.assertIn('stable / ' + expected, out)
                self.assertFalse((self.home / '.local').exists())
        self.os_release.write_text('ID=linuxmint\nID_LIKE="ubuntu debian"\nUBUNTU_CODENAME=noble\n')
        self.assertIn('stable / ubuntu24.04', self.run_script('--dry-run'))

    def test_atomic_overrides_fedora_and_derivatives(self):
        (self.root / 'ostree-booted').touch()
        for distro in ('fedora', 'aurora', 'bazzite', 'silverblue'):
            self.os_release.write_text(f'ID={distro}\nVERSION_ID=44\n')
            out = self.run_script('--dry-run')
            self.assertIn('Atomic', out)
            self.assertIn('stable / appimage', out)

    def test_fallback_only_for_missing_stable_release(self):
        self.env['STABLE'] = '404'
        out = self.run_script('--dry-run')
        self.assertIn('falling back to nightly', out)
        self.assertIn('nightly / ubuntu24.04', out)
        for status in ('403', '500', '000'):
            self.env['STABLE'] = status
            self.assertNotIn('falling back', self.run_script('--dry-run', ok=False))

    def test_nightly_missing_does_not_fallback(self):
        self.env['NIGHTLY'] = '404'
        self.assertIn('HTTP 404', self.run_script('--channel', 'nightly', '--dry-run', ok=False))

    def test_appimage_install_and_replace(self):
        self.env['SUFFIX'] = '-x86_64.AppImage'
        for unused in range(2):
            self.run_script('--appimage')
            installed = self.home / '.local/bin/putgd.AppImage'
            self.assertEqual(installed.read_bytes(), PAYLOAD)
            self.assertEqual(installed.stat().st_mode & 0o777, 0o755)
        self.assertFalse(any(Path(call[0]).name == 'sudo' for call in self.calls()))
        self.assertFalse(list(self.root.glob('putgd-install.*')))

    def test_native_uses_only_one_sudo_step(self):
        for distro, package, manager, extension in (
                ('ubuntu', 'ubuntu24.04', 'apt', 'deb'), ('fedora', 'fedora44', 'dnf', 'rpm')):
            self.log.unlink(missing_ok=True)
            self.os_release.write_text(f'ID={distro}\nVERSION_ID={"24.04" if distro == "ubuntu" else "44"}\n')
            self.env['SUFFIX'] = f'-{package}-x86_64.{extension}'
            self.run_script()
            sudo = [call for call in self.calls() if Path(call[0]).name == 'sudo']
            self.assertEqual(len(sudo), 1)
            self.assertEqual(sudo[0][1:], [manager, 'install', './putgd-26.1031.1' + self.env['SUFFIX']])

    def test_bad_metadata_and_downloads_never_install(self):
        for mode in ('bad', 'path', 'missing', 'duplicate', 'download-failure'):
            self.env.update(CHECKSUM_MODE=mode, SUFFIX='-x86_64.AppImage')
            self.run_script('--appimage', ok=False)
            self.assertFalse((self.home / '.local').exists())
            self.assertFalse(list(self.root.glob('putgd-install.*')))
        self.assertFalse(any(Path(call[0]).name == 'sudo' for call in self.calls()))

    def test_gnome_50_guidance(self):
        self.tool('gnome-shell', "print('GNOME Shell 50.1')")
        self.env['SUFFIX'] = '-x86_64.AppImage'
        self.assertIn('gnome-extensions enable putgd@put.gd', self.run_script('--appimage'))

    def test_arguments_architecture_and_missing_dependencies(self):
        self.assertIn('Usage:', self.run_script('--help'))
        for args in (('--channel',), ('--channel', 'beta'), ('--nonsense',)):
            self.run_script(*args, ok=False)
        self.env['ARCH'] = 'aarch64'
        self.assertIn('Unsupported architecture', self.run_script('--dry-run', ok=False))
        self.env['ARCH'] = 'x86_64'
        for name in ('curl', 'sha256sum'):
            (self.bin / name).rename(self.bin / (name + '.disabled'))
            self.assertIn('Missing ' + name, self.run_script('--dry-run', ok=False))
            (self.bin / (name + '.disabled')).rename(self.bin / name)

    def test_incomplete_download_defines_but_never_calls_main(self):
        source = self.script.read_text()
        for marker in ('    base=', '    umask ', '\nmain "$@"'):
            self.script.write_text(source[:source.index(marker)])
            subprocess.run(['/bin/sh', str(self.script)], env=self.env, capture_output=True, timeout=5)
            self.assertEqual(self.calls(), [])


if __name__ == '__main__':
    unittest.main()
