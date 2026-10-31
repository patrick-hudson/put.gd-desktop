"""A stalled offline proof command fails within its configured deadline."""
import json
import os
from pathlib import Path
import select
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/proton/native_relink'))
import proof_command


class Commands(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='pg-proof-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.logs = self.root / 'logs'
        self.proof = self.root / 'proof'
        self.logs.mkdir()
        self.proof.mkdir()

    def run_command(self, script, seconds):
        return proof_command.run([sys.executable, '-c', script], 'synthetic.log',
            logs=self.logs, proof=self.proof, timeout_seconds=seconds)

    def test_success_retains_complete_log(self):
        self.assertEqual(self.run_command("print('synthetic proof completed')", 5), 'synthetic proof completed\n')
        self.assertFalse((self.proof / 'command-timeout.json').exists())

    def test_stalled_command_fails_and_records_stage(self):
        with self.assertRaisesRegex(SystemExit, 'timed out: synthetic.log'):
            self.run_command('import time; time.sleep(60)', .05)
        result = json.loads((self.proof / 'command-timeout.json').read_text())
        self.assertEqual(result, {'stage': 'synthetic.log', 'reason': 'command_timeout', 'timeoutSeconds': .05})

    def test_stalled_descendant_is_terminated_with_its_parent(self):
        pid_file = self.root / 'descendant.pid'
        script = ("import pathlib,subprocess,sys,time; "
                  "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)']); "
                  f"pathlib.Path({str(pid_file)!r}).write_text(str(child.pid)); time.sleep(60)")
        child_fd = None
        try:
            with self.assertRaisesRegex(SystemExit, 'timed out: synthetic.log'):
                self.run_command(script, 2)
            self.assertTrue(pid_file.is_file(), 'descendant must start before the deadline')
            try:
                child_fd = os.pidfd_open(int(pid_file.read_text()))
            except ProcessLookupError:
                return
            self.assertTrue(select.select([child_fd], [], [], 2)[0], 'descendant must have exited')
        finally:
            # Cleanup remains bounded even if the regression leaves a live child.
            if child_fd is not None:
                try:
                    signal.pidfd_send_signal(child_fd, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                os.close(child_fd)

    def test_binary_stdout_uses_the_same_deadline_and_keeps_stderr_separate(self):
        script = ("import os,stat,sys; assert stat.S_ISFIFO(os.fstat(1).st_mode); "
                  "sys.stdout.buffer.write(b'\\x00\\xff'*(512*1024)); sys.stderr.write('separate stderr')")
        result = proof_command.run([sys.executable, '-c', script],
            'stdout.log', logs=self.logs, proof=self.proof, timeout_seconds=5, capture_stdout=True)
        self.assertEqual(result.stdout, b'\x00\xff' * (512 * 1024))
        self.assertEqual(result.stderr, b'separate stderr')
        self.assertEqual((self.logs / 'stdout.log').read_bytes(), result.stdout)
        with self.assertRaisesRegex(SystemExit, 'timed out: stdout.log'):
            proof_command.run([sys.executable, '-c', 'import time; time.sleep(60)'],
                'stdout.log', logs=self.logs, proof=self.proof, timeout_seconds=.05, capture_stdout=True)
        self.assertEqual(json.loads((self.proof / 'command-timeout.json').read_text())['stage'], 'stdout.log')

    def test_invalid_deadline_cannot_launch_a_command(self):
        for seconds in (0, -1, float('inf'), float('nan')):
            with self.subTest(seconds=seconds), patch.object(proof_command.subprocess, 'Popen') as run:
                with self.assertRaises(ValueError):
                    self.run_command('raise RuntimeError()', seconds)
                run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
