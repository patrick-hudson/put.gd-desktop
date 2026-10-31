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
                        NIGHTLY='302', CHECKSUM_MODE='ok', SUFFIX='-ubuntu24.04-x86_64.deb',
                        CODEC='openh264', CISCO_REPO='enabled')
        for name in ('cat', 'mktemp', 'rm', 'awk', 'sha256sum', 'cp', 'chmod', 'mv', 'mkdir', 'sh'):
            (self.bin / name).symlink_to(shutil.which(name))
        self.tool('uname', "import os; print(os.environ['ARCH'])")
        self.tool('getconf', "import os; print(os.environ.get('GLIBC', 'glibc 2.39'))")
        for name in ('apt', 'dnf', 'sudo', 'rpm'):
            self.tool(name, "import json, os, sys\nwith open(os.environ['CALLS'], 'a') as f: "
                      "f.write(json.dumps(sys.argv) + '\\n')")
        with (self.bin / 'sudo').open('a') as f:
            f.write("import subprocess\nsys.exit(subprocess.call(sys.argv[1:]))\n")
        with (self.bin / 'rpm').open('a') as f:
            f.write("sys.exit(0 if sys.argv[2] in os.environ['CODEC'].split(',') else 1)\n")
        with (self.bin / 'dnf').open('a') as f:
            f.write('''if 'repolist' in sys.argv:
    if os.environ['CISCO_REPO'] == 'error': sys.exit(1)
    print('repo id  repo name')
    print('fedora  Fedora 44')
    if os.environ['CISCO_REPO'] == 'enabled':
        print('fedora-cisco-openh264  Fedora 44 openh264 (From Cisco)')
elif sys.argv[1:3] == ['install', '-y']:
    sys.exit(int(os.environ.get('CODEC_INSTALL_STATUS', '0')))
elif os.environ.get('PACKAGE_INSTALL_FAIL') == '1': sys.exit(1)
''')
        self.tool('curl', '''import hashlib, json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ['CALLS'], 'a') as f: f.write(json.dumps(['curl'] + args) + '\\n')
url = args[-1]
assert url.startswith('https://app.put.gd/dl/'), url
assert args[args.index('--proto') + 1] == '=https'
if '--write-out' in args:
    print(os.environ[url.split('/')[-2].upper()], end='')
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
        probes = [call[-1] for call in self.calls() if '--write-out' in call]
        self.assertEqual(probes, ['https://app.put.gd/dl/stable/ubuntu24.04',
                                  'https://app.put.gd/dl/nightly/ubuntu24.04'])
        for status in ('403', '500', '000'):
            self.env['STABLE'] = status
            self.assertNotIn('falling back', self.run_script('--dry-run', ok=False))

    def test_package_404_never_reaches_sudo_or_package_manager(self):
        self.env.update(STABLE='404', NIGHTLY='404')
        out = self.run_script(ok=False)
        self.assertIn('falling back to nightly', out)
        self.assertIn('nightly ubuntu24.04 download (HTTP 404)', out)
        self.assertTrue(all(call[0] == 'curl' for call in self.calls()))
        self.assertFalse(list(self.root.glob('putgd-install.*')))

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

    def test_appimage_rejects_incompatible_libc_before_downloading(self):
        for version in ('glibc 2.35', 'glibc 2.38', 'musl 1.2'):
            self.env['GLIBC'] = version
            self.assertIn('glibc 2.39+', self.run_script('--appimage', ok=False))
            self.assertEqual(self.calls(), [])
        self.env['GLIBC'] = 'glibc 2.43'
        self.assertIn('stable / appimage', self.run_script('--appimage', '--dry-run'))

    def test_native_uses_only_one_sudo_step(self):
        for distro, package, manager, extension in (
                ('ubuntu', 'ubuntu24.04', 'apt', 'deb'), ('fedora', 'fedora44', 'dnf', 'rpm')):
            self.log.unlink(missing_ok=True)
            self.os_release.write_text(f'ID={distro}\nVERSION_ID={"24.04" if distro == "ubuntu" else "44"}\n')
            self.env['SUFFIX'] = f'-{package}-x86_64.{extension}'
            self.run_script()
            sudo = [call for call in self.calls() if Path(call[0]).name == 'sudo']
            self.assertEqual(len(sudo), 1)
            if distro == 'ubuntu':
                self.assertEqual(sudo[0][1:], [manager, 'install', './putgd-26.1031.1' + self.env['SUFFIX']])
            else:
                self.assertEqual(sudo[0][1:3], ['sh', '-c'])
                self.assertEqual(sudo[0][-2:], ['sh', './putgd-26.1031.1' + self.env['SUFFIX']])
                self.assertEqual(self.dnf_installs(), [['install', './putgd-26.1031.1' + self.env['SUFFIX']]])

    def fedora(self):
        self.os_release.write_text('ID=fedora\nVERSION_ID=44\n')
        self.env['SUFFIX'] = '-fedora44-x86_64.rpm'

    def dnf_installs(self):
        return [call[1:] for call in self.calls()
                if Path(call[0]).name == 'dnf' and call[1] == 'install']

    def test_fedora_explicit_codec_install_with_one_sudo(self):
        self.fedora()
        for codec in ('noopenh264', '', 'noopenh264,openh264'):
            with self.subTest(codec=codec):
                self.log.unlink(missing_ok=True)
                self.env['CODEC'] = codec
                out = self.run_script()
                self.assertIn('Installing Cisco OpenH264 for recording', out)
                self.assertEqual(self.dnf_installs(), [
                    ['install', './putgd-26.1031.1-fedora44-x86_64.rpm'],
                    ['install', '-y', 'openh264', 'gstreamer1-plugin-openh264']])
                self.assertEqual(sum(Path(call[0]).name == 'sudo' for call in self.calls()), 1)

    def test_fedora_disabled_or_unavailable_repo_only_prints_commands(self):
        self.fedora()
        self.env['CODEC'] = 'noopenh264'
        for repo in ('disabled', 'error'):
            with self.subTest(repo=repo):
                self.log.unlink(missing_ok=True)
                self.env['CISCO_REPO'] = repo
                out = self.run_script()
                self.assertIn('sudo dnf config-manager setopt fedora-cisco-openh264.enabled=1\n', out)
                self.assertIn('sudo dnf install -y openh264 gstreamer1-plugin-openh264\n', out)
                self.assertEqual(len(self.dnf_installs()), 1)
                self.assertFalse(any('config-manager' in call for call in self.calls()))

    def test_fedora_codec_or_package_install_failure_is_reported(self):
        self.fedora()
        self.env.update(CODEC='noopenh264', CODEC_INSTALL_STATUS='1')
        self.assertIn('Package installation failed', self.run_script(ok=False))
        self.log.unlink()
        self.env['PACKAGE_INSTALL_FAIL'] = '1'
        self.run_script(ok=False)
        self.assertEqual(len(self.dnf_installs()), 1)
        self.assertFalse(any(Path(call[0]).name == 'rpm' for call in self.calls()))

    def test_fedora_dry_run_shows_conditional_codec_command_without_installing(self):
        self.fedora()
        self.env['CODEC'] = 'noopenh264'
        out = self.run_script('--dry-run')
        self.assertIn('same sudo step', out)
        self.assertIn('dnf install -y openh264 gstreamer1-plugin-openh264', out)
        self.assertIn('sudo dnf config-manager setopt fedora-cisco-openh264.enabled=1', out)
        self.assertTrue(all(call[0] == 'curl' for call in self.calls()))

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
