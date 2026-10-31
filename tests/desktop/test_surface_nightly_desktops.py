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

    def test_worker_wait_and_setup_consume_budget_and_retry_releases_slot(self):
        self.worker_timeout_cleanup()

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
            else:
                if not finished:
                    self.assertIn('deadline exhausted', records[0]['notRunReason'])
                retire.assert_called_once()
                reset.assert_called_once()
                lock.return_value.release.assert_called_once()
                self.assertFalse(slot.exists())

    def test_attempt_timeout_caps_remaining_budget_and_refuses_exhaustion(self):
        with patch.object(nightly.time, 'monotonic', return_value=40):
            self.assertEqual(nightly.remaining_timeout(100), 60)
            self.assertEqual(nightly.remaining_timeout(10000), nightly.ATTEMPT_TIMEOUT)
            with self.assertRaisesRegex(nightly.NotRun, 'deadline exhausted'):
                nightly.remaining_timeout(40)
        self.assertGreater(nightly.HOST_TIMEOUT - nightly.WORKER_SECONDS, nightly.TERMINATE_GRACE)

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

    def test_remote_worker_has_its_own_watchdog(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = root/'inputs.tar'; archive.touch()
            with patch.object(nightly, 'command', side_effect=[None, None, None, '[]']) as command, \
                    patch.object(nightly.subprocess, 'run', return_value=SimpleNamespace(returncode=0)):
                nightly.run_host('d69', dict(ssh=['ssh', 'builder'], root='/repo', config='/private'),
                                 'vtest', root, archive, root, local_workers=Mock())
            args = command.call_args_list[2]
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

    def receipt(self, root, *, functional='passed', review=True, cleanup=None, note='pixel oracle mismatch'):
        value = dict(environment=dict(build=BUILD, observed_build=dict(gitCommit=BUILD,dirty=False),
                coverage_export=dict(published=True,scanned_before_copy=True,copy_scan_passed=True),
                cleanup_errors=cleanup), secret_scan=dict(passed=True),
            interactions=[dict(recipe='outcomes.region_upload',functional=functional,review=review,visual='frames',note=note)])
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

    def test_grey_reason_is_one_table_line_and_disabled_is_explicit(self):
        result=dict(desktops=[dict(desktop='Aurora',host='aurora-amd',notRunReason='load at core count',cells={
            'region_upload':nightly.cell(assertion='load at core count'),
            'history_delete':nightly.cell('green')})])
        table=nightly.table(result)
        self.assertIn('⚪',table);self.assertNotIn('🔴',table)
        self.assertEqual(table.count('load at core count'),1)
        self.assertIn('🟢 | — | —',table)

    def test_daily_skips_tested_latest_release_and_now_forces_it(self):
        releases=[dict(tag_name=f'v26.1031.{i}-nightly',draft=False,published_at=f'2026-10-31T0{i}:00:00Z') for i in (1,2,3)]
        latest=releases[-1]['tag_name']
        self.assertEqual(nightly.select_release(releases)['tag_name'],latest)
        self.assertIsNone(nightly.select_release(releases,[latest]))
        self.assertEqual(nightly.select_release(releases,[latest],True)['tag_name'],latest)
        timer=(nightly.ROOT/'tools/lab/putgd-nightly-desktops.timer').read_text()
        self.assertIn('OnCalendar=*-*-* 07:30:00 UTC',timer)
        self.assertNotIn('*:00,30',timer)

    def test_all_five_hosts_are_dispatched_concurrently(self):
        barrier=threading.Barrier(5)
        seen=[]
        def dispatch(host,builder,*args):
            seen.append(host);barrier.wait(timeout=3)
            return [dict(desktop=host,host=host,cells={},totalSeconds=0)]
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);artifacts=root/'artifacts';artifacts.mkdir()
            result=dict(desktops=[])
            nightly.run_hosts({h:dict(rows=[{}]) for h in nightly.HOSTS},'tag',artifacts,root,result,dispatch)
            self.assertCountEqual(seen,nightly.HOSTS)
            self.assertEqual(len(result['desktops']),5)

    def test_capacity_wait_has_twenty_minute_default_and_holds_no_lease(self):
        with tempfile.TemporaryDirectory() as tmp:
            clock=[0.0]
            def sleep(seconds):clock[0]+=seconds
            config=dict(host='m74',rows=[dict(live=True)])
            with patch.object(nightly.Path,'home',return_value=Path(tmp)),patch('tools.lab.desktop_lock.Desktop') as lock,patch.object(nightly,'preflight',side_effect=nightly.NotRun('RAM short')):
                lock.return_value.status.return_value=dict(state='free')
                with self.assertRaisesRegex(nightly.NotRun,'1200s'):
                    nightly.await_host(config,Path(tmp),clock=lambda:clock[0],sleep=sleep)
                self.assertEqual(clock[0],1200)
                lock.return_value.acquire.assert_not_called()

    def test_capacity_wait_proceeds_once_resources_return(self):
        with tempfile.TemporaryDirectory() as tmp:
            clock=[0.0]
            def sleep(seconds):clock[0]+=seconds
            with patch.object(nightly.Path,'home',return_value=Path(tmp)),patch('tools.lab.desktop_lock.Desktop') as lock,patch.object(nightly,'preflight',side_effect=[nightly.NotRun('load high'),dict(ready=True)]):
                lock.return_value.status.return_value=dict(state='free')
                waited=nightly.await_host(dict(host='d69',rows=[{}]),Path(tmp),clock=lambda:clock[0],sleep=sleep)
                self.assertEqual(waited,30)

    def test_one_grouped_failure_issue_per_desktop_and_grey_does_not_close(self):
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
            self.assertFalse(any(w[0]=='PATCH' and w[1].endswith('/2') for w in github.writes))

    def test_grey_infrastructure_creates_no_failure_issue(self):
        github=FakeGitHub()
        row=nightly.not_run_rows('f44',dict(rows=[dict(name='Fedora GNOME')]),'unreachable',Path('/owned'))[0]
        with tempfile.TemporaryDirectory() as tmp:
            nightly.publish(github,dict(release='tag',date='today',build='unverified',desktops=[row]),Path(tmp))
        self.assertEqual([w[1] for w in github.writes if w[0]=='issue'],['Nightly desktop results'])

    def test_timer_qualification_requires_three_verified_real_desktops(self):
        result=dict(build=BUILD,desktops=[])
        for _ in range(2):result['desktops'].append(dict(cells=dict(region_upload=nightly.cell('green',terminal={'proof':True},observedBuild=BUILD))))
        self.assertEqual(nightly.eligible_desktops(result),2)
        result['desktops'].append(dict(cells=dict(region_upload=nightly.cell('red',terminal={'proof':True},observedBuild=BUILD))))
        self.assertEqual(nightly.eligible_desktops(result),3)
        result['desktops'][0]['cells']['region_upload'].update(status='not_run')
        self.assertEqual(nightly.eligible_desktops(result),2)

    def test_clipboard_columns_are_enabled_and_app_failure_gets_release_blocker(self):
        self.assertEqual([nightly.FLOWS[name] for name in ('auto_copy', 'upload_clipboard', 'copy_image', 'survives_exit')],
                         ['Auto-copy', 'Upload clipboard', 'Copy image', 'Survives exit'])
        self.assertTrue({'auto_copy', 'upload_clipboard', 'copy_image', 'survives_exit'} <= set(nightly.LOCAL))
        github = FakeGitHub()
        row = dict(desktop='GNOME',host='d69',evidence='/owned',cells={'auto_copy':nightly.cell('red','wrong link')})
        with tempfile.TemporaryDirectory() as tmp:
            nightly.publish(github,dict(release='tag',date='today',build=BUILD,desktops=[row]),Path(tmp))
        self.assertTrue(any(w[0] == 'POST' and w[1].endswith('/labels') and w[2] == {'labels':['release-blocker']}
                            for w in github.writes))

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
