"""Execute the neon repair command against fixtures; no desktop acceptance."""
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/lab/bootstrap-neon-guest.sh'
ABSOLUTE = b'/usr/share/wayland-sessions/plasma.desktop'


class NeonAutologinRepair(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='neon-524-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.config = self.directory / 'plasma login.conf'

    def repair(self, **kwargs):
        return subprocess.run(['sh', str(SCRIPT), '--fix-autologin', str(self.config)],
                              capture_output=True, text=True, timeout=10, **kwargs)

    def backups(self):
        return list(self.directory.glob(self.config.name + '.putgd-524-*.bak'))

    def test_repairs_only_autologin_and_preserves_every_other_byte_and_metadata(self):
        original = (b'# installer configuration\nSession=' + ABSOLUTE + b'\n'
                    b'[General]\nSession=' + ABSOLUTE + b'\n'
                    b'[Autologin]\nUser=lab\nSession=' + ABSOLUTE + b'\nRelogin=false\n'
                    b'[Other]\nSession=' + ABSOLUTE + b'\n# preserved: \xff\n')
        self.config.write_bytes(original)
        self.config.chmod(0o640)
        before = self.config.stat()
        result = self.repair()
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = original.replace(b'User=lab\nSession=' + ABSOLUTE,
                                    b'User=lab\nSession=plasma.desktop')
        self.assertEqual(self.config.read_bytes(), expected)
        after = self.config.stat()
        self.assertEqual((stat.S_IMODE(after.st_mode), after.st_uid, after.st_gid),
                         (0o640, before.st_uid, before.st_gid))
        self.assertEqual(len(self.backups()), 1)
        self.assertEqual(self.backups()[0].read_bytes(), original)
        self.assertEqual(stat.S_IMODE(self.backups()[0].stat().st_mode), 0o600)
        self.assertIn('no session restarted', result.stdout)

    def test_accepts_spacing_crlf_and_missing_final_newline(self):
        for ending in (b'\n', b'\r\n', b''):
            with self.subTest(ending=ending):
                original = b' [Autologin] \r\n\tSession \t= \t' + ABSOLUTE + b' \t' + ending
                self.config.write_bytes(original)
                result = self.repair()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.config.read_bytes(), original.replace(ABSOLUTE, b'plasma.desktop'))

    def test_repeat_is_a_noop_and_retains_the_first_backup(self):
        original = b'[Autologin]\nSession=' + ABSOLUTE + b'\n'
        self.config.write_bytes(original)
        self.assertEqual(self.repair().returncode, 0)
        before = self.config.stat()
        backups = self.backups()
        result = self.repair()
        self.assertEqual(result.returncode, 0, result.stderr)
        after = self.config.stat()
        self.assertEqual((after.st_ino, after.st_mtime_ns, after.st_ctime_ns),
                         (before.st_ino, before.st_mtime_ns, before.st_ctime_ns))
        self.assertEqual(self.backups(), backups)
        self.assertEqual(backups[0].read_bytes(), original)
        # A later installer rewrite must not destroy the first recovery copy.
        self.config.write_bytes(original + b'# updated by installer\n')
        self.assertEqual(self.repair().returncode, 0)
        self.assertEqual(len(self.backups()), 2)
        self.assertEqual(backups[0].read_bytes(), original)

    def test_unaffected_values_and_sections_are_not_rewritten_or_backed_up(self):
        for value in (b'plasma.desktop', b'plasma', b'', b'/custom/plasma.desktop',
                      b'/usr/share/wayland-sessions/other.desktop', ABSOLUTE + b'.old'):
            with self.subTest(value=value):
                original = (b'[Autologin]\nSession=' + value + b'\n# Session=' + ABSOLUTE +
                            b'\n;Session=' + ABSOLUTE + b'\n[Other]\nSession=' + ABSOLUTE + b'\n')
                self.config.write_bytes(original)
                before = self.config.stat()
                result = self.repair()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.config.read_bytes(), original)
                self.assertEqual(self.config.stat().st_ino, before.st_ino)
                self.assertEqual(self.backups(), [])

    def test_missing_config_is_a_noop(self):
        result = self.repair()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.config.exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_refuses_symlink_directory_and_fifo(self):
        target = self.directory / 'target'
        target.write_bytes(b'[Autologin]\nSession=' + ABSOLUTE + b'\n')
        for kind in ('symlink', 'directory', 'fifo'):
            with self.subTest(kind=kind):
                if kind == 'symlink':
                    self.config.symlink_to(target)
                elif kind == 'directory':
                    self.config.mkdir()
                else:
                    os.mkfifo(self.config)
                result = self.repair()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('regular file', result.stderr)
                self.assertIn(ABSOLUTE, target.read_bytes())
                self.assertEqual(self.backups(), [])
                if kind == 'directory':
                    self.config.rmdir()
                else:
                    self.config.unlink()

    def test_replace_failure_retains_original_and_backup_and_cleans_staging(self):
        original = b'[Autologin]\nSession=' + ABSOLUTE + b'\n'
        self.config.write_bytes(original)
        # Fault the filesystem replacement in the actual embedded repair, keeping
        # parsing, backup and cleanup code unchanged.
        binary = self.directory / 'bin'
        binary.mkdir()
        python = binary / 'python3'
        python.write_text(f'#!{sys.executable}\nimport os, sys\n'
                          'def fail(*args):\n    raise OSError("synthetic replace failure")\n'
                          'os.replace = fail\nsys.argv = sys.argv[1:]\nexec(sys.stdin.read())\n')
        python.chmod(0o700)
        result = self.repair(env=dict(os.environ, PATH=str(binary) + ':' + os.environ['PATH']))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('synthetic replace failure', result.stderr)
        self.assertEqual(self.config.read_bytes(), original)
        self.assertEqual(self.backups()[0].read_bytes(), original)
        self.assertEqual(set(self.directory.iterdir()), {self.config, binary, *self.backups()})

    def test_repair_mode_rejects_extra_arguments_before_any_changes(self):
        result = subprocess.run(['sh', str(SCRIPT), '--fix-autologin', str(self.config), 'extra'],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(self.directory.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
