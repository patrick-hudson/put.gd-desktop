"""Secret-scan regressions; binary controls also run with the pinned CI scanner."""
import importlib.util
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('sanitize_gitleaks', ROOT / 'tools/ci/sanitize_gitleaks_sarif.py')
sanitizer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sanitizer)


def raw_report():
    return {'version': '2.1.0', 'properties': {'unneeded': 'PRIVATE_SENTINEL'}, 'runs': [{
        'tool': {'driver': {'name': 'gitleaks'}},
        'results': [{'ruleId': 'aws-access-token',
                     'message': {'text': 'PRIVATE_SENTINEL'},
                     'partialFingerprints': {'commitSha': 'a' * 40, 'commitMessage': 'PRIVATE_SENTINEL',
                                             'author': 'PRIVATE_SENTINEL', 'email': 'PRIVATE_SENTINEL',
                                             'date': 'PRIVATE_SENTINEL'},
                     'properties': {'tags': ['PRIVATE_SENTINEL']},
                     'locations': [{'physicalLocation': {
                         'artifactLocation': {'uri': 'config.txt'},
                         'region': {'startLine': 2, 'endLine': 2, 'startColumn': 1, 'endColumn': 20,
                                    'snippet': {'text': 'PRIVATE_SENTINEL'}}}}]}]}]}


class Sanitization(unittest.TestCase):
    def test_only_diagnostic_locations_rule_and_commit_survive(self):
        clean = sanitizer.sanitize(raw_report())
        self.assertNotIn('PRIVATE_SENTINEL', json.dumps(clean))
        result = clean['runs'][0]['results'][0]
        self.assertEqual(result['ruleId'], 'aws-access-token')
        self.assertEqual(result['partialFingerprints'], {'commitSha': 'a' * 40})
        physical = result['locations'][0]['physicalLocation']
        self.assertEqual(physical['artifactLocation']['uri'], 'config.txt')
        self.assertEqual(physical['region'], {'startLine': 2, 'endLine': 2, 'startColumn': 1, 'endColumn': 20})

    def test_empty_reports_and_multiple_runs(self):
        report = raw_report()
        report['runs'].append({'results': []})
        clean = sanitizer.sanitize(report)
        self.assertEqual(clean['runs'][1]['results'], [])
        self.assertEqual(clean['version'], '2.1.0')
        self.assertEqual(sanitizer.sanitize({'version': '2.1.0', 'runs': []})['runs'], [])

    def test_invalid_report_types_and_versions_are_rejected(self):
        for report in (None, [], 'PRIVATE_SENTINEL', {}, {'version': '2.0.0', 'runs': []},
                       {'version': 'PRIVATE_SENTINEL', 'runs': []}):
            with self.subTest(report=report), self.assertRaises(ValueError):
                sanitizer.sanitize(report)

    def test_malformed_collections_and_retained_fields_are_rejected(self):
        for value in (None, {}, '', 1, [None], ['PRIVATE_SENTINEL']):
            for path in (('runs',), ('runs', 0, 'results'), ('runs', 0, 'results', 0, 'locations')):
                report = copy.deepcopy(raw_report())
                target = report
                for key in path[:-1]:
                    target = target[key]
                target[path[-1]] = value
                with self.subTest(path=path, value=value), self.assertRaises(ValueError):
                    sanitizer.sanitize(report)
        for path, value in ((('ruleId',), {}), (('partialFingerprints',), []),
                            (('partialFingerprints', 'commitSha'), {}),
                            (('locations', 0, 'physicalLocation', 'region', 'startLine'), 'PRIVATE_SENTINEL'),
                            (('locations', 0, 'physicalLocation', 'region', 'startColumn'), False),
                            (('locations', 0, 'physicalLocation', 'region', 'endLine'), 0),
                            (('locations', 0, 'physicalLocation', 'region'), {}),
                            (('locations', 0, 'physicalLocation', 'region'), {'startColumn': 2}),
                            (('locations', 0, 'physicalLocation', 'artifactLocation', 'uri'), {})):
            report = raw_report()
            target = report['runs'][0]['results'][0]
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
            with self.subTest(path=path, value=value), self.assertRaises(ValueError):
                sanitizer.sanitize(report)

    def test_failed_scan_is_sanitized_before_always_upload_and_status_is_preserved(self):
        workflow = (ROOT / '.github/workflows/ci.yml').read_text()
        scan = workflow.split('- name: Scan every branch and tag for leaked secrets', 1)[1].split('- name:', 1)[0]
        # Exercise the actual workflow commands after binary provisioning.
        script = '\n'.join(line[10:] for line in scan.splitlines())
        script = script[script.index('mkdir -p build/contracts'):].strip()
        for status, malformed in ((0, False), (1, False), (2, False), (1, True)):
            with self.subTest(status=status, malformed=malformed), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / 'tools').symlink_to(ROOT / 'tools', target_is_directory=True)
                runtime = root / 'runtime'
                runtime.mkdir()
                scanner = runtime / 'gitleaks'
                scanner.write_text('#!' + sys.executable + '\nimport pathlib, sys\n'
                                   'path = pathlib.Path(sys.argv[sys.argv.index("--report-path") + 1])\n'
                                   'path.write_text(' + repr('PRIVATE_SENTINEL' if malformed else json.dumps(raw_report())) + ')\n'
                                   'sys.exit(' + str(status) + ')\n')
                scanner.chmod(0o700)
                result = subprocess.run(['bash', '-eo', 'pipefail', '-c', script], cwd=root,
                                        env=dict(os.environ, RUNNER_TEMP=str(runtime)), capture_output=True, text=True)
                self.assertEqual(result.returncode, 1 if malformed else status, result.stderr)
                output = root / 'build/contracts/gitleaks.sarif'
                if malformed:
                    self.assertFalse(output.exists())
                else:
                    self.assertNotIn('PRIVATE_SENTINEL', output.read_text())
                self.assertNotIn('PRIVATE_SENTINEL', result.stdout + result.stderr)
                self.assertEqual(list(runtime.glob('gitleaks-report.*')), [])
                self.assertEqual(sorted(p.name for p in (root / 'build/contracts').iterdir()),
                                 [] if malformed else ['gitleaks.sarif'])


class Configuration(unittest.TestCase):
    def test_shapes_require_rule_path_and_value_and_no_line_markers_remain(self):
        config = tomllib.loads((ROOT / '.gitleaks.toml').read_text())
        self.assertTrue(config['extend']['useDefault'])
        for allowlist in config['allowlists']:
            self.assertEqual(allowlist['regexTarget'], 'secret')
            if 'paths' in allowlist:
                self.assertEqual(allowlist['condition'], 'AND')
                self.assertTrue(allowlist['targetRules'])
                self.assertTrue(allowlist['regexes'])
        for allowlist in config['allowlists'][:3]:
            self.assertTrue(allowlist['paths'])
            self.assertTrue(allowlist['targetRules'])
            self.assertNotIn('squarespace-access-token', allowlist['targetRules'])


@unittest.skipUnless(os.environ.get('GITLEAKS_BIN'), 'set GITLEAKS_BIN to the pinned 8.30.1 scanner')
class BinaryControls(unittest.TestCase):
    def test_every_ref_reports_tokens_despite_shapes_and_fixture_markers(self):
        binary = str(Path(os.environ['GITLEAKS_BIN']).resolve())
        version = subprocess.check_output([binary, 'version'], text=True).strip()
        self.assertEqual(version, '8.30.1')
        with tempfile.TemporaryDirectory(prefix='pg-leaks-') as directory:
            root = Path(directory)
            env = dict(os.environ, GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
                       GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid')

            def git(*args):
                return subprocess.check_output(['git', '-C', str(root), '-c', 'maintenance.auto=false',
                                                '-c', 'gc.auto=0', *args], env=env, stderr=subprocess.DEVNULL)

            def write(path, value):
                file = root / path
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(value + '\n')

            # Assemble fake controls at runtime so their source is not itself a finding.
            digest = '29ac83fd' + '75be1024' + 'd9f063a1' + 'b725ec84' + '36d01fa9'
            uuid = '-'.join(('29ac83fd', '75be', '4024', 'a9f0', '63a1b725ec84'))
            aws = 'AKIA' + 'QWERTYUI' + 'ASDFGHJK'
            git('init', '-q', '-b', 'main')
            write('base.txt', 'scratch fixture')
            git('add', '.')
            git('commit', '-qm', 'base')
            git('checkout', '-qb', 'sourcegraph-control')
            write('sourcegraph.txt', 'sourcegraph_token = "' + digest + '"')
            git('add', '.')
            git('commit', '-qm', 'branch-only control')
            git('checkout', '-q', 'main')
            write('squarespace.txt', 'squarespace_token = "' + uuid + '"')
            git('add', '.')
            git('commit', '-qm', 'tag-only control')
            git('tag', 'squarespace-control')
            git('reset', '--hard', 'HEAD^')
            write('aws.txt', 'synthetic aws_key = "' + aws + '"')
            write('redact.txt', 'redact::text("' + aws + '")')
            write('quarterly.txt', 'Quarterly Secret Plan.png ' + aws)
            write('src/windows/tests/test_diagnostics.cpp', 'synthetic ' + aws)
            # Check the rule boundary even inside an allowed evidence path.
            write('docs/evidence/control/manifest.json', 'sourcegraph_token = "' + digest + '"\n'
                  'squarespace_token = "' + uuid + '"')
            # Same digest/UUID are benign only for generic-api-key in the scoped paths.
            write('docs/evidence/control/receipt.json', 'api_key = "' + digest + '"\n'
                  'object_key = "' + uuid + '"')
            write('outside.txt', 'api_key = "' + digest + '"\nobject_key = "' + uuid + '"')
            git('add', '.')
            git('commit', '-qm', 'PRIVATE_SENTINEL commit metadata')
            raw = root / 'raw.sarif'
            result = subprocess.run([binary, 'git', str(root), '--config', str(ROOT / '.gitleaks.toml'),
                                     '--log-opts=--all', '--redact', '--no-banner', '--exit-code', '1',
                                     '--report-format', 'sarif', '--report-path', str(raw)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, 'negative controls must fail the scan')
            report = json.loads(raw.read_text())
            findings = {(f['ruleId'], f['locations'][0]['physicalLocation']['artifactLocation']['uri'])
                        for run in report['runs'] for f in run['results']}
            for expected in (('sourcegraph-access-token', 'sourcegraph.txt'),
                             ('squarespace-access-token', 'squarespace.txt'), ('aws-access-token', 'aws.txt'),
                             ('aws-access-token', 'redact.txt'), ('aws-access-token', 'quarterly.txt'),
                             ('aws-access-token', 'src/windows/tests/test_diagnostics.cpp'),
                             ('sourcegraph-access-token', 'docs/evidence/control/manifest.json'),
                             ('squarespace-access-token', 'docs/evidence/control/manifest.json'),
                             ('generic-api-key', 'outside.txt')):
                self.assertIn(expected, findings)
            self.assertNotIn(('generic-api-key', 'docs/evidence/control/receipt.json'), findings)
            clean = json.dumps(sanitizer.sanitize(report))
            for value in (digest, uuid, aws, 'PRIVATE_SENTINEL'):
                self.assertNotIn(value, clean)


if __name__ == '__main__':
    unittest.main()
