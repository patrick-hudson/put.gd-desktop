#!/usr/bin/env python3
"""Offline installer contracts. Fakes are download/package tools, never desktop proof."""
import fcntl
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
                               .replace('/run/ostree-booted', str(self.root / 'ostree-booted'))
                               .replace('/etc/pacman', str(self.root / 'pacman')))
        self.log = self.root / 'calls.jsonl'
        self.env = dict(os.environ, HOME=str(self.home), TMPDIR=str(self.root),
                        PATH=str(self.bin), CALLS=str(self.log), ARCH='x86_64', STABLE='302',
                        NIGHTLY='302', CHECKSUM_MODE='ok', SUFFIX='-ubuntu24.04-x86_64.deb',
                        CODEC='openh264,gstreamer1-plugin-openh264', CISCO_REPO='enabled')
        for name in ('cat', 'mktemp', 'rm', 'awk', 'sha256sum', 'cp', 'chmod', 'mv', 'mkdir', 'sh', 'find', 'rmdir', 'grep'):
            (self.bin / name).symlink_to(shutil.which(name))
        self.tool('uname', "import os; print(os.environ['ARCH'])")
        self.tool('getconf', "import os; print(os.environ.get('GLIBC', 'glibc 2.39'))")
        for name in ('apt', 'dnf', 'sudo', 'rpm', 'pacman', 'pacman-key'):
            self.tool(name, "import json, os, sys\nwith open(os.environ['CALLS'], 'a') as f: "
                      "f.write(json.dumps(sys.argv) + '\\n')")
        with (self.bin / 'sudo').open('a') as f:
            f.write("import subprocess\nsys.exit(subprocess.call(sys.argv[1:]))\n")
        # `pacman -Q putgd` asks whether the package is installed; INSTALLED=1 says it is. Not a recorded call.
        pacman = self.bin / 'pacman'
        pacman.write_text(pacman.read_text().replace(
            'import json, os, sys\n', "import json, os, sys\nif sys.argv[1:2] == ['-Q']: sys.exit(0 if os.environ.get('INSTALLED') == '1' else 1)\n", 1))
        with (self.bin / 'pacman').open('a') as f:
            # Outside Arch Linux itself the installer asks `pacman -T` whether the installed system has the OpenSSL
            # soname the package links. RESOLVE picks the answer: unset = satisfied, unsatisfied = exit 127 (what
            # pacman -T returns for a missing dependency), failed = a pacman error (exit 1).
            f.write('''args, mode = sys.argv[1:], os.environ.get('RESOLVE', '')
if args[:1] == ['--config'] and args[2:3] == ['-T']: args = args[2:]  # the probe's own configuration
if args[:1] == ['-T'] and mode == 'unsatisfied':
    print(args[1])
    sys.exit(127)
if args[:1] == ['-T'] and mode == 'failed':
    sys.stderr.write('error: failed to initialize alpm library\\n')
    sys.exit(1)
''')
        # The installed putgd-cli: CLI=broken is a binary whose shared library the system does not have.
        self.tool('putgd-cli', "import os, sys\nif os.environ.get('CLI') == 'broken':\n"
                  "    sys.stderr.write('putgd-cli: error while loading shared libraries: libcrypto.so.4: cannot open shared object file\\n')\n"
                  "    sys.exit(127)\nprint('putgd-cli 26.1031.1')")
        with (self.bin / 'rpm').open('a') as f:
            f.write("sys.exit(0 if all(p in os.environ['CODEC'].split(',') for p in sys.argv[2:]) else 1)\n")
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
assert url.startswith(('https://app.put.gd/dl/', 'https://pkg.put.gd/keys/')), url
assert args[args.index('--proto') + 1] == '=https'
if '--write-out' in args:
    channel, package = url.split('/')[-2:]
    print(os.environ.get(channel.upper() + '_' + package.upper().replace('.', '_'), os.environ[channel.upper()]), end='')
    sys.exit(0)
if os.environ.get('REJECT_RELEASE_DOWNLOADS') == '1' and url.startswith('https://app.put.gd/dl/'):
    sys.exit(22)
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

    def run_script(self, *args, ok=True, terminal_input=None):
        master, slave = pty.openpty()
        def terminal():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        try:
            if terminal_input is not None:
                os.write(master, terminal_input.encode())
            result = subprocess.run(['/bin/sh', str(self.script), *args], env=self.env,
                                    input='', stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    pass_fds=(slave,), preexec_fn=terminal, text=True, timeout=15)
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
            ('fedora:44', 'fedora44'), ('fedora:45', 'fedora45'), ('fedora:46', 'appimage'),
            ('arch:rolling', 'archlinux'), ('ubuntu:22.04', 'appimage')):
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
        for distro, version in (('fedora', '44'), ('fedora', '45'), ('aurora', '45'),
                                ('bazzite', '45'), ('silverblue', '45')):
            self.os_release.write_text(f'ID={distro}\nVERSION_ID={version}\n')
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

    def test_arch_installer_sets_signed_repo_and_preserves_channel(self):
        self.os_release.write_text('ID=endeavouros\nID_LIKE="arch"\n')
        self.env['SUFFIX'] = '-archlinux-x86_64.pkg.tar.zst'
        self.tool('gpg', 'print("fpr:::::::::4032E28D893BAF66F4AC23D9A11D38D55536B371:")')
        (self.root / 'pacman.d').mkdir(exist_ok=True)
        (self.root / 'pacman.conf').write_text('[options]\n')
        self.run_script('--channel', 'nightly')
        config = self.root / 'pacman.d/putgd.conf'
        self.assertIn('PackageRequired DatabaseRequired', config.read_text())
        self.assertIn('/pacman/x86_64/nightly', config.read_text())
        self.assertEqual(config.stat().st_mode & 0o777, 0o644)
        before = config.read_text()
        config.chmod(0o600)
        self.run_script('--channel', 'nightly')
        self.assertEqual(config.stat().st_mode & 0o777, 0o644)
        self.assertEqual(config.read_text(), before)
        self.assertEqual((self.root / 'pacman.conf').read_text().count('Include = '), 1)

    def arch_with_updater(self, status=0):
        # Command availability, including on an unrecognized derivative, selects the updater.
        self.os_release.write_text('ID=example\nID_LIKE=arch\n')
        self.env['SUFFIX'] = '-archlinux-x86_64.pkg.tar.zst'
        self.tool('gpg', 'print("fpr:::::::::4032E28D893BAF66F4AC23D9A11D38D55536B371:")')
        (self.root / 'pacman.d').mkdir(exist_ok=True)
        (self.root / 'pacman.conf').write_text('[options]\n')
        self.tool('omarchy-update', "import os,sys,json\nassert os.isatty(0)\n"
                  "with open(os.environ['CALLS'],'a') as f: f.write(json.dumps(sys.argv)+'\\n')\n"
                  f"sys.exit({status})")

    def test_omarchy_update_precedes_repo_and_install(self):
        self.arch_with_updater()
        out = self.run_script('--channel', 'nightly', terminal_input='y\n')
        self.assertIn("Omarchy's system update", out)
        calls = self.calls()
        update = next(i for i,c in enumerate(calls) if Path(c[0]).name == 'omarchy-update')
        trust = next(i for i,c in enumerate(calls) if Path(c[0]).name == 'pacman-key')
        self.assertLess(update, trust)
        self.assertEqual(calls[update][1:], ['-y'])
        self.assertEqual(self.pacman_calls(), [['-T', 'libcrypto.so=4-64'], ['--config', str(self.root / 'pacman.d/putgd.conf'), '-Sy'], ['-S', 'putgd']])

    def test_omarchy_dry_run_explains_update_without_running_it(self):
        self.arch_with_updater()
        out = self.run_script('--channel', 'nightly', '--dry-run')
        self.assertIn("Omarchy's system update", out)
        self.assertTrue(all(Path(c[0]).name == 'curl' for c in self.calls()))

    def test_omarchy_update_failure_preserves_configuration(self):
        self.arch_with_updater(status=1)
        config = self.root / 'pacman.d/putgd.conf'
        config.write_text('existing admin choice\n')
        before = (self.root / 'pacman.conf').read_bytes()
        self.assertIn('system update failed', self.run_script('--channel', 'nightly', ok=False, terminal_input='y\n'))
        self.assertEqual(config.read_text(), 'existing admin choice\n')
        self.assertEqual((self.root / 'pacman.conf').read_bytes(), before)
        self.assertFalse(any(Path(c[0]).name in ('sudo','pacman','pacman-key') for c in self.calls()))

    def test_unreadable_conf_is_made_readable_only_by_the_native_install(self):
        self.arch_with_updater()
        config = self.root / 'pacman.d/putgd.conf'
        config.write_text('existing channel choice\n')
        config.chmod(0)
        self.run_script('--channel', 'nightly', terminal_input='y\n')
        # No separate privileged chmod before the update: the native install's own step is the only sudo call.
        sudo = [c for c in self.calls() if Path(c[0]).name == 'sudo']
        self.assertEqual([c[1:3] for c in sudo], [['sh', '-c']])
        self.assertEqual(config.read_text(), 'existing channel choice\n')
        self.assertEqual(config.stat().st_mode & 0o777, 0o644)

    def test_appimage_fallback_leaves_the_repository_file_alone(self):
        self.arch_cannot_satisfy(updater=True)
        config = self.root / 'pacman.d/putgd.conf'
        config.write_text('existing channel choice\n')
        config.chmod(0)
        pacman_conf = (self.root / 'pacman.conf').read_bytes()
        self.run_script('--channel', 'nightly', terminal_input='y\n')
        self.assertEqual(config.stat().st_mode & 0o777, 0)
        self.assertEqual((self.root / 'pacman.conf').read_bytes(), pacman_conf)
        self.assertFalse(any(Path(c[0]).name in ('sudo', 'pacman-key') for c in self.calls()))
        self.assertTrue((self.home / '.local/bin/putgd.AppImage').exists())
        config.chmod(0o600)

    def test_omarchy_decline_leaves_repo_and_keys_untouched(self):
        self.arch_with_updater()
        self.assertIn('declined', self.run_script('--channel', 'nightly', ok=False, terminal_input='n\n'))
        self.assertFalse((self.root / 'pacman.d/putgd.conf').exists())
        self.assertFalse(any(Path(c[0]).name in ('sudo','pacman','pacman-key','omarchy-update') for c in self.calls()))

    def test_omarchy_without_terminal_stops_with_recovery(self):
        self.arch_with_updater()
        r = subprocess.run(['/bin/sh', str(self.script), '--channel', 'nightly'], env=self.env,
                           input='', capture_output=True, text=True, start_new_session=True, timeout=15)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('Run omarchy update', r.stdout+r.stderr)
        self.assertFalse((self.root / 'pacman.d/putgd.conf').exists())
        self.assertFalse(any(Path(c[0]).name in ('sudo','pacman','pacman-key','omarchy-update') for c in self.calls()))

    def test_omarchy_name_without_updater_keeps_arch_upgrade(self):
        self.os_release.write_text('ID=omarchy\nID_LIKE=arch\n')
        self.env['SUFFIX'] = '-archlinux-x86_64.pkg.tar.zst'
        self.tool('gpg', 'print("fpr:::::::::4032E28D893BAF66F4AC23D9A11D38D55536B371:")')
        (self.root / 'pacman.d').mkdir(exist_ok=True)
        (self.root / 'pacman.conf').write_text('[options]\n')
        self.run_script('--channel', 'nightly')
        self.assertEqual(self.pacman_calls(), [['-T', 'libcrypto.so=4-64'], ['-Syu', 'putgd']])

    def test_arch_installs_from_signed_repo_without_direct_release_download(self):
        self.os_release.write_text('ID=arch\n')
        self.env['REJECT_RELEASE_DOWNLOADS'] = '1'
        self.tool('gpg', 'print("fpr:::::::::4032E28D893BAF66F4AC23D9A11D38D55536B371:")')
        (self.root / 'pacman.d').mkdir(exist_ok=True)
        (self.root / 'pacman.conf').write_text('[options]\n')
        self.run_script('--channel', 'nightly')
        config = self.root / 'pacman.d/putgd.conf'
        self.assertIn('PackageRequired DatabaseRequired', config.read_text())
        # Arch Linux itself: no probe, -Syu brings its OpenSSL to the one the package was built against.
        self.assertEqual(self.pacman_calls(), [['-Syu', 'putgd']])

    def arch_cannot_satisfy(self, updater=False, distro='manjaro'):
        if updater:
            self.arch_with_updater()
        else:
            self.os_release.write_text(f'ID={distro}\nID_LIKE=arch\n')
            self.tool('gpg', 'print("fpr:::::::::4032E28D893BAF66F4AC23D9A11D38D55536B371:")')
            (self.root / 'pacman.d').mkdir(exist_ok=True)
            (self.root / 'pacman.conf').write_text('[options]\n')
        # The release checksums name the AppImage: that is the only download this run makes.
        self.env.update(SUFFIX='-x86_64.AppImage', RESOLVE='unsatisfied')

    def pacman_calls(self):
        calls = [c[1:] for c in self.calls() if Path(c[0]).name == 'pacman']
        # The probe runs on a configuration of its own, in the private download directory.
        return [c[2:] if c[:1] == ['--config'] and c[2:3] == ['-T'] else c for c in calls]

    def test_manjaro_without_the_openssl_gets_the_appimage_and_never_the_repository(self):
        self.arch_cannot_satisfy()
        out = self.run_script('--channel', 'nightly')
        self.assertIn('Using the AppImage', out)
        self.assertIn('putgd-cli and S3 and R2', out)
        self.assertNotIn('B2', out)
        self.assertEqual(self.pacman_calls(), [['-T', 'libcrypto.so=4-64']])
        self.assertEqual((self.home / '.local/bin/putgd.AppImage').read_bytes(), PAYLOAD)
        # The Arch asset is never asked about: only the package that is chosen is checked.
        probes = [call[-1] for call in self.calls() if '--write-out' in call]
        self.assertEqual(probes, ['https://app.put.gd/dl/nightly/appimage'])
        # Nothing was trusted, configured or synced on the way to the AppImage.
        self.assertFalse((self.root / 'pacman.d/putgd.conf').exists())
        self.assertFalse(any(Path(c[0]).name in ('sudo', 'pacman-key') for c in self.calls()))
        self.assertFalse(list(self.root.glob('putgd-install.*')))

    def test_manjaro_with_the_openssl_installs_natively(self):
        self.arch_cannot_satisfy()
        del self.env['RESOLVE']
        self.run_script('--channel', 'nightly')
        self.assertEqual(self.pacman_calls(), [['-T', 'libcrypto.so=4-64'], ['-Syu', 'putgd']])
        self.assertFalse((self.home / '.local').exists())

    def test_arch_linux_itself_never_probes(self):
        # Even a system the probe would refuse: the -Syu in the install brings its OpenSSL up.
        self.os_release.write_text('ID=arch\n')
        self.tool('gpg', 'print("fpr:::::::::4032E28D893BAF66F4AC23D9A11D38D55536B371:")')
        (self.root / 'pacman.d').mkdir(exist_ok=True)
        (self.root / 'pacman.conf').write_text('[options]\n')
        self.env.update(RESOLVE='unsatisfied')
        self.run_script('--channel', 'nightly')
        self.assertEqual(self.pacman_calls(), [['-Syu', 'putgd']])
        self.assertFalse((self.home / '.local').exists())

    def test_omarchy_with_the_openssl_installs_natively_after_its_update(self):
        self.arch_with_updater()
        self.run_script('--channel', 'nightly', terminal_input='y\n')
        calls = self.calls()
        update = next(i for i, c in enumerate(calls) if Path(c[0]).name == 'omarchy-update')
        probe = next(i for i, c in enumerate(calls) if Path(c[0]).name == 'pacman' and '-T' in c)
        self.assertLess(update, probe)
        self.assertEqual(self.pacman_calls(), [['-T', 'libcrypto.so=4-64'], ['--config', str(self.root / 'pacman.d/putgd.conf'), '-Sy'], ['-S', 'putgd']])

    def test_omarchy_without_the_openssl_gets_the_appimage_after_its_update(self):
        self.arch_cannot_satisfy(updater=True)
        out = self.run_script('--channel', 'nightly', terminal_input='y\n')
        calls = self.calls()
        update = next(i for i, c in enumerate(calls) if Path(c[0]).name == 'omarchy-update')
        probe = next(i for i, c in enumerate(calls) if Path(c[0]).name == 'pacman' and '-T' in c)
        self.assertLess(update, probe)
        self.assertEqual(self.pacman_calls(), [['-T', 'libcrypto.so=4-64']])
        self.assertIn('Using the AppImage', out)
        self.assertEqual((self.home / '.local/bin/putgd.AppImage').read_bytes(), PAYLOAD)
        self.assertFalse((self.root / 'pacman.d/putgd.conf').exists())

    def test_fallback_warns_when_the_package_is_already_installed(self):
        self.arch_cannot_satisfy()
        self.assertNotIn('sudo pacman -R putgd', self.run_script('--channel', 'nightly'))
        self.env['INSTALLED'] = '1'
        out = self.run_script('--channel', 'nightly')
        self.assertIn('sudo pacman -R putgd', out)
        self.assertIn('cannot run putgd-cli or upload to S3 or R2', out)
        self.assertNotIn('B2', out)

    def test_fallback_keeps_the_requested_channel(self):
        # No stable Arch asset: the AppImage is still chosen for the channel the user asked for.
        self.arch_cannot_satisfy()
        self.env.update(STABLE_ARCHLINUX='404')
        out = self.run_script()
        probes = [call[-1] for call in self.calls() if '--write-out' in call]
        self.assertEqual(probes, ['https://app.put.gd/dl/stable/appimage'])
        self.assertNotIn('nightly', out)
        fetched = [call[-1] for call in self.calls() if call[0] == 'curl' and '--output' in call and '--write-out' not in call]
        self.assertEqual(fetched, ['https://app.put.gd/dl/stable/checksums', 'https://app.put.gd/dl/stable/appimage'])

    def test_missing_arch_assets_do_not_stop_an_appimage_install(self):
        self.arch_cannot_satisfy()
        self.env.update(STABLE_ARCHLINUX='404', NIGHTLY_ARCHLINUX='404')
        self.run_script()
        self.assertEqual((self.home / '.local/bin/putgd.AppImage').read_bytes(), PAYLOAD)

    def test_native_choice_still_needs_the_arch_asset(self):
        self.arch_cannot_satisfy()
        del self.env['RESOLVE']
        self.env.update(STABLE_ARCHLINUX='404', NIGHTLY_ARCHLINUX='404')
        self.assertIn('No usable nightly archlinux download', self.run_script(ok=False))
        self.assertEqual(self.pacman_calls(), [['-T', 'libcrypto.so=4-64']])
        self.assertFalse(any(Path(c[0]).name in ('sudo', 'pacman-key') for c in self.calls()))

    def broken_native_install(self, distro):
        self.arch_cannot_satisfy(distro=distro) if distro != 'arch' else None
        if distro == 'arch':
            self.os_release.write_text('ID=arch\n')
            self.tool('gpg', 'print("fpr:::::::::4032E28D893BAF66F4AC23D9A11D38D55536B371:")')
            (self.root / 'pacman.d').mkdir(exist_ok=True)
            (self.root / 'pacman.conf').write_text('[options]\n')
        # pacman -T passes (or is not asked), the install succeeds, and the installed putgd-cli cannot load.
        self.env.pop('RESOLVE', None)
        self.env.update(CLI='broken', SUFFIX='-x86_64.AppImage')

    def test_a_native_install_whose_cli_does_not_start_is_replaced_by_the_appimage(self):
        for distro in ('manjaro', 'arch'):
            with self.subTest(distro=distro):
                self.log.unlink(missing_ok=True)
                self.broken_native_install(distro)
                out = self.run_script('--channel', 'nightly')
                self.assertIn('putgd-cli does not start', out)
                calls = self.pacman_calls()
                self.assertEqual(calls[-2:], [['-Syu', 'putgd'], ['-R', '--noconfirm', 'putgd']])
                self.assertEqual((self.home / '.local/bin/putgd.AppImage').read_bytes(), PAYLOAD)
                (self.home / '.local/bin/putgd.AppImage').unlink()

    def test_the_proof_keeps_the_requested_channel_and_needs_an_appimage_host(self):
        self.broken_native_install('manjaro')
        self.env['GLIBC'] = 'glibc 2.35'
        out = self.run_script('--channel', 'nightly', ok=False)
        self.assertIn('glibc 2.39+', out)
        self.assertEqual(self.pacman_calls()[-1], ['-R', '--noconfirm', 'putgd'])
        self.assertFalse((self.home / '.local').exists())
        self.log.unlink()
        self.env.pop('GLIBC')
        self.tool('gpg', 'print("fpr:::::::::C0962EFA929F5A5477A4509C0CC9E5A0F0B6FC1C:")')
        self.run_script()
        probes = [call[-1] for call in self.calls() if '--write-out' in call]
        self.assertEqual(probes, ['https://app.put.gd/dl/stable/archlinux', 'https://app.put.gd/dl/stable/appimage'])

    def test_a_working_cli_keeps_the_native_install(self):
        self.arch_cannot_satisfy()
        del self.env['RESOLVE']
        self.run_script('--channel', 'nightly')
        self.assertNotIn(['-R', '--noconfirm', 'putgd'], self.pacman_calls())
        self.assertFalse((self.home / '.local').exists())

    def test_fallback_still_needs_a_usable_appimage_host(self):
        self.arch_cannot_satisfy()
        self.env['GLIBC'] = 'glibc 2.35'
        self.assertIn('glibc 2.39+', self.run_script('--channel', 'nightly', ok=False))
        self.assertFalse((self.home / '.local').exists())

    def test_other_pacman_failures_stop_the_install(self):
        # Neither 0 (satisfied) nor 127 (unsatisfied): no AppImage and no native install.
        self.arch_cannot_satisfy()
        self.env['RESOLVE'] = 'failed'
        out = self.run_script('--channel', 'nightly', ok=False)
        self.assertIn('failed to initialize alpm library', out)
        self.assertIn('nothing installed', out)
        self.assertEqual(self.pacman_calls(), [['-T', 'libcrypto.so=4-64']])
        self.assertFalse((self.home / '.local').exists())
        self.assertFalse((self.root / 'pacman.d/putgd.conf').exists())

    def test_arch_dry_run_says_the_appimage_is_the_fallback(self):
        self.os_release.write_text('ID=arch\n')
        self.assertIn('other than Arch Linux itself', self.run_script('--dry-run'))

    def test_arch_key_mismatch_stops_before_trusting_key(self):
        self.os_release.write_text('ID=arch\n')
        self.env['SUFFIX'] = '-archlinux-x86_64.pkg.tar.zst'
        self.tool('gpg', 'print("fpr:::::::::BAD:")')
        self.run_script('--channel', 'nightly', ok=False)
        self.assertFalse((self.root / 'pacman.d/putgd.conf').exists())

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
        for distro, version, package, manager, extension in (
                ('ubuntu', '24.04', 'ubuntu24.04', 'apt', 'deb'),
                ('fedora', '44', 'fedora44', 'dnf', 'rpm'),
                ('fedora', '45', 'fedora45', 'dnf', 'rpm')):
            self.log.unlink(missing_ok=True)
            self.os_release.write_text(f'ID={distro}\nVERSION_ID={version}\n')
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
        for codec in ('noopenh264', '', 'noopenh264,openh264', 'openh264',
                      'gstreamer1-plugin-openh264'):
            with self.subTest(codec=codec):
                self.log.unlink(missing_ok=True)
                self.env['CODEC'] = codec
                out = self.run_script()
                self.assertIn('Installing Cisco OpenH264 for recording', out)
                self.assertEqual(self.dnf_installs(), [
                    ['install', './putgd-26.1031.1-fedora44-x86_64.rpm'],
                    ['install', '-y', 'openh264', 'gstreamer1-plugin-openh264']])
                self.assertEqual(sum(Path(call[0]).name == 'sudo' for call in self.calls()), 1)

    def test_fedora_complete_codecs_skip_repo_probe_and_extra_transaction(self):
        self.fedora()
        self.env['CISCO_REPO'] = 'disabled'
        out = self.run_script()
        self.assertNotIn('Installing Cisco', out)
        self.assertNotIn('config-manager', out)
        self.assertEqual(len(self.dnf_installs()), 1)
        self.assertFalse(any('repolist' in call for call in self.calls()))

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
