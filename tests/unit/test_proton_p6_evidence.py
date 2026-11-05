"""Replay committed P6 fixtures; excluded from curated production-source exports."""
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DesktopEvidenceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_desktop_replay_agrees_with_receipt_and_rejects_changed_window_values(self):
        evidence = ROOT / 'tools/proton/native_relink/evidence'
        with tarfile.open(evidence / 'p6/measurements.tar.gz') as archive:
            archive.extractall(self.root, filter='data')
        replay = evidence / 'p6/replay.py'
        for desktop in ('cinnamon', 'gnome', 'kde'):
            result = subprocess.run([sys.executable, str(replay), str(self.root / desktop)],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
        original = (evidence / 'desktop-resource-current.json').read_text()
        changed = self.root / 'changed-receipt.json'
        for window in (0, 1):
            for field in ('seconds', 'rssPeakMiB', 'cpuPercentOneCore'):
                with self.subTest(window=window, field=field):
                    receipt = json.loads(original)
                    subject = next(item for item in receipt['desktops'] if item['desktop'] == 'Cinnamon')
                    subject['idleWindows'][window][field] += 1
                    changed.write_text(json.dumps(receipt))
                    result = subprocess.run([sys.executable, str(replay), str(self.root / 'cinnamon'),
                                             '--receipt', str(changed)],
                                            capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('Receipt window disagrees with replay', result.stderr)



if __name__ == '__main__':
    unittest.main()
