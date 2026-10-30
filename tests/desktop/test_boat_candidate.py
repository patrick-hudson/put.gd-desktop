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
import tarfile
import signal
import sys
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
            for suffix in ('work/ignored/result', '.git/result'):
                with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                    candidate.prepare_output(root / suffix)

    def test_output_rejects_bare_repositories(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '--bare', '-q', str(root)], check=True)
            with self.assertRaises(ValueError):
                candidate.prepare_output(root / 'result')

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

    def test_fixture_is_unmistakably_test_only(self):
        import yaml
        packages = yaml.safe_load((ROOT / '.github/workflows/package.yml').read_text())
        _, script, _ = candidate.recipe('lab-fixture', packages, {})
        self.assertIn('CPACK_DEBIAN_PACKAGE_NAME=putgd-lab-fixture', script)
        self.assertIn('-DPUTGD_PACKAGE_TARGET= -DPUTGD_UPDATE_CHECK=OFF', script)
        self.assertIn('dpkg-deb -f', script)
        self.assertIn('CPACK_DEBIAN_PACKAGE_CONFLICTS=putgd', script)
        self.assertIn('CPACK_DEBIAN_PACKAGE_REPLACES=putgd', script)
        self.assertIn('Disposable lab fixture build', script)
        self.assertIn('native.py audit intentionally not run', ' '.join(candidate.ci_differences('lab-fixture')))

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
                if target == 'appimage':
                    self.assertLess(script.index('LOG_CFG=$PWD/tools/ci/aqt-logging.ini'),
                                    script.index('/opt/aqt/bin/aqt install-qt'))
                    self.assertIn('RUNNER_TEMP=$runner_state', script)
                    self.assertLess(script.index('install-packages.sh python3\n'),
                                    script.index('tools/ci/hosted_apt_mirrors.py'))
                    self.assertIn('QT_PLUGIN_PATH=$QT_ROOT_DIR/plugins QML2_IMPORT_PATH=$QT_ROOT_DIR/qml', script)
                self.assertNotIn('/out/summary', script)
                self.assertNotIn('/out/aqt', script)

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
            def closed_output(*args, **kwargs):
                self.assertTrue((output / 'manifest.json').is_file())
                self.assertTrue((output / 'SHA256SUMS').is_file())
                self.assertTrue(json.loads((output / 'manifest.json').read_text())['passed'])
                raise BrokenPipeError('closed pipe')
            with patch.object(candidate, 'boat', side_effect=boat), \
                 patch.object(candidate, 'load_workflows', return_value=({}, {})), \
                 patch.object(candidate, 'recipe', return_value=('image', 'script', 'release')), \
                 patch.object(candidate, 'build_worker', side_effect=worker), \
                 patch.object(candidate.signal, 'signal'), \
                 patch('builtins.print', side_effect=closed_output) as printer:
                code = candidate.main([SHA, '--output', str(output), '--artifact', 'appimage', '--artifact', 'fedora44'])
            self.assertTrue(printer.called)
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

    def make_archive(self, path, entries):
        with tarfile.open(path, 'w') as stream:
            for name, kind in entries:
                member = tarfile.TarInfo(name)
                member.mode = 0o755 if name.endswith('.AppImage') else 0o644
                if kind == 'file':
                    member.size = 4
                    stream.addfile(member, io.BytesIO(b'data'))
                else:
                    member.type = kind
                    member.linkname = '/etc/passwd'
                    stream.addfile(member)

    def test_archive_restores_nested_catalog_after_hash_verification(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / 'artifact.tar'
            self.make_archive(archive, [('ui-catalog/window.png', 'file'), ('putgd.AppImage', 'file')])
            candidate.restore_archive(archive, root / 'artifacts', candidate.sha256(archive))
            self.assertEqual((root / 'artifacts/ui-catalog/window.png').read_bytes(), b'data')
            self.assertEqual((root / 'artifacts/putgd.AppImage').stat().st_mode & 0o777, 0o700)
            self.assertEqual((root / 'artifacts/ui-catalog/window.png').stat().st_mode & 0o777, 0o600)
            with self.assertRaisesRegex(ValueError, 'SHA-256'):
                candidate.restore_archive(archive, root / 'bad', 'a' * 64)
            self.assertFalse((root / 'bad').exists())

    def test_archive_rejects_traversal_links_devices_and_duplicates_before_extracting(self):
        bad_entries = [('../escape', 'file'), ('/absolute', 'file'),
                       ('symlink', tarfile.SYMTYPE), ('hardlink', tarfile.LNKTYPE),
                       ('device', tarfile.CHRTYPE), ('pipe', tarfile.FIFOTYPE)]
        for entry in bad_entries + [('safe', 'file')]:
            with self.subTest(entry=entry), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = root / 'artifact.tar'
                self.make_archive(archive, [('safe', 'file'), entry])
                with self.assertRaises(ValueError):
                    candidate.restore_archive(archive, root / 'out', candidate.sha256(archive))
                self.assertFalse((root / 'out').exists())

    def test_collection_uses_one_archive_and_keeps_reserve_and_failure_evidence(self):
        for budget in (1, 600):
            with self.subTest(budget=budget), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = root / 'artifacts.tar'
                self.make_archive(archive, [('catalog/window.png', 'file')])
                digest = candidate.sha256(archive)
                receipt = {}
                with patch.object(candidate, 'run', side_effect=['1000 .\n1\nfilesystem\n', digest + '  artifacts.tar', '']) as run:
                    candidate.collect_artifacts(['ssh'], ['-F', '/dev/null'], 'host', '22', root, receipt, None, timeout=budget)
                self.assertEqual(run.call_count, 3)
                self.assertIn('tar -cf', run.call_args_list[1].args[0][-1])
                scp = run.call_args_list[2].args[0]
                self.assertEqual(scp[0], 'scp')
                self.assertNotIn('-r', scp)
                self.assertAlmostEqual(run.call_args_list[0].kwargs['timeout'], max(150, budget), delta=1)
                self.assertEqual(receipt['transport']['sha256'], digest)
                self.assertIn('collectionSeconds', receipt)
        receipt = {}
        with patch.object(candidate, 'run', side_effect=TimeoutError('copy failed')), \
             self.assertRaisesRegex(RuntimeError, 'build failed'):
            candidate.collect_artifacts([], [], 'host', '22', Path('/unused'), receipt, RuntimeError('build failed'))
        self.assertEqual(receipt['collectionError'], 'copy failed')

    def test_optional_legacy_benchmark_cannot_consume_collection_reserve(self):
        for fails in (False, True):
            with self.subTest(benchmarkFails=fails), tempfile.TemporaryDirectory() as temporary:
                def run(argv, **kwargs):
                    if argv[0] == 'boat':
                        kwargs['log'].write('{"id":"bx_owned"}\n')
                    if fails and argv[0] == 'scp':
                        raise TimeoutError('legacy deadline exceeded')
                    return ''
                def boat(*args, **kwargs):
                    if args[0] == 'info':
                        return {'sandbox': {'hydrated': True, 'state': 'ready', 'sshEndpoint': 'host:22'}}
                    return {'stdout': 'ssh-ed25519 synthetic-key'}
                # An exhausted build budget leaves the optional benchmark one second,
                # while collect_artifacts independently retains its 150-second reserve.
                args = argparse.Namespace(commit=SHA, type='large', ttl=180, run_nonce='0123456789abcdef', benchmark_catalog_scp=True)
                with patch.object(candidate, 'run', side_effect=run) as calls, \
                     patch.object(candidate, 'boat', side_effect=boat), \
                     patch.object(candidate, 'cleanup_orphans', return_value={'verified': True}), \
                     patch.object(candidate, 'cleanup_workers', return_value={'verified': True}), \
                     patch.object(candidate, 'collect_artifacts') as collection, \
                     patch.object(candidate, 'verify_identity', return_value={}), \
                     patch.object(candidate, 'inventory', return_value=[]):
                    result = candidate.build_worker('ui-catalog', args, Path(temporary), ('image', 'build', 'dev'))
                self.assertEqual(result['state'], 'passed')
                self.assertEqual(result['legacyScpStatus'], 'failed' if fails else 'passed')
                if fails:
                    self.assertEqual(result['legacyScpError'], 'legacy deadline exceeded')
                    self.assertNotIn('legacyScpBytesMatch', result)
                collection.assert_called_once()
                legacy = [call for call in calls.call_args_list if call.args[0][0] == 'scp']
                self.assertEqual(len(legacy), 1)
                self.assertEqual(legacy[0].kwargs['timeout'], 1)
                self.assertEqual(collection.call_args.kwargs['timeout'], 1)


    def test_ssh_is_independent_of_user_config_and_disables_forwarding(self):
        options = candidate.ssh_options(Path('/private/pin'))
        self.assertEqual(options[:2], ['-F', '/dev/null'])
        for option in ('ForwardAgent=no', 'ClearAllForwardings=yes', 'ForwardX11=no',
                       'ControlPath=none', 'ProxyCommand=none', 'GlobalKnownHostsFile=/dev/null',
                       'StrictHostKeyChecking=yes', 'UserKnownHostsFile=/private/pin'):
            self.assertIn(option, options)

    def test_children_are_in_a_new_session_and_cancel_kills_them(self):
        with patch.object(candidate.subprocess, 'Popen') as popen:
            process = popen.return_value
            process.communicate.side_effect = [subprocess.TimeoutExpired('boat', 1), ('', '')]
            candidate.CANCEL.set()
            try:
                with self.assertRaises(TimeoutError):
                    candidate.run(['boat'])
            finally:
                candidate.CANCEL.clear()
            self.assertTrue(popen.call_args.kwargs['start_new_session'])
            process.kill.assert_called_once()

    def test_cleanup_polls_all_states_and_keeps_pending_request_distinct_from_absence(self):
        name = candidate.worker_name(SHA, 'appimage')
        unrelated = {'id': 'other', 'name': 'another-worker'}
        owned = {'id': 'bx_owned', 'name': name, 'state': 'stopped'}
        def boat(*args, **kwargs):
            self.assertTrue(kwargs['cleanup'])
            if args[0] == 'delete':
                self.assertEqual(args, ('delete', 'bx_owned', '--yes'))
                return {'operation': {'id': 'op', 'status': 'pending'}}
            self.assertEqual(args, ('list', '--all'))
            return {'sandboxes': listings.pop(0)}
        listings = [[owned, unrelated], [owned, unrelated], [unrelated]]
        candidate.CANCEL.set()
        try:
            with patch.object(candidate, 'boat', side_effect=boat), patch.object(candidate.time, 'sleep'):
                result = candidate.cleanup_workers(name)
        finally:
            candidate.CANCEL.clear()
        self.assertTrue(result['verified'])
        self.assertEqual(result['finalState'], 'absent')
        self.assertEqual(result['requests'][0]['operation']['status'], 'pending')
        self.assertEqual(result['deletedIds'], ['bx_owned'])

    def test_cleanup_never_claims_success_when_stopped_sandbox_remains(self):
        with patch.object(candidate, 'all_sandboxes', return_value=[{'id': 'owned', 'name': 'exact'}]), \
             patch.object(candidate, 'boat', return_value={'operation': {'status': 'pending'}}), \
             self.assertRaisesRegex(RuntimeError, 'not complete'):
            candidate.cleanup_workers('exact', timeout=0)

    def test_creation_without_an_id_recovers_by_nonce_for_full_create_window(self):
        with tempfile.TemporaryDirectory() as temporary:
            def create(argv, **kwargs):
                raise TimeoutError('interrupted before first ID')
            cleanup = {'verified': True, 'finalState': 'absent', 'deletedIds': ['bx_orphan']}
            args = argparse.Namespace(commit=SHA, type='large', ttl=10800, run_nonce='0123456789abcdef')
            with patch.object(candidate, 'run', side_effect=create), \
                 patch.object(candidate, 'cleanup_orphans', return_value={'verified': True}), \
                 patch.object(candidate, 'cleanup_workers', return_value=cleanup) as cleaner:
                result = candidate.build_worker('appimage', args, Path(temporary), ('image', 'build', 'release'))
            self.assertEqual(result['state'], 'failed')
            self.assertTrue(result['cleanupVerified'])
            self.assertEqual(result['cleanup']['deletedIds'], ['bx_orphan'])
            self.assertEqual(cleaner.call_args.args, ('putgd-aaaaaaaaaaaa-appimage-0123456789abcdef', None))
            self.assertGreater(cleaner.call_args.kwargs['recovery_seconds'], 119)
            self.assertGreaterEqual(cleaner.call_args.kwargs['timeout'], 239)

    def test_nonce_sweep_preserves_live_and_unknown_but_deletes_dead_and_expired(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(candidate, 'LEASE_ROOT', Path(temporary)):
            names = ['000000000000000' + str(i) for i in range(4)]
            with candidate.run_lease(names[1]):
                pass  # dead local lease, retained for orphan recovery
            def worker(nonce, **fields):
                return dict(id=nonce, name=candidate.worker_name(SHA, 'appimage', nonce), **fields)
            with candidate.run_lease(names[0]), \
                 patch.object(candidate, 'all_sandboxes', return_value=[worker(n) for n in names[:3]] + [
                     worker(names[3], createdAt='2000-01-01T00:00:00Z', ttlSeconds=600),
                     {'id': 'unrelated', 'name': 'keep-me'}]), \
                 patch.object(candidate, 'cleanup_workers', return_value={'verified': True}) as cleanup:
                result = candidate.cleanup_orphans(SHA, 'appimage')
            self.assertEqual(result['preserved'], names[:1] + names[2:3])
            self.assertEqual([call.args[1] for call in cleanup.call_args_list], [names[1], names[3]])

    def test_two_controllers_have_distinct_names_and_live_leases(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(candidate, 'LEASE_ROOT', Path(temporary)):
            a, b = 'a' * 16, 'b' * 16
            with candidate.run_lease(a), candidate.run_lease(b):
                self.assertNotEqual(candidate.worker_name(SHA, 'debian13', a),
                                    candidate.worker_name(SHA, 'debian13', b))
                self.assertFalse(candidate.orphaned({}, a))
                self.assertFalse(candidate.orphaned({}, b))
            self.assertTrue(candidate.orphaned({}, a))

    def test_late_create_is_found_after_ten_seconds_and_other_nonce_is_untouched(self):
        now = [0]
        name = candidate.worker_name(SHA, 'appimage', 'a' * 16)
        other = {'id': 'peer', 'name': candidate.worker_name(SHA, 'appimage', 'b' * 16)}
        def listing():
            return [other] + ([{'id': 'late', 'name': name}] if 30 <= now[0] < 34 else [])
        def sleep(seconds):
            now[0] += seconds
        with patch.object(candidate.time, 'monotonic', side_effect=lambda: now[0]), \
             patch.object(candidate.time, 'sleep', side_effect=sleep), \
             patch.object(candidate, 'all_sandboxes', side_effect=listing), \
             patch.object(candidate, 'boat', return_value={'operation': {'status': 'pending'}}) as boat:
            result = candidate.cleanup_workers(name, recovery_seconds=120, timeout=240)
        self.assertEqual(result['deletedIds'], ['late'])
        self.assertEqual(now[0], 120)
        boat.assert_called_once_with('delete', 'late', '--yes', cleanup=True)

    def test_appimage_pkg_config_appends_qt_and_preserves_existing_paths(self):
        import yaml
        packages = yaml.safe_load((ROOT / '.github/workflows/package.yml').read_text())
        _, script, _ = candidate.recipe('appimage', packages, {})
        export = next(line for line in script.splitlines() if line.startswith('export PKG_CONFIG_PATH='))
        for existing, expected in [('', '/qt/lib/pkgconfig'), ('/existing', '/existing:/qt/lib/pkgconfig')]:
            result = subprocess.run(['bash', '-c', export + '\nprintf "%s" "$PKG_CONFIG_PATH"'],
                                    env={'QT_ROOT_DIR': '/qt', 'PKG_CONFIG_PATH': existing},
                                    capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout, expected)

    def test_collection_budget_scales_for_slow_large_outputs(self):
        self.assertGreater(candidate.collection_budget(65 * 1024 * 1024, 9), 650)
        self.assertGreater(candidate.collection_budget(0, 10000), 600)
        self.assertEqual(candidate.collection_budget(0, 0), 150)

    def test_closed_output_stream_is_nonfatal_and_is_replaced_for_shutdown(self):
        closed = io.StringIO()
        closed.close()
        with patch.object(sys, 'stdout', closed):
            candidate.safe_print('completed')
            self.assertIsNot(sys.stdout, closed)
            self.assertTrue(sys.stdout.writable())
            sys.stdout.close()

    def test_cleanup_mode_requires_no_recipes_capacity_or_new_workers_and_handles_all_signals(self):
        with tempfile.TemporaryDirectory() as temporary:
            with patch.object(candidate, 'cleanup_orphans', return_value={'verified': True}) as cleaner, \
                 patch.object(candidate, 'load_workflows') as load, \
                 patch.object(candidate.signal, 'signal') as signals:
                code = candidate.main(['--cleanup', SHA, '--output', str(Path(temporary) / 'out')])
            self.assertEqual(code, 0)
            self.assertEqual(cleaner.call_count, 7)
            load.assert_not_called()
            self.assertEqual({call.args[0] for call in signals.call_args_list},
                             {signal.SIGINT, signal.SIGTERM, signal.SIGHUP})
            for call in signals.call_args_list:
                call.args[1]()
                call.args[1]()  # second Ctrl-C cannot raise during cleanup
            self.assertTrue(candidate.CANCEL.is_set())
            candidate.CANCEL.clear()


if __name__ == '__main__':
    unittest.main()
