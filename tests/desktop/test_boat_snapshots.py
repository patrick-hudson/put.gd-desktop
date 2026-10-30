"""Network-free contracts for additive snapshot recipes and provenance."""
import argparse
import contextlib
import importlib.util
import io
import json
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('boat_snapshots', ROOT / 'tools/lab/boat/snapshots.py')
snapshots = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snapshots)
SHA = 'a' * 40


class SnapshotContracts(unittest.TestCase):
    def test_old_names_are_always_refused(self):
        for name in snapshots.OLD_NAMES:
            with self.subTest(name=name), self.assertRaises(argparse.ArgumentTypeError):
                snapshots.snapshot_name(name)

    def test_invalid_names_and_partial_commits_are_refused(self):
        for name in ('putgd-build-base-v1', 'other-v2', 'putgd-x-v2;true', '../putgd-x-v2'):
            with self.subTest(name=name), self.assertRaises(argparse.ArgumentTypeError):
                snapshots.snapshot_name(name)
        for value in ('nightly', 'a' * 12, 'g' * 40):
            with self.assertRaises(argparse.ArgumentTypeError):
                snapshots.sha(value)

    def test_cli_requires_exact_source_and_bounded_jobs(self):
        with contextlib.redirect_stderr(io.StringIO()):
            for extra in (['--commit', 'nightly'], ['--commit', SHA, '--jobs', '9']):
                with self.assertRaises(SystemExit):
                    snapshots.parser().parse_args(['host-script', '--checkout', '/tmp/source', *extra])

    def test_package_targets_distinguish_host_and_gnome_guest(self):
        for name in ('putgd-build-base-v3', 'putgd-lab-cinnamon-v3'):
            self.assertEqual(snapshots.package_metadata(name)['packageTarget'], 'ubuntu24.04')
        metadata = snapshots.package_metadata('putgd-lab-ubuntu-gnome-v4')
        self.assertEqual(metadata['packageTarget'], 'ubuntu26.04')
        self.assertEqual(metadata['hostPackageTarget'], 'ubuntu24.04')
        self.assertEqual(metadata['packageBuild']['method'], 'candidate-native-target')
        self.assertIn('--artifact ubuntu26.04', metadata['packageBuild']['command'])
        with self.assertRaises(ValueError):
            snapshots.package_metadata('putgd-lab-ubuntu-gnome-v4', 'ubuntu24.04')

    def test_package_validation_rejects_noble_deb_for_gnome_and_wrong_version(self):
        package = {'packageTarget': 'ubuntu24.04', 'buildInfo': {'gitCommit': SHA, 'dirty': False}}
        snapshots.validate_package(package, 'ubuntu24.04', SHA)
        with self.assertRaises(ValueError):
            snapshots.validate_package(package, 'ubuntu26.04', SHA)
        with self.assertRaises(ValueError):
            snapshots.validate_package(package, 'ubuntu24.04', 'b' * 40)
        package['buildInfo']['dirty'] = True
        with self.assertRaises(ValueError):
            snapshots.validate_package(package, 'ubuntu24.04', SHA)

    def test_wrong_target_is_rejected_before_sealing_writes_any_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / 'work/lab-images/new/image.qcow2'
            with patch.object(snapshots, 'run') as run, self.assertRaises(ValueError):
                snapshots.seal('/source', '/overlay', destination, 'b' * 64, SHA,
                               'putgd-lab-ubuntu-gnome-v4', ['python3'], 'ubuntu24.04')
            run.assert_not_called()
            self.assertFalse(destination.parent.exists())

    def test_manifest_arguments_reject_unsupported_package_targets(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            snapshots.parser().parse_args(['manifest', '--checkout', '/source', '--name',
                'putgd-lab-ubuntu-gnome-v4', '--golden', '/golden', '--source', '/base',
                '--source-sha256', 'b' * 64, '--output', '/manifest', '--package-target', 'noble'])

    def test_host_smoke_requires_package_identity_and_valid_vnc_port(self):
        args = snapshots.parser().parse_args(['host-package', '--checkout', '/source', '--deb',
            '/package.deb', '--commit', SHA, '--evidence', '/new-evidence'])
        self.assertEqual(args.vnc_port, 5901)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            snapshots.parser().parse_args(['host-package', '--checkout', '/source', '--deb',
                '/package.deb', '--commit', SHA, '--evidence', '/new-evidence', '--vnc-port', '0'])

    def test_current_ci_packages_include_missing_xinput(self):
        packages = snapshots.ci_packages(ROOT)
        self.assertIn('libxcb-xinput-dev', packages)
        self.assertIn('npm', packages)
        self.assertEqual(len(packages), len(set(packages)))

    def test_changed_ci_recipe_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / '.github/workflows').mkdir(parents=True)
            (root / '.github/workflows/ci.yml').write_text('apt-get install something\n')
            with self.assertRaises(ValueError):
                snapshots.ci_packages(root)

    def test_verification_script_cannot_install_or_reprovision(self):
        script = snapshots.host_script(ROOT, SHA, 4, False)
        self.assertNotIn('apt-get', script)
        self.assertNotIn('provision_runtime.py', script)
        self.assertNotIn('provision-linux-sdk.sh', script)
        self.assertIn('cmake --preset dev', script)
        self.assertIn('ctest_retry.py', script)
        self.assertIn('ctest_preflight.py', script)
        self.assertIn('--junit \"$PWD/build/dev/snapshot-tests.xml\"', script)

    def test_preparation_uses_distro_npm_and_ci_provisioners(self):
        script = snapshots.host_script(ROOT, SHA, 4, True)
        self.assertLess(script.index('PATH=/usr/bin:/bin:'), script.index('provision_runtime.py'))
        self.assertIn('# CI tools/ci/provision-linux-sdk.sh', script)
        self.assertIn('python3 tools/verify_dependencies.py build/dependencies/aws-sdk-cpp', script)
        self.assertIn('python3 tools/proton/provision_runtime.py', script)
        self.assertIn('nice -n 10', script)

    def test_probe_check_is_read_only_and_covers_every_tool(self):
        script = snapshots.probe_script()
        self.assertNotIn('sudo', script)
        self.assertNotIn('apt-get', script)
        for tool in ('dbus-monitor', 'xdotool', 'xclip', 'wl-copy', 'wl-paste', 'tesseract'):
            self.assertIn('command -v ' + tool + '\n', script)
        self.assertIn('pyatspi, dbus', script)
        self.assertIn('PyQt6.QtGui', script)

    def test_probe_install_covers_package_contract(self):
        script = snapshots.probe_script(True)
        for package in snapshots.PROBE_PACKAGES:
            self.assertIn(package, script)

    def test_noncanonical_and_symlink_roots_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(ValueError):
                snapshots.canonical_image(root / 'golden/image.qcow2')
            (root / 'real').mkdir()
            (root / 'work').symlink_to(root / 'real', target_is_directory=True)
            with self.assertRaises(ValueError):
                snapshots.canonical_image(root / 'work/lab-images/new/image.qcow2')

    def test_manifest_binds_both_images_commit_packages_and_placement(self):
        result = snapshots.make_manifest('putgd-lab-cinnamon-v2', SHA, '/source/image.qcow2',
                                         'b' * 64, '/tmp/work/lab-images/new/image.qcow2',
                                         'c' * 64, ['python3'], 32 * 1024**3)
        self.assertEqual(result['builtTreeCommit'], SHA)
        self.assertEqual(result['placement'], 'boat')
        self.assertEqual(result['sourceGolden']['sha256'], 'b' * 64)
        self.assertEqual(result['golden']['sha256'], 'c' * 64)
        self.assertTrue(result['golden']['standalone'])
        self.assertIn('tesseract-ocr', result['probePackages'])
        self.assertEqual(result['packageTarget'], 'ubuntu24.04')
        self.assertEqual(result['packageBuild']['artifact'], 'ubuntu24.04')

    def test_manifest_refuses_unchanged_golden_or_duplicate_packages(self):
        for image_hash, packages in (('b' * 64, ['python3']), ('c' * 64, ['python3', 'python3'])):
            with self.assertRaises(ValueError):
                snapshots.make_manifest('putgd-lab-cinnamon-v2', SHA, '/source/image.qcow2',
                                         'b' * 64, '/tmp/work/lab-images/new/image.qcow2',
                                         image_hash, packages, 32 * 1024**3)

    def test_generated_shell_handles_checkout_paths_with_spaces(self):
        with patch.object(snapshots, 'ci_packages', return_value=['python3']):
            script = snapshots.host_script(Path("/tmp/a b'c"), SHA, 4, False)
        subprocess.run(['bash', '-n'], input=script, text=True, check=True)
        capacity_line = next(line for line in script.splitlines() if line.startswith('python3 -c '))
        import shlex
        code = shlex.split(capacity_line)[2]
        compile(code, '<capacity>', 'exec')

    def test_build_manifest_refuses_dirty_or_wrong_build_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            checkout = Path(temporary)
            identity = checkout / 'build/dev/build-identity/build-info.json'
            identity.parent.mkdir(parents=True)
            for data in ({'gitCommit': SHA, 'dirty': True}, {'gitCommit': 'b' * 40, 'dirty': False}):
                identity.write_text(json.dumps(data))
                with patch.object(snapshots, 'run', return_value=SHA), self.assertRaises(ValueError):
                    snapshots.build_manifest(checkout, 'putgd-build-base-v2', Path('/golden'),
                                             Path('/source'), 'b' * 64, checkout / 'manifest.json')

    def test_build_manifest_rejects_empty_successful_ctest_ledger(self):
        with tempfile.TemporaryDirectory() as temporary:
            checkout = Path(temporary)
            directory = checkout / 'build/dev'
            (directory / 'build-identity').mkdir(parents=True)
            (directory / 'build-identity/build-info.json').write_text(json.dumps({'gitCommit': SHA, 'dirty': False}))
            (directory / 'snapshot-flakes.json').write_text(json.dumps({'summary': {'ok': True, 'tests': 0}}))
            with patch.object(snapshots, 'run', return_value=SHA), patch.object(snapshots, 'digest', return_value='b' * 64), self.assertRaises(ValueError):
                snapshots.build_manifest(checkout, 'putgd-build-base-v2', Path('/golden'),
                                         Path('/source'), 'b' * 64, checkout / 'manifest.json')

    def test_manifest_write_never_overwrites(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'manifest.json'
            snapshots.exclusive_json(path, {'first': True})
            with self.assertRaises(FileExistsError):
                snapshots.exclusive_json(path, {'second': True})
            self.assertEqual(json.loads(path.read_text()), {'first': True})

    def test_publish_refuses_existing_new_name_before_mutation(self):
        with patch.object(snapshots, 'run', return_value=json.dumps({'named': [{'name': 'putgd-lab-cinnamon-v2'}]})) as run:
            with self.assertRaises(ValueError):
                snapshots.publish('bx_owned', 'putgd-lab-cinnamon-v2', '/unused', '/unused')
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.args[0][2], 'snapshots')

    def test_publish_handles_jsonl_and_reads_back_exact_snapshot_id(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / 'manifest.json'
            manifest.write_text(json.dumps({'snapshotName': 'putgd-lab-cinnamon-v2', 'placement': 'boat'}))
            saved = {'name': 'putgd-lab-cinnamon-v2', 'sourceSandboxId': 'bx_owned',
                     'status': 'ready', 'snapshotId': 'exact-id'}
            responses = [json.dumps({'named': []}), '{"event":"saving"}\n{"event":"saved"}\n',
                         json.dumps({'named': [saved]})]
            with patch.object(snapshots, 'run', side_effect=responses) as run:
                snapshots.publish('bx_owned', saved['name'], manifest, root / 'receipt.json')
            result = json.loads((root / 'receipt.json').read_text())
            self.assertEqual(result['snapshot']['snapshotId'], 'exact-id')
            self.assertEqual(len(result['request']['events']), 2)
            self.assertEqual(sum(call.args[0][2] == 'snapshot' for call in run.call_args_list), 1)

    def test_lost_save_reply_recovers_without_repeating_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / 'manifest.json'
            manifest.write_text(json.dumps({'snapshotName': 'putgd-lab-cinnamon-v2', 'placement': 'boat'}))
            saved = {'name': 'putgd-lab-cinnamon-v2', 'sourceSandboxId': 'bx_owned',
                     'status': 'ready', 'snapshotId': 'exact-id'}
            responses = [json.dumps({'named': []}), RuntimeError('reply lost'), json.dumps({'named': [saved]})]
            with patch.object(snapshots, 'run', side_effect=responses) as run:
                snapshots.publish('bx_owned', saved['name'], manifest, root / 'receipt.json')
            self.assertEqual(sum(call.args[0][2] == 'snapshot' for call in run.call_args_list), 1)
            self.assertEqual(json.loads((root / 'receipt.json').read_text())['request']['errorType'], 'RuntimeError')

    def test_receipt_rejects_a_different_source_worker_without_writing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / 'manifest.json'
            manifest.write_text(json.dumps({'snapshotName': 'putgd-lab-cinnamon-v2', 'placement': 'boat'}))
            saved = {'name': 'putgd-lab-cinnamon-v2', 'sourceSandboxId': 'bx_other', 'status': 'ready', 'snapshotId': 'wrong'}
            with patch.object(snapshots, 'run', return_value=json.dumps({'named': [saved]})), self.assertRaises(ValueError):
                snapshots.publication_receipt('bx_owned', saved['name'], manifest, root / 'receipt.json')
            self.assertFalse((root / 'receipt.json').exists())

    def test_receipt_waits_for_ready_binding_without_issuing_save(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / 'manifest.json'
            manifest.write_text(json.dumps({'snapshotName': 'putgd-lab-cinnamon-v2', 'placement': 'boat'}))
            pending = {'name': 'putgd-lab-cinnamon-v2', 'sourceSandboxId': 'bx_owned', 'status': 'saving'}
            ready = {**pending, 'status': 'ready', 'snapshotId': 'exact-id'}
            with patch.object(snapshots, 'run', side_effect=[json.dumps({'named': [pending]}), json.dumps({'named': [ready]})]) as run, patch.object(snapshots.time, 'sleep'):
                snapshots.publication_receipt('bx_owned', pending['name'], manifest, root / 'receipt.json')
            self.assertTrue(all(call.args[0][2] == 'snapshots' for call in run.call_args_list))

    def test_seal_refuses_existing_directory_before_running_qemu(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / 'work/lab-images/existing/image.qcow2'
            destination.parent.mkdir(parents=True)
            with patch.object(snapshots, 'run') as run, self.assertRaises(ValueError):
                snapshots.seal('/source', '/overlay', destination, 'b' * 64, SHA,
                               'putgd-lab-cinnamon-v2', ['python3'])
            run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
