"""Replay retained baseline/exhaustion evidence and reject unrelated failures."""
import copy
import json
import pathlib
import tempfile
import unittest
from reproducer_checks import assert_expected_failure

class ExpectedFailureChecks(unittest.TestCase):
    def test_retained_reports_and_negative_fixtures(self):
        runs = json.loads(pathlib.Path(__file__).with_name('summary.json').read_text())['runs']
        with tempfile.TemporaryDirectory() as directory:
            archive = pathlib.Path(directory)
            for name in ('d254b2', 'd254e1'):
                report = copy.deepcopy(next(run for run in runs if run['run'] == name))
                report['after'] = report['stop']
                assert_expected_failure(report, archive)
                mutations = [
                    lambda r: r['after'].update(ok=True),
                    lambda r: r['after']['error'].update(code='recording_helper_crashed'),
                    lambda r: r['operationBeforeUpload'].update(state='completed'),
                    lambda r: r['operationBeforeUpload'].update(items=[{'id': 'unexpected-publication'}]),
                    lambda r: r['operationBeforeUpload']['error'].update(code='recording_cancelled'),
                ]
                if report['baseline']:
                    mutations.append(lambda r: r['operationBeforeUpload']['error'].update(message='Unrelated codec failure'))
                else:
                    mutations.extend([
                        lambda r: r['operationBeforeUpload']['error']['details'].update(reason='max_file_bytes'),
                        lambda r: r['operationBeforeUpload']['error']['details'].update(finalized=True),
                    ])
                for index, mutate in enumerate(mutations):
                    with self.subTest(run=name, mutation=index):
                        altered = copy.deepcopy(report)
                        mutate(altered)
                        with self.assertRaises(AssertionError):
                            assert_expected_failure(altered, archive)
                (archive/'unexpected').write_text('synthetic publication')
                with self.assertRaises(AssertionError):
                    assert_expected_failure(report, archive)
                (archive/'unexpected').unlink()

if __name__ == '__main__':
    unittest.main(verbosity=2)
