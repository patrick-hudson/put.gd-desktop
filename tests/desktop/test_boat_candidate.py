"""Light, network-free controller contracts; no compilation or desktop claims."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import tarfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('boat_candidate', ROOT / 'tools/lab/boat/candidate.py')
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)
SHA = 'a' * 40


class CandidateContracts(unittest.TestCase):
    def test_output_rejects_git_even_when_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            for suffix in ('work/ignored/result', '.git/result'):
                with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                    candidate.prepare_output(root / suffix)

    def test_output_is_private_and_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'new'
            self.assertEqual(candidate.prepare_output(output), output)
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            with self.assertRaises(FileExistsError):
                candidate.prepare_output(output)

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

    def test_complete_manifest_binds_every_artifact_hash_commit_and_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'collected'
            def boat(command):
                if command == 'limits':
                    return {'canStart': True, 'maxActiveSandboxes': 10, 'activeSandboxes': 1,
                            'starts': {p: {'remaining': 10} for p in ('minute', 'hour', 'day')},
                            'machineTypes': [{'type': 'large', 'available': True}]}
                self.assertEqual(command, 'snapshots')
                return {'named': [{'name': 'putgd-build-base-v3', 'snapshotId': 'snapshot-exact', 'createdAt': 'test'}]}
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

    def test_ssh_is_independent_of_user_config_and_disables_forwarding(self):
        options = candidate.ssh_options(Path('/private/pin'))
        self.assertEqual(options[:2], ['-F', '/dev/null'])
        for option in ('ForwardAgent=no', 'ClearAllForwardings=yes', 'ForwardX11=no',
                       'ControlPath=none', 'ProxyCommand=none', 'GlobalKnownHostsFile=/dev/null',
                       'StrictHostKeyChecking=yes', 'UserKnownHostsFile=/private/pin'):
            self.assertIn(option, options)

    def test_cleanup_never_claims_success_when_stopped_sandbox_remains(self):
        with patch.object(candidate, 'all_sandboxes', return_value=[{'id': 'owned', 'name': 'exact'}]), \
             patch.object(candidate, 'boat', return_value={'operation': {'status': 'pending'}}), \
             self.assertRaisesRegex(RuntimeError, 'not complete'):
            candidate.cleanup_workers('exact', timeout=0)

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
            self.assertEqual([call.args for call in cleanup.call_args_list], [(None, names[1]), (None, names[3])])

    def test_two_controllers_have_distinct_names_and_live_leases(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(candidate, 'LEASE_ROOT', Path(temporary)):
            a, b = 'a' * 16, 'b' * 16
            with candidate.run_lease(a), candidate.run_lease(b):
                self.assertNotEqual(candidate.worker_name(SHA, 'debian13', a),
                                    candidate.worker_name(SHA, 'debian13', b))
                self.assertFalse(candidate.orphaned({}, a))
                self.assertFalse(candidate.orphaned({}, b))
            self.assertTrue(candidate.orphaned({}, a))

    def test_expired_ttl_never_overrides_a_live_local_nonce_lease(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(candidate, 'LEASE_ROOT', Path(temporary)):
            nonce = 'd' * 16
            worker = {'createdAt': '2000-01-01T00:00:00Z', 'ttlSeconds': 600}
            with candidate.run_lease(nonce):
                self.assertFalse(candidate.orphaned(worker, nonce))
            self.assertTrue(candidate.orphaned(worker, nonce))

    @contextlib.contextmanager
    def fake_boat_cli(self, root):
        """Real subprocess boundary, fake service; no Boat credentials/network."""
        executable = root / 'boat'
        executable.write_text('''#!/usr/bin/env python3
import fcntl,json,os,signal,sys
from pathlib import Path
root=Path(os.environ['FAKE_BOAT_ROOT'])
with (root/'lock').open('a') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX)
 path=root/'service.json'
 state=json.loads(path.read_text()) if path.exists() else {'sandboxes':[], 'events':[], 'lists':0}
 args=sys.argv[1:]
 args=[a for a in args if a not in ('--no-update','--json')]
 command=args[0]
 if command=='new':
  name=args[args.index('--name')+1]
  item={'id':'bx_'+str(len(state['events'])), 'name':name, 'visibleAfter':state['lists']+(9 if os.environ.get('FAKE_BOAT_LATE') else 0)}
  state['sandboxes'].append(item)
  state['events'].append({'create':item['id'],'name':name})
  result={} if os.environ.get('FAKE_BOAT_LATE') else {'id':item['id']}
 elif command=='list':
  state['lists']+=1
  result={'sandboxes':[s for s in state['sandboxes'] if s['visibleAfter']<=state['lists']]}
 elif command=='delete':
  state['events'].append({'delete':args[1]})
  state['sandboxes']=[s for s in state['sandboxes'] if s['id']!=args[1]]
  result={'operation':{'status':'pending'}}
 else:
  raise SystemExit('unsupported fake command '+command)
 path.write_text(json.dumps(state))
 if command=='new' and os.environ.get('FAKE_BOAT_LATE'):
  os.kill(os.getpid(),signal.SIGTERM)  # accepted server-side, killed before any ID
 print(json.dumps(result))
''')
        executable.chmod(0o700)
        with patch.dict(os.environ, {'PATH': str(root) + os.pathsep + os.environ['PATH'],
                                    'FAKE_BOAT_ROOT': str(root)}), \
             patch.object(candidate, 'LEASE_ROOT', root / 'leases'):
            yield

    def test_fake_cli_same_sha_live_runs_never_delete_each_others_workers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.fake_boat_cli(root), candidate.run_lease('a' * 16), candidate.run_lease('b' * 16):
                a = candidate.boat('new', '--name', candidate.worker_name(SHA, 'appimage', 'a' * 16))['id']
                b = candidate.boat('new', '--name', candidate.worker_name(SHA, 'appimage', 'b' * 16))['id']
                result = candidate.cleanup_orphans(SHA, 'appimage')
                self.assertEqual(result['preserved'], [a, b])
                with patch.object(candidate.time, 'sleep'):
                    candidate.cleanup_workers(candidate.worker_name(SHA, 'appimage', 'a' * 16), a)
                self.assertEqual([s['id'] for s in candidate.all_sandboxes()], [b])
                with patch.object(candidate.time, 'sleep'):
                    candidate.cleanup_workers(candidate.worker_name(SHA, 'appimage', 'b' * 16), b)
            service = json.loads((root / 'service.json').read_text())
            self.assertEqual([e['delete'] for e in service['events'] if 'delete' in e], [a, b])
            self.assertEqual(service['sandboxes'], [])

    def test_orphan_sweep_deletes_only_expired_legacy_id_with_shared_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            name = candidate.worker_name(SHA, 'appimage')
            sandboxes = [
                {'id': 'expired', 'name': name, 'visibleAfter': 0,
                 'createdAt': '2000-01-01T00:00:00Z', 'ttlSeconds': 600},
                {'id': 'live', 'name': name, 'visibleAfter': 0,
                 'createdAt': '2999-01-01T00:00:00Z', 'ttlSeconds': 600},
                {'id': 'unnamed', 'visibleAfter': 0}]
            (root / 'service.json').write_text(json.dumps(
                {'sandboxes': sandboxes, 'events': [], 'lists': 0}))
            with self.fake_boat_cli(root), patch.object(candidate.time, 'sleep'):
                result = candidate.cleanup_orphans(SHA, 'appimage')
                remaining = candidate.all_sandboxes()
            self.assertEqual([s['id'] for s in remaining], ['live', 'unnamed'])
            self.assertEqual(result['deleted'][0]['deletedIds'], ['expired'])
            self.assertEqual(result['preserved'], ['live'])

    def test_manifest_rejects_truthy_qualified_cleanup_even_if_build_passed(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'out'
            limits = {'canStart': True, 'maxActiveSandboxes': 10, 'activeSandboxes': 0,
                      'starts': {p: {'remaining': 10} for p in ('minute', 'hour', 'day')},
                      'machineTypes': [{'type': 'large', 'available': True}]}
            snapshots = {'named': [{'name': 'putgd-build-base-v3', 'snapshotId': 'test', 'createdAt': 'test'}]}
            receipt = {'artifact': 'appimage', 'state': 'passed', 'cleanupVerified': 'qualified',
                       'name': 'unresolved-create', 'files': []}
            with patch.object(candidate, 'boat', side_effect=[limits, snapshots]), \
                 patch.object(candidate, 'load_workflows', return_value=({}, {})), \
                 patch.object(candidate, 'recipe', return_value=('image', 'script', 'release')), \
                 patch.object(candidate, 'build_worker', return_value=receipt), \
                 patch.object(candidate.signal, 'signal'), \
                 patch.object(candidate, 'safe_print'):
                code = candidate.main([SHA, '--output', str(output), '--artifact', 'appimage'])
            manifest = json.loads((output / 'manifest.json').read_text())
            self.assertEqual(code, 1)
            self.assertFalse(manifest['passed'])
            self.assertEqual(manifest['ownedSandboxesRemaining'], ['unresolved-create'])


if __name__ == '__main__':
    unittest.main()
