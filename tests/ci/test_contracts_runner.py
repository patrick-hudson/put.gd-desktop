"""tools/ci/contracts.py: every suite runs, and a failure is diagnosable from the job log.

Synthetic suites in a temporary root; no repository suite runs here.
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import textwrap
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('putgd_contracts', ROOT / 'tools/ci/contracts.py')
contracts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contracts)


class ContractsRunnerTests(unittest.TestCase):
    def test_failing_suite_prints_its_case_and_traceback_to_the_job_log(self):
        with tempfile.TemporaryDirectory(prefix='contracts-runner-') as directory:
            root = Path(directory)
            (root / 'suites').mkdir()
            (root / 'suites/test_a_pass.py').write_text('print("quiet pass")\n')
            (root / 'suites/test_b_fail.py').write_text(textwrap.dedent('''\
                import unittest
                class Synthetic(unittest.TestCase):
                    def test_reaped_descendant(self):
                        raise ProcessLookupError(3, 'No such process')
                unittest.main()
            '''))
            output = root / 'out'
            output.mkdir()
            stdout, stderr = io.StringIO(), io.StringIO()
            groups = {'synthetic': {'globs': ('suites/test_*.py',), 'scope': 'synthetic'}}
            with patch.object(contracts, 'ROOT', root), patch.dict(contracts.GROUPS, groups), \
                    contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                code = contracts.run_group('synthetic', output)
            self.assertEqual(code, 1)
            self.assertIn('ok   suites/test_a_pass.py', stdout.getvalue())
            self.assertIn('FAIL suites/test_b_fail.py', stdout.getvalue())
            log = stderr.getvalue()
            self.assertIn('ERROR: test_reaped_descendant', log)
            self.assertIn('ProcessLookupError: [Errno 3] No such process', log)
            self.assertNotIn('quiet pass', log)
            self.assertTrue(log.rstrip().endswith('Failed: suites/test_b_fail.py'))
            report = json.loads((output / 'synthetic-contracts.json').read_text())
            self.assertEqual([suite['exitCode'] for suite in report['suites']], [0, 1])
            self.assertIn('quiet pass', (output / 'synthetic-contracts.log').read_text())


if __name__ == '__main__':
    unittest.main()
