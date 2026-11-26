"""Controller policy tests; these do not claim graphical desktop acceptance."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import unittest
import uuid
from types import SimpleNamespace
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab import nightly_desktops as nightly
from tools.lab.surface_driver.collect import mark_terminal

BUILD = 'a'*40


class FakeGitHub:
    repo = 'owner/repo'
    pinned = dict(number=1, title='Nightly desktop results', node_id='PIN', html_url='https://example.test/1')

    def __init__(self, failure=None):
        self.failure, self.writes = failure, []

    def issues(self): return [self.pinned]+([self.failure] if self.failure else [])

    def issue(self, title, body, existing=None):
        self.writes.append(('issue', title, existing, body))
        return dict(existing or self.pinned, html_url='https://example.test/issue')

    def api(self, path, method='GET', body=None):
        self.writes.append((method, path, body))
        if method == 'GET': return dict(node_id='REPO')
        return dict(html_url='https://example.test/receipt')

    def gh(self, *args):
        return json.dumps(dict(data=dict(node=dict(pinnedIssues=dict(nodes=[dict(issue=dict(id='PIN'))])))))


class NightlyTests(unittest.TestCase):
    def test_controller_ssh_proof_requires_parent_start_exact_argv_and_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            proc = Path(tmp)
            def process(pid, parent, start, argv, executable='ssh'):
                path = proc/str(pid)
                path.mkdir()
                (path/'stat').write_text(f'{pid} (name with spaces) S {parent} '+ '0 '*17 + str(start))
                (path/'cmdline').write_bytes(os.fsencode('\0'.join(argv)+'\0'))
                (path/'exe').symlink_to('/usr/bin/'+executable)
                return path
            prefix = ['ssh', '-i', '/private/f44-identity', 'other-host']
            parent = process(10, 1, 90, ['python3', 'nightly_desktops.py'])
            # SSH clients are spawned in controller dispatch threads, not its main thread.
            task = parent/'task/11'
            task.mkdir(parents=True)
            process(31, 10, 100, prefix+['putgd worker'])
            process(32, 20, 101, prefix+['putgd worker'])  # Own worker/unrelated parent.
            process(33, 10, 102, ['ssh', 'unrelated-host', 'putgd'])
            process(34, 10, 103, prefix+['putgd worker'], executable='putgd-ui')
            (task/'children').write_text('31 32 33 34 35')  # 35 exited.
            context = dict(pid=10, start='90', ssh=[prefix])
            with patch.dict(os.environ, PUTGD_NIGHTLY_CONTROLLER=json.dumps(context)), \
                    patch.object(nightly.os, 'getppid', return_value=10):
                self.assertEqual(nightly.controller_ssh_identities(proc=proc), ['process:31:100'])
                (parent/'stat').write_text('10 (reused) S 1 '+ '0 '*17 + '91')
                self.assertEqual(nightly.controller_ssh_identities(proc=proc), [])
            with patch.dict(os.environ, PUTGD_NIGHTLY_CONTROLLER=json.dumps(context)), \
                    patch.object(nightly.os, 'getppid', return_value=20):
                self.assertEqual(nightly.controller_ssh_identities(proc=proc), [])

    def test_controller_ssh_churn_releases_only_other_rows_exact_identities(self):
        from tools.lab import desktop_lock as locks
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            items = []
            desktop = locks.Desktop(root/'putgd-lab/locks', 'd69', inventory=lambda:(list(items), []))
            def item(pid, command, start=100):
                return dict(kind='process', id=str(pid), key=f'process:{pid}:{start}', detail=command)
            # Controller SSH clients inherit DISPLAY and are consequently inventoried.
            with patch.object(locks, 'process_start', return_value='100'):
                observed, _ = locks.collect_inventory(root,
                    run=lambda argv:(0, '31 ssh -i /private/f44-identity remote putgd')
                        if argv[0] == 'pgrep' else (None, ''),
                    environment=lambda pid:{'DISPLAY'}, own=set(), tmp=lambda: None)
            self.assertEqual(observed, [item(31, 'ssh -i /private/f44-identity remote putgd')])
            items.extend(observed)
            with patch.object(nightly.Path, 'home', return_value=root), \
                    patch('tools.lab.desktop_lock.Desktop', return_value=desktop), \
                    patch.object(nightly, 'controller_ssh_identities', return_value=['process:32:100']):
                with nightly.lease('d69', 'owned-session', root):
                    items[:] = [item(32, 'ssh -i /private/f44-identity remote putgd')]
                release = json.loads((root/'lock-release.json').read_text())
                self.assertEqual([i['key'] for i in release['check']['notMine']], ['process:32:100'])
                self.assertFalse(desktop.lock.exists())
                # Own guest, input helper, app, unrelated SSH and a reused PID all block release.
                for leftover in (item(40, 'qemu-system-x86_64 putgd'), item(41, 'host_input.py'),
                                 item(42, 'putgd-ui'), item(43, 'ssh unrelated putgd'),
                                 item(32, 'putgd-ui', start=101)):
                    with self.subTest(leftover=leftover['detail']):
                        items.clear()
                        with self.assertRaises(locks.LockError) as refused:
                            with nightly.lease('d69', 'owned-session', root):
                                items[:] = [leftover]
                        evidence = root/'lock-release-error.json'
                        error = json.loads(evidence.read_text())
                        self.assertEqual(error['message'], str(refused.exception))
                        self.assertEqual(error['details']['check']['left'], [leftover])
                        self.assertEqual(evidence.stat().st_mode & 0o777, 0o600)
                        self.assertTrue(desktop.lock.exists())
                        items.clear()
                        desktop.release('owned-session', verified_clean=True)
                        slot = root/'putgd-lab/locks/tier1-guest.lock.d'
                        self.assertTrue((slot/'retained.json').exists())
                        for name in ('session', 'owner.json', 'retained.json'):
                            (slot/name).unlink()
                        slot.rmdir()

    def test_worker_identity_refuses_live_pid_and_detects_exit_reuse_and_reboot(self):
        owner = dict(worker=nightly.worker_identity())
        self.assertFalse(nightly.previous_worker_gone(owner))
        with patch('tools.lab.desktop_lock.process_start', return_value='different-start'):
            self.assertTrue(nightly.previous_worker_gone(owner))
        with patch('tools.lab.desktop_lock.process_start', return_value=''):
            self.assertFalse(nightly.previous_worker_gone(owner))
        owner['worker']['boot'] = 'previous-boot'
        self.assertTrue(nightly.previous_worker_gone(owner))

    def stale_fixture(self, root, *, live=False, legacy=False, lend=None, missing=True):
        from tools.lab.desktop_lock import Desktop, now_utc
        locks = root/'putgd-lab/locks'
        locks.mkdir(parents=True)
        slot = locks/'tier1-guest.lock.d'
        slot.mkdir(mode=0o700)
        run = root/'work/lab-runs/n12345678'
        evidence = root/'old/row'
        evidence.mkdir(parents=True)
        out = root/'new'
        out.mkdir()
        owner = dict(run=str(run), session='old-nightly', evidence=str(evidence),
                     worker=dict(nightly.worker_identity(), boot='old-boot'))
        if legacy:
            owner.pop('worker')
            nightly.save(evidence/'result.json', dict(session=owner['session'], finishedAt='yesterday'))
        nightly.save(slot/'retained.json', owner)
        (slot/'session').write_text(owner['session'])
        desktop = Desktop(locks, 'd69', inventory=lambda:([], []))
        desktop.acquire('nightly-desktops', owner['session'], 'test', 45)
        if not live:
            record = desktop.state(now_utc())['lock']
            record.update(heartbeat='2020-01-01T00:00:00Z', expires='2020-01-01T00:45:00Z')
            from tools.lab.desktop_lock import render_lock
            desktop.replace(desktop.lock, render_lock(record))
        proton = Desktop(locks, 'd69', inventory=lambda:([], []), resource='proton')
        if lend:
            proton.acquire('nightly-desktops', owner['session'], 'test', 45)
            folder = root/'putgd-lab/proton/receipts/test'
            nightly.save(folder/'lend.json', dict(run=str(run), session=owner['session'], step=lend))
            nightly.save(folder/'provision.json', dict(schema=1, provider='proton', target='guest',
                run=str(run), kind='chain_guest', credentialReference=dict(mode='keychain',
                reference='putgd-lab-11111111-1111-4111-8111-111111111111'), state='cleanup_pending', cleanupAllowed=True))
        if not missing:
            run.mkdir(parents=True)
            (run/'state.json').write_text('{}')
        return slot, out, owner, desktop, proton

    def test_dead_slot_missing_run_recovers_checked_leases_and_archives_owner(self):
        for legacy in (False, True):
            with self.subTest(legacy=legacy), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                slot, out, owner, desktop, proton = self.stale_fixture(root, legacy=legacy, lend='carried_back')
                with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                        patch('tools.lab.desktop_lock.Desktop', side_effect=lambda *a, **kw:
                              proton if kw.get('resource') == 'proton' else desktop):
                    nightly.recover_stale_guest(dict(host='d69'), out)
                self.assertFalse(slot.exists())
                self.assertEqual(desktop.status()['state'], 'free')
                self.assertEqual(proton.status()['state'], 'free')
                archived = list((root/'putgd-lab/locks/history/recovered').glob('*-n12345678-*.json'))
                record = json.loads(archived[0].read_text())
                # The record keeps the slot's own files and says why the slot could go.
                self.assertEqual((record['owner'], record['recoveredBy']), (owner, 'nightly'))
                # Names only: file contents never reach history, only the validated owner.
                self.assertEqual(record['slotFiles'], ['session', 'retained.json'])
                self.assertEqual((record['cleared']['guest'], record['cleared']['protonLend']),
                                 ('run directory already gone', 'carried_back'))
                self.assertTrue(record['desktopRelease'])

    def test_live_foreign_and_unresolved_baton_are_never_released(self):
        for mode in ('live', 'foreign', 'unresolved', 'worker-live'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                slot, out, owner, desktop, proton = self.stale_fixture(root, live=mode == 'live',
                    lend='provisioned' if mode == 'unresolved' else None)
                if mode == 'foreign':
                    from tools.lab.desktop_lock import render_lock, now_utc
                    record = desktop.state(now_utc())['lock']
                    desktop.replace(desktop.lock, render_lock(dict(record, session='other-session')))
                if mode == 'worker-live':
                    nightly.save(slot/'retained.json', dict(owner, worker=nightly.worker_identity()))
                with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                        patch('tools.lab.desktop_lock.Desktop', side_effect=lambda *a, **kw:
                              proton if kw.get('resource') == 'proton' else desktop), \
                        patch.object(nightly, 'retire') as retire, patch.object(nightly, 'command') as command:
                    with self.assertRaises(nightly.NotRun):
                        nightly.recover_stale_guest(dict(host='d69'), out)
                retire.assert_not_called()
                command.assert_not_called()
                self.assertTrue(slot.exists())
                self.assertNotEqual(desktop.status()['state'], 'free')

    def test_owned_recovery_calls_bounded_proton_recover_then_guarded_retirement(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.stale_fixture(root, lend='carried_back', missing=False)
            folder = root/'putgd-lab/proton/receipts/test'
            def recovered(*args, **kwargs):
                self.assertEqual(kwargs['timeout'], 120)
                self.assertIn('recover', args[0])
                nightly.save(folder/'lend.json', dict(run=owner['run'], session=owner['session'], step='recovered'))
            with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                    patch('tools.lab.desktop_lock.Desktop', side_effect=lambda *a, **kw:
                          proton if kw.get('resource') == 'proton' else desktop), \
                    patch.object(nightly, 'retire') as retire, patch.object(nightly, 'command', side_effect=recovered):
                nightly.recover_stale_guest(dict(host='d69'), out)
            retire.assert_called_once_with(Path(owner['run']), Path(owner['evidence']), {})
            self.assertFalse(slot.exists())

    def test_failed_stale_recovery_names_a_receipt_that_exists(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.stale_fixture(root)
            (slot/'retained.json').write_text('not json')
            with patch.object(nightly.Path, 'home', return_value=root), \
                    self.assertRaisesRegex(nightly.NotRun, 'stale-recovery-error.json'):
                nightly.recover_stale_guest(dict(host='d69'), out)
            self.assertEqual(json.loads((out/'stale-recovery-error.json').read_text()),
                             dict(type='JSONDecodeError'))
            self.assertTrue(slot.exists())

    def test_row_recovery_keeps_specific_reasons_and_clears_the_reservation(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            reservation = dict(run='/run', retained=True, reason='guest retained')
            with patch.object(nightly, 'recover_guest', side_effect=nightly.NotRun('specific reason')):
                with self.assertRaisesRegex(nightly.NotRun, 'specific reason'):
                    nightly.recover_row({}, reservation, 's', out, {})
            with patch.object(nightly, 'recover_guest', side_effect=ValueError('private payload')):
                with self.assertRaisesRegex(nightly.NotRun, 'explicit recovery required') as caught:
                    nightly.recover_row({}, reservation, 's', out, {})
                self.assertNotIn('private payload', str(caught.exception))
            self.assertTrue(reservation['retained'])
            with patch.object(nightly.Path, 'mkdir', side_effect=OSError('private path')), \
                    self.assertRaisesRegex(nightly.NotRun, 'explicit recovery required'):
                nightly.recover_row({}, reservation, 's', out, {})
            with patch.object(nightly, 'recover_guest'):
                nightly.recover_row({}, reservation, 's', out, {})
            self.assertEqual(reservation, dict(run='/run'))

    def test_recovery_retires_only_when_every_cleanup_error_is_guest_local(self):
        """The receipt says which errors only the guest holds; the wording of a message decides nothing."""
        four = ['notification monitor cleanup unavailable',
                'resident quit transport unavailable; private session retained',
                'previous clipboard restoration unavailable; private session retained',
                'exact mirrored KWin entry cleanup refused; private session retained']
        cases = {
            'one guest-unreachable error': (four[1:2], four[1:2], {}, True),
            'four errors, the whole close unreachable': (four, four, {}, True),
            'one error the guest answered': (four, four[:3], {}, False),
            'unknown cleanup': (['fixture cleanup failed; publication outstanding'], four, {}, False),
            'no structured record': (four[1:2], None, {}, False),
            'unresolved live cleanup': (four, four, {'unresolved_live_cleanup': True}, False),
            'live cleanup flag without errors': ([], [], {'unresolved_live_cleanup': True}, False),
        }
        for name, (errors, local, extra, safe) in cases.items():
            with self.subTest(name), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                slot, out, owner, desktop, proton = self.stale_fixture(root, missing=False)
                environment = dict(cleanup_errors=errors, private_cleanup='not-owned-or-retained', **extra)
                if local is not None:
                    environment['guest_local_cleanup'] = local
                nightly.save(Path(owner['evidence'])/'flow/receipt.json', dict(environment=environment))
                with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                        patch('tools.lab.desktop_lock.Desktop', side_effect=lambda *a, **kw:
                              proton if kw.get('resource') == 'proton' else desktop), \
                        patch.object(nightly, 'retire') as retire:
                    if safe:
                        nightly.recover_stale_guest(dict(host='d69'), out)
                        retire.assert_called_once()
                        self.assertFalse(slot.exists())
                    else:
                        with self.assertRaisesRegex(nightly.NotRun, 'retained'):
                            nightly.recover_stale_guest(dict(host='d69'), out)
                        retire.assert_not_called()
                        self.assertTrue(slot.exists())

    def test_exact_active_guest_client_prevents_recovery(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.stale_fixture(root)
            with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                    patch('tools.lab.desktop_lock.Desktop', return_value=desktop), \
                    patch.object(nightly, 'guest_clients_live', return_value=True), \
                    patch.object(nightly, 'retire') as retire:
                with self.assertRaisesRegex(nightly.NotRun, 'still live'):
                    nightly.recover_stale_guest(dict(host='d69'), out)
            retire.assert_not_called()
            self.assertTrue(slot.exists())

    def test_retained_guest_keeps_durable_reservation_and_blocks_lease(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'putgd-lab/locks').mkdir(parents=True)
            reservation = dict(run='/owned/run', retained=True, reason='cleanup recovery required')
            with patch.object(nightly.Path, 'home', return_value=root), \
                    patch('tools.lab.desktop_lock.Desktop') as lock:
                lock.return_value.acquire.return_value = {}
                lock.return_value.heartbeat.return_value = {}
                with nightly.lease('d69', 'owned-session', root, reservation=reservation):
                    pass
                lock.return_value.release.assert_not_called()
                lock.return_value.heartbeat.assert_called_once_with('owned-session', blocked=reservation['reason'])
            slot = root/'putgd-lab/locks/tier1-guest.lock.d'
            self.assertEqual((slot/'session').read_text().strip(), 'owned-session')
            self.assertEqual(json.loads((slot/'retained.json').read_text())['run'], '/owned/run')

    def test_corrupt_cleanup_receipt_keeps_reservation_for_recovery(self):
        self.worker_timeout_cleanup(corrupt_receipt=True)

    def test_completed_flows_retire_only_after_cleanup_state_is_checked(self):
        for environment in ({}, {'cleanup_errors':['synthetic retained process']},
                            {'private_cleanup':'retained for explicit recovery'}):
            with self.subTest(environment=environment):
                self.worker_timeout_cleanup(finished=True, environment=environment)

    def worker_timeout_cleanup(self, *, corrupt_receipt=False, finished=False, environment=None):
        from tools.lab import lab, nightly_desktop_guest as guest
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'putgd-lab/locks').mkdir(parents=True)
            pins = root/'pins'
            pins.write_text('localhost ssh-ed25519 synthetic-public-pin\n')
            artifacts = root/'artifacts'
            (artifacts/'packages').mkdir(parents=True)
            package = artifacts/'packages/putgd-test-test.deb'
            package.write_bytes(b'synthetic package')
            identity = dict(driver=BUILD, gitCommit=BUILD,
                            packages={package.name:nightly.digest(package)})
            row = dict(name='Test', profile='test', knownHosts=str(pins), bootstrapKey='unused',
                       package='deb', asset='test.deb', desktop='test', golden='/unused/golden')
            clock = [0]
            attempts = []
            runs = []
            def wait(*args, **kwargs):
                self.assertEqual(kwargs['worker_deadline'], 100)
                clock[0] += 40
                return 40
            def prepare(*args):
                clock[0] += 20
                return root/'profiles'
            def command(argv, **kwargs):
                if argv[:2] == ['git', 'rev-parse']:
                    return BUILD
                if 'launch' in argv:
                    run = Path(argv[argv.index('--run')+1])
                    run.mkdir(parents=True)
                    (run/'state.json').write_text('{}')
                    (run/'disk.qcow2').touch()
                    runs.append(run)
                if '-m' in argv:
                    attempts.append(kwargs['timeout'])
                    if finished:
                        target = Path(argv[argv.index('--out')+1])
                        target.mkdir()
                        (target/'receipt.json').write_text(json.dumps(dict(environment=environment)))
                        return
                    clock[0] += kwargs['timeout']
                    if corrupt_receipt:
                        target = Path(argv[argv.index('--out')+1])
                        target.mkdir()
                        (target/'receipt.json').write_text('invalid receipt')
                    raise subprocess.TimeoutExpired('synthetic attempt', kwargs['timeout'])
            verdict_patch = (patch.object(nightly, 'verdict', return_value=nightly.cell('green'))
                             if finished else patch.object(nightly, 'verdict', wraps=nightly.verdict))
            with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                    patch.object(nightly.time, 'monotonic', side_effect=lambda:clock[0]), \
                    patch.object(nightly, 'await_host', side_effect=wait), \
                    patch.object(nightly, 'preflight', return_value={}), \
                    patch.object(nightly, 'command', side_effect=command), \
                    patch.object(guest, 'prepare', side_effect=prepare), \
                    patch.object(guest, 'hold_package_updates') as hold, \
                    patch.object(lab, 'state', return_value=dict(id='test', golden_sha256='a'*64)), \
                    patch.object(lab, 'reset') as reset, \
                    patch.object(nightly, 'retire', side_effect=lambda run, *_:(run/'disk.qcow2').unlink()) as retire, \
                    patch.object(nightly, 'LOCAL', ['region_upload'] if finished else nightly.LOCAL), \
                    verdict_patch, \
                    patch('tools.lab.desktop_lock.Desktop') as lock:
                lock.return_value.acquire.return_value = {}
                lock.return_value.release.return_value = {}
                lock.return_value.heartbeat.return_value = {}
                records = nightly.worker_run(dict(host='d69', rows=[row]), 'vtest', identity,
                                             artifacts, root/'output', 100)
            self.assertEqual(attempts, [40], records[0].get('notRunReason'))
            self.assertEqual(hold.call_args.args[0], runs[0])
            if not finished:
                self.assertEqual(records[0]['cells']['window_desktop']['status'], 'not_run')
            slot = root/'putgd-lab/locks/tier1-guest.lock.d'
            if corrupt_receipt or environment:
                retire.assert_not_called()
                reset.assert_not_called()
                lock.return_value.release.assert_not_called()
                self.assertTrue((slot/'retained.json').exists())
                self.assertTrue((runs[0]/'disk.qcow2').exists())
                self.assertTrue(records[0]['cleanupBlocked'])
                self.assertTrue(all(c['status'] == 'not_run' for c in records[0]['cells'].values()))
                self.assertIn(records[0]['notRunReason'][:80], nightly.table(dict(desktops=records)))
            else:
                if not finished:
                    self.assertIn('deadline exhausted', records[0]['notRunReason'])
                retire.assert_called_once()
                reset.assert_called_once()
                lock.return_value.release.assert_called_once()
                self.assertFalse(slot.exists())

    def lost_session_row(self, errors, rows=({'functional': 'blocked', 'note': 'target session command unavailable: LabError exit 255'},),
                         local=None, marker=False, **extra):
        """Run a row whose first cell fails with the given cleanup errors (#1214)."""
        from tools.lab import lab, nightly_desktop_guest as guest
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'putgd-lab/locks').mkdir(parents=True)
            pins = root/'pins'
            pins.write_text('localhost ssh-ed25519 synthetic-public-pin\n')
            artifacts = root/'artifacts'
            (artifacts/'packages').mkdir(parents=True)
            package = artifacts/'packages/putgd-test-test.deb'
            package.write_bytes(b'synthetic package')
            identity = dict(driver=BUILD, gitCommit=BUILD, packages={package.name:nightly.digest(package)})
            row = dict(name='Test', profile='test', knownHosts=str(pins), bootstrapKey='unused',
                       package='deb', asset='test.deb', desktop='test', golden='/unused/golden')
            cells = []
            def command(argv, **kwargs):
                if argv[:2] == ['git', 'rev-parse']:
                    return BUILD
                if 'launch' in argv:
                    run = Path(argv[argv.index('--run')+1])
                    run.mkdir(parents=True)
                    (run/'state.json').write_text('{}')
                    if marker:
                        (run/lab.LIVE_CLEANUP_MARKER).write_text('{}')
                if '-m' in argv:
                    cells.append(Path(argv[argv.index('--out')+1]).name)
                    target = Path(argv[argv.index('--out')+1])
                    target.mkdir()
                    (target/'receipt.json').write_text(json.dumps(dict(environment=dict(
                        cleanup_errors=errors, guest_local_cleanup=errors if local is None else local, **extra),
                        interactions=list(rows))))
            observed = dict(nightly.cell(assertion='private target cleanup failed'),
                            observations=['target session command unavailable: LabError exit 255'])
            with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                    patch.object(nightly, 'await_host', return_value=0), patch.object(nightly, 'preflight', return_value={}), \
                    patch.object(nightly, 'command', side_effect=command), patch.object(nightly, 'verdict', return_value=observed), \
                    patch.object(guest, 'prepare', return_value=root/'profiles'), \
                    patch.object(guest, 'hold_package_updates'), \
                    patch.object(lab, 'state', return_value=dict(id='test', golden_sha256='a'*64)), \
                    patch.object(lab, 'reset') as reset, patch.object(nightly, 'guest_clients_live', return_value=False), \
                    patch.object(nightly, 'retire') as retire, patch('tools.lab.desktop_lock.Desktop') as lock:
                lock.return_value.acquire.return_value = {}
                lock.return_value.release.return_value = {}
                lock.return_value.heartbeat.return_value = {}
                lock.return_value.status.return_value = dict(state='free')
                records = nightly.worker_run(dict(host='d69', rows=[row]), 'vtest', identity, artifacts, root/'output',
                                             nightly.time.monotonic() + 10**6)
            self.reset = reset
            return records[0], cells, retire, (root/'putgd-lab/locks/tier1-guest.lock.d').exists()

    def proton_lend_row(self, lends, provision_state='cleanup_confirmed', provider='ksecretd'):
        """Run a live Proton-only row whose lends play LENDS in turn: 'clean' stops before the test with
        its import cleaned up, 'green' and 'red' carry back after a passed or failed test (#2105)."""
        from tools.lab import lab, nightly_desktop_guest as guest, provision_live_credentials as credentials
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            locks = root/'putgd-lab/locks'
            locks.mkdir(parents=True)
            (locks/'proton-registry.json').write_text('{}')
            pins = root/'pins'
            pins.write_text('localhost ssh-ed25519 synthetic-public-pin\n')
            artifacts = root/'artifacts'
            (artifacts/'packages').mkdir(parents=True)
            package = artifacts/'packages/putgd-test-test.deb'
            package.write_bytes(b'synthetic package')
            identity = dict(driver=BUILD, gitCommit=BUILD, packages={package.name:nightly.digest(package)})
            row = dict(name='Test', profile='test', knownHosts=str(pins), bootstrapKey='unused', live=True,
                       package='deb', asset='test.deb', desktop='test', golden='/unused/golden')
            plays = list(lends)
            attempts, results = [], []
            def private(path, value):
                path.write_text(json.dumps(value))
                path.chmod(0o600)
            def command(argv, **kwargs):
                if argv[:2] == ['git', 'rev-parse']:
                    return BUILD
                if 'launch' in argv:
                    run = Path(argv[argv.index('--run')+1])
                    run.mkdir(parents=True)
                    (run/'state.json').write_text('{}')
                if 'lend' in argv:
                    run = argv[argv.index('lend')+1]
                    attempts.append(Path(argv[argv.index('--out')+1]).name)
                    folder = root/f'putgd-lab/proton/receipts/2026-11-26T12-59-0{len(attempts)}Z-run'
                    folder.mkdir(parents=True)
                    play = plays.pop(0)
                    results.append(play)
                    if play in ('green', 'red'):
                        private(folder/'lend.json', dict(run=run, step='cleaned', reference='putgd-lab-x'))
                        (locks/'proton-registry.json').write_text(json.dumps(dict(lastLend=dict(
                            run=run, carriedBack='carry-back-1.json', receipts=str(folder)))))
                        return
                    for name in ('provision-1.json', 'provision.json'):
                        private(folder/name, dict(state=provision_state))
                    private(folder/'lend.json', dict(
                        run=run, step='provision_failed_abandoned', failedStep='import', reference=None,
                        failure='synthetic private text', guestKeyring=dict(provider=provider, restarted=True),
                        provisionTries=[dict(attempt=1), dict(attempt=2)]))
                    raise RuntimeError('lend failed (exit 1); private log retained')
            verdict = nightly.verdict
            def observed(target, recipe):
                last = results[-1] if results else None
                return (nightly.cell('green') if last == 'green' else
                        nightly.cell('red', 'synthetic functional failure') if last == 'red' else verdict(target, recipe))
            loaded = lambda path: (path, json.loads(Path(path).read_text()))
            with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                    patch.object(nightly, 'await_host', return_value=0), patch.object(nightly, 'preflight', return_value={}), \
                    patch.object(nightly, 'command', side_effect=command), patch.object(nightly, 'verdict', side_effect=observed), \
                    patch.object(nightly, 'LOCAL', []), patch.object(nightly, 'LIVE', ['proton_region_upload']), \
                    patch.object(credentials, '_load_receipt', side_effect=loaded), \
                    patch.object(guest, 'prepare', return_value=root/'profiles'), \
                    patch.object(guest, 'hold_package_updates'), \
                    patch.object(lab, 'state', return_value=dict(id='test', golden_sha256='a'*64)), \
                    patch.object(lab, 'reset'), patch.object(nightly, 'guest_clients_live', return_value=False), \
                    patch.object(nightly, 'retire'), patch('tools.lab.desktop_lock.Desktop') as lock:
                lock.return_value.acquire.return_value = {}
                lock.return_value.release.return_value = {}
                lock.return_value.heartbeat.return_value = {}
                lock.return_value.status.return_value = dict(state='free')
                records = nightly.worker_run(dict(host='d69', rows=[row]), 'vtest', identity, artifacts, root/'output',
                                             nightly.time.monotonic() + 10**6)
            return records[0], attempts

    def test_a_cleanly_failed_proton_lend_runs_the_cells_second_attempt(self):
        record, attempts = self.proton_lend_row(['clean', 'green'])
        self.assertEqual(attempts, ['proton_region_upload-1', 'proton_region_upload-2'])
        upload = record['cells']['proton_region_upload']
        self.assertEqual(upload['status'], 'green')
        first = upload['attempts'][0]
        self.assertEqual(first['status'], 'not_run')
        self.assertEqual(first['assertion'], 'Proton lend stopped before the test: guest keychain import refused on '
                                             '2 tries; guest keyring ksecretd (just restarted); nothing left in the guest')
        self.assertNotIn('notRunReason', record)

    def test_a_proton_lend_that_fails_cleanly_twice_names_the_lend_step_not_a_missing_receipt(self):
        record, attempts = self.proton_lend_row(['clean', 'clean'])
        self.assertEqual(len(attempts), 2)
        upload = record['cells']['proton_region_upload']
        self.assertEqual(upload['status'], 'not_run')
        self.assertTrue(upload['assertion'].startswith('Proton lend stopped before the test: guest keychain import'))
        self.assertNotIn('receipt missing', upload['assertion'])
        self.assertNotIn('synthetic private text', json.dumps(record))

    def test_a_clean_lend_failure_after_an_earlier_attempts_carry_back_still_names_the_lend(self):
        record, attempts = self.proton_lend_row(['red', 'clean'])
        upload = record['cells']['proton_region_upload']
        self.assertEqual([a['status'] for a in upload['attempts']], ['red', 'not_run'])
        self.assertTrue(upload['attempts'][1]['assertion'].startswith('Proton lend stopped before the test'))
        self.assertEqual(upload['status'], 'not_run')

    def test_an_unknown_keyring_provider_is_left_out_of_the_assertion(self):
        record, _ = self.proton_lend_row(['clean', 'clean'], provider='synthetic-secret')
        assertion = record['cells']['proton_region_upload']['assertion']
        self.assertNotIn('synthetic-secret', json.dumps(record))
        self.assertNotIn('guest keyring', assertion)

    def test_a_proton_lend_whose_import_is_not_proven_clean_still_stops_the_row(self):
        record, attempts = self.proton_lend_row(['clean', 'green'], provision_state='import_pending')
        self.assertEqual(attempts, ['proton_region_upload-1'])
        self.assertIn('Proton guest provisioning did not complete', record['notRunReason'])

    def test_lost_session_stops_the_row_with_the_first_failure(self):
        record, cells, retire, slot = self.lost_session_row(['notification monitor cleanup unavailable',
                                                             'resident quit transport unavailable; private session retained'])
        self.assertEqual(cells, ['idle_memory-1'])
        reason = record['notRunReason']
        self.assertIn('guest session lost in idle_memory attempt 1: target session command unavailable', reason)
        self.assertEqual(record['stoppedEarly']['flow'], 'idle_memory')
        self.assertEqual(record['cells']['idle_memory']['status'], 'not_run')
        self.assertEqual(record['cells']['idle_memory']['assertion'], reason)
        untouched = [v for k, v in record['cells'].items() if k != 'idle_memory']
        self.assertTrue(all(v['status'] == 'not_run' and not v.get('attempts') and v['assertion'].endswith(reason) for v in untouched))
        retire.assert_called_once()
        self.assertFalse(slot)
        self.assertNotIn('cleanupBlocked', record)

    def test_lost_session_that_also_left_unsafe_cleanup_stays_retained(self):
        errors = ['resident quit transport unavailable; private session retained',
                  'previous clipboard restoration unavailable; private session retained']
        # The clipboard failure was an answer from the reachable guest, so the receipt does not call it guest-local.
        record, cells, retire, slot = self.lost_session_row(errors, local=errors[:1])
        self.assertEqual(len(cells), 1)
        retire.assert_not_called()
        self.assertTrue(slot and record['cleanupBlocked'])
        self.assertTrue(record['notRunReason'].startswith('guest session lost in idle_memory attempt 1'))

    def test_lost_session_with_every_cleanup_step_unreachable_is_retired(self):
        record, cells, retire, slot = self.lost_session_row(
            ['notification monitor cleanup unavailable',
             'resident quit transport unavailable; private session retained',
             'previous clipboard restoration unavailable; private session retained',
             'exact mirrored KWin entry cleanup refused; private session retained'])
        retire.assert_called_once()
        self.assertFalse(slot)
        self.assertNotIn('cleanupBlocked', record)

    def test_unresolved_live_cleanup_keeps_the_guest_even_when_the_session_was_lost(self):
        errors = ['notification monitor cleanup unavailable',
                  'resident quit transport unavailable; private session retained']
        record, cells, retire, slot = self.lost_session_row(errors, unresolved_live_cleanup=True)
        retire.assert_not_called()
        self.assertTrue(slot and record['cleanupBlocked'])
        self.assertIn('live remote cleanup unresolved', record['notRunReason'])

    def test_failed_recipe_note_names_the_failed_pass_and_step_not_the_earlier_counters(self):
        # #2051: the expiring-link pass failed; the public pass's cloudfront_after_delete_status=200 is not the cause.
        with tempfile.TemporaryDirectory() as out:
            def receipt(note):
                Path(out, 'receipt.json').write_text(json.dumps({'interactions': [
                    {'interaction': 'region.to-upload', 'functional': 'failed', 'note': note}]}))
            receipt('s3_missing_head_404_count=1; cloudfront_after_delete_status=200; presigned_status=200; unsigned_status=403; '
                    'deletion_settled=status=unknown, error=s3_transport_unknown, stage=delete_object, curl=35, http=-1; '
                    'failed_step=expiring-link pass, History delete: History confirms object removal (status=unknown, '
                    'error=s3_transport_unknown, stage=delete_object, curl=35, http=-1, removal not confirmed); '
                    'History confirms object removal (status=unknown); fixture cleanup failed: owned history and cache: RecipeBlocked; '
                    'accepted settings: RecipeBlocked')
            self.assertEqual(nightly.failed_recipe_note(out, 'fallback'),
                             'expiring-link pass, History delete: History confirms object removal (status=unknown, '
                             'error=s3_transport_unknown, stage=delete_object, curl=35, http=-1, removal not confirmed); '
                             'fixture cleanup failed: owned history and cache: RecipeBlocked; accepted settings: RecipeBlocked')
            # A recipe that names no step keeps its whole note.
            receipt('one; two')
            self.assertEqual(nightly.failed_recipe_note(out, 'fallback'), 'one; two')

    def test_an_unresolved_live_cleanup_stops_the_row_before_any_retry_or_later_flow(self):
        record, cells, retire, slot = self.lost_session_row([], marker=True)
        self.assertEqual(cells, ['idle_memory-1'])
        self.assertTrue(record['notRunReason'].startswith('live remote cleanup unresolved in idle_memory attempt 1'))
        self.assertEqual(record['stoppedEarly']['flow'], 'idle_memory')
        retire.assert_not_called()
        self.reset.assert_not_called()
        self.assertTrue(slot and record['cleanupBlocked'])

    def test_a_live_cleanup_marker_keeps_the_guest_without_any_receipt_flag(self):
        """A runner killed after publishing writes no receipt flag; the marker file in the run directory is enough."""
        errors = ['notification monitor cleanup unavailable',
                  'resident quit transport unavailable; private session retained']
        record, cells, retire, slot = self.lost_session_row(errors, marker=True)
        retire.assert_not_called()
        self.reset.assert_not_called()
        self.assertTrue(slot and record['cleanupBlocked'])
        self.assertIn('live remote cleanup unresolved', record['notRunReason'])

    def test_recovery_and_lab_reset_refuse_a_run_with_a_live_cleanup_marker(self):
        from tools.lab import lab
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.stale_fixture(root, missing=False)
            run = Path(owner['run'])
            (run/lab.LIVE_CLEANUP_MARKER).write_text('{}')
            with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                    patch('tools.lab.desktop_lock.Desktop', side_effect=lambda *a, **kw:
                          proton if kw.get('resource') == 'proton' else desktop), \
                    patch.object(nightly, 'retire') as retire:
                with self.assertRaisesRegex(nightly.NotRun, 'live remote cleanup unresolved'):
                    nightly.recover_stale_guest(dict(host='d69'), out)
            retire.assert_not_called()
            self.assertTrue(slot.exists())
            # The lowest layer refuses too, so no caller can stop or delete the guest by another path.
            with self.assertRaisesRegex(lab.LabError, 'live remote cleanup unresolved'):
                lab.reset(run)
            with self.assertRaisesRegex(lab.LabError, 'live remote cleanup unresolved'):
                lab.cleanup_overlay(run)
            (run/lab.LIVE_CLEANUP_MARKER).unlink()
            self.assertIn(lab.reset(run)['status'], ('stopped', 'already-stopped'))

    def kept_live_row(self, root, *, errors=(), guest_local=()):
        """A retained slot whose row receipt and run marker record an unresolved live publication (#1660)."""
        from tools.lab import lab
        slot, out, owner, desktop, proton = self.stale_fixture(root, missing=False)
        run, evidence = Path(owner['run']), Path(owner['evidence'])
        (evidence/'idle-1').mkdir()
        (evidence/'idle-1/receipt.json').write_text(json.dumps(dict(environment=dict(
            unresolved_live_cleanup=True, cleanup_errors=list(errors), guest_local_cleanup=list(guest_local)))))
        nightly.save(run/lab.LIVE_CLEANUP_MARKER, dict(schemaVersion=1, destination='s3', prefix='acceptance/tier1-live/x/',
                     objectKey='acceptance/tier1-live/x/shot.png', runId='n12345678'))
        return slot, out, owner, desktop, proton

    def recover_kept(self, root, out, desktop, proton):
        from tools.lab import lab
        with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                patch('tools.lab.desktop_lock.Desktop', side_effect=lambda *a, **kw:
                      proton if kw.get('resource') == 'proton' else desktop), \
                patch.object(lab, 'state', return_value=dict(id='n12345678')), \
                patch.object(nightly, 'retire') as retire:
            # Each nightly run recovers into its own new output directory.
            attempt = out/uuid.uuid4().hex
            attempt.mkdir()
            try:
                nightly.recover_stale_guest(dict(host='d69'), attempt)
            except nightly.NotRun as error:
                return retire, str(error)
        return retire, None

    def resolve(self, root, **kwargs):
        from tools.lab import lab
        with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                patch.object(lab, 'state', return_value=dict(id=kwargs.pop('run_id', 'n12345678'))), \
                patch.object(nightly, 'guest_paused', return_value=kwargs.pop('paused', 0)):
            return nightly.resolve_live_cleanup(nightly.retained_owner(), kwargs.pop('checked', 'n12345678'), **kwargs)

    def test_a_confirmed_live_cleanup_record_lets_recovery_retire_the_kept_guest(self):
        from tools.lab import lab
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.kept_live_row(root)
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIn('live remote cleanup unresolved', error)
            record = self.resolve(root)
            self.assertFalse((Path(owner['run'])/lab.LIVE_CLEANUP_MARKER).exists())
            self.assertEqual(record['publication'], dict(destination='s3', prefix='acceptance/tier1-live/x/',
                             objectKey='acceptance/tier1-live/x/shot.png'))
            self.assertTrue(record['markerPresent'] and record['objectConfirmedGone'])
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIsNone(error)
            retire.assert_called_once()
            self.assertFalse(slot.exists())

    def test_a_live_cleanup_record_speaks_only_for_the_receipts_it_hashed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.kept_live_row(root)
            self.resolve(root)
            # A later attempt (or a changed receipt) the person never saw keeps the guest.
            evidence = Path(owner['evidence'])
            (evidence/'idle-2').mkdir()
            (evidence/'idle-2/receipt.json').write_text(json.dumps(dict(environment=dict(unresolved_live_cleanup=True))))
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIn('idle-2', error)
            retire.assert_not_called()
            self.assertTrue(slot.exists())

    def test_a_live_cleanup_record_clears_no_other_cleanup_error_unless_named(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.kept_live_row(root, errors=['private session root cleanup refused'])
            self.resolve(root)
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIn('inspect private cleanup', error)
            retire.assert_not_called()
            with self.assertRaisesRegex(nightly.NotRun, 'exact cleanup error'):
                self.resolve(root, guest_only_errors=['some other error'],
                             publication=dict(destination='s3', prefix='acceptance/tier1-live/x/',
                                              objectKey='acceptance/tier1-live/x/shot.png'))
            self.resolve(root, guest_only_errors=['private session root cleanup refused'],
                         publication=dict(destination='s3', prefix='acceptance/tier1-live/x/',
                                          objectKey='acceptance/tier1-live/x/shot.png'))
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIsNone(error)
            retire.assert_called_once()

    def test_resolving_live_cleanup_refuses_a_marker_or_publication_of_another_run(self):
        from tools.lab import lab
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.kept_live_row(root)
            marker = Path(owner['run'])/lab.LIVE_CLEANUP_MARKER
            with self.assertRaisesRegex(nightly.NotRun, 'does not belong to this run'):
                self.resolve(root, run_id='n87654321')
            # The person names the run they checked; a different guest in the slot is refused.
            with self.assertRaisesRegex(nightly.NotRun, 'not the run you checked'):
                self.resolve(root, checked='n87654321')
            # An orphaned recipe or lend may still be publishing.
            with patch.object(nightly, 'guest_clients_live', return_value=True), \
                    self.assertRaisesRegex(nightly.NotRun, 'still live'):
                self.resolve(root)
            # put.gd in a running guest can still finish an upload; only a check made while the VM is paused counts.
            with self.assertRaisesRegex(nightly.NotRun, 'VM is running'):
                self.resolve(root, paused=None)
            # A pause too recent for an already-sent upload to have committed does not count yet.
            with self.assertRaisesRegex(nightly.NotRun, 'paused under'):
                self.resolve(root, paused=nightly.time.time()-10)
            with self.assertRaisesRegex(nightly.NotRun, 'differs from the marker'):
                self.resolve(root, publication=dict(destination='s3', prefix='p/', objectKey='p/other.png'))
            self.assertTrue(marker.exists())
            self.assertFalse((Path(owner['evidence'])/nightly.LIVE_RESOLUTION).exists())
            marker.unlink()
            with self.assertRaisesRegex(nightly.NotRun, 'give --destination'):
                self.resolve(root)
            with self.assertRaisesRegex(nightly.NotRun, 'outside its prefix'):
                self.resolve(root, publication=dict(destination='s3', prefix='p/', objectKey='q/shot.png'))
            # A record for another session or run id is refused, never ignored.
            self.resolve(root, publication=dict(destination='s3', prefix='p/', objectKey='p/shot.png'))
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIsNone(error)
            path = Path(owner['evidence'])/nightly.LIVE_RESOLUTION
            nightly.save(path, dict(json.loads(path.read_text()), session='other'))
            with self.assertRaisesRegex(nightly.NotRun, 'does not record a confirmed cleanup'):
                nightly.live_resolution(Path(owner['run']), Path(owner['evidence']), owner['session'])

    def test_operator_commands_refuse_before_writing_or_checking_anything(self):
        from tools.lab import lab
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.kept_live_row(root)
            evidence = Path(owner['evidence'])
            def main(*argv):
                with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                        patch.object(lab, 'state', return_value=dict(id='n12345678')), \
                        patch.object(sys, 'argv', ['nightly_desktops.py', *argv]):
                    with self.assertRaises(SystemExit) as caught:
                        nightly.main()
                return str(caught.exception)
            # A wrong run is refused before the unaudited record is written.
            self.assertIn('not the run you checked', main('--unaudited-attempt', 'idle-1', '--resolve-live-cleanup', 'n87654321'))
            self.assertFalse((evidence/'idle-1'/nightly.UNAUDITED_ATTEMPT).exists())
            # No absence check while an orphaned recipe or lend may still publish.
            with patch.object(nightly, 'guest_clients_live', return_value=True), \
                    patch.object(nightly, 'guest_paused') as paused, patch.object(nightly, 'check_s3_absent') as check:
                self.assertIn('still live', main('--check-live-cleanup'))
            check.assert_not_called()
            paused.assert_not_called()
            # The VM is paused before the destination is read, and a guest that won't pause is never checked.
            order = []
            with patch.object(nightly, 'guest_paused', side_effect=lambda run, pause=False: order.append('pause')), \
                    patch.object(nightly, 'check_s3_absent', side_effect=lambda p: order.append('check')):
                self.assertIn('could not pause', main('--check-live-cleanup'))
            self.assertEqual(order, ['pause'])
            # After a pause, the destination is read only once the settle period has passed.
            paused_at = nightly.time.time()
            with patch.object(nightly, 'guest_paused', return_value=paused_at), \
                    patch.object(nightly.time, 'sleep', side_effect=lambda seconds: order.append(round(seconds, -1))), \
                    patch.object(nightly, 'check_s3_absent', side_effect=lambda p: order.append('check') or {}):
                main('--check-live-cleanup')
            self.assertEqual(order[1:], [nightly.PAUSE_SETTLE_SECONDS, 'check'])
            # A resume during the wait voids the pause; the destination is never read.
            del order[:]
            with patch.object(nightly, 'guest_paused', side_effect=lambda run, pause=False: paused_at if pause else None), \
                    patch.object(nightly.time, 'sleep'), \
                    patch.object(nightly, 'check_s3_absent', side_effect=lambda p: order.append('check')):
                self.assertIn('resumed during the wait', main('--check-live-cleanup'))
            self.assertEqual(order, [])
            self.assertTrue((Path(owner['run'])/lab.LIVE_CLEANUP_MARKER).exists())

    def lost_ssh_row(self, root, errors=('fixture cleanup failed: ssh connection lost',)):
        """A slot kept only because a fixture cleanup lost the guest connection: no live publication (2026-11-20, d69)."""
        slot, out, owner, desktop, proton = self.stale_fixture(root, missing=False)
        evidence = Path(owner['evidence'])
        (evidence/'region_upload-1').mkdir()
        nightly.save(evidence/'region_upload-1/receipt.json', dict(environment=dict(
            cleanup_errors=list(errors), guest_local_cleanup=[])))
        return slot, out, owner, desktop, proton

    def operator(self, root, desktop, proton, *argv, host='d69'):
        """Run an operator command as on the builder; returns (exit text or None, printed JSON or None)."""
        from tools.lab import lab
        import contextlib, io
        config = root/'config.json'
        if not config.exists():
            config.write_text(json.dumps(dict(builders={host: dict(hostnames=['builder-'+host])})))
            config.chmod(0o600)
        printed = io.StringIO()
        with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                patch('tools.lab.desktop_lock.Desktop', side_effect=lambda *a, **kw:
                      proton if kw.get('resource') == 'proton' else desktop), \
                patch.object(lab, 'state', return_value=dict(id='n12345678')), \
                patch.object(nightly.socket, 'gethostname', return_value='builder-'+host), \
                patch.object(sys, 'argv', ['nightly_desktops.py', '--config', str(config), *argv]), \
                contextlib.redirect_stdout(printed):
            try:
                nightly.main()
                error = None
            except SystemExit as caught:
                error = str(caught)
        text = printed.getvalue()
        return error, json.loads(text) if text.strip() else None

    def test_a_row_with_no_live_publication_records_guest_only_errors_without_a_pause(self):
        from tools.lab import lab
        error_text = 'fixture cleanup failed: ssh connection lost'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.lost_ssh_row(root)
            evidence = Path(owner['evidence'])
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIn('inspect private cleanup', error)
            with patch.object(nightly, 'guest_paused', return_value=None) as paused, patch.object(nightly, 'retire'):
                # Without the explicit flag the command still expects a confirmed live publication.
                error, _ = self.operator(root, desktop, proton, '--resolve-live-cleanup', 'n12345678',
                                         '--guest-only-error', error_text)
                self.assertIn('VM is running', error)
                # Nothing to record: say what to run instead, and write nothing.
                error, _ = self.operator(root, desktop, proton, '--resolve-live-cleanup', 'n12345678', '--no-live-publication')
                self.assertIn('--recover-retained', error)
                self.assertFalse((evidence/nightly.LIVE_RESOLUTION).exists())
                error, record = self.operator(root, desktop, proton, '--resolve-live-cleanup', 'n12345678',
                                              '--no-live-publication', '--guest-only-error', error_text)
            # No publication is pending, so no pause or destination check is needed.
            self.assertEqual(paused.call_count, 1)  # Only the refused call without the flag.
            self.assertIsNone(error)
            self.assertEqual((record['publication'], record['objectConfirmedGone'], record['livePublicationPending']),
                             (None, False, False))
            self.assertEqual(record['guestOnlyErrors'], {'region_upload-1': [error_text]})
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIsNone(error)
            retire.assert_called_once()
            self.assertFalse(slot.exists())

    def test_a_record_of_no_live_publication_never_clears_a_live_cleanup_flag(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.kept_live_row(root, errors=['private session root cleanup refused'])
            run, evidence = Path(owner['run']), Path(owner['evidence'])
            # Without the marker, a receipt's flag still means a publication is pending.
            (run/'live-cleanup-pending.json').unlink()
            with self.assertRaisesRegex(nightly.NotRun, 'give --destination'):
                self.resolve(root, guest_only_errors=['private session root cleanup refused'])
            with self.assertRaisesRegex(nightly.NotRun, 'live publication is pending'):
                self.resolve(root, guest_only_errors=['private session root cleanup refused'], no_live_publication=True)
            self.assertFalse((evidence/nightly.LIVE_RESOLUTION).exists())
            # A hand-made record claiming nothing was pending clears only the errors it names.
            nightly.save(evidence/nightly.LIVE_RESOLUTION, dict(
                schemaVersion=1, kind='live-cleanup-resolution', run=str(run), runId='n12345678',
                session=owner['session'], publication=None, objectConfirmedGone=False, livePublicationPending=False,
                markerPresent=False, receipts={'idle-1': nightly.digest(evidence/'idle-1/receipt.json')},
                guestOnlyErrors={'idle-1': ['private session root cleanup refused']}))
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIn('live remote cleanup unresolved', error)
            retire.assert_not_called()
            # A record mixing the two forms is refused, never read as either.
            nightly.save(evidence/nightly.LIVE_RESOLUTION, dict(json.loads((evidence/nightly.LIVE_RESOLUTION).read_text()),
                                                                 objectConfirmedGone=True))
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIn('does not record a confirmed cleanup', error)
            self.assertTrue(slot.exists())

    def test_recover_retained_recovers_the_checked_slot_now_and_records_why(self):
        error_text = 'fixture cleanup failed: ssh connection lost'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.lost_ssh_row(root)
            evidence = Path(owner['evidence'])
            with patch.object(nightly, 'retire') as retire:
                # The wrong run, or a recipe still running for this one, is refused before anything is written.
                error, _ = self.operator(root, desktop, proton, '--recover-retained', 'n87654321')
                self.assertIn('not the run you checked', error)
                with patch.object(nightly, 'guest_clients_live', return_value=True):
                    error, _ = self.operator(root, desktop, proton, '--recover-retained', 'n12345678')
                self.assertIn('still live', error)
                self.assertFalse((evidence/'operator-recovery').exists())
                # Every nightly check still applies: an unchecked cleanup error keeps the guest.
                error, _ = self.operator(root, desktop, proton, '--recover-retained', 'n12345678')
                self.assertIn('inspect private cleanup', error)
                retire.assert_not_called()
                self.assertTrue(slot.exists())
                self.assertNotEqual(desktop.status()['state'], 'free')
                self.operator(root, desktop, proton, '--resolve-live-cleanup', 'n12345678', '--no-live-publication',
                              '--guest-only-error', error_text)
                # It runs alone: a combined check could pause the guest and exit without recovering.
                with patch.object(nightly, 'guest_paused') as paused, patch.object(sys, 'stderr'):
                    for extra in (('--check-live-cleanup',), ('--guest-only-error', error_text), ('--no-live-publication',)):
                        error, _ = self.operator(root, desktop, proton, '--recover-retained', 'n12345678', *extra)
                        self.assertEqual(error, '2')
                    # An empty or malformed run never falls through to the controller.
                    for argv in (('--recover-retained', '', '--now'), ('--recover-retained', '../n12345678'),
                                 ('--resolve-live-cleanup', '', '--no-live-publication'),
                                 ('--check-live-cleanup', '--resolve-live-cleanup', 'n12345678', '--no-live-publication')):
                        with patch.object(nightly, 'controller') as controller:
                            error, _ = self.operator(root, desktop, proton, *argv)
                        self.assertEqual(error, '2')
                        controller.assert_not_called()
                paused.assert_not_called()
                error, summary = self.operator(root, desktop, proton, '--recover-retained', 'n12345678')
            self.assertIsNone(error)
            retire.assert_called_once_with(Path(owner['run']), evidence, {})
            self.assertFalse(slot.exists())
            self.assertEqual(desktop.status()['state'], 'free')
            self.assertEqual(summary['resolution']['guestOnlyErrors'], {'region_upload-1': [error_text]})
            archived, = (root/'putgd-lab/locks/history/recovered').glob('*-n12345678-*.json')
            record = json.loads(archived.read_text())
            self.assertEqual(record['recoveredBy'], 'operator')
            self.assertEqual(record['keptReason'], None)
            self.assertEqual(record['cleared'], summary)
            self.assertTrue(Path(record['desktopRelease']).is_file())
            self.assertEqual(Path(record['recovery']).parents[1], evidence/'operator-recovery')
            # Nothing is left to recover.
            error, _ = self.operator(root, desktop, proton, '--recover-retained', 'n12345678')
            self.assertIn('no retained nightly guest slot', error)

    def test_recover_retained_never_takes_a_live_lease_or_an_unreturned_proton_lend(self):
        for mode in ('live lease', 'unreturned lend', 'not this builder'):
            with self.subTest(mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                slot, out, owner, desktop, proton = self.stale_fixture(
                    root, missing=False, live=mode == 'live lease', lend='provisioned' if mode == 'unreturned lend' else None)
                with patch.object(nightly, 'retire') as retire, patch.object(nightly, 'command',
                        side_effect=RuntimeError('proton recover failed')):
                    if mode == 'not this builder':
                        (root/'config.json').write_text(json.dumps(dict(builders={'f44': dict(hostnames=['elsewhere'])})))
                        (root/'config.json').chmod(0o600)
                    error, _ = self.operator(root, desktop, proton, '--recover-retained', 'n12345678')
                self.assertIn({'live lease': 'desktop lease is live', 'unreturned lend': 'recovery incomplete',
                               'not this builder': 'not exactly one configured nightly builder'}[mode], error)
                retire.assert_not_called()
                self.assertTrue(slot.exists())
                self.assertNotEqual(desktop.status()['state'], 'free')

    def test_a_kept_live_row_whose_qemu_exited_resolves_and_recovers_through_the_commands(self):
        """2026-11-20: the transient unit SIGKILLed QEMU with its worker, so nothing could pause the VM."""
        from tools.lab import lab
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.kept_live_row(root)
            run = Path(owner['run'])
            (run/'qmp.sock').write_text('')
            (run/'disk.qcow2').write_text('')
            with patch.object(lab, 'QMP', side_effect=ConnectionRefusedError), \
                    patch.object(nightly, 'vm_process_gone', return_value=True), \
                    patch.object(nightly.time, 'sleep') as sleep, \
                    patch.object(nightly, 'check_s3_absent', return_value=dict(
                        objectKeyAbsent=True, prefixEmpty=True, versionsGone=True)) as check:
                error, result = self.operator(root, desktop, proton, '--check-live-cleanup')
                self.assertEqual(error, '0')
                self.assertEqual(result, dict(guestPaused=True, guestExited=True, objectKeyAbsent=True,
                                              prefixEmpty=True, versionsGone=True))
                check.assert_called_once()
                self.assertAlmostEqual(sleep.call_args.args[0], nightly.PAUSE_SETTLE_SECONDS, delta=5)
                # Within the settle period the resolution still refuses.
                error, _ = self.operator(root, desktop, proton, '--resolve-live-cleanup', 'n12345678')
                self.assertIn('paused under', error)
                record = run/nightly.PAUSE_RECORD
                nightly.save(record, dict(json.loads(record.read_text()), pausedAt=nightly.time.time()-200))
                error, resolution = self.operator(root, desktop, proton, '--resolve-live-cleanup', 'n12345678')
                self.assertIsNone(error)
                self.assertTrue(resolution['objectConfirmedGone'])
                with patch.object(nightly, 'retire') as retire:
                    error, summary = self.operator(root, desktop, proton, '--recover-retained', 'n12345678')
            self.assertIsNone(error)
            retire.assert_called_once()
            self.assertEqual(summary['resolution']['publication']['objectKey'], 'acceptance/tier1-live/x/shot.png')
            self.assertFalse(slot.exists())

    def test_guest_paused_stops_the_vm_and_reports_only_a_confirmed_pause(self):
        from tools.lab import lab
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            # Without a QMP socket only a run with neither disk nor state has no VM.
            self.assertEqual(nightly.guest_paused(run), 0)
            for name in ('state.json', 'disk.qcow2'):
                (run/name).write_text('{}')
                self.assertIsNone(nightly.guest_paused(run))
                (run/name).unlink()
            (run/'state.json').write_text('{}')
            (run/'disk.qcow2').write_text('')
            (run/'qmp.sock').write_text('')
            status = {'running': True}
            qmp = Mock()
            qmp.__enter__ = Mock(return_value=qmp)
            qmp.__exit__ = Mock(return_value=False)
            def execute(name, *a):
                if name == 'stop':
                    status['running'] = False
                return dict(status) if name == 'query-status' else {}
            qmp.execute.side_effect = execute
            with patch.object(lab, 'QMP', return_value=qmp):
                self.assertIsNone(nightly.guest_paused(run))
                paused = nightly.guest_paused(run, pause=True)
                self.assertAlmostEqual(paused, nightly.time.time(), delta=5)
                # A second check of a still-paused VM keeps the first pause time.
                self.assertEqual(nightly.guest_paused(run, pause=True), paused)
            self.assertEqual([c.args[0] for c in qmp.execute.call_args_list],
                             ['query-status', 'query-status', 'stop', 'query-status', 'query-status'])
            # A pause long past its settle period, then a resume QMP reports.
            nightly.save(run/nightly.PAUSE_RECORD, dict(pausedAt=1.0, vm='paused'))
            status['running'] = True
            with patch.object(lab, 'QMP', return_value=qmp):
                self.assertIsNone(nightly.guest_paused(run))
                # The resume voids the old pause, even when the VM is next paused by something else.
                self.assertFalse((run/nightly.PAUSE_RECORD).exists())
                status['running'] = False
                self.assertIsNone(nightly.guest_paused(run))
                self.assertAlmostEqual(nightly.guest_paused(run, pause=True), nightly.time.time(), delta=5)
                status['running'] = True
            # A QMP that does not answer is no pause while QEMU may still run on the overlay.
            with patch.object(lab, 'QMP', side_effect=OSError), patch.object(nightly, 'vm_process_gone', return_value=False):
                self.assertIsNone(nightly.guest_paused(run, pause=True))
            # A stop whose reply is lost leaves no old pause time behind for a retry to reuse.
            def lost(name, *a):
                if name == 'stop':
                    raise lab.LabError('timeout')
                return {'running': True}
            qmp.execute.side_effect = lost
            with patch.object(lab, 'QMP', return_value=qmp), patch.object(nightly, 'vm_process_gone', return_value=False):
                self.assertIsNone(nightly.guest_paused(run, pause=True))
            self.assertFalse((run/nightly.PAUSE_RECORD).exists())

    def test_a_guest_whose_qemu_exited_counts_as_stopped_only_from_when_its_exit_was_seen(self):
        from tools.lab import lab
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            for name in ('state.json', 'disk.qcow2', 'qmp.sock'):
                (run/name).write_text('')
            gone = patch.object(nightly, 'vm_process_gone', return_value=True)
            gone.start()
            self.addCleanup(gone.stop)
            # A transient unit killed QEMU with its worker: the socket file is left, nothing answers it.
            with patch.object(lab, 'QMP', side_effect=ConnectionRefusedError):
                # Until a check records the exit, no stop counts.
                self.assertIsNone(nightly.guest_paused(run))
                with patch.object(nightly, 'vm_process_gone', return_value=False):
                    self.assertIsNone(nightly.guest_paused(run, pause=True))
                self.assertFalse((run/nightly.PAUSE_RECORD).exists())
                seen = nightly.guest_paused(run, pause=True)
                self.assertAlmostEqual(seen, nightly.time.time(), delta=5)
                self.assertEqual(nightly.guest_paused(run), seen)
                self.assertEqual(nightly.guest_paused(run, pause=True), seen)
                # A QEMU that turns up on the overlay again voids the exit.
                with patch.object(nightly, 'vm_process_gone', return_value=False):
                    self.assertIsNone(nightly.guest_paused(run))
            # An answering VM is never covered by an exit record, even when it is paused.
            qmp = Mock()
            qmp.__enter__ = Mock(return_value=qmp)
            qmp.__exit__ = Mock(return_value=False)
            qmp.execute.return_value = {'running': False}
            with patch.object(lab, 'QMP', return_value=qmp):
                self.assertIsNone(nightly.guest_paused(run))
                paused = nightly.guest_paused(run, pause=True)
                self.assertGreaterEqual(paused, seen)
                self.assertEqual(json.loads((run/nightly.PAUSE_RECORD).read_text())['vm'], 'paused')
            # A pause recorded before QEMU died does not count: the VM may have been resumed in between.
            with patch.object(lab, 'QMP', side_effect=ConnectionRefusedError):
                self.assertIsNone(nightly.guest_paused(run))
                self.assertGreaterEqual(nightly.guest_paused(run, pause=True), paused)
            # A QMP that completes its greeting proves a QEMU ran, even if it drops before answering.
            nightly.save(run/nightly.PAUSE_RECORD, dict(pausedAt=1.0, vm='exited', disk=nightly.disk_identity(run)))
            self.assertEqual(nightly.guest_paused(run), 1.0)
            dropping = Mock()
            dropping.__enter__ = Mock(return_value=dropping)
            dropping.__exit__ = Mock(return_value=False)
            dropping.execute.side_effect = ConnectionResetError
            with patch.object(lab, 'QMP', return_value=dropping):
                self.assertIsNone(nightly.guest_paused(run))
            self.assertFalse((run/nightly.PAUSE_RECORD).exists())
            # The same when it drops during the greeting or negotiation, before the session opens.
            for failure in (ConnectionResetError, BrokenPipeError, TimeoutError, lab.LabError('Missing QMP greeting')):
                nightly.save(run/nightly.PAUSE_RECORD, dict(pausedAt=1.0, vm='exited', disk=nightly.disk_identity(run)))
                with patch.object(lab, 'QMP', side_effect=failure):
                    self.assertIsNone(nightly.guest_paused(run))
                self.assertFalse((run/nightly.PAUSE_RECORD).exists())
            with patch.object(lab, 'QMP', side_effect=ConnectionRefusedError):
                nightly.guest_paused(run, pause=True)
            # The socket file can be gone too.
            (run/'qmp.sock').unlink()
            self.assertIsNotNone(nightly.guest_paused(run))
            # A QEMU that ran on the overlay between two looks wrote to it: its exit starts a new settle period.
            (run/'disk.qcow2').write_text('written by a later QEMU')
            self.assertIsNone(nightly.guest_paused(run))

    def test_vm_process_gone_uses_the_lab_overlay_inventory(self):
        from tools.lab import overlay_cleanup
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            # No overlay file: nothing to match by identity, so no proof.
            self.assertFalse(nightly.vm_process_gone(run))
            (run/'disk.qcow2').write_text('')
            with patch.object(overlay_cleanup, '_check_processes') as check:
                self.assertTrue(nightly.vm_process_gone(run))
            check.assert_called_once_with(run, run/'disk.qcow2')
            for refusal in (overlay_cleanup.CleanupError('Hidden /proc inventory prevents safe cleanup'),
                            overlay_cleanup.CleanupError('A process still has the overlay open'), PermissionError()):
                with patch.object(overlay_cleanup, '_check_processes', side_effect=refusal):
                    self.assertFalse(nightly.vm_process_gone(run))
            (run/'disk.qcow2').unlink()
            (run/'disk.qcow2').symlink_to(run/'elsewhere')
            with patch.object(overlay_cleanup, '_check_processes'):
                self.assertFalse(nightly.vm_process_gone(run))

    def test_a_no_publication_record_leaves_a_marker_that_appeared_after_its_check(self):
        from tools.lab import lab
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.lost_ssh_row(root)
            marker = Path(owner['run'])/lab.LIVE_CLEANUP_MARKER
            real = nightly.resolution_overlay
            def raced(*args):
                value = real(*args)
                nightly.save(marker, dict(schemaVersion=1, destination='s3', prefix='p/', objectKey='p/x.png', runId='n12345678'))
                return value
            with patch.object(nightly, 'resolution_overlay', side_effect=raced):
                self.resolve(root, guest_only_errors=['fixture cleanup failed: ssh connection lost'], no_live_publication=True)
            self.assertTrue(marker.exists())
            retire, error = self.recover_kept(root, out, desktop, proton)
            self.assertIn('live remote cleanup unresolved', error)
            retire.assert_not_called()

    def test_a_retry_after_a_refused_release_reverifies_the_earlier_retirement(self):
        from tools.lab import lab
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.stale_fixture(root, missing=False)
            evidence = Path(owner['evidence'])
            run = Path(owner['run'])
            # The first recovery got to guarded cleanup and wrote its manifest: done (no disk left) or only
            # prepared (disk still there) when it stopped.
            nightly.save(evidence/'completion-manifest.json', dict(files={}))
            for disk_left in (False, True):
                # With no disk, the guarded path decides even without a cleanup receipt.
                if disk_left:
                    (run/'disk.qcow2').write_text('')
                    nightly.save(run/'overlay-cleanup.json', dict(status='prepared'))
                    slot.mkdir(mode=0o700)
                    nightly.save(slot/'retained.json', owner)
                    (slot/'session').write_text(owner['session'])
                with self.subTest(disk_left=disk_left), patch.object(nightly, 'ROOT', root), \
                        patch.object(nightly.Path, 'home', return_value=root), \
                        patch('tools.lab.desktop_lock.Desktop', side_effect=lambda *a, **kw:
                              proton if kw.get('resource') == 'proton' else desktop), \
                        patch.object(lab, 'state', return_value=dict(id='n12345678', golden=str(root/'g/a/b/golden'))), \
                        patch.object(lab, 'cleanup_overlay', return_value={'status': 'completed'}) as cleanup, \
                        patch.object(nightly, 'retire') as retire:
                    attempt = out/str(disk_left)
                    attempt.mkdir()
                    summary = nightly.recover_stale_guest(dict(host='d69'), attempt)
                    retire.assert_not_called()
                    cleanup.assert_called_once_with(run, evidence_manifest=evidence/'completion-manifest.json',
                                                    images_root=root/'g/a')
                    self.assertEqual(summary['guest'], 'cleanup finished or re-checked from the earlier manifest')
                    self.assertFalse(slot.exists())

    def test_the_s3_cleanup_check_reads_only_the_marker_key_and_prefix(self):
        class NoSuchKey(Exception):
            pass
        def client(listing=None, versioning=None, versions=None):
            value = Mock()
            value.exceptions.NoSuchKey = NoSuchKey
            value.get_object.side_effect = NoSuchKey()
            value.list_objects_v2.return_value = {'KeyCount': 0, 'IsTruncated': False} if listing is None else listing
            value.get_bucket_versioning.return_value = versioning or {}
            value.list_object_versions.return_value = versions or {'IsTruncated': False}
            return value
        prefix = 'acceptance/tier1-live/'+'a'*32+'/'
        publication = dict(destination='s3', prefix=prefix, objectKey=prefix+'shot.png')
        quiet = client()
        self.assertEqual(nightly.check_s3_absent(publication, client=quiet),
                         dict(objectKeyAbsent=True, prefixEmpty=True, versionsGone=True))
        quiet.get_object.assert_called_once_with(Bucket='put.gd', Key=prefix+'shot.png')
        quiet.list_objects_v2.assert_called_once_with(Bucket='put.gd', Prefix=prefix, MaxKeys=1)
        quiet.list_object_versions.assert_not_called()
        present = client(listing={'KeyCount': 1, 'IsTruncated': False, 'Contents': [{}]})
        present.get_object.side_effect = None
        present.get_object.return_value = {'Body': Mock()}
        self.assertEqual(nightly.check_s3_absent(publication, client=present),
                         dict(objectKeyAbsent=False, prefixEmpty=False, versionsGone=True))
        # Only an explicit, complete, empty listing counts.
        for listing in ({}, {'KeyCount': 0, 'IsTruncated': True}, {'KeyCount': 0}):
            self.assertFalse(nightly.check_s3_absent(publication, client=client(listing=listing))['prefixEmpty'])
        # A versioned bucket can keep the upload behind a delete marker.
        kept = client(versioning={'Status': 'Suspended'}, versions={'IsTruncated': False, 'Versions': [{}]})
        self.assertFalse(nightly.check_s3_absent(publication, client=kept)['versionsGone'])
        kept.list_object_versions.assert_called_once_with(Bucket='put.gd', Prefix=prefix, MaxKeys=1000)
        # Pages of delete markers alone do not hide a version: follow every page.
        paged = client(versioning={'Status': 'Enabled'})
        paged.list_object_versions.side_effect = [
            {'IsTruncated': True, 'DeleteMarkers': [{}], 'NextKeyMarker': 'k', 'NextVersionIdMarker': 'v'},
            {'IsTruncated': False, 'DeleteMarkers': [{}]}]
        self.assertTrue(nightly.check_s3_absent(publication, client=paged)['versionsGone'])
        self.assertEqual(paged.list_object_versions.call_args.kwargs['KeyMarker'], 'k')
        # A key marker without its version marker could skip that key's remaining versions.
        cursor = client(versioning={'Status': 'Enabled'})
        cursor.list_object_versions.side_effect = [
            {'IsTruncated': True, 'DeleteMarkers': [{}], 'NextKeyMarker': 'k'}, {'IsTruncated': False}]
        self.assertFalse(nightly.check_s3_absent(publication, client=cursor)['versionsGone'])
        cursor.list_object_versions.assert_called_once()
        broken = client()
        broken.get_bucket_versioning.side_effect = RuntimeError('secret-looking detail')
        with self.assertRaises(nightly.NotRun) as caught:
            nightly.check_s3_absent(publication, client=broken)
        self.assertNotIn('secret', str(caught.exception))
        self.assertIsNone(caught.exception.__cause__)
        for bad in (dict(publication, destination='proton'), dict(publication, prefix='acceptance/'),
                    dict(publication, objectKey='other/shot.png')):
            with self.assertRaises(nightly.NotRun):
                nightly.check_s3_absent(bad, client=quiet)
        for value in (quiet, present, kept):
            self.assertFalse(value.delete_object.called or value.put_object.called)

    def test_an_attempt_killed_before_its_terminal_proof_retires_only_once_recorded_unaudited(self):
        from tools.lab import lab
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            slot, out, owner, desktop, proton = self.stale_fixture(root, missing=False)
            run, evidence = Path(owner['run']), Path(owner['evidence'])
            (evidence/'idle-1').mkdir()
            (evidence/'idle-1/receipt.json').write_text('{"environment": {}}')
            with patch.object(lab, 'state', return_value=dict(id='n12345678', golden=str(root/'g/a/b/golden'))), \
                    patch.object(lab, 'reset') as reset, patch.object(lab, 'cleanup_overlay', return_value={}) as cleanup:
                with self.assertRaisesRegex(RuntimeError, 'collection refused'):
                    nightly.retire(run, evidence, {})
                cleanup.assert_not_called()
                with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root):
                    record = nightly.record_unaudited_attempt(nightly.retained_owner(), 'idle-1')
                    with self.assertRaisesRegex(nightly.NotRun, 'directory of this row'):
                        nightly.record_unaudited_attempt(owner, '../idle-1')
                self.assertEqual(record['receiptSha256'], nightly.digest(evidence/'idle-1/receipt.json'))
                # A terminal proof that exists but fails its checks is a refusal, never an interruption.
                (evidence/'idle-2').mkdir()
                (evidence/'idle-2/receipt.json').write_text('{}')
                (evidence/'idle-2/terminal.json').write_text('{}')
                with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                        self.assertRaisesRegex(nightly.NotRun, 'has a terminal.json'):
                    nightly.record_unaudited_attempt(owner, 'idle-2')
                (evidence/'idle-2/terminal.json').unlink()
                # A dangling terminal.json symlink is not an absent proof, at record or retirement time.
                (evidence/'idle-1/terminal.json').symlink_to(evidence/'missing')
                with self.assertRaisesRegex(nightly.NotRun, 'does not match'):
                    nightly.unaudited_attempt(evidence/'idle-1')
                (evidence/'idle-1/terminal.json').unlink()
                (evidence/'idle-2/receipt.json').unlink()
                (evidence/'idle-2').rmdir()
                nightly.retire(run, evidence, {})
                cleanup.assert_called_once()
            manifest = json.loads((evidence/'completion-manifest.json').read_text())
            self.assertEqual(manifest['outcome'], 'failed')
            self.assertEqual(manifest['unauditedAttempts'], ['idle-1'])
            self.assertEqual(sorted(manifest['files']), ['completion.json', 'idle-1/receipt.json', 'idle-1/unaudited-attempt.json'])
            self.assertTrue((evidence/'idle-1/receipt.json').exists())
            # A receipt changed after the record is no longer the one the person saw.
            (evidence/'idle-1/receipt.json').write_text('{"environment": {"changed": true}}')
            with self.assertRaisesRegex(nightly.NotRun, 'does not match'):
                nightly.unaudited_attempt(evidence/'idle-1')

    def retirement_fixture(self, root):
        """A real guarded cleanup over one stopped overlay and one terminal attempt; only QEMU probes are faked."""
        from contextlib import ExitStack, contextmanager
        from tools.lab import lab
        # The module lab.py itself calls (it may be imported under its plain name).
        overlay_cleanup = lab.overlay_cleanup
        golden = root/'work/lab-images/golden/disk.qcow2'
        golden.parent.mkdir(parents=True)
        golden.write_text('golden')
        run = root/'work/lab-runs/n12345678'
        run.mkdir(parents=True)
        (run/'disk.qcow2').write_text('overlay')
        nightly.save(run/'state.json', dict(schemaVersion=1, overlay=str(run/'disk.qcow2'), golden=str(golden), id='n12345678'))
        out = root/'row'
        attempt = out/'idle_memory-1'
        attempt.mkdir(parents=True)
        (attempt/'receipt.json').write_text(json.dumps(dict(secret_scan=dict(passed=True), environment=dict(
            coverage_export=dict(published=True, scanned_before_copy=True, copy_scan_passed=True)))))
        (attempt/'coverage.json').write_text('{}')
        mark_terminal(attempt, 0)
        @contextmanager
        def image_lock(_disk):
            yield None
        stack = ExitStack()
        stack.enter_context(patch.object(overlay_cleanup, 'ROOT', root))
        stack.enter_context(patch.object(overlay_cleanup, '_image_lock', image_lock))
        stack.enter_context(patch.object(overlay_cleanup, '_check_chains'))
        stack.enter_context(patch.object(overlay_cleanup, '_lock_alive'))
        stack.enter_context(patch.object(lab, 'reset'))
        processes = stack.enter_context(patch.object(overlay_cleanup, '_check_processes'))
        return stack, run, out, processes

    def interrupt_after_prepared(self, run, out, processes):
        # The recovery deadline fires between the prepared receipt and the unlink.
        processes.side_effect = [None, nightly.NotRun('guest recovery deadline exhausted; explicit recovery required')]
        with self.assertRaisesRegex(nightly.NotRun, 'deadline'):
            nightly.retire(run, out, {'idle_memory-1': {'status': 'green'}})
        processes.side_effect = None
        receipt = json.loads((run/'overlay-cleanup.json').read_text())
        self.assertEqual(receipt['status'], 'prepared')
        self.assertTrue((run/'disk.qcow2').exists())
        return receipt

    def test_a_retirement_retried_after_an_interrupted_cleanup_finishes_from_its_own_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            stack, run, out, processes = self.retirement_fixture(root)
            with stack:
                # Interrupted after the archive, before any manifest names it.
                real_save = nightly.save
                def stopped(path, value):
                    if Path(path).name == 'completion.json':
                        raise nightly.NotRun('guest recovery deadline exhausted; explicit recovery required')
                    real_save(path, value)
                with patch.object(nightly, 'save', side_effect=stopped), self.assertRaises(nightly.NotRun):
                    nightly.retire(run, out, {'idle_memory-1': {'status': 'green'}})
                self.assertFalse((out/'completion-manifest.json').exists())
                # Interrupted before guarded cleanup prepared anything: the retry reuses the checked archive.
                processes.side_effect = nightly.NotRun('guest recovery deadline exhausted; explicit recovery required')
                with self.assertRaises(nightly.NotRun):
                    nightly.retire(run, out, {'idle_memory-1': {'status': 'green'}})
                self.assertFalse((run/'overlay-cleanup.json').exists())
                receipt = self.interrupt_after_prepared(run, out, processes)
                self.assertEqual(sorted(p.name for p in out.glob('attachments*.tgz')), ['attachments.tgz'])
                manifest = nightly.digest(out/'completion-manifest.json')
                self.assertEqual(receipt['evidence']['sha256'], manifest)
                # The retry (recovery passes no cells, so its own outcome would differ) writes no new evidence.
                nightly.retire(run, out, {})
                self.assertFalse((run/'disk.qcow2').exists())
                done = json.loads((run/'overlay-cleanup.json').read_text())
                self.assertEqual((done['status'], done['evidence']), ('completed', receipt['evidence']))
                self.assertEqual(nightly.digest(out/'completion-manifest.json'), manifest)
                self.assertEqual(json.loads((out/'retirement.json').read_text())['status'], 'completed')
                self.assertEqual(sorted(p.name for p in out.glob('attachments*.tgz')), ['attachments.tgz'])
                # Retrying a finished retirement only re-checks it.
                nightly.retire(run, out, {})
                self.assertEqual(json.loads((run/'overlay-cleanup.json').read_text())['status'], 'completed')

    def test_a_retry_puts_back_the_manifest_a_later_attempt_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            stack, run, out, processes = self.retirement_fixture(root)
            with stack:
                receipt = self.interrupt_after_prepared(run, out, processes)
                # An older controller (or the manual retire call) retried by writing new evidence over it.
                for name in ('completion.json', 'completion-manifest.json'):
                    nightly.keep_aside(out/name)
                nightly.save(out/'completion.json', dict(json.loads((out/'completion.json').read_text()), outcome='failed'))
                nightly.save(out/'completion-manifest.json', dict(json.loads((out/'completion.json').read_text()),
                             files={'completion.json': nightly.digest(out/'completion.json')}))
                nightly.retire(run, out, {})
                self.assertFalse((run/'disk.qcow2').exists())
                self.assertEqual(nightly.digest(out/'completion-manifest.json'), receipt['evidence']['sha256'])
                self.assertEqual(json.loads((out/'completion.json').read_text())['outcome'], 'passed')
                # The replaced files stay in the evidence.
                outcomes = sorted(json.loads(p.read_text())['outcome'] for p in out.glob('completion.json.before-recovery-*'))
                self.assertIn('failed', outcomes)

    def test_a_retry_never_reuses_an_archive_missing_its_gzip_footer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            stack, run, out, processes = self.retirement_fixture(root)
            with stack:
                processes.side_effect = nightly.NotRun('guest recovery deadline exhausted; explicit recovery required')
                with self.assertRaises(nightly.NotRun):
                    nightly.retire(run, out, {'idle_memory-1': {'status': 'green'}})
                (out/'completion-manifest.json').unlink()
                complete = (out/'attachments.tgz').read_bytes()
                (out/'attachments.tgz').write_bytes(complete[:-8])
                self.assertEqual(nightly.archived_files(out/'attachments.tgz'), nightly.attempt_files(out, [out/'idle_memory-1']))
                self.assertIsNone(nightly.earlier_archive(out, [out/'idle_memory-1']))
                # Nor one with a second gzip member appended after the tar.
                import gzip, io, tarfile as tar
                extra = io.BytesIO()
                with tar.open(fileobj=extra, mode='w') as stream:
                    info = tar.TarInfo('idle_memory-1/extra.txt')
                    info.size = 5
                    stream.addfile(info, io.BytesIO(b'extra'))
                (out/'attachments.tgz').write_bytes(complete+gzip.compress(extra.getvalue()))
                self.assertEqual(nightly.archived_files(out/'attachments.tgz'), nightly.attempt_files(out, [out/'idle_memory-1']))
                self.assertIsNone(nightly.earlier_archive(out, [out/'idle_memory-1']))
                (out/'attachments.tgz').write_bytes(complete)
                self.assertEqual(nightly.earlier_archive(out, [out/'idle_memory-1']), out/'attachments.tgz')

    def test_a_retry_never_puts_evidence_back_over_a_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            stack, run, out, processes = self.retirement_fixture(root)
            with stack:
                self.interrupt_after_prepared(run, out, processes)
                nightly.keep_aside(out/'completion-manifest.json')
                (out/'completion-manifest.json').unlink()
                (out/'completion-manifest.json').symlink_to(root/'elsewhere.json')
                with self.assertRaisesRegex(nightly.NotRun, 'not an owned single-link regular file'):
                    nightly.retire(run, out, {})
                self.assertTrue((out/'completion-manifest.json').is_symlink())
                self.assertTrue((run/'disk.qcow2').exists())

    def test_a_retry_refuses_a_manifest_or_member_changed_by_anything_else(self):
        from tools.lab import lab
        for change in ('manifest', 'member'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp).resolve()
                stack, run, out, processes = self.retirement_fixture(root)
                with stack:
                    receipt = self.interrupt_after_prepared(run, out, processes)
                    if change == 'manifest':
                        # Bytes no earlier attempt wrote: nothing in the evidence matches the receipt.
                        value = json.loads((out/'completion-manifest.json').read_text())
                        nightly.save(out/'completion-manifest.json', dict(value, outcome='passed', extra=True))
                        expected = (nightly.NotRun, 'names a completion manifest that is not in the evidence')
                    else:
                        with (out/'attachments.tgz').open('ab') as stream:
                            stream.write(b'tampered')
                        expected = (lab.LabError, 'Evidence member checksum mismatch')
                    before = sorted(p.name for p in out.iterdir())
                    with self.assertRaisesRegex(*expected):
                        nightly.retire(run, out, {})
                    with self.assertRaisesRegex(*expected):
                        nightly.finish_retirement(run, out)
                    self.assertTrue((run/'disk.qcow2').exists())
                    self.assertEqual(json.loads((run/'overlay-cleanup.json').read_text()), receipt)
                    self.assertEqual(sorted(p.name for p in out.iterdir()), before)

    def test_wait_does_not_outlive_worker_deadline(self):
        with tempfile.TemporaryDirectory() as tmp:
            clock = [0]
            def sleep(seconds): clock[0] += seconds
            with patch.object(nightly.Path, 'home', return_value=Path(tmp)), \
                    patch('tools.lab.desktop_lock.Desktop') as lock:
                lock.return_value.status.return_value = dict(state='held')
                with self.assertRaisesRegex(nightly.NotRun, 'awaiting host'):
                    nightly.await_host(dict(host='d69', rows=[{}], waitSeconds=1200), Path(tmp), {},
                                       clock=lambda:clock[0], sleep=sleep, worker_deadline=45)
            self.assertEqual(clock[0], 45)

    def test_worker_termination_and_deadline_unwind_and_restore_signal_handlers(self):
        for signum in (signal.SIGTERM, signal.SIGALRM):
            previous = {sig:signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGALRM)}
            cleaned = []
            with self.subTest(signal=signum), self.assertRaises(nightly.NotRun):
                with nightly.worker_deadline():
                    try:
                        os.kill(os.getpid(), signum)
                    finally:
                        self.assertEqual(signal.getitimer(signal.ITIMER_REAL)[0], 0)
                        cleaned.append(True)
            self.assertEqual(cleaned, [True])
            self.assertEqual({sig:signal.getsignal(sig) for sig in previous}, previous)

    def test_outer_timeout_terminates_then_kills_only_after_grace(self):
        for ignores_term in (False, True):
            with self.subTest(ignores_term=ignores_term), patch.object(nightly.subprocess, 'Popen') as popen:
                process = popen.return_value
                timeout = subprocess.TimeoutExpired('synthetic', 10)
                process.communicate.side_effect = [timeout, timeout, (None, None)] if ignores_term else [timeout, (None, None)]
                with self.assertRaises(subprocess.TimeoutExpired):
                    nightly.run_command(['synthetic'], env=None, stdout=None, stderr=None,
                                        timeout=10, terminate_grace=30)
                process.terminate.assert_called_once()
                self.assertEqual(process.kill.call_count, int(ignores_term))
                self.assertEqual(process.communicate.call_args_list[1].kwargs['timeout'], 30)

    @staticmethod
    def remote_inputs(root):
        (root/'identity.json').write_text(json.dumps(dict(driver=BUILD)))
        artifacts = root/'stage/artifacts'
        (artifacts/'packages').mkdir(parents=True)
        (artifacts/'packages/put gd.deb').touch()
        with nightly.tarfile.open(root/'inputs.tar', 'w') as stream:
            stream.add(artifacts, arcname='artifacts')
        return root/'inputs.tar'

    def test_unreachable_driver_is_grey_and_launches_no_worker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = self.remote_inputs(root)
            builder = dict(ssh=['ssh', 'builder'], root='/repo', config='/private',
                           rows=[dict(name='Row', live=False)])
            with patch.object(nightly, 'command', side_effect=[None, RuntimeError('git failed')]) as command:
                rows = nightly.run_host('d69', builder, 'vtest', root, archive, root, local_workers=Mock())
            self.assertEqual(command.call_count, 2)
            self.assertEqual({c['assertion'] for c in rows[0]['cells'].values()},
                             {'builder checkout cannot reach the controller driver'})

    def test_finished_run_deletes_exactly_what_it_unpacked(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = self.remote_inputs(root)
            with patch.object(nightly, 'command', side_effect=[None, None, None, None, '[]', None]) as command, \
                    patch.object(nightly.subprocess, 'run', return_value=SimpleNamespace(returncode=0)):
                nightly.run_host('d69', dict(ssh=['ssh', 'builder'], root='/repo', config='/private'),
                                 'vtest', root, archive, root, local_workers=Mock())
            # The remote shell removes exactly what was unpacked once its worker exits, keeping its status.
            worker = next(c.args[0][-1] for c in command.call_args_list if '--worker' in c.args[0][-1])
            inputs = '/repo/inputs/'+root.name
            cleanup = (f"rm -f -- '{inputs}/artifacts/packages/put gd.deb' && "
                       f'rmdir -- {inputs}/artifacts/packages {inputs}/artifacts {inputs}')
            self.assertTrue(worker.endswith('; status=$?; '+cleanup+'; exit $status'))
            self.assertNotIn('-r', cleanup.split())
            self.assertFalse(any(c.args[0][-1].startswith('rm ') for c in command.call_args_list))

    def test_a_recheck_runs_and_accepts_only_the_selected_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = self.remote_inputs(root)
            builder = dict(ssh=['ssh', 'builder'], root='/repo', config='/private',
                           rows=[dict(name='Budgie', desktop='fedora-budgie-wayland', live=False)])
            # The worker returned a row the controller did not select (and has no package for).
            other = json.dumps([dict(desktop='Ubuntu GNOME', cells={})])
            with patch.object(nightly, 'command', side_effect=[None, None, None, None, other, None]) as command, \
                    patch.object(nightly.subprocess, 'run', return_value=SimpleNamespace(returncode=0)):
                rows = nightly.run_host('d69', builder, 'vtest', root, archive, root, local_workers=Mock())
            worker = next(c.args[0][-1] for c in command.call_args_list if '--worker' in c.args[0][-1])
            self.assertIn('--row Budgie', worker)
            self.assertEqual([r['desktop'] for r in rows], ['Budgie'])
            self.assertIn('other rows than the controller selected', rows[0]['notRunReason'])

    def test_a_worker_that_stops_after_its_first_row_still_reports_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = self.remote_inputs(root)
            builder = dict(ssh=['ssh', 'builder'], root='/repo', config='/private',
                           rows=[dict(name='Ubuntu GNOME', desktop='ubuntu-gnome-wayland'),
                                 dict(name='Budgie', desktop='fedora-budgie-wayland')])
            saved = json.dumps([dict(desktop='Ubuntu GNOME', cells={'idle_memory': nightly.cell('green')})])
            with patch.object(nightly, 'command', side_effect=[None, None, None, subprocess.TimeoutExpired('ssh', 1),
                                                               saved, None]) as command, \
                    patch.object(nightly.subprocess, 'run', return_value=SimpleNamespace(returncode=0)):
                rows = nightly.run_host('d69', builder, 'vtest', root, archive, root, local_workers=Mock())
            self.assertEqual([r['desktop'] for r in rows], ['Ubuntu GNOME', 'Budgie'])
            self.assertEqual(rows[0]['cells']['idle_memory']['status'], 'green')
            self.assertIn('worker timed out', rows[1]['notRunReason'])
            # The controller never deletes the inputs itself: a dropped connection may hide a running worker.
            self.assertFalse(any(c.args[0][-1].startswith('rm ') for c in command.call_args_list))

    def test_a_stop_request_starts_no_further_row(self):
        with tempfile.TemporaryDirectory() as tmp:
            rows = [dict(name='Ubuntu GNOME', desktop='ubuntu-gnome-wayland', profile='gnome-wayland'),
                    dict(name='Budgie', desktop='fedora-budgie-wayland', profile='fedora45-budgie')]
            identity = dict(driver='d'*40, gitCommit='a'*40)
            with patch.object(nightly, 'command', side_effect=lambda argv, **kw: identity['driver'] if 'rev-parse' in argv else None), \
                    patch.object(nightly, 'lease', side_effect=AssertionError('a lease was taken')), \
                    patch.object(nightly, 'recover_stale_guest', side_effect=AssertionError('a guest was touched')), \
                    patch.dict(nightly.STOP, reason='worker terminated; cleanup required'):
                result = nightly.worker_run(dict(host='d69', rows=rows), 'vtest', identity, Path(tmp), Path(tmp)/'out',
                                            nightly.time.monotonic()+600)
            self.assertEqual([r['desktop'] for r in result], ['Ubuntu GNOME', 'Budgie'])
            self.assertTrue(all('row not started: worker terminated' in r['notRunReason'] for r in result))
            self.assertEqual(json.loads((Path(tmp)/'out/results.json').read_text())[1]['desktop'], 'Budgie')

    def test_failed_cleanup_keeps_the_workers_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = self.remote_inputs(root)
            with patch.object(nightly, 'command', side_effect=[None, None, None, None, '[]']) as command, \
                    patch.object(nightly.subprocess, 'run', return_value=SimpleNamespace(returncode=0)):
                nightly.run_host('d69', dict(ssh=['ssh', 'builder'], root='/repo', config='/private'),
                                 'vtest', root, archive, root, local_workers=Mock())
            line = next(c.args[0][-1] for c in command.call_args_list if '--worker' in c.args[0][-1])
            worker, _, rest = line.partition('; status=$?; ')
            self.assertTrue(rest.endswith('; exit $status'))
            # Run the generated line with a worker that exits 3 and a cleanup step that fails.
            shell = 'sh -c "exit 3"; status=$?; ' + rest.replace(rest[:-len('; exit $status')], 'false')
            self.assertEqual(subprocess.run(['sh', '-c', shell]).returncode, 3)

    def test_remote_worker_has_its_own_watchdog(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = self.remote_inputs(root)
            with patch.object(nightly, 'command', side_effect=[None, None, None, None, '[]', None]) as command, \
                    patch.object(nightly.subprocess, 'run', return_value=SimpleNamespace(returncode=0)):
                nightly.run_host('d69', dict(ssh=['ssh', 'builder'], root='/repo', config='/private'),
                                 'vtest', root, archive, root, local_workers=Mock())
            args = command.call_args_list[3]
            self.assertIn('timeout --foreground --signal=TERM --kill-after=300 18000', args.args[0][-1])
            self.assertEqual(args.kwargs['terminate_grace'], nightly.TERMINATE_GRACE)
            self.assertIsNone(args.kwargs['local_workers'])

    def test_local_dispatch_registers_worker_for_controller_shutdown(self):
        workers = nightly.LocalWorkers()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with patch.object(nightly, 'command') as command:
                nightly.run_host('d69', dict(root='/repo', config='/private', rows=[]),
                                 'vtest', root, root/'unused.tar', root, local_workers=workers)
            self.assertIs(command.call_args.kwargs['local_workers'], workers)

    def test_only_local_worker_receives_controller_proof(self):
        context = dict(pid=123, start='456', ssh=[['ssh', 'other-host']])
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with patch.object(nightly, 'command') as command:
                nightly.run_host('d69', dict(root='/repo', config='/private', rows=[]),
                                 'vtest', root, root/'unused.tar', root, controller=context)
            env = command.call_args.kwargs['env']
            self.assertEqual(json.loads(env['PUTGD_NIGHTLY_CONTROLLER']), context)

    def test_controller_stop_bounds_grace_and_rejects_late_local_spawns(self):
        workers = nightly.LocalWorkers()
        process = Mock()
        process.wait.side_effect = [subprocess.TimeoutExpired('synthetic', 300), 0]
        with patch.object(nightly.subprocess, 'Popen', return_value=process):
            self.assertIs(workers.start(['synthetic']), process)
            workers.stop()
            process.terminate.assert_called_once()
            process.kill.assert_called_once()
            self.assertLessEqual(process.wait.call_args_list[0].kwargs['timeout'], nightly.TERMINATE_GRACE)
            with self.assertRaisesRegex(nightly.NotRun, 'controller stopping'):
                workers.start(['late synthetic'])
        service = (nightly.ROOT/'tools/lab/putgd-nightly-desktops.service').read_text()
        self.assertIn('KillMode=mixed', service)
        stop_seconds = int(next(line.split('=')[1][:-3] for line in service.splitlines()
                                if line.startswith('TimeoutStopSec=') and line.endswith('min'))) * 60
        self.assertGreaterEqual(stop_seconds, nightly.TERMINATE_GRACE + 30)

    def test_controller_sigterm_waits_for_real_local_worker_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            clean = Path(tmp)/'cleaned'
            worker_code = f'''from tools.lab import nightly_desktops as n
import os, signal
from pathlib import Path
try:
    with n.worker_deadline():
        print('READY '+str(os.getpid()), flush=True)
        signal.pause()
finally:
    Path({str(clean)!r}).write_text('cleanup completed')
'''
            controller_code = f'''from tools.lab import nightly_desktops as n
from concurrent.futures import ThreadPoolExecutor
import sys
workers = n.LocalWorkers()
with n.controller_shutdown(workers):
    with ThreadPoolExecutor() as pool:
        pool.submit(n.run_command, [sys.executable, '-c', {worker_code!r}],
                    env=None, stdout=None, stderr=None, timeout=60,
                    terminate_grace=5, local_workers=workers).result()
'''
            with subprocess.Popen([sys.executable, '-c', controller_code], cwd=nightly.ROOT,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True) as process:
                child_fd = None
                try:
                    # Bound the readiness read as well as shutdown if startup regresses.
                    import select
                    self.assertTrue(select.select([process.stdout], [], [], 10)[0])
                    ready = process.stdout.readline().split()
                    self.assertEqual(ready[0], 'READY')
                    child_fd = os.pidfd_open(int(ready[1]))
                    process.terminate()
                    process.communicate(timeout=10)
                    self.assertEqual(process.returncode, 128+signal.SIGTERM)
                    self.assertEqual(clean.read_text(), 'cleanup completed')
                finally:
                    if process.poll() is None:
                        process.kill()
                    if child_fd is not None:
                        try:
                            signal.pidfd_send_signal(child_fd, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        os.close(child_fd)
    def test_candidate_packages_keep_original_ci_names_and_release_names_still_work(self):
        root = Path('/private/artifacts')
        row = dict(asset='ubuntu26.04-x86_64.deb')
        candidate = dict(packageFiles={row['asset']: 'putgd-26.1031.1-dev.7-ubuntu26.04-x86_64.deb'})
        self.assertEqual(nightly.package_path(root, 'candidate-deps-20261031', row, candidate),
                         root/'packages'/candidate['packageFiles'][row['asset']])
        self.assertEqual(nightly.package_path(root, 'v26.1031.1-nightly', row, {}),
                         root/'packages/putgd-26.1031.1-nightly-ubuntu26.04-x86_64.deb')
        for name in ('../outside.deb', '/outside.deb', '', '.', '..'):
            with self.subTest(name=name), self.assertRaises(nightly.NotRun):
                nightly.package_path(root, 'candidate', row, dict(packageFiles={row['asset']: name}))

    def test_fedora_activation_still_refuses_a_covered_target(self):
        from test_surface_input_guards import context
        from tools.lab.surface_driver.api import RecipeFailed
        ctx, directory = context({'session':'wayland', 'compositor':{'kind':'gnome-lab-witness'},
                                  'focus':{'raise_before_input':True}})
        self.addCleanup(directory.cleanup)
        ctx._probe = Mock(return_value={'ok':False, 'obscuring_window':'Foreign confirmation', 'active':42})
        with self.assertRaisesRegex(RecipeFailed, 'Foreign confirmation'):
            ctx.click_at(25,35)
        self.assertTrue(ctx._probe.call_args.args[1]['raise'])
        ctx.target.pointer.assert_not_called()

    def catalog_run(self, *, staged, conclusion, ci, status_filter):
        class Runs:
            repo = 'owner/repo'
            def __init__(self):
                self.downloaded = False
            def api(self, path):
                if '/jobs' in path:
                    return dict(jobs=[dict(name='ci / packages / sbom', conclusion='success'),
                                      dict(name='ci / CI', conclusion=ci)])
                if '/artifacts' in path:
                    return dict(artifacts=[dict(name='ui-catalog', expired=False)])
                status_filter.append('status=success' in path)
                return dict(workflow_runs=[dict(id=7, conclusion=conclusion)])
            def gh(self, *args):
                self.downloaded = True
                (Path(args[args.index('--dir')+1])/'manifest.json').write_text(
                    json.dumps(dict(build=dict(gitCommit=BUILD, dirty=False))))
        with tempfile.TemporaryDirectory() as tmp:
            github = Runs()
            return nightly.catalog(github, BUILD, Path(tmp), staged=staged)['runId'], github.downloaded

    def test_stable_draft_catalog_accepts_a_failed_release_run_only_when_its_ci_passed(self):
        filters = []
        self.assertEqual(self.catalog_run(staged=True, conclusion='failure', ci='success', status_filter=filters), (7, True))
        self.assertEqual(filters, [False])
        with self.assertRaisesRegex(RuntimeError, 'no exact-build UI catalog'):
            self.catalog_run(staged=True, conclusion='failure', ci='failure', status_filter=[])
        with self.assertRaisesRegex(RuntimeError, 'no exact-build UI catalog'):
            self.catalog_run(staged=True, conclusion=None, ci='cancelled', status_filter=[])

    def test_nightly_catalog_still_requires_a_successful_run(self):
        filters = []
        self.assertEqual(self.catalog_run(staged=False, conclusion='success', ci='success', status_filter=filters), (7, True))
        self.assertEqual(filters, [True])
        with self.assertRaisesRegex(RuntimeError, 'no exact-build UI catalog'):
            self.catalog_run(staged=False, conclusion='failure', ci='success', status_filter=[])

    def rpm_install(self, root, *, failure=False, desktop_changed=False, verify_failure=False):
        from tools.lab import nightly_desktop_guest as guest
        run, out, package = root/'run', root/'out', root/'published.rpm'
        run.mkdir(); out.mkdir(); package.write_bytes(b'published RPM test input')
        (run/'package.json').write_text('{"stale":true}')
        before = ('gnome-shell.x86_64 0:50.0-1.fc44\nqt6-qtbase.x86_64 0:6.10.2-2.fc44\n'
                  'kernel-core.x86_64 0:6.19.0-1.fc44\nkernel-core.x86_64 0:6.19.1-1.fc44\n')
        after = before.replace('6.10.2-2', '6.11.2-3')+'putgd.x86_64 0:26.1031.3384~nightly-1.fc44\n'
        if desktop_changed:
            after = after.replace('50.0-1', '50.5-1')
        with patch.object(guest.lab, 'verify_owned_guest'), patch.object(guest.lab, 'ssh', side_effect=[before, after]), \
                patch.object(guest.lab, 'put'), patch.object(guest.lab, 'ssh_args', return_value=['ssh']), \
                patch.object(guest.lab, 'command', return_value=SimpleNamespace(
                    returncode=1 if failure else 0, stdout='private transaction', stderr='solver diagnostic')) as command, \
                patch.object(guest.lab, 'verify_installed_binaries', side_effect=RuntimeError('binary mismatch')
                             if verify_failure else None, return_value={'/usr/bin/putgd': 'a'*64}) as verify:
            if failure or desktop_changed or verify_failure:
                with self.assertRaises(RuntimeError):
                    guest.install_rpm(run, package, out)
                self.assertFalse((run/'package.json').exists())
                if not verify_failure:
                    verify.assert_not_called()
            else:
                receipt = guest.install_rpm(run, package, out)
                self.assertEqual(receipt['installedBinarySha256'], {'/usr/bin/putgd': 'a'*64})
                self.assertEqual(json.loads((run/'package.json').read_text()), receipt)
                self.assertEqual([v['package'] for v in receipt['rpmTransaction']['packagesChanged']],
                                 ['putgd.x86_64', 'qt6-qtbase.x86_64'])
            script = command.call_args.kwargs['input']
            self.assertIn('sudo -n dnf ', script)
            self.assertNotIn('sudo -n rpm', script)
            for protected in guest.FEDORA_DESKTOP:
                self.assertIn(protected, script)
            for probe in guest.FEDORA_TOOLS.split():
                self.assertIn(probe, script)
            inventory = json.loads((out/'rpm-before.json').read_text())
            self.assertEqual(len(inventory['kernel-core.x86_64']), 2)
            self.assertIn('solver diagnostic', (out/'rpm-install.txt').read_text())

    def test_fedora_gnome_gets_the_appindicator_tray_a_user_installs_and_nothing_else_moves(self):
        from tools.lab import nightly_desktop_guest as guest
        before = 'gnome-shell.x86_64 0:50.0-1.fc44\nmutter.x86_64 0:50.0-1.fc44\nputgd.x86_64 0:26.1120.1-1.fc44\n'
        added = before + ('gnome-shell-extension-appindicator.noarch 0:64-1.fc44\n'
                          'libappindicator-gtk3.x86_64 0:12.10.1-10.fc44\n')
        cases = {'installed': (added, 0, None), 'failed': (before, 1, 'could not be installed'),
                 'shell moved': (added.replace('gnome-shell.x86_64 0:50.0', 'gnome-shell.x86_64 0:50.6'), 0, 'changed the desktop'),
                 'upgrade': (added.replace('putgd.x86_64 0:26.1120.1', 'putgd.x86_64 0:26.1120.2'), 0, 'changed the desktop')}
        for name, (after, code, error) in cases.items():
            with self.subTest(name), tempfile.TemporaryDirectory() as tmp, \
                    patch.object(guest.lab, 'verify_owned_guest'), patch.object(guest.lab, 'ssh', side_effect=[before, after]), \
                    patch.object(guest.lab, 'ssh_args', return_value=['ssh']), \
                    patch.object(guest.lab, 'command', return_value=SimpleNamespace(returncode=code, stdout='', stderr='')) as command:
                if error:
                    with self.assertRaisesRegex(RuntimeError, error):
                        guest.install_tray_host(Path(tmp), Path(tmp))
                else:
                    report = guest.install_tray_host(Path(tmp), Path(tmp))
                    self.assertTrue(report['installed'] and report['desktopUnchanged'] and report['nothingUpgraded'])
                script = command.call_args.kwargs['input']
                # Through the guest's passwordless dnf; the Shell, Mutter, GDM and portals are held where they are.
                self.assertIn('sudo -n dnf ', script)
                self.assertIn(' install -y gnome-shell-extension-appindicator', script)
                for held in ('gnome-shell,', 'mutter*', 'gdm*', 'xdg-desktop-portal*'):
                    self.assertIn(held, script)
                self.assertNotIn('gnome-shell*', script)
        # The check after the Shell restart enables it and waits for a registered StatusNotifier host.
        self.assertIn("'gnome-extensions','enable',uuid", guest.TRAY_HOST_READY)
        self.assertIn('IsStatusNotifierHostRegistered', guest.TRAY_HOST_READY)
        from tools.lab.gnome_tray_host import TRAY_HOST_EXTENSION
        self.assertIn(TRAY_HOST_EXTENSION, guest.TRAY_HOST_READY)

    def test_guest_package_updaters_are_held_or_the_row_stops(self):
        from tools.lab import nightly_desktop_guest as guest
        def reply(**override):
            rows = {unit: 'not-found not-found inactive drained' for unit in guest.UPDATE_UNITS}
            rows.update({'apt-daily-upgrade.timer': 'loaded masked inactive drained',
                         'apt-daily-upgrade.service': 'loaded masked inactive drained'}, **override)
            return ''.join(f'{unit} {value}\n' for unit, value in rows.items())
        with tempfile.TemporaryDirectory() as tmp, patch.object(guest.lab, 'verify_owned_guest'):
            out = Path(tmp)
            with patch.object(guest.lab, 'ssh', return_value=reply()) as ssh:
                report = guest.hold_package_updates(out, out)
            self.assertEqual(report['held'], ['apt-daily-upgrade.service', 'apt-daily-upgrade.timer'])
            self.assertEqual(json.loads((out/'package-updates.json').read_text())['notHeld'], [])
            script = ssh.call_args.args[1]
            # Root comes only from the goldens' passwordless apt-get, which must neither fetch, wait for the
            # lists lock a running apt-daily holds, nor read the script on stdin; no guest password is used.
            for option in ('sudo -n /usr/bin/apt-get', 'Debug::NoLocking=1', 'Dir::Etc::sourcelist=/dev/null',
                           'APT::Update::Pre-Invoke::=sh $1', '< /dev/null'):
                self.assertIn(option, script)
            self.assertNotIn('putgd-lab', script)
            # Only timers are stopped. An updater service is never stopped (apt's KillMode=process leaves
            # unattended-upgrade running; Mint's would lose dpkg mid-way): it is masked once it has drained.
            services = script[script.index("<<'NIGHTLY_SERVICES'"):]
            self.assertNotIn('systemctl stop', services)
            self.assertIn('idle "$unit"; then', services)
            self.assertIn('hold=$(mktemp -d) || exit 1', script)
            self.assertNotIn('--now', script)
            # Whatever the unit looked like before, it must end absent or masked, not running and drained;
            # a unit the guest did not report stops the row too.
            for broken in (reply(**{'apt-daily-upgrade.service': 'loaded masked deactivating drained'}),
                           reply(**{'apt-daily.timer': 'loaded loaded active drained'}),
                           reply(**{'apt-daily-upgrade.service': 'masked masked activating busy'}),
                           reply(**{'apt-daily-upgrade.service': 'loaded loaded inactive busy'}),
                           reply(**{'apt-daily-upgrade.service': 'masked masked inactive busy'}),
                           reply(**{'apt-daily.service': 'unknown loaded active busy'}),
                           reply().replace('apt-daily.service not-found not-found inactive drained\n', '')):
                with self.subTest(broken=broken[:40]), patch.object(guest.lab, 'ssh', return_value=broken):
                    with self.assertRaisesRegex(RuntimeError, 'could not be held'):
                        guest.hold_package_updates(out, out)
            # A lost guest still leaves the report that says why setup stopped.
            with patch.object(guest.lab, 'ssh', side_effect=guest.lab.LabError('ssh failed (exit 255)')):
                with self.assertRaises(guest.lab.LabError):
                    guest.hold_package_updates(out, out)
            report = json.loads((out/'package-updates.json').read_text())
            self.assertEqual((report['error'], len(report['notHeld'])), ('LabError', len(guest.UPDATE_UNITS)))

    def test_an_updater_counts_as_idle_only_on_positive_evidence(self):
        from tools.lab import nightly_desktop_guest as guest
        if not Path('/sys/fs/cgroup/cgroup.controllers').exists():
            self.skipTest('needs a cgroup v2 host')
        unit = 'putgd-test-absent-'+uuid.uuid4().hex+'.service'
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp)/'systemctl'
            listing = Path(tmp)/'ls'
            def idle(jobs_exit, ls_exit=None):
                fake.write_text('#!/bin/sh\ncase "$1" in show) echo inactive;; list-jobs) exit %d;; esac\n' % jobs_exit)
                fake.chmod(0o700)
                listing.unlink(missing_ok=True)
                if ls_exit is not None:
                    listing.write_text('#!/bin/sh\nexit %d\n' % ls_exit)
                    listing.chmod(0o700)
                return subprocess.run(['sh', '-c', guest._IDLE+'idle "$1"', 'sh', unit],
                                      env=dict(os.environ, PATH=tmp+':'+os.environ['PATH'])).returncode
            # A job list or a cgroup listing that could not be read is not an empty queue or cgroup.
            self.assertEqual((idle(0), idle(1), idle(0, ls_exit=2)), (0, 1, 1))

    def test_budgie_window_rules_put_each_dialog_where_the_witness_looks(self):
        from tools.lab import nightly_desktop_guest as guest
        from tools.lab.surface_driver import probe
        from tools.lab.surface_driver.profiles import load_profile
        compositor = load_profile('fedora-budgie-wayland')['compositor']
        rules = compositor['window_rules']
        text = ('<labwc_config version="5"><windowRules><windowRule identifier="nemo-desktop" title="Desktop">'
                '<action name="ToggleAlwaysOnBottom" /></windowRule></windowRules></labwc_config>')
        merged = guest.merge_labwc_rules(text, rules)
        self.assertEqual(guest.merge_labwc_rules(merged, rules), merged)  # a rerun replaces, never adds
        import xml.etree.ElementTree as ET
        written = {(e.get('identifier'), e.get('title')): e for e in ET.fromstring(merged).iter('windowRule')}
        self.assertIn(('nemo-desktop', 'Desktop'), written)
        for rule in rules:
            element = written[(rule['app_id'], rule['title'])]
            # No decoration, so the rule's position is the content's: the rectangle the witness checks.
            self.assertEqual(element.get('serverDecoration'), 'no')
            move = element.find('action')
            self.assertEqual([move.get('name'), int(move.get('x')), int(move.get('y'))], ['MoveTo', *rule['position']])
            self.assertEqual(probe.rule_rect(compositor, rule['title'], rule['app_id'], [10, 20]), [*rule['position'], 10, 20])
        with self.assertRaises(ValueError):
            guest.merge_labwc_rules(text, [dict(rules[0], title='Delete*')])
        # A bad rule is refused on the host, before the guest's bridge is stopped.
        with patch.object(guest.lab, 'verify_owned_guest'), patch.object(guest.lab, 'guest_session') as session:
            with self.assertRaises(ValueError):
                guest.prepare_labwc_rules('run', 'out', [dict(rules[0], title='Delete*')])
        session.assert_not_called()

    def test_fedora_install_resolves_runtime_and_records_exact_binary_and_package_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.rpm_install(Path(tmp))

    def test_fedora_failed_transaction_cannot_certify_a_previous_install(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.rpm_install(Path(tmp), failure=True)

    def test_fedora_desktop_change_or_binary_mismatch_blocks_install_receipt(self):
        for kwargs in (dict(desktop_changed=True), dict(verify_failure=True)):
            with self.subTest(**kwargs), tempfile.TemporaryDirectory() as tmp:
                self.rpm_install(Path(tmp), **kwargs)

    def test_filtered_run_preserves_other_desktops_and_exact_build(self):
        builders = {host: dict(rows=[dict(name='Aurora' if host == 'aurora-amd' else host,
                                         desktop=host+'-profile')]) for host in nightly.HOSTS}
        self.assertEqual(list(nightly.selected_builders(builders, ['Aurora'])), ['aurora-amd'])
        self.assertEqual(list(nightly.selected_builders(builders, ['aurora-amd-profile'])), ['aurora-amd'])
        with self.assertRaises(RuntimeError):
            nightly.selected_builders(builders, ['typo'])
        previous = dict(release='tag', build=BUILD, desktops=[dict(host=h, desktop=h, cells={'old': h}) for h in builders])
        result = dict(release='tag', build=BUILD, desktops=[dict(host='aurora-amd', desktop='aurora-amd', cells={'new': True})])
        merged = nightly.merge_desktops(previous, result)
        self.assertIs(merged[0], previous['desktops'][0])
        self.assertIs(merged[3], result['desktops'][0])
        with self.assertRaises(RuntimeError):
            nightly.merge_desktops(previous, dict(result, build='b'*40))

    def test_a_builder_with_two_rows_rechecks_only_the_named_row(self):
        builders = {host: dict(rows=[dict(name=host, desktop=host+'-profile')]) for host in nightly.HOSTS}
        builders['d69']['rows'].append(dict(name='Budgie', desktop='fedora-budgie-wayland'))
        only = nightly.selected_builders(builders, ['Budgie'])
        self.assertEqual(list(only), ['d69'])
        self.assertEqual([r['name'] for r in only['d69']['rows']], ['Budgie'])
        self.assertEqual(len(builders['d69']['rows']), 2)  # The configuration itself is untouched.
        both = nightly.selected_builders(builders, ['d69'])
        self.assertEqual([r['name'] for r in both['d69']['rows']], ['d69', 'Budgie'])
        previous = dict(release='tag', build=BUILD, desktops=[dict(host='d69', desktop='d69', cells={}),
                                                              dict(host='d69', desktop='Budgie', cells={})])
        result = dict(release='tag', build=BUILD, desktops=[dict(host='d69', desktop='Budgie', cells={'new': 1})])
        merged = nightly.merge_desktops(previous, result)
        self.assertIs(merged[0], previous['desktops'][0])
        self.assertIs(merged[1], result['desktops'][0])

    def test_unsupported_cells_are_neither_red_nor_run(self):
        with patch.dict(nightly.UNSUPPORTED, {('budgie', 'window_recording'): 'matrix: not offered'}):
            cells = nightly.row_cells(dict(desktop='budgie', live=True), 'setup')
            self.assertEqual(cells['window_recording']['status'], 'unsupported')
            self.assertFalse(cells['window_recording']['green'])
            self.assertTrue(nightly.passed(cells['window_recording']))
            self.assertEqual(cells['region_upload']['status'], 'not_run')
            self.assertEqual(set(cells), set(nightly.FLOWS))
            row = dict(desktop='Budgie', host='d69', cells=cells)
            self.assertIn('➖', nightly.table(dict(desktops=[row])))
        self.assertEqual(set(nightly.row_cells(dict(desktop='x'), 'setup')), set(nightly.LOCAL))
        no_proton = nightly.row_cells(dict(desktop='x', live=True, proton=False), 'setup')
        self.assertEqual(set(no_proton), set(nightly.FLOWS) - {'proton_region_upload'})

    def test_guest_service_selection_is_explicit_and_never_changes_host_default(self):
        from tools.lab import lab, lab_keyring
        self.assertEqual(lab.guest_secret_prefix({}), '')
        prefix = lab.guest_secret_prefix({'secret_service': ':1.123'})
        self.assertIn('SECRET_SERVICE_BUS_NAME=:1.123', prefix)
        self.assertIn('QTKEYCHAIN_BACKEND=libsecret', prefix)
        for name in ('org.freedesktop.secrets', ':1.2\nexec bad', 123):
            with self.assertRaises(lab.LabError):
                lab.guest_secret_prefix({'secret_service': name})
        from types import SimpleNamespace
        calls = []
        def run(argv, **kwargs):
            calls.append(argv)
            return SimpleNamespace(returncode=0, stdout='o "/"' if 'ReadAlias' in argv else '')
        with patch.dict('os.environ', {'SECRET_SERVICE_BUS_NAME': ':1.123'}):
            lab_keyring.secret_service(run)
        self.assertTrue(all(':1.123' in call for call in calls))

    def test_filtered_publication_keeps_existing_receipt_links_and_does_not_close(self):
        failure=dict(number=2,title='nightly: Aurora failing',state='open')
        github=FakeGitHub(failure)
        prior_row=dict(desktop='Mint',host='m74',evidence='/old',cells={
            'region_upload':dict(nightly.cell('green'),receiptUrl='https://example.test/old')})
        row=dict(desktop='Aurora',host='aurora-amd',evidence='/new',cells={'region_upload':nightly.cell('green')})
        previous=dict(release='tag',build=BUILD,desktops=[prior_row,dict(row)])
        result=dict(release='tag',build=BUILD,date='today',desktops=[row])
        with tempfile.TemporaryDirectory() as tmp:
            nightly.publish(github,result,Path(tmp),previous=previous)
            self.assertIn('https://example.test/old',(Path(tmp)/'results.md').read_text())
        self.assertEqual(len(result['desktops']),2)
        self.assertEqual(sum(w[0]=='POST' and w[1].endswith('/1/comments') for w in github.writes),1)
        self.assertFalse(any(w[0]=='PATCH' for w in github.writes))

    def receipt(self, root, *, functional='passed', review=True, cleanup=None, note='pixel oracle mismatch', recipe='outcomes.region_upload'):
        value = dict(environment=dict(build=BUILD, observed_build=dict(gitCommit=BUILD,dirty=False),
                coverage_export=dict(published=True,scanned_before_copy=True,copy_scan_passed=True),
                cleanup_errors=cleanup), secret_scan=dict(passed=True),
            interactions=[dict(recipe=recipe,functional=functional,review=review,visual='frames',note=note)])
        (root/'receipt.json').write_text(json.dumps(value))
        (root/'coverage.json').write_text('{}')
        mark_terminal(root, 0 if functional == 'passed' else 2)

    def test_review_does_not_gate_automatic_oracles(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.receipt(Path(tmp))
            self.assertEqual(nightly.verdict(tmp,'outcomes.region_upload')['status'],'green')

    def test_only_verified_app_oracle_failure_is_red(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.receipt(root,functional='failed')
            self.assertEqual(nightly.verdict(root,'outcomes.region_upload')['status'],'red')
            for kwargs in (dict(functional='blocked'),dict(cleanup=['owned process retained']),
                           dict(functional='failed',note='driver error: OSError'),
                           dict(functional='failed',note='recipe deadline exceeded')):
                self.receipt(root,**kwargs)
                self.assertEqual(nightly.verdict(root,'outcomes.region_upload')['status'],'not_run')
            self.receipt(root)
            self.assertEqual(nightly.verdict(root,'outcomes.history_delete')['status'],'not_run')
            (root/'receipt.json').write_text('{}')
            self.assertEqual(nightly.verdict(root,'outcomes.region_upload')['status'],'not_run')

    def test_unverified_installed_build_is_grey(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);self.receipt(root,functional='failed')
            value=json.loads((root/'receipt.json').read_text())
            value['environment']['observed_build']['gitCommit']='b'*40
            (root/'receipt.json').write_text(json.dumps(value));mark_terminal(root,2)
            self.assertEqual(nightly.verdict(root,'outcomes.region_upload')['status'],'not_run')

    def test_overview_or_missing_focus_refuses_input_without_app_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for note in ("input target 'outcome-pattern' is obscured by 'no focused window' (window None)",
                         "uploaded/local sha256=verified; input target 'history' is obscured by 'shell overview' (window None)",
                         "desktop input is obscured by 'shell overview'"):
                with self.subTest(note=note):
                    self.receipt(root, functional='failed', note=note)
                    self.assertEqual(nightly.verdict(root, 'outcomes.region_upload')['status'], 'not_run')
            for note in ("pixel mismatch; no focused window", "resident recording.prepare: recording_unavailable",
                         "input target 'editor' is obscured by 'put.gd confirmation' (window 42)"):
                with self.subTest(note=note):
                    self.receipt(root, functional='failed', note=note)
                    self.assertEqual(nightly.verdict(root, 'outcomes.region_upload')['status'], 'red')

    def test_daily_skips_tested_latest_release_and_now_forces_it(self):
        releases=[dict(tag_name=f'v26.1031.{i}-nightly',draft=False,published_at=f'2026-10-31T0{i}:00:00Z') for i in (1,2,3)]
        latest=releases[-1]['tag_name']
        self.assertEqual(nightly.select_release(releases)['tag_name'],latest)
        self.assertIsNone(nightly.select_release(releases,[latest]))
        self.assertEqual(nightly.select_release(releases,[latest],True)['tag_name'],latest)
        timer=(nightly.ROOT/'tools/lab/putgd-nightly-desktops.timer').read_text()
        self.assertIn('OnCalendar=*-*-* 12:30:00 UTC',timer)
        self.assertNotIn('*:00,30',timer)

    def test_all_five_hosts_are_dispatched_concurrently(self):
        barrier=threading.Barrier(5)
        seen=[]
        def dispatch(host,builder,*args,**kwargs):
            seen.append(host);barrier.wait(timeout=3)
            return [dict(desktop=host,host=host,cells={},totalSeconds=0)]
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);artifacts=root/'artifacts';artifacts.mkdir()
            result=dict(desktops=[])
            nightly.run_hosts({h:dict(rows=[{}]) for h in nightly.HOSTS},'tag',artifacts,root,result,dispatch)
            self.assertCountEqual(seen,nightly.HOSTS)
            self.assertEqual(len(result['desktops']),5)

    def test_one_grouped_failure_issue_per_desktop_grey_keeps_it_and_full_green_closes(self):
        failure=dict(number=2,title='nightly: Cinnamon failing',state='open')
        github=FakeGitHub(failure)
        row=dict(desktop='Cinnamon',host='m74',evidence='/owned/run',cells={
            'region_upload':nightly.cell('red','pixel mismatch'),
            'history_delete':nightly.cell('red','object still exists'),
            'link_browser':nightly.cell(assertion='browser unavailable')})
        result=dict(release='v26.1031.3384-nightly',build=BUILD,date='2026-10-31',desktops=[row])
        with tempfile.TemporaryDirectory() as tmp:
            nightly.publish(github,result,Path(tmp))
            updates=[w for w in github.writes if w[0]=='issue' and w[1]==failure['title']]
            self.assertEqual(len(updates),1);self.assertIs(updates[0][2],failure)
            self.assertIn('Region',updates[0][3]);self.assertIn('History delete',updates[0][3])
            github.writes.clear()
            for v in row['cells'].values():v.update(status='not_run')
            nightly.publish(github,result,Path(tmp))
            self.assertFalse(any(w[0]=='issue' and w[1]==failure['title'] for w in github.writes))
            self.assertFalse(any(w[0]=='PATCH' for w in github.writes))
            github.writes.clear()
            for v in row['cells'].values():v.update(status='green')
            nightly.publish(github,result,Path(tmp))
            self.assertIn(('PATCH','repos/owner/repo/issues/2',dict(state='closed')),github.writes)

    def test_grey_infrastructure_creates_no_failure_issue(self):
        github=FakeGitHub()
        row=nightly.not_run_rows('f44',dict(rows=[dict(name='Fedora GNOME')]),'unreachable',Path('/owned'))[0]
        with tempfile.TemporaryDirectory() as tmp:
            nightly.publish(github,dict(release='tag',date='today',build='unverified',desktops=[row]),Path(tmp))
        self.assertEqual([w[1] for w in github.writes if w[0]=='issue'],['Nightly desktop results'])

    def test_a_row_its_builder_keeps_from_running_two_nights_running_is_flagged(self):
        # #1978: k72's and f44's rows were all not run on 11-22 and 11-23 (RAM taken by /tmp scratch).
        ram = 'RAM below guest requirement plus 4 GiB headroom; not run after 1200s'
        def night(date, reason, ran=False):
            rows = nightly.not_run_rows('k72', dict(rows=[dict(name='Kubuntu KDE', desktop='kde')]), reason, Path('/owned'))
            rows += nightly.not_run_rows('m74', dict(rows=[dict(name='Mint Cinnamon')]), 'unrelated', Path('/owned'))
            if ran:
                rows[0]['cells']['region_upload'] = nightly.cell('green')
            return dict(release='v26.1123.1-nightly', build=BUILD, date=date+'T12:30:00+00:00', desktops=rows)
        first = night('2026-11-22', ram)
        nightly.count_host_blocked(first, None)
        self.assertEqual(first['desktops'][0]['hostBlockedNights'], 1)
        self.assertNotIn('hostBlockedNights', first['desktops'][1])  # not a host reason
        second = night('2026-11-23', 'worker stopped early; '+ram)
        nightly.count_host_blocked(second, first)
        self.assertEqual(second['desktops'][0]['hostBlockedNights'], 2)
        rerun = night('2026-11-23', ram)
        nightly.count_host_blocked(rerun, second)
        self.assertEqual(rerun['desktops'][0]['hostBlockedNights'], 2)  # the same day again
        legacy = night('2026-11-23', ram)  # results written before the count existed
        nightly.count_host_blocked(legacy, dict(night('2026-11-22', ram), desktops=night('2026-11-22', ram)['desktops']))
        self.assertEqual(legacy['desktops'][0]['hostBlockedNights'], 2)
        slot = night('2026-11-24', 'nightly guest slot belongs to a live or unverified worker')
        nightly.count_host_blocked(slot, second)  # met by stale-guest recovery, before await_host
        self.assertEqual(slot['desktops'][0]['hostBlockedNights'], 3)
        kept = night('2026-11-24', 'guest retained: live remote cleanup unresolved (marker in the run directory); '
                                   'resolve it before recovery')
        nightly.count_host_blocked(kept, second)
        self.assertEqual(kept['desktops'][0]['hostBlockedNights'], 3)
        # A filtered recheck of another row in the morning carries this row over with a newer table
        # date; the evening's full run is still its next blocked run.
        morning = dict(first, date='2026-11-23T08:00:00+00:00')
        evening = night('2026-11-23', ram)
        nightly.count_host_blocked(evening, morning)
        self.assertEqual(evening['desktops'][0]['hostBlockedNights'], 2)
        self.assertEqual(evening['desktops'][0]['hostBlockedDate'], '2026-11-23')
        recovered = night('2026-11-24', ram, ran=True)
        nightly.count_host_blocked(recovered, second)
        self.assertNotIn('hostBlockedNights', recovered['desktops'][0])
        github = FakeGitHub()
        with tempfile.TemporaryDirectory() as tmp:
            nightly.publish(github, second, Path(tmp))
            table = (Path(tmp)/'results.md').read_text()
        alert = 'Kubuntu KDE / k72 ran no cell for 2 nights running: worker stopped early; '+ram
        self.assertTrue(table.startswith('Release: **v26.1123.1-nightly**'))
        self.assertIn('> [!WARNING]\n> '+alert, table)
        self.assertIn('| ⚠️ 2 nights running: worker stopped early; RAM below', table)
        comments = [w[2]['body'] for w in github.writes if w[0] == 'POST' and w[1].endswith('/comments')]
        self.assertIn('> [!WARNING]\n> '+alert, comments[0])
        self.assertIn('"hostBlockedNights": 2', comments[0])
        self.assertNotIn('[!WARNING]', comments[1])
        # Grey still files no issue.
        self.assertEqual([w[1] for w in github.writes if w[0] == 'issue'], ['Nightly desktop results'])

    def test_the_last_results_are_read_from_the_state_directory_or_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            state = Path(tmp)
            self.assertIsNone(nightly.last_results(state))
            (state/'last-run.json').write_text(json.dumps(dict(results=str(state/'gone.json'))))
            self.assertIsNone(nightly.last_results(state))
            (state/'r.json').write_text(json.dumps(dict(release='t', desktops=[])))
            (state/'last-run.json').write_text(json.dumps(dict(results=str(state/'r.json'))))
            self.assertEqual(nightly.last_results(state), dict(release='t', desktops=[]))
            # A malformed earlier file counts nothing rather than failing the night.
            result = dict(date='2026-11-24', desktops=nightly.not_run_rows(
                'f44', dict(rows=[dict(name='Fedora GNOME')]), 'golden missing', Path('/owned')))
            nightly.count_host_blocked(result, dict(date=7, desktops=[dict(host='f44', desktop='Fedora GNOME')]))
            self.assertEqual(result['desktops'][0]['hostBlockedNights'], 1)

    def test_secret_file_permissions(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'token';path.write_text('synthetic-only');path.chmod(0o644)
            with self.assertRaises(RuntimeError):nightly.private_file(path)
            path.chmod(0o600);self.assertEqual(nightly.private_file(path),path)
            link=Path(tmp)/'link';link.symlink_to(path)
            with self.assertRaises(RuntimeError):nightly.private_file(link)

    def test_pre_export_provisioning_failure_is_not_an_outstanding_baton(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(nightly.Path,'home',return_value=Path(tmp)):
            locks=Path(tmp)/'putgd-lab/locks';locks.mkdir(parents=True)
            registry=locks/'proton-registry.json'
            registry.write_text(json.dumps(dict(lastLend=dict(run='/previous',carriedBack='receipt.json'))))
            self.assertFalse(nightly.baton_returned('/current'))
            (locks/'proton.lock').write_text('blocked: lent copy outstanding, run recover\n')
            with self.assertRaisesRegex(RuntimeError,'carry-back unresolved'):
                nightly.baton_returned('/current')
            (locks/'proton.lock').unlink()
            registry.write_text(json.dumps(dict(lastLend=dict(run='/current',carriedBack='receipt.json'))))
            self.assertTrue(nightly.baton_returned('/current'))
            (locks/'proton.lock').write_text('blocked: lent copy outstanding on second attempt\n')
            with self.assertRaisesRegex(RuntimeError,'carry-back unresolved'):
                nightly.baton_returned('/current')

    def test_stable_draft_requires_explicit_named_opt_in_and_timer_ignores_it(self):
        draft = dict(tag_name='v26.1104.3870', draft=True, published_at=None)
        published = dict(tag_name='v26.1104.3869-nightly', draft=False, published_at='today')
        nightly.validate_release(draft, draft['tag_name'], True)
        for release, tag, opt_in in ((draft, draft['tag_name'], False),
                                     (published, published['tag_name'], True),
                                     (dict(draft, draft=False), draft['tag_name'], True),
                                     (dict(draft, prerelease=True), draft['tag_name'], True),
                                     (draft, 'v26.1104.9999', True)):
            with self.subTest(tag=tag, opt_in=opt_in), self.assertRaises(RuntimeError):
                nightly.validate_release(release, tag, opt_in)
        self.assertIs(nightly.select_release([draft, published]), published)

    def test_draft_lookup_lists_releases_and_requires_exactly_one_match(self):
        tag = 'v26.1104.3870'
        draft = dict(tag_name=tag, draft=True)
        github = Mock(repo='owner/repo')
        github.api.return_value = [dict(tag_name=tag, draft=False), draft]
        self.assertIs(nightly.draft_release(github, tag), draft)
        github.api.side_effect = [[dict(tag_name='other', draft=False)]*100, [draft]]
        self.assertIs(nightly.draft_release(github, tag), draft)
        github.api.side_effect = None
        for listed in ([], [dict(tag_name=tag, draft=False)], [draft, dict(draft)]):
            github.api.return_value = listed
            with self.subTest(listed=listed), self.assertRaisesRegex(RuntimeError, 'exactly one draft'):
                nightly.draft_release(github, tag)

    def draft_assets(self, root, *, corrupt=False, build=BUILD):
        import hashlib
        tag = 'v26.1104.3870'
        package = f'putgd-{tag[1:]}-ubuntu26.04-x86_64.deb'
        payloads = {package: b'synthetic package', 'release-builds.json': json.dumps(
            dict(release=dict(tag=tag, gitCommit=build))).encode()}
        payloads['SHA256SUMS'] = ''.join(hashlib.sha256(data).hexdigest()+'  '+name+'\n'
                                       for name, data in payloads.items()).encode()
        names = list(payloads)
        github = Mock(repo='owner/repo')
        github.api.side_effect = [dict(object=dict(type='tag', sha='b'*40)),
                                  dict(object=dict(type='commit', sha=BUILD))]
        def asset(asset_id, target):
            target.write_bytes(b'corrupted' if corrupt and asset_id == 0 else payloads[names[asset_id]])
        github.asset.side_effect = asset
        release = dict(assets=[dict(id=i, name=name) for i, name in enumerate(names)])
        identity = nightly.release_assets(github, tag, root/'packages',
            [dict(asset='ubuntu26.04-x86_64.deb')], release=release)
        github.gh.assert_not_called()
        self.assertEqual(github.asset.call_count, 3)
        self.assertEqual(identity['gitCommit'], BUILD)

    def test_draft_download_uses_authenticated_asset_ids_and_verifies_sums_and_tag(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.draft_assets(Path(tmp))
        for kwargs, reason in ((dict(corrupt=True), 'checksum mismatch'),
                               (dict(build='c'*40), 'differs from tag commit')):
            with tempfile.TemporaryDirectory() as tmp, self.assertRaisesRegex(RuntimeError, reason):
                self.draft_assets(Path(tmp), **kwargs)

    def test_stable_draft_controller_isolates_state_and_has_no_github_writes(self):
        tag = 'v26.1104.3870'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            state = root/'state'
            state.mkdir()
            (state/'last-run.json').write_text('nightly sentinel')
            github = Mock(repo='owner/repo')
            github.api.return_value = [dict(tag_name=tag, draft=True),
                                       dict(tag_name=tag+'-nightly', draft=False)]
            config = dict(state=str(state), repository='owner/repo', builders={
                h: dict(rows=[dict(name=h, desktop=h, asset='ubuntu26.04-x86_64.deb')])
                for h in nightly.HOSTS})
            args = SimpleNamespace(stable_draft=True, tag=tag, now=False, desktops=['d69'],
                                   config=root/'config.json', draft_token_file=root/'draft-token')
            def assets(_github, _tag, out, rows, **kwargs):
                self.assertTrue(kwargs['release']['draft'])
                self.assertEqual(len(rows), 1)
                out.mkdir()
                (out/'SHA256SUMS').write_text('synthetic checksums')
                return dict(gitCommit=BUILD)
            def run(_builders, _tag, _artifacts, out, result, **kwargs):
                self.assertEqual(list(_builders), ['d69'])
                result['desktops'] = nightly.not_run_rows('d69', config['builders']['d69'], 'dry run', out)
            with patch.object(nightly, 'GitHub', return_value=github) as client, \
                 patch.object(nightly, 'release_assets', side_effect=assets), \
                 patch.object(nightly, 'catalog', return_value={}), \
                 patch.object(nightly, 'command', return_value=BUILD), \
                 patch.object(nightly, 'run_hosts', side_effect=run), \
                 patch.object(nightly, 'follow_nightly') as follow:
                nightly.controller(args, config)
                client.assert_called_with('owner/repo', root/'draft-token')
                prior = json.loads((state/'stable-drafts'/tag/'last-run.json').read_text())
                # Draft snapshots are independent: a later run never reads an
                # earlier snapshot's results.
                Path(prior['results']).write_text('obsolete draft snapshot')
                nightly.controller(args, config)
            follow.assert_not_called()
            github.issues.assert_not_called()
            github.issue.assert_not_called()
            self.assertEqual(github.api.call_count, 2)  # Only read the named release per snapshot.
            self.assertNotIn('releases/tags/', str(github.api.call_args_list))
            self.assertEqual((state/'last-run.json').read_text(), 'nightly sentinel')
            self.assertFalse((state/(tag+'.json')).exists())
            marker = json.loads((state/'stable-drafts'/tag/'last-run.json').read_text())
            out = Path(marker['results']).parent
            self.assertTrue((out/'results.md').is_file())
            self.assertEqual(len(list(out.glob('d69-d69-*-receipt.json'))), 1)  # One receipt per builder and row.
            self.assertIn('dry run', (out/'results.md').read_text())

    def test_checksum_failure_blocks_before_guest_launch(self):
        class GitHub:
            repo='owner/repo'
            def gh(self,*args):
                target=Path(args[args.index('--dir')+1]);name=args[args.index('--pattern')+1]
                (target/name).write_text('0'*64+'  package.deb\n' if name=='SHA256SUMS' else '{}')
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError,'checksum mismatch'):
                nightly.release_assets(GitHub(),'v26.1031.3384-nightly',Path(tmp)/'packages',[dict(asset='ubuntu26.04-x86_64.deb')])


if __name__=='__main__':unittest.main()
