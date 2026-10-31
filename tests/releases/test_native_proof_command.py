"""A stalled offline proof command fails within its configured deadline."""
import json
from pathlib import Path
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

    def test_invalid_deadline_cannot_launch_a_command(self):
        for seconds in (0, -1, float('inf'), float('nan')):
            with self.subTest(seconds=seconds), patch.object(proof_command.subprocess, 'run') as run:
                with self.assertRaises(ValueError):
                    self.run_command('raise RuntimeError()', seconds)
                run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
