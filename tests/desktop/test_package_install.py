"""Run each supervisor in its own process: it adopts only its package descendants."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PackageInstallContracts(unittest.TestCase):
    def run_supervisor(self, body, timeout, recovery, status='Status: install ok installed\n', query_error=False):
        with tempfile.TemporaryDirectory() as directory:
            script = '''import json, sys
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, sys.argv[1])
import package_install as install
commands = [[sys.executable, '-c', sys.argv[3]]]
queries = []
def query(args, **kwargs):
    queries.append(args)
    if sys.argv[7] == 'True':
        raise FileNotFoundError('query unavailable')
    return install.subprocess.CompletedProcess(args, 0, sys.argv[6] if '-s' in args else '', '')
with patch.object(install.subprocess, 'run', side_effect=query):
    result = install.supervise(commands, Path(sys.argv[2]), float(sys.argv[4]), float(sys.argv[5]))
print(json.dumps({'result': result, 'queries': queries}))
'''
            result = subprocess.run([sys.executable, '-c', script, str(ROOT / 'tools/lab'), directory,
                                     body, str(timeout), str(recovery), status, str(query_error)],
                                    text=True, capture_output=True, timeout=20, check=True)
            assert (Path(directory) / 'progress.json').is_file()
            assert (Path(directory) / 'result.json').is_file()
            return json.loads(result.stdout), (Path(directory) / 'apt.log').read_text()

    def test_timeout_waits_for_exact_descendants_then_verifies_package(self):
        output, log = self.run_supervisor(
            "import subprocess,sys; print('partial apt output', flush=True); "
            "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(.8)'])", .2, 3)
        result = output['result']
        self.assertTrue(result['timedOut'])
        self.assertTrue(result['ok'], result)
        self.assertGreaterEqual(result['elapsedSeconds'], .8)
        self.assertGreaterEqual(len(result['ownedProcesses']), 2)
        self.assertEqual(result['remainingPids'], [])
        self.assertEqual(output['queries'], [['dpkg', '-s', 'putgd'], ['dpkg', '--audit']])
        self.assertIn('partial apt output', log)

    def test_abort_kills_only_owned_pids_and_reports_recovery_state(self):
        stranger = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
        try:
            output, _ = self.run_supervisor(
                "import subprocess,sys,time; subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']); time.sleep(30)",
                .3, .3, 'Status: install ok unpacked\n')
            result = output['result']
            self.assertTrue(result['aborted'])
            self.assertFalse(result['ok'])
            self.assertIsNone(stranger.poll())
            self.assertGreaterEqual(len(result['stoppedPids']), 2)
            self.assertEqual(set(result['stoppedPids']), {p['pid'] for p in result['ownedProcesses']})
            self.assertEqual(result['remainingPids'], [])
            self.assertIn('unpacked', result['dpkgStatus']['stdout'])
            for pid in result['stoppedPids']:
                self.assertFalse(Path(f'/proc/{pid}').exists())
        finally:
            stranger.terminate()
            stranger.wait(timeout=5)

    def test_missing_dpkg_still_writes_final_recovery_report(self):
        output, _ = self.run_supervisor('pass', 2, 2, query_error=True)
        result = output['result']
        self.assertFalse(result['ok'])
        self.assertEqual(result['dpkgStatus'], {'error': 'FileNotFoundError'})
        self.assertEqual(result['dpkgAudit'], {'error': 'FileNotFoundError'})
        self.assertEqual(result['remainingPids'], [])

    def test_bad_package_state_cannot_certify_successful_command(self):
        output, _ = self.run_supervisor('pass', 2, 2, 'Status: install ok unpacked\n')
        self.assertFalse(output['result']['ok'])


if __name__ == '__main__':
    unittest.main()
