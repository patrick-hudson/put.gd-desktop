"""Network-free contracts for additive snapshot recipes and provenance."""
import argparse
import importlib.util
import json
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
    def test_published_inventory_binds_ids_targets_and_canonical_image_hashes(self):
        records = sorted((ROOT / 'tools/lab/boat/manifests').glob('*.json'))
        self.assertEqual({path.stem for path in records}, {
            'putgd-build-base-v2', 'putgd-build-base-v3', 'putgd-lab-cinnamon-v2',
            'putgd-lab-cinnamon-v3', 'putgd-lab-ubuntu-gnome-v3', 'putgd-lab-ubuntu-gnome-v4'})
        for path in records:
            data = json.loads(path.read_text())
            with self.subTest(snapshot=data['snapshotName']):
                self.assertEqual(path.stem, snapshots.snapshot_name(data['snapshotName']))
                self.assertRegex(data['snapshotId'], r'^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$')
                snapshots.sha(data['builtTreeCommit'])
                for key in ('golden', 'sourceGolden'):
                    snapshots.sha(data[key]['sha256'], 64)
                    self.assertIn('/work/lab-images/', data[key]['path'])
                self.assertEqual(data['packageTarget'], snapshots.package_metadata(path.stem)['packageTarget'])
                self.assertTrue(data['hostPackages'])
                self.assertIn('@sha256:', data['packageBuild']['containerImage'])
                self.assertIn(data['lifecycle'], ('superseded', 'recommended'))

    def test_old_names_are_always_refused(self):
        for name in snapshots.OLD_NAMES:
            with self.subTest(name=name), self.assertRaises(argparse.ArgumentTypeError):
                snapshots.snapshot_name(name)

    def test_wrong_target_is_rejected_before_sealing_writes_any_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / 'work/lab-images/new/image.qcow2'
            with patch.object(snapshots, 'run') as run, self.assertRaises(ValueError):
                snapshots.seal('/source', '/overlay', destination, 'b' * 64, SHA,
                               'putgd-lab-ubuntu-gnome-v4', ['python3'], 'ubuntu24.04')
            run.assert_not_called()
            self.assertFalse(destination.parent.exists())

    def test_verification_script_cannot_install_or_reprovision(self):
        script = snapshots.host_script(ROOT, SHA, 4, False)
        self.assertNotIn('apt-get', script)
        self.assertNotIn('useradd', script)
        self.assertNotIn('provision_runtime.py', script)
        self.assertNotIn('provision-linux-sdk.sh', script)
        self.assertIn('cmake --preset dev', script)
        self.assertNotIn('ctest_retry.py', script)
        self.assertIn('ctest --preset dev --parallel 12', script)
        self.assertIn('scratch_base=${PUTGD_CI_TMPDIR:-/dev/shm}', script)
        self.assertIn('ctest_preflight.py', script)
        self.assertIn('--output-junit \"$PWD/build/dev/snapshot-tests.xml\"', script)

    def test_direct_ctest_requires_exact_complete_passing_registry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / 'build/dev'
            build.mkdir(parents=True)
            (build / 'registered-tests.json').write_text(json.dumps({'tests': [{'name': 'a'}, {'name': 'b'}]}))
            for xml, status, expected in (
                ('<testsuite><testcase name="a"/><testcase name="b"/></testsuite>', 0, True),
                ('<testsuite><testcase name="a"/></testsuite>', 0, False),
                ('<testsuite><testcase name="a"/><testcase name="a"/></testsuite>', 0, False),
                ('<testsuite><testcase name="a"><failure/></testcase><testcase name="b"/></testsuite>', 1, False),
                ('<testsuite><testcase name="a"/><testcase name="b"/></testsuite>', 1, False),
            ):
                (build / 'snapshot-tests.xml').write_text(xml)
                with patch.object(snapshots, 'run', return_value=SHA):
                    self.assertEqual(snapshots.record_ctest(root, status)['ok'], expected)
                ledger = json.loads((build / 'snapshot-flakes.json').read_text())
                self.assertIs(ledger['retryEnabled'], False)
                self.assertEqual(ledger['summary']['retried'], [])

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
