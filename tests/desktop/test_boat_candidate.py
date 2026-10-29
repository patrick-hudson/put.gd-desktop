"""Light, network-free controller contracts; no compilation or desktop claims."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('boat_candidate', ROOT / 'tools/lab/boat/candidate.py')
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)
SHA = 'a' * 40


class CandidateContracts(unittest.TestCase):
    def test_requires_full_sha_and_new_output_argument(self):
        with contextlib.redirect_stderr(io.StringIO()):
            for value in ('nightly', 'a' * 12, 'a' * 39, 'g' * 40, '--help-me'):
                with self.subTest(value=value), self.assertRaises(SystemExit):
                    candidate.parser().parse_args([value, '--output', '/tmp/result'])
        self.assertEqual(candidate.exact_sha('A' * 40), SHA)

    def test_ttl_is_finite_and_bounded(self):
        for value in ('0', '-1', '599', '14401', 'abc'):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                candidate.ttl_seconds(value)
        self.assertEqual(candidate.ttl_seconds('10800'), 10800)

    def test_artifact_names_are_closed(self):
        self.assertEqual(len(candidate.TARGETS), 7)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            candidate.parser().parse_args([SHA, '--output', '/tmp/result', '--artifact', 'latest'])

    def test_sha_must_identify_a_commit_object_before_any_worker_is_started(self):
        with patch.object(candidate, 'run', return_value='tree\n'), self.assertRaises(ValueError):
            candidate.load_workflows(SHA)

    def test_capacity_requires_room_for_entire_fanout(self):
        limits = {'canStart': True, 'maxActiveSandboxes': 100, 'activeSandboxes': 3,
                  'starts': {period: {'remaining': 10} for period in ('minute', 'hour', 'day')}}
        candidate.check_capacity(limits, 7)
        limits['starts']['minute']['remaining'] = 6
        with self.assertRaises(ValueError):
            candidate.check_capacity(limits, 7)

    def test_output_rejects_git_even_when_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            with self.assertRaises(ValueError):
                candidate.prepare_output(root / 'work/ignored/result')

    def test_output_is_private_and_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'new'
            self.assertEqual(candidate.prepare_output(output), output)
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            with self.assertRaises(FileExistsError):
                candidate.prepare_output(output)

    def test_manifest_inventory_hashes_nested_catalog_and_package_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'ui-catalog').mkdir()
            (root / 'ui-catalog/window.png').write_bytes(b'synthetic PNG')
            (root / 'candidate.deb').write_bytes(b'synthetic DEB')
            files = candidate.inventory(root)
            self.assertEqual([x['path'] for x in files], ['candidate.deb', 'ui-catalog/window.png'])
            self.assertEqual(files[0]['sha256'], hashlib.sha256(b'synthetic DEB').hexdigest())
            self.assertEqual(files[0]['bytes'], 13)

    def test_inventory_refuses_symlinks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'escape').symlink_to('/etc/passwd')
            with self.assertRaises(ValueError):
                candidate.inventory(root)

    def test_identity_rejects_dirty_wrong_commit_or_mismatched_binary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'candidate.deb').write_bytes(b'package')
            identity = {'gitCommit': SHA, 'version': '26.1029.1-dev', 'dirty': False}
            candidate.write_json(root / 'build-info.json', identity)
            candidate.write_json(root / 'binary-version.json', identity)
            self.assertEqual(candidate.verify_identity(root, SHA, 'ubuntu24.04'), identity)
            with self.assertRaises(ValueError):
                candidate.verify_identity(root, 'b' * 40, 'ubuntu24.04')
            for field, value in [('dirty', True), ('version', 'wrong')]:
                bad = dict(identity, **{field: value})
                candidate.write_json(root / ('build-info.json' if field == 'dirty' else 'binary-version.json'), bad)
                with self.assertRaises(ValueError):
                    candidate.verify_identity(root, SHA, 'ubuntu24.04')
                candidate.write_json(root / 'build-info.json', identity)

    def test_missing_or_duplicate_package_is_not_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            identity = {'gitCommit': SHA, 'version': 'test', 'dirty': False}
            for name in ('build-info.json', 'binary-version.json'):
                candidate.write_json(root / name, identity)
            with self.assertRaises(ValueError):
                candidate.verify_identity(root, SHA, 'fedora44')
            (root / 'one.rpm').touch()
            (root / 'two.rpm').touch()
            with self.assertRaises(ValueError):
                candidate.verify_identity(root, SHA, 'fedora44')

    def test_recipe_expressions_fail_closed(self):
        job = {'steps': [{'name': 'build', 'run': '${{ matrix.name }} ${{ other.value }}'}]}
        with self.assertRaises(ValueError):
            candidate.step(job, 'build', {'matrix.name': 'ubuntu24.04'})
        with self.assertRaises(ValueError):
            candidate.step(job, 'missing')

    def test_inline_ci_steps_cannot_join_into_one_shell_token(self):
        job = {'steps': [{'name': 'proton', 'run': 'python3 provision.py'}]}
        self.assertEqual(candidate.step(job, 'proton') + 'export FORMAT=DEB\n',
                         'python3 provision.py\nexport FORMAT=DEB\n')

    def test_current_workflow_recipes_keep_pins_audits_and_socket_preflight(self):
        import yaml
        packages = yaml.safe_load((ROOT / '.github/workflows/package.yml').read_text())
        ci = yaml.safe_load((ROOT / '.github/workflows/ci.yml').read_text())
        for target in candidate.TARGETS:
            with self.subTest(target=target):
                image, script, preset = candidate.recipe(target, packages, ci)
                self.assertRegex(image, r'@sha256:[a-f0-9]{64}$')
                self.assertNotIn('${{', script)
                self.assertIn('tools/proton/provision_runtime.py\n', script)
                if target in ('ubuntu24.04', 'ubuntu26.04', 'debian13', 'fedora44'):
                    self.assertIn('tools/releases/native.py', script)
                if target == 'ui-catalog':
                    self.assertLess(script.index('check_socket_paths.py'), script.index('build/dev/putgd-ui-catalog'))
                    self.assertNotIn('XDG_RUNTIME_DIR="$(mktemp -d)"', script)
                if target == 'ubuntu24.04':
                    self.assertIn('tools/releases/source_archive.py', script)

    def test_duplicate_artifacts_are_rejected_before_provider_calls(self):
        with patch.object(candidate, 'boat') as boat, contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            candidate.main([SHA, '--output', '/tmp/example', '--artifact', 'appimage', '--artifact', 'appimage'])
        boat.assert_not_called()

    def test_complete_manifest_binds_every_artifact_hash_commit_and_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'collected'
            def boat(command):
                if command == 'limits':
                    return {'canStart': True, 'maxActiveSandboxes': 10, 'activeSandboxes': 1,
                            'starts': {p: {'remaining': 10} for p in ('minute', 'hour', 'day')},
                            'machineTypes': [{'type': 'large', 'available': True}]}
                self.assertEqual(command, 'snapshots')
                return {'named': [{'name': 'putgd-build-base', 'snapshotId': 'snapshot-exact', 'createdAt': 'test'}]}
            def worker(target, args, root, specification):
                artifact = root / target / 'artifacts'
                artifact.mkdir(parents=True)
                (artifact / 'package').write_bytes(target.encode())
                return {'artifact': target, 'commit': args.commit, 'sandboxId': 'bx_' + target,
                        'state': 'passed', 'cleanupVerified': True, 'files': candidate.inventory(artifact)}
            with patch.object(candidate, 'boat', side_effect=boat), \
                 patch.object(candidate, 'load_workflows', return_value=({}, {})), \
                 patch.object(candidate, 'recipe', return_value=('image', 'script', 'release')), \
                 patch.object(candidate, 'build_worker', side_effect=worker), \
                 patch.object(candidate.signal, 'signal'), contextlib.redirect_stdout(io.StringIO()):
                code = candidate.main([SHA, '--output', str(output), '--artifact', 'appimage', '--artifact', 'fedora44'])
            manifest = json.loads((output / 'manifest.json').read_text())
            self.assertEqual(code, 0)
            self.assertEqual(manifest['commit'], SHA)
            self.assertEqual(manifest['snapshot']['snapshotId'], 'snapshot-exact')
            self.assertEqual(manifest['ownedSandboxesRemaining'], [])
            self.assertTrue(manifest['passed'])
            self.assertEqual([x['artifact'] for x in manifest['artifacts']], ['appimage', 'fedora44'])
            sums = (output / 'SHA256SUMS').read_text().splitlines()
            self.assertEqual(len(sums), 2)
            for line in sums:
                digest, path = line.split('  ')
                self.assertEqual(candidate.sha256(output / path), digest)

    def test_failed_creation_after_created_event_still_deletes_owned_worker(self):
        with tempfile.TemporaryDirectory() as temporary:
            def create(argv, **kwargs):
                kwargs['log'].write('{"event":"created","id":"bx_owned"}\n')
                kwargs['log'].flush()
                raise RuntimeError('startup failed')
            def boat(*args, **kwargs):
                if args[0] == 'delete':
                    self.assertEqual(args[1], 'bx_owned')
                    return {'operation': {'id': 'bdop_owned', 'targetId': 'bx_owned',
                                          'kind': 'sandbox', 'status': 'pending', 'stage': 'removing'}}
                self.assertEqual(args, ('list',))
                return {'sandboxes': [{'id': 'bx_unrelated'}]}
            args = argparse.Namespace(commit=SHA, type='large', ttl=10800)
            with patch.object(candidate, 'run', side_effect=create), patch.object(candidate, 'boat', side_effect=boat), contextlib.redirect_stdout(io.StringIO()):
                result = candidate.build_worker('appimage', args, Path(temporary), ('image', 'build', 'release'))
            self.assertEqual(result['state'], 'failed')
            self.assertTrue(result['cleanupVerified'])
            self.assertEqual(result['sandboxId'], 'bx_owned')
            self.assertEqual(result['deletion']['id'], 'bdop_owned')
            self.assertEqual(result['deletion']['targetId'], 'bx_owned')
            self.assertEqual(result['deletion']['status'], 'pending')


if __name__ == '__main__':
    unittest.main()
