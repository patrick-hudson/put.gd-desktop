"""Synthetic staging regressions adopted from the independent #761 validator.

Real local Linux processes and guest shell guards only; no apt, VM or desktop.
"""
import contextlib
import errno
import fcntl
import importlib.util
import io
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
STAGING = ROOT / 'tools/lab/package_staging.py'
spec = importlib.util.spec_from_file_location('putgd_lab', ROOT / 'tools/lab/lab.py')
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)
JOB = 'install-' + 'e' * 32
# Budget for things that will happen: a process exits, a handshake line
# arrives, a guard script finishes. A passing wait ends as soon as its
# condition holds; fixed 3 to 5 s budgets expired on a CPU-starved runner.
# Timings that must elapse (admission waits, steady checks) stay literal.
WAIT = 30 * float(os.environ.get('PUTGD_TEST_TIME_SCALE', '1'))


def identity(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return {'pid': pid, 'startTicks': int(fields[19]), 'sessionId': pid}


def wait_for(predicate):
    deadline = time.monotonic() + WAIT
    while not predicate():
        if time.monotonic() >= deadline:
            raise TimeoutError('synthetic staging handshake')
        time.sleep(.01)


def dead(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0] in ('Z', 'X')
    except (FileNotFoundError, ProcessLookupError):
        # An orphan can be reaped after the open but before the read, which
        # then fails with ESRCH; package_staging.proc_fields callers do the same.
        return True


class StagingRecoveryContracts(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='stg761-')
        self.addCleanup(directory.cleanup)
        self.home = Path(directory.name)
        self.remote = self.home / 'putgd-lab' / JOB
        self.remote.mkdir(parents=True)
        self.active = self.home / 'putgd-lab/install-active'
        self.active.mkdir()
        (self.active / 'job').write_text(JOB)
        self.evidence = self.home / JOB
        self.evidence.mkdir()
        lab.write_json(self.evidence / 'staging-failed.json', {'error': 'KeyboardInterrupt'})

    def ssh(self, _run, script, **kwargs):
        result = subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
            env={**os.environ, 'HOME': str(self.home)}, timeout=WAIT)
        if result.returncode:
            raise lab.LabError('synthetic guest guard refused recovery')
        return result.stdout

    def reconcile(self, mode='pidfd', abandoned=True):
        prelude = {
            'pidfd': '',
            'missing': 'os.__dict__.pop("pidfd_open", None); ',
            'enosys': 'from unittest.mock import patch; import errno; '
                      'patch.object(os, "pidfd_open", side_effect=OSError(errno.ENOSYS, "old kernel"), create=True).start(); ',
            'denied-leader': 'from unittest.mock import patch; '
                      'patch.object(os, "pidfd_open", side_effect=PermissionError(), create=True).start(); ',
            'denied-scan': 'from pathlib import Path; from unittest.mock import patch; '
                      'original = Path.iterdir; '
                      'scan = lambda p: (_ for _ in ()).throw(PermissionError()) if str(p) == "/proc" else original(p); '
                      'patch.object(Path, "iterdir", scan).start(); ',
            'denied-stat': 'from pathlib import Path; from unittest.mock import patch; '
                      'original = Path.read_text; '
                      'read = lambda p, *a, **kw: (_ for _ in ()).throw(PermissionError()) '
                      'if str(p).startswith("/proc/") and str(p).endswith("/stat") else original(p, *a, **kw); '
                      'patch.object(Path, "read_text", read).start(); ',
            'first-scan-misses': 'from pathlib import Path; from unittest.mock import patch; '
                      'original = Path.iterdir; calls = []; '
                      'scan = lambda p: (calls.append(1) or iter(())) if str(p) == "/proc" and not calls else original(p); '
                      'patch.object(Path, "iterdir", scan).start(); ',
            'hidden-proc': 'from pathlib import Path; from unittest.mock import patch; '
                      'original = Path.read_text; '
                      'read = lambda p, *a, **kw: "proc /proc proc rw,hidepid=2 0 0" '
                      'if str(p) == "/proc/mounts" else original(p, *a, **kw); '
                      'patch.object(Path, "read_text", read).start(); ',
        }[mode]
        script = 'import os, runpy; ' + prelude + f'runpy.run_path({str(STAGING)!r}, run_name="__main__")'
        result = subprocess.run([sys.executable, '-c', script, 'reconcile', str(self.remote), '0'] +
            (['--abandoned'] if abandoned else []), text=True, capture_output=True, check=True, timeout=WAIT)
        self.assertEqual(result.stderr, '')
        return json.loads(result.stdout)

    def recover(self, mode='pidfd', abandoned=True):
        with patch.object(lab, 'verify_owned_guest'), patch.object(lab, 'ssh', side_effect=self.ssh), \
                patch.object(lab, 'reconcile_staging', side_effect=lambda *a, **kw: self.reconcile(mode, kw['abandoned'])):
            return lab.recover_staging(self.home, JOB, abandoned=abandoned)

    def test_validator_live_dead_reused_zombie_and_flock_both_pidfd_paths(self):
        live = subprocess.Popen([sys.executable, '-c', 'import sys; sys.stdin.read()'], stdin=subprocess.PIPE)
        zombie = subprocess.Popen(['true'])
        gone = subprocess.run([sys.executable, '-c',
            'import os,json; from pathlib import Path; print(json.dumps({"pid":os.getpid(),'
            '"sessionId":os.getpid(),"startTicks":int(Path("/proc/self/stat").read_text().rsplit(")",1)[1].split()[19])}))'],
            text=True, capture_output=True, check=True)
        wait_for(lambda: dead(zombie.pid))
        try:
            for mode in ('pidfd', 'missing', 'enosys'):
                for case in ('live', 'dead', 'reused', 'zombie', 'flock'):
                    with self.subTest(mode=mode, case=case):
                        saved = json.loads(gone.stdout) if case in ('dead', 'flock') else identity(
                            zombie.pid if case == 'zombie' else live.pid)
                        if case == 'reused':
                            saved['startTicks'] -= 1
                        lab.write_json(self.remote / 'staging-process.json', saved)
                        holder = None
                        if case == 'flock':
                            holder = subprocess.Popen([sys.executable, '-c',
                                f'import fcntl,sys; f=open({str(self.remote / "staging.lock")!r},"a"); '
                                'fcntl.flock(f,fcntl.LOCK_EX); print("held",flush=True); sys.stdin.read()'],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                            self.assertEqual(holder.stdout.readline().strip(), 'held')
                        try:
                            if case in ('live', 'flock'):
                                with self.assertRaisesRegex(lab.LabError, 'remains held'):
                                    self.recover(mode)
                                self.assertEqual((self.active / 'job').read_text(), JOB)
                            else:
                                report = self.recover(mode)
                                self.assertFalse(report['exactProcessAlive'])
                                self.assertFalse(self.active.exists())
                                self.active.mkdir()
                                (self.active / 'job').write_text(JOB)
                            self.assertIsNone(live.poll())  # Recovery never signals a reused PID.
                        finally:
                            if holder:
                                holder.stdin.close()
                                holder.wait(timeout=WAIT)
                                holder.stdout.close()
        finally:
            live.stdin.close()
            live.wait(timeout=WAIT)
            zombie.wait(timeout=WAIT)

    def test_dead_counts_a_process_reaped_during_the_read_as_gone(self):
        # Nightly 41008430494: the orphaned descendant was reaped between the
        # open and the read of /proc/PID/stat, and the read raised ESRCH.
        for error in (FileNotFoundError(errno.ENOENT, 'gone'), ProcessLookupError(errno.ESRCH, 'reaped')):
            with self.subTest(error=type(error).__name__), patch.object(Path, 'read_text', side_effect=error):
                self.assertTrue(dead(os.getpid()))
        self.assertFalse(dead(os.getpid()))

    def test_validator_real_wrapper_then_descendant_survives_sigkill(self):
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote),
            'echo $$ > descendant.pid; while [ ! -e finish ]; do sleep .02; done'], start_new_session=True)
        child = None
        try:
            wait_for(lambda: (self.remote / 'descendant.pid').exists() and
                (self.remote / 'descendant.pid').read_text().strip())
            child = int((self.remote / 'descendant.pid').read_text())
            saved = lab.read_json(self.remote / 'staging-process.json')
            # Explicitly isolate the baseline wrapper too, so this regression
            # reaches the killed-wrapper defect on d1abdfeb. Its extra metadata
            # is ignored by that old reconciler. Session creation has its own test.
            saved.setdefault('sessionId', wrapper.pid)
            lab.write_json(self.remote / 'staging-process.json', saved)
            self.assertEqual(os.getsid(child), wrapper.pid)
            for mode in ('pidfd', 'missing', 'enosys'):
                with self.assertRaisesRegex(lab.LabError, 'staging-running'):
                    self.recover(mode)
            wrapper.kill()  # Only the exact wrapper started by this test.
            wrapper.wait(timeout=WAIT)
            self.assertFalse(dead(child))
            # Even a matching terminal receipt cannot override a live session.
            lab.write_json(self.remote / 'staging-result.json', {**saved, 'finished': True, 'exitCode': 0})
            for abandoned in (True, False):
                for mode in ('pidfd', 'missing', 'enosys'):
                    with self.assertRaisesRegex(lab.LabError, 'remains held'):
                        self.recover(mode, abandoned)
                    self.assertEqual((self.active / 'job').read_text(), JOB)
                    self.assertFalse(dead(child))
            (self.remote / 'finish').touch()
            wait_for(lambda: dead(child))
            self.assertEqual(self.recover()['state'], 'staging-abandoned')
            self.assertFalse(self.active.exists())
        finally:
            (self.remote / 'finish').touch()
            if wrapper.poll() is None:
                wrapper.wait(timeout=WAIT)
            if child:
                wait_for(lambda: dead(child))

    def test_session_enumeration_and_leader_uncertainty_refuse_both_paths(self):
        saved = identity(os.getpid())
        saved['startTicks'] -= 1  # Reused identity; only the scan can block recovery.
        for mode in ('denied-leader', 'denied-scan', 'denied-stat', 'hidden-proc'):
            for abandoned in (False, True):
                with self.subTest(mode=mode, abandoned=abandoned):
                    lab.write_json(self.remote / 'staging-process.json', saved)
                    lab.write_json(self.remote / 'staging-result.json', {**saved, 'finished': True})
                    with self.assertRaisesRegex(lab.LabError, 'Cannot (enumerate|establish) staging session'):
                        self.recover(mode, abandoned)
                    self.assertTrue(self.active.exists())
        for field in ('pid', 'startTicks', 'sessionId'):
            invalid = dict(saved)
            del invalid[field]
            lab.write_json(self.remote / 'staging-process.json', invalid)
            with self.assertRaisesRegex(lab.LabError, 'Cannot establish staging session leader identity'):
                self.recover()

    def test_recovery_preserves_install_reconciliation(self):
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=WAIT), 0)
        original = {'finished': False, 'state': 'staging-running'}
        lab.write_json(self.evidence / 'staging-reconciliation.json', original)
        report = self.recover(abandoned=False)
        self.assertEqual(report['state'], 'staging-finished')
        self.assertEqual(lab.read_json(self.evidence / 'staging-reconciliation.json'), original)
        self.assertEqual(lab.read_json(self.evidence / 'staging-recovery.json'), report)

    def test_missing_controller_receipt_reconstructs_from_guest_identity(self):
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=WAIT), 0)
        (self.evidence / 'staging-failed.json').unlink()
        report = self.recover(abandoned=False)
        self.assertEqual(report['state'], 'staging-finished')
        self.assertEqual(report['process']['pid'], wrapper.pid)
        self.assertEqual(lab.read_json(self.evidence / 'staging-recovery.json'), report)

    def test_wrapper_records_its_own_new_session(self):
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=WAIT), 0)
        saved = lab.read_json(self.remote / 'staging-process.json')
        self.assertEqual(saved['sessionId'], wrapper.pid)
        self.assertNotEqual(saved['sessionId'], os.getsid(0))
        self.assertEqual(saved, {key: lab.read_json(self.remote / 'staging-result.json')[key]
                               for key in ('pid', 'startTicks', 'sessionId')})

    def test_pidfd_enosys_falls_back_to_proc(self):
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=WAIT), 0)
        report = self.reconcile('enosys')
        self.assertIs(report['exactProcessAlive'], False)
        self.assertTrue(report['finished'])

    def test_release_interruption_reports_actual_lock_state(self):
        for phase in ('before', 'after', 'unknown'):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory(prefix='stg761-') as directory:
                home = Path(directory)
                lab.write_json(home / 'state.json', {'profile': 'cinnamon-x11'})
                def ssh(_run, script, **kwargs):
                    release = 'rmdir' in script
                    if (release and phase != 'after') or ('echo released' in script and phase == 'unknown'):
                        raise KeyboardInterrupt()
                    result = subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
                        env={**os.environ, 'HOME': directory}, check=True, timeout=WAIT).stdout
                    if release:
                        raise KeyboardInterrupt()
                    return result
                warning = io.StringIO()
                with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put'), patch.object(lab, 'get'), \
                        patch.object(lab, 'stage_install_helper', side_effect=lab.LabError('staging failed')), \
                        patch.object(lab, 'reconcile_staging', return_value={'finished': True}), contextlib.redirect_stderr(warning):
                    with self.assertRaises(KeyboardInterrupt):
                        lab.install_package(home, 'synthetic.deb')
                expected = {'before': 'retains install-active', 'after': 'the lock is released',
                            'unknown': 'unknown install-active state'}[phase]
                self.assertIn(expected, warning.getvalue())
                self.assertEqual((home / 'putgd-lab/install-active').exists(), phase != 'after')
                if phase == 'after':
                    self.assertNotIn('retains install-active', warning.getvalue())

    def test_original_staging_error_remains_cause_of_non_interrupt_failure(self):
        lab.write_json(self.home / 'state.json', {'profile': 'cinnamon-x11'})
        original, reconciliation = lab.LabError('original staging'), OSError('reconcile offline')
        with patch.object(lab, 'ssh', return_value=''), patch.object(lab, 'put'), patch.object(lab, 'get'), \
                patch.object(lab, 'stage_install_helper', side_effect=original), \
                patch.object(lab, 'reconcile_staging', side_effect=reconciliation), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(lab.LabError) as caught:
                lab.install_package(self.home, 'synthetic.deb')
        self.assertIs(caught.exception.__cause__, original)
        self.assertIs(caught.exception.__context__, reconciliation)

    def test_validator_double_ctrl_c_reconcile_get_and_receipt(self):
        for phase in ('reconcile', 'get', 'receipt'):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory(prefix='stg761-') as directory:
                home = Path(directory)
                controller = subprocess.Popen([sys.executable, '-c', INTERRUPTED_CONTROLLER,
                    str(ROOT / 'tools/lab/lab.py'), str(home), phase, str(STAGING)],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                remote = home / 'putgd-lab' / JOB
                try:
                    for expected in ('first', 'second-window'):
                        line = b''
                        deadline = time.monotonic() + WAIT
                        while not line.endswith(b'\n'):
                            self.assertGreater(deadline - time.monotonic(), 0)
                            self.assertTrue(select.select([controller.stdout], [], [], max(0, deadline - time.monotonic()))[0])
                            chunk = os.read(controller.stdout.fileno(), 1)
                            self.assertTrue(chunk)
                            line += chunk
                        self.assertEqual(line.decode().strip(), expected)
                    if phase == 'get':
                        (remote / 'finish').touch()  # Finished before interrupted log collection.
                    os.kill(controller.pid, signal.SIGINT)
                    _, error = controller.communicate(timeout=WAIT)
                    self.assertNotEqual(controller.returncode, 0)
                    if phase == 'get':
                        self.assertFalse((home / 'putgd-lab/install-active').exists())
                    else:
                        self.assertEqual((home / 'putgd-lab/install-active/job').read_text(), JOB)
                        self.assertIn(f'recover-staging --run {home} --job {JOB}', error)
                        if phase == 'receipt':
                            self.assertFalse((home / JOB / 'staging-failed.json').exists())
                        (remote / 'finish').touch()
                        pid = lab.read_json(remote / 'staging-process.json')['pid']
                        wait_for(lambda: dead(pid))
                        # Recover through the actual guest shell and production
                        # reconciler, including reconstruction after receipt loss.
                        def ssh(_run, script, **kwargs):
                            return subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
                                env={**os.environ, 'HOME': directory}, check=True, timeout=WAIT).stdout
                        with patch.object(lab, 'verify_owned_guest'), patch.object(lab, 'ssh', side_effect=ssh):
                            self.assertTrue(lab.recover_staging(home, JOB)['finished'])
                finally:
                    (remote / 'finish').touch()
                    if controller.poll() is None:
                        controller.kill()
                    controller.communicate(timeout=WAIT)
                    if (remote / 'staging-process.json').exists():
                        wait_for(lambda: dead(lab.read_json(remote / 'staging-process.json')['pid']))


    def hold(self, path):
        # A real process holding a flock, as a live controller or guard holder would.
        holder = subprocess.Popen([sys.executable, '-c',
            f'import fcntl,sys; f=open({str(path)!r},"a"); fcntl.flock(f,fcntl.LOCK_EX); '
            'print("held",flush=True); sys.stdin.read()'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.assertEqual(holder.stdout.readline().strip(), 'held')
        def release():
            if holder.poll() is None:
                holder.stdin.close()
                holder.wait(timeout=WAIT)
            holder.stdout.close()
        self.addCleanup(release)
        return release

    def test_validator_missing_receipt_recovery_refused_during_live_controller_window(self):
        # F1 (rev761b test_missing_receipt_recovery_during_live_controller_window):
        # staging succeeded and the controller is still uploading package.deb or
        # launching the supervisor, so there is no progress.json and no receipt.
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=WAIT), 0)
        (self.evidence / 'staging-failed.json').unlink()
        release = self.hold(self.home / 'install.lock')
        for abandoned in (False, True):
            with self.subTest(abandoned=abandoned):
                with self.assertRaisesRegex(lab.LabError, 'install controller .* is still running'):
                    self.recover(abandoned=abandoned)
                self.assertEqual((self.active / 'job').read_text(), JOB)
                self.assertFalse((self.evidence / 'staging-recovery.json').exists())
        release()
        # The same guest state stays recoverable once the controller is dead.
        self.assertEqual(self.recover(abandoned=False)['state'], 'staging-finished')
        self.assertFalse(self.active.exists())

    def test_live_controller_upload_window_refuses_recovery_end_to_end(self):
        with tempfile.TemporaryDirectory(prefix='stg761-') as directory:
            home = Path(directory)
            controller = subprocess.Popen([sys.executable, '-c', UPLOADING_CONTROLLER,
                str(ROOT / 'tools/lab/lab.py'), str(home), str(STAGING)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            remote, evidence = home / 'putgd-lab' / JOB, home / JOB
            try:
                self.assertEqual(controller.stdout.readline().strip(), 'uploading')
                self.assertTrue((evidence / 'helper.json').exists())  # Staging succeeded.
                self.assertTrue((remote / 'staging-result.json').exists())
                self.assertFalse((remote / 'progress.json').exists())
                def ssh(_run, script, **kwargs):
                    return subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
                        env={**os.environ, 'HOME': directory}, check=True, timeout=WAIT).stdout
                for abandoned in (False, True):
                    with patch.object(lab, 'verify_owned_guest'), patch.object(lab, 'ssh', side_effect=ssh), \
                            self.assertRaisesRegex(lab.LabError, 'install controller .* is still running'):
                        lab.recover_staging(home, JOB, abandoned=abandoned)
                    self.assertEqual((home / 'putgd-lab/install-active/job').read_text(), JOB)
                # A concurrent install controller is refused on the host as well.
                with patch.object(lab, 'ssh') as guest, self.assertRaisesRegex(lab.LabError, 'Install refused'):
                    lab.install_package(home, 'synthetic.deb')
                guest.assert_not_called()
                (home / 'upload-failed').touch()
                _, error = controller.communicate(timeout=WAIT)
                self.assertNotEqual(controller.returncode, 0)
                self.assertIn('upload failed', error)
                # Only the controller itself released its lock.
                self.assertFalse((home / 'putgd-lab/install-active').exists())
                self.assertFalse((evidence / 'staging-recovery.json').exists())
            finally:
                (home / 'upload-failed').touch()
                if controller.poll() is None:
                    controller.communicate(timeout=WAIT)

    def supervisor(self, wait=None, sudo_uid=None):
        # The real root helper as the lab user. Fake apt/dpkg record any start,
        # so an unadmitted supervisor on old code cannot touch the workstation.
        self.remote.chmod(0o700)
        tools = self.home / 'fake-bin'
        tools.mkdir(exist_ok=True)
        for name in ('apt-get', 'dpkg'):
            (tools / name).write_text(f'#!/bin/sh\necho {name} "$@" >> {self.home}/apt-started\nexit 1\n')
            (tools / name).chmod(0o700)
        env = {key: value for key, value in os.environ.items() if key != 'SUDO_UID'}
        env['PATH'] = f'{tools}:{env["PATH"]}'
        helper = str(ROOT / 'tools/lab/package_install.py')
        if wait is None:
            return subprocess.run([sys.executable, helper, str(self.remote), '5', '5'],
                text=True, capture_output=True, env=env, timeout=30)
        return subprocess.Popen([sys.executable, '-c',
            'import importlib.util, os, sys; spec = importlib.util.spec_from_file_location("helper", sys.argv[1]); '
            'helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper); '
            'helper.enter_directory(sys.argv[2], os.getuid()); '
            'helper.admit(os.path.basename(sys.argv[2]), os.getuid(), wait=float(sys.argv[3]))',
            helper, str(self.remote), str(wait)], text=True, stderr=subprocess.PIPE, env=env)

    def assert_supervisor_never_started(self, result):
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.remote / 'progress.json').exists())
        self.assertFalse((self.remote / 'result.json').exists())
        self.assertFalse((self.home / 'apt-started').exists())

    def test_supervisor_refuses_to_run_without_its_install_token(self):
        (self.home / 'putgd-lab/install-guard').touch()
        for case in ('released', 'replacement', 'linked-job', 'missing-guard'):
            with self.subTest(case=case):
                if case == 'released':
                    (self.active / 'job').unlink()
                    self.active.rmdir()
                elif case == 'replacement':
                    (self.active / 'job').write_text('install-' + 'c' * 32)
                elif case == 'linked-job':
                    (self.active / 'job').unlink()
                    (self.home / 'elsewhere').write_text(JOB)
                    (self.active / 'job').symlink_to(self.home / 'elsewhere')
                else:
                    (self.home / 'putgd-lab/install-guard').unlink()
                result = self.supervisor()
                self.assertIn('Install supervisor refused to start', result.stderr)
                self.assert_supervisor_never_started(result)
                if case == 'released':
                    self.active.mkdir()
                (self.active / 'job').unlink(missing_ok=True)
                (self.active / 'job').write_text(JOB)
                (self.home / 'putgd-lab/install-guard').touch()

    def test_supervisor_admission_records_start_while_holding_token(self):
        (self.home / 'putgd-lab/install-guard').touch()
        admission = self.supervisor(wait=1)
        _, error = admission.communicate(timeout=WAIT)
        self.assertEqual(admission.returncode, 0, error)
        self.assertEqual(lab.read_json(self.remote / 'progress.json')['admittedJob'], JOB)
        # The supervisor has started: guarded recovery now refuses and keeps the token.
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=WAIT), 0)
        for abandoned in (False, True):
            with self.assertRaisesRegex(lab.LabError, 'guest guard refused'):
                self.recover(abandoned=abandoned)
            self.assertEqual((self.active / 'job').read_text(), JOB)

    def test_delayed_supervisor_start_after_recovery_never_runs_unlocked(self):
        # The controller died after a launch whose sudo start was delayed.
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=WAIT), 0)
        (self.evidence / 'staging-failed.json').unlink()
        self.assertEqual(self.recover(abandoned=False)['state'], 'staging-finished')
        self.assertFalse(self.active.exists())
        self.assert_supervisor_never_started(self.supervisor())
        # A replacement job's token does not admit this job's supervisor either.
        self.active.mkdir()
        (self.active / 'job').write_text('install-' + 'c' * 32)
        self.assert_supervisor_never_started(self.supervisor())

    def test_recovery_release_and_supervisor_admission_serialize_on_install_guard(self):
        guard = self.home / 'putgd-lab/install-guard'
        # Recovery's guarded release holds install-guard; admission must wait for it.
        release = self.hold(guard)
        admission = self.supervisor(wait=5)
        time.sleep(.3)
        self.assertIsNone(admission.poll())
        self.assertFalse((self.remote / 'progress.json').exists())
        (self.active / 'job').unlink()
        self.active.rmdir()
        release()
        _, error = admission.communicate(timeout=WAIT)
        self.assertNotEqual(admission.returncode, 0)
        self.assertIn('install-active', error)
        self.assertFalse((self.remote / 'progress.json').exists())
        # Admission cannot outwait a guard that stays held.
        self.active.mkdir()
        (self.active / 'job').write_text(JOB)
        self.hold(guard)
        admission = self.supervisor(wait=.3)
        _, error = admission.communicate(timeout=WAIT)
        self.assertIn('TimeoutError', error)
        self.assertFalse((self.remote / 'progress.json').exists())

    def admit_supervisor(self):
        admission = self.supervisor(wait=1)
        _, error = admission.communicate(timeout=WAIT)
        self.assertEqual(admission.returncode, 0, error)
        self.assertTrue((self.remote / 'progress.json').exists())

    def test_validator_admitted_supervisor_keeps_lock_with_or_without_receipt(self):
        # B1 (validator r6): under set -e a failed `test ! -e progress.json &&`
        # does not exit, so the identity check or rm that followed still ran.
        (self.home / 'putgd-lab/install-guard').touch()
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=WAIT), 0)
        self.admit_supervisor()
        for receipt in (True, False):
            if not receipt:
                (self.evidence / 'staging-failed.json').unlink()
            for abandoned in (False, True):
                with self.subTest(receipt=receipt, abandoned=abandoned):
                    with self.assertRaisesRegex(lab.LabError, 'guest guard refused'):
                        self.recover(abandoned=abandoned)
                    self.assertEqual((self.active / 'job').read_text(), JOB)

    def test_validator_admission_between_precheck_and_release_keeps_lock(self):
        # B1 (validator r1, deterministic): the supervisor is admitted after the
        # pre-check passed. The release under install-guard must see it.
        (self.home / 'putgd-lab/install-guard').touch()
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=WAIT), 0)
        for receipt in (True, False):
            with self.subTest(receipt=receipt):
                (self.remote / 'progress.json').unlink(missing_ok=True)
                if not receipt:
                    (self.evidence / 'staging-failed.json').unlink()
                def reconcile(*args, **kwargs):
                    self.admit_supervisor()
                    return self.reconcile('pidfd', kwargs['abandoned'])
                with patch.object(lab, 'verify_owned_guest'), patch.object(lab, 'ssh', side_effect=self.ssh), \
                        patch.object(lab, 'reconcile_staging', side_effect=reconcile):
                    with self.assertRaisesRegex(lab.LabError, 'guest guard refused'):
                        lab.recover_staging(self.home, JOB)
                self.assertEqual((self.active / 'job').read_text(), JOB)
                # A refused release leaves no recovery receipt claiming success.
                self.assertFalse((self.evidence / 'staging-recovery.json').exists())

    def test_controller_dead_before_staging_identity_recovers_as_not_started(self):
        # N1 (validator r3): the controller died after the guest lock and before
        # the wrapper wrote staging-process.json. The free controller fence proves
        # it is gone; reconcile fences a delayed wrapper launch.
        with tempfile.TemporaryDirectory(prefix='stg761-') as directory:
            home = Path(directory)
            controller = subprocess.run([sys.executable, '-c', DYING_CONTROLLER,
                str(ROOT / 'tools/lab/lab.py'), str(home)], capture_output=True, text=True, timeout=WAIT)
            self.assertEqual(controller.returncode, 137, controller.stderr)
            remote = home / 'putgd-lab' / JOB
            self.assertEqual((home / 'putgd-lab/install-active/job').read_text(), JOB)
            self.assertFalse((home / JOB / 'staging-failed.json').exists())
            self.assertFalse((remote / 'staging-process.json').exists())
            def ssh(_run, script, **kwargs):
                result = subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
                    env={**os.environ, 'HOME': directory}, timeout=40)
                if result.returncode:
                    raise lab.LabError('synthetic guest guard refused recovery')
                return result.stdout
            with patch.object(lab, 'verify_owned_guest'), patch.object(lab, 'ssh', side_effect=ssh):
                # Abandonment still needs a recorded identity.
                with self.assertRaisesRegex(lab.LabError, 'Cannot establish staging session leader identity'):
                    lab.recover_staging(home, JOB, abandoned=True)
                self.assertEqual((home / 'putgd-lab/install-active/job').read_text(), JOB)
                report = lab.recover_staging(home, JOB)
            self.assertEqual(report, {'finished': True, 'state': 'staging-not-started'})
            self.assertFalse((home / 'putgd-lab/install-active').exists())
            self.assertEqual(lab.read_json(home / JOB / 'staging-recovery.json'), report)
            late = subprocess.run([sys.executable, str(STAGING), 'run', str(remote), 'touch ran'], timeout=WAIT)
            self.assertEqual(late.returncode, 125)
            self.assertFalse((remote / 'ran').exists())

    def test_subreaper_failure_before_admission_leaves_job_recoverable(self):
        # N2: a failure between admission and the guarded supervisor body would
        # leave progress.json without result.json, a permanent hold.
        (self.home / 'putgd-lab/install-guard').touch()
        self.remote.chmod(0o700)
        helper = str(ROOT / 'tools/lab/package_install.py')
        failing = ('import ctypes, runpy, sys; from unittest.mock import patch; '
                   'patch.object(ctypes, "CDLL", lambda *a, **kw: type("L", (), '
                   '{"prctl": staticmethod(lambda *a: -1)})()).start(); '
                   f'runpy.run_path({helper!r}, run_name="__main__")')
        result = subprocess.run([sys.executable, '-c', failing, str(self.remote), '5', '5'],
            text=True, capture_output=True, timeout=30)
        self.assertIn('Install supervisor refused to start', result.stderr)
        self.assert_supervisor_never_started(result)
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=WAIT), 0)
        self.assertEqual(self.recover(abandoned=False)['state'], 'staging-finished')
        self.assertFalse(self.active.exists())

    def test_subreaper_failure_after_admission_writes_terminal_result(self):
        self.remote.chmod(0o700)
        tools = self.home / 'fake-bin'
        tools.mkdir()
        for name in ('apt-get', 'dpkg'):
            (tools / name).write_text(f'#!/bin/sh\necho {name} "$@" >> {self.home}/apt-started\nexit 1\n')
            (tools / name).chmod(0o700)
        helper = str(ROOT / 'tools/lab/package_install.py')
        script = ('import importlib.util, sys; from pathlib import Path; from unittest.mock import patch; '
                  'spec = importlib.util.spec_from_file_location("helper", sys.argv[1]); '
                  'helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper); '
                  'patch.object(helper, "become_subreaper", side_effect=OSError("no subreaper")).start(); '
                  'helper.supervise([["apt-get", "update"]], Path(sys.argv[2]), 5, 5)')
        subprocess.run([sys.executable, '-c', script, helper, str(self.remote)], text=True, capture_output=True,
            env={**os.environ, 'PATH': f'{tools}:{os.environ["PATH"]}'}, timeout=30)
        report = lab.read_json(self.remote / 'result.json')
        self.assertTrue(report['aborted'])
        self.assertEqual(report['error'], 'OSError')
        self.assertFalse(report['ok'])
        self.assertNotIn('apt-get', (self.home / 'apt-started').read_text())  # Only the dpkg queries ran.

    def test_session_must_be_empty_on_two_scans(self):
        # N3: a member that forks and exits during one /proc pass can be missed.
        member = None
        leader = subprocess.Popen(['setsid', 'sh', '-c',
            'while [ ! -e finish ]; do sleep .02; done & echo $!; exit 0'],
            cwd=self.remote, stdout=subprocess.PIPE, text=True)
        try:
            member = int(leader.stdout.readline())
            leader.wait(timeout=WAIT)
            lab.write_json(self.remote / 'staging-process.json',
                           {'pid': leader.pid, 'sessionId': leader.pid, 'startTicks': 1})
            report = self.reconcile('first-scan-misses', abandoned=True)
            self.assertIs(report['sessionAlive'], True)
            self.assertIn(member, report['sessionPids'])
            self.assertFalse(report['finished'])
        finally:
            (self.remote / 'finish').touch()
            leader.stdout.close()
            if member:
                wait_for(lambda: dead(member))

    def test_reconcile_waits_for_leader_that_released_flock_but_is_still_exiting(self):
        # run() drops staging.lock at return, before interpreter shutdown ends.
        leader = subprocess.Popen([sys.executable, '-c',
            'import fcntl, json, os, sys, time; from pathlib import Path\n'
            'fence = open("staging.lock", "a"); fcntl.flock(fence, fcntl.LOCK_EX)\n'
            'me = {"pid": os.getpid(), "sessionId": os.getsid(0), "startTicks": '
            'int(Path("/proc/self/stat").read_text().rsplit(")", 1)[1].split()[19])}\n'
            'Path("staging-process.json").write_text(json.dumps(me))\n'
            'Path("staging-result.json").write_text(json.dumps({**me, "finished": True, "exitCode": 0}))\n'
            'fence.close(); print("released", flush=True)\n'
            'while not Path("exit").exists(): time.sleep(.01)\n'],
            cwd=self.remote, stdout=subprocess.PIPE, text=True, start_new_session=True)
        try:
            self.assertEqual(leader.stdout.readline().strip(), 'released')
            # Handshake: the real reconciler reports each inspection. Release the
            # leader only after reconcile has seen it live, then require a later
            # inspection (the retry) before it may finish.
            reconcile = subprocess.Popen([sys.executable, '-c',
                'import importlib.util, sys; spec = importlib.util.spec_from_file_location("staging", sys.argv[1]); '
                'staging = importlib.util.module_from_spec(spec); spec.loader.exec_module(staging); '
                'inspect = staging.process_status; '
                'staging.process_status = lambda: (lambda status: (print("inspected", status["exactProcessAlive"], '
                'file=sys.stderr, flush=True), status)[1])(inspect()); '
                'sys.argv = sys.argv[1:]; sys.exit(staging.main())',
                str(STAGING), 'reconcile', str(self.remote), '5'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(reconcile.stderr.readline().strip(), 'inspected True')
            (self.remote / 'exit').touch()
            leader.wait(timeout=WAIT)
            output, inspections = reconcile.communicate(timeout=WAIT)
            self.assertIn('inspected False', inspections)
            report = json.loads(output)
            self.assertEqual(report['state'], 'staging-finished')
            self.assertTrue(report['finished'])
        finally:
            (self.remote / 'exit').touch()
            leader.wait(timeout=WAIT)
            leader.stdout.close()

    def test_dead_leader_with_live_session_reports_session_pids(self):
        # F3: the leader is reaped but its PID stays referenced as the session
        # ID. Kernels such as 6.14 then fail pidfd_open with EINVAL; others
        # (the hosted CI runner) report ESRCH. Both must reach the session scan.
        member = None
        leader = subprocess.Popen(['setsid', 'sh', '-c',
            'while [ ! -e finish ]; do sleep .02; done & echo $!; exit 0'],
            cwd=self.remote, stdout=subprocess.PIPE, text=True)
        try:
            member = int(leader.stdout.readline())
            leader.wait(timeout=WAIT)
            self.assertEqual(os.getsid(member), leader.pid)
            with self.assertRaises(OSError) as einval:
                os.close(os.pidfd_open(leader.pid))
            self.assertIn(einval.exception.errno, (errno.EINVAL, errno.ESRCH))
            lab.write_json(self.remote / 'staging-process.json',
                           {'pid': leader.pid, 'sessionId': leader.pid, 'startTicks': 1})
            for mode in ('pidfd', 'missing', 'enosys'):
                for abandoned in (False, True):
                    with self.subTest(mode=mode, abandoned=abandoned):
                        report = self.reconcile(mode, abandoned)
                        self.assertNotIn('inspectionError', report)
                        self.assertIs(report['exactProcessAlive'], False)
                        self.assertIs(report['sessionAlive'], True)
                        self.assertIn(member, report['sessionPids'])
                        self.assertFalse(report['finished'])
            with self.assertRaisesRegex(lab.LabError, rf'session processes are still running: PIDs \[.*{member}'):
                self.recover()
            self.assertEqual((self.active / 'job').read_text(), JOB)
            (self.remote / 'finish').touch()
            wait_for(lambda: dead(member))
            report = self.recover()
            self.assertEqual(report['state'], 'staging-abandoned')
            self.assertFalse(self.active.exists())
        finally:
            (self.remote / 'finish').touch()
            leader.stdout.close()
            if member:
                wait_for(lambda: dead(member))

    def test_validator_orphaned_grandchild_and_reused_session_leader_refuse(self):
        # rev761b test_grandchild_in_session_refused and test_sid_reused_by_unrelated.
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote),
            'echo $$ > descendant.pid; '
            '(sh -c \'echo $$ > grandchild.pid; while [ ! -e finish ]; do sleep .02; done\' &); '
            'while [ ! -e finish ]; do sleep .02; done'], start_new_session=True)
        descendant = grandchild = None
        try:
            wait_for(lambda: (self.remote / 'grandchild.pid').exists() and
                (self.remote / 'grandchild.pid').read_text().strip())
            descendant = int((self.remote / 'descendant.pid').read_text())
            grandchild = int((self.remote / 'grandchild.pid').read_text())
            wrapper.kill()  # Only the exact wrapper started by this test.
            wrapper.wait(timeout=WAIT)
            self.assertEqual(os.getsid(grandchild), wrapper.pid)
            for mode in ('pidfd', 'missing', 'enosys'):
                for abandoned in (False, True):
                    with self.assertRaisesRegex(lab.LabError, 'remains held'):
                        self.recover(mode, abandoned)
            self.assertEqual((self.active / 'job').read_text(), JOB)
        finally:
            (self.remote / 'finish').touch()
            # Both loops must see finish before tearDown deletes it; a loop that
            # outlives its directory never ends and holds the suite's stdout.
            for pid in (descendant, grandchild):
                if pid:
                    wait_for(lambda: dead(pid))
        unrelated = subprocess.Popen(['setsid', 'sh', '-c', 'while [ ! -e finish-unrelated ]; do sleep .02; done'],
                                     cwd=self.remote)
        try:
            wait_for(lambda: os.getsid(unrelated.pid) == unrelated.pid)
            saved = identity(unrelated.pid)
            saved['startTicks'] -= 1  # The recorded leader died; its PID now leads a stranger's session.
            lab.write_json(self.remote / 'staging-process.json', saved)
            for mode in ('pidfd', 'missing', 'enosys'):
                with self.assertRaisesRegex(lab.LabError, 'remains held'):
                    self.recover(mode)
            self.assertEqual((self.active / 'job').read_text(), JOB)
            self.assertIsNone(unrelated.poll())  # Never signalled.
        finally:
            (self.remote / 'finish-unrelated').touch()
            unrelated.wait(timeout=WAIT)

    def test_validator_setsid_escape_is_the_documented_abandoned_limit(self):
        # rev761b test_grandchild_setsid_escapes. While the wrapper lives it waits,
        # as subreaper, for a descendant that left the session. After the wrapper
        # is killed only the session scan remains; the report states that limit.
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote),
            'setsid sh -c \'echo $$ > escaped.pid; while [ ! -e finish ]; do sleep .02; done\' & exit 0'],
            start_new_session=True)
        escaped = None
        try:
            wait_for(lambda: (self.remote / 'escaped.pid').exists() and
                (self.remote / 'escaped.pid').read_text().strip())
            escaped = int((self.remote / 'escaped.pid').read_text())
            self.assertNotEqual(os.getsid(escaped), wrapper.pid)
            time.sleep(.2)
            self.assertIsNone(wrapper.poll())  # No terminal receipt while it lives.
            with self.assertRaisesRegex(lab.LabError, 'staging-running'):
                self.recover(abandoned=False)
            wrapper.kill()  # Only the exact wrapper started by this test.
            wrapper.wait(timeout=WAIT)
            with self.assertRaisesRegex(lab.LabError, 'remains held'):
                self.recover(abandoned=False)
            report = self.recover(abandoned=True)
            self.assertIn('setsid()', report['limit'])
            self.assertEqual(lab.read_json(self.evidence / 'staging-recovery.json')['limit'], report['limit'])
        finally:
            (self.remote / 'finish').touch()
            if wrapper.poll() is None:
                wrapper.wait(timeout=WAIT)
            if escaped:
                wait_for(lambda: dead(escaped))

# The independent validator's double-SIGINT controller, with explicit handshakes
# and a finish gate instead of time-based staging completion.
INTERRUPTED_CONTROLLER = r'''
import importlib.util, os, signal, sys, time, types, subprocess
from pathlib import Path
from unittest.mock import patch
spec = importlib.util.spec_from_file_location('putgd_lab', sys.argv[1])
lab = importlib.util.module_from_spec(spec); spec.loader.exec_module(lab)
home = Path(sys.argv[2]); phase = sys.argv[3]; staging = sys.argv[4]
lab.write_json(home / 'state.json', {'profile': 'cinnamon-x11'})
lab.uuid.uuid4 = lambda: types.SimpleNamespace(hex='e' * 32)
def ssh(_run, script, timeout=5, **kw):
    return subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
        env={**os.environ, 'HOME': str(home)}, timeout=timeout, check=True).stdout
def stage(run, remote, helper, digest):
    subprocess.Popen([sys.executable, staging, 'run', str(home / remote),
        'while [ ! -e finish ]; do sleep .02; done'], start_new_session=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    while not (home / remote / 'staging-process.json').exists(): time.sleep(.01)
    print('first', flush=True)
    os.kill(os.getpid(), signal.SIGINT)
def window():
    print('second-window', flush=True)
    # Short sleeps: a SIGINT that arrives after the print but before one long
    # sleep starts would only be handled when that whole sleep ends.
    for _ in range(3000): time.sleep(.01)
orig_write = lab.write_json
def write_json(path, value):
    if phase == 'receipt' and str(path).endswith('staging-failed.json'):
        window()
    return orig_write(path, value)
def get(*a, **kw):
    if phase == 'get':
        window()
orig_rec = lab.reconcile_staging
def rec(*a, **kw):
    if phase == 'reconcile':
        window()
    return orig_rec(*a, **kw)
with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put'), patch.object(lab, 'get', side_effect=get), \
        patch.object(lab, 'stage_install_helper', side_effect=stage), patch.object(lab, 'write_json', side_effect=write_json), \
        patch.object(lab, 'reconcile_staging', side_effect=rec):
    lab.install_package(home, 'synthetic.deb')
'''


# A real controller that dies abruptly (no cleanup, like SIGKILL) while it
# uploads the helper: after the guest lock, before any staging identity.
DYING_CONTROLLER = r'''
import importlib.util, os, subprocess, sys, types
from pathlib import Path
from unittest.mock import patch
spec = importlib.util.spec_from_file_location('putgd_lab', sys.argv[1])
lab = importlib.util.module_from_spec(spec); spec.loader.exec_module(lab)
home = Path(sys.argv[2])
lab.write_json(home / 'state.json', {'profile': 'cinnamon-x11'})
lab.uuid.uuid4 = lambda: types.SimpleNamespace(hex='e' * 32)
def ssh(_run, script, timeout=5, **kw):
    return subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
        env={**os.environ, 'HOME': str(home)}, timeout=timeout, check=True).stdout
def put(run, local, remote):
    os._exit(137)
with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put', side_effect=put):
    lab.install_package(home, 'synthetic.deb')
'''


# A real controller after successful staging, blocked in the package upload
# (no progress.json yet), then failing that upload.
UPLOADING_CONTROLLER = r'''
import importlib.util, os, sys, time, types, subprocess
from pathlib import Path
from unittest.mock import patch
spec = importlib.util.spec_from_file_location('putgd_lab', sys.argv[1])
lab = importlib.util.module_from_spec(spec); spec.loader.exec_module(lab)
home = Path(sys.argv[2]); staging = sys.argv[3]
lab.write_json(home / 'state.json', {'profile': 'cinnamon-x11'})
lab.uuid.uuid4 = lambda: types.SimpleNamespace(hex='e' * 32)
def ssh(_run, script, timeout=5, **kw):
    return subprocess.run(['sh', '-s'], input=script, text=True, capture_output=True,
        env={**os.environ, 'HOME': str(home)}, timeout=timeout, check=True).stdout
def stage(run, remote, helper, digest):
    if subprocess.run([sys.executable, staging, 'run', str(home / remote), 'true']).returncode:
        raise lab.LabError('staging failed')
def put(run, local, remote):
    if remote.endswith('package.deb'):
        print('uploading', flush=True)
        while not (home / 'upload-failed').exists(): time.sleep(.01)
        raise lab.LabError('upload failed')
with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put', side_effect=put), patch.object(lab, 'get'), \
        patch.object(lab, 'stage_install_helper', side_effect=stage):
    lab.install_package(home, 'synthetic.deb')
'''


if __name__ == '__main__':
    unittest.main()
