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
                    environment=lambda pid:{'DISPLAY'}, own=set())
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
                archived = list((root/'putgd-lab/locks/history/recovered').glob('*.json'))
                self.assertEqual(json.loads(archived[0].read_text()), owner)

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

    def test_guest_local_quit_loss_can_retire_but_unknown_cleanup_cannot(self):
        for safe in (True, False):
            with self.subTest(safe=safe), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                slot, out, owner, desktop, proton = self.stale_fixture(root, missing=False)
                message = ('resident quit transport unavailable; private session retained'
                           if safe else 'fixture cleanup failed; publication outstanding')
                nightly.save(Path(owner['evidence'])/'flow/receipt.json', dict(environment=dict(
                    cleanup_errors=[message], private_cleanup='not-owned-or-retained')))
                with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                        patch('tools.lab.desktop_lock.Desktop', side_effect=lambda *a, **kw:
                              proton if kw.get('resource') == 'proton' else desktop), \
                        patch.object(nightly, 'retire') as retire:
                    if safe:
                        nightly.recover_stale_guest(dict(host='d69'), out)
                        retire.assert_called_once()
                        self.assertFalse(slot.exists())
                    else:
                        with self.assertRaisesRegex(nightly.NotRun, 'private cleanup'):
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

    def lost_session_row(self, errors, rows=({'functional': 'blocked', 'note': 'target session command unavailable: LabError exit 255'},)):
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
                if '-m' in argv:
                    cells.append(Path(argv[argv.index('--out')+1]).name)
                    target = Path(argv[argv.index('--out')+1])
                    target.mkdir()
                    (target/'receipt.json').write_text(json.dumps(dict(environment=dict(cleanup_errors=errors), interactions=list(rows))))
            observed = dict(nightly.cell(assertion='private target cleanup failed'),
                            observations=['target session command unavailable: LabError exit 255'])
            with patch.object(nightly, 'ROOT', root), patch.object(nightly.Path, 'home', return_value=root), \
                    patch.object(nightly, 'await_host', return_value=0), patch.object(nightly, 'preflight', return_value={}), \
                    patch.object(nightly, 'command', side_effect=command), patch.object(nightly, 'verdict', return_value=observed), \
                    patch.object(guest, 'prepare', return_value=root/'profiles'), \
                    patch.object(lab, 'state', return_value=dict(id='test', golden_sha256='a'*64)), \
                    patch.object(lab, 'reset'), patch.object(nightly, 'guest_clients_live', return_value=False), \
                    patch.object(nightly, 'retire') as retire, patch('tools.lab.desktop_lock.Desktop') as lock:
                lock.return_value.acquire.return_value = {}
                lock.return_value.release.return_value = {}
                lock.return_value.heartbeat.return_value = {}
                lock.return_value.status.return_value = dict(state='free')
                records = nightly.worker_run(dict(host='d69', rows=[row]), 'vtest', identity, artifacts, root/'output',
                                             time.monotonic() + 10**6)
            return records[0], cells, retire, (root/'putgd-lab/locks/tier1-guest.lock.d').exists()

    def test_lost_session_stops_the_row_with_the_first_failure(self):
        record, cells, retire, slot = self.lost_session_row(['resident quit transport unavailable; private session retained'])
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
        record, cells, retire, slot = self.lost_session_row(['resident quit transport unavailable; private session retained',
                                                             'previous clipboard restoration unavailable; private session retained'])
        self.assertEqual(len(cells), 1)
        retire.assert_not_called()
        self.assertTrue(slot and record['cleanupBlocked'])
        self.assertTrue(record['notRunReason'].startswith('guest session lost in idle_memory attempt 1'))

    def test_wait_does_not_outlive_worker_deadline(self):
        with tempfile.TemporaryDirectory() as tmp:
            clock = [0]
            def sleep(seconds): clock[0] += seconds
            with patch.object(nightly.Path, 'home', return_value=Path(tmp)), \
                    patch('tools.lab.desktop_lock.Desktop') as lock:
                lock.return_value.status.return_value = dict(state='held')
                with self.assertRaisesRegex(nightly.NotRun, 'awaiting host'):
                    nightly.await_host(dict(host='d69', rows=[{}], waitSeconds=1200), Path(tmp),
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
            cleanup = command.call_args_list[-1].args[0][-1]
            inputs = '/repo/inputs/'+root.name
            self.assertEqual(cleanup, f"rm -f -- '{inputs}/artifacts/packages/put gd.deb' && "
                                      f'rmdir -- {inputs}/artifacts/packages {inputs}/artifacts {inputs}')
            self.assertNotIn('-r', cleanup.split())

    def test_failed_cleanup_keeps_the_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = self.remote_inputs(root)
            with patch.object(nightly, 'command', side_effect=[None, None, None, None, '[]', RuntimeError('rm failed')]), \
                    patch.object(nightly.subprocess, 'run', return_value=SimpleNamespace(returncode=0)):
                self.assertEqual(nightly.run_host('d69', dict(ssh=['ssh', 'builder'], root='/repo', config='/private'),
                                                  'vtest', root, archive, root, local_workers=Mock()), [])

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
        previous = dict(release='tag', build=BUILD, desktops=[dict(host=h, cells={'old': h}) for h in builders])
        result = dict(release='tag', build=BUILD, desktops=[dict(host='aurora-amd', cells={'new': True})])
        merged = nightly.merge_desktops(previous, result)
        self.assertIs(merged[0], previous['desktops'][0])
        self.assertIs(merged[3], result['desktops'][0])
        with self.assertRaises(RuntimeError):
            nightly.merge_desktops(previous, dict(result, build='b'*40))

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
            self.assertTrue((out/'d69-receipt.json').is_file())
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
