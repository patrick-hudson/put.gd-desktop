"""Synthetic staging regressions adopted from the independent #761 validator.

Real local Linux processes and guest shell guards only; no apt, VM or desktop.
"""
import contextlib
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


def identity(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return {'pid': pid, 'startTicks': int(fields[19]), 'sessionId': pid}


def wait_for(predicate):
    deadline = time.monotonic() + 5
    while not predicate():
        if time.monotonic() >= deadline:
            raise TimeoutError('synthetic staging handshake')
        time.sleep(.01)


def dead(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0] in ('Z', 'X')
    except FileNotFoundError:
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
            env={**os.environ, 'HOME': str(self.home)}, timeout=5)
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
            'hidden-proc': 'from pathlib import Path; from unittest.mock import patch; '
                      'original = Path.read_text; '
                      'read = lambda p, *a, **kw: "proc /proc proc rw,hidepid=2 0 0" '
                      'if str(p) == "/proc/mounts" else original(p, *a, **kw); '
                      'patch.object(Path, "read_text", read).start(); ',
        }[mode]
        script = 'import os, runpy; ' + prelude + f'runpy.run_path({str(STAGING)!r}, run_name="__main__")'
        result = subprocess.run([sys.executable, '-c', script, 'reconcile', str(self.remote), '0'] +
            (['--abandoned'] if abandoned else []), text=True, capture_output=True, check=True, timeout=5)
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
                                holder.wait(timeout=3)
                                holder.stdout.close()
        finally:
            live.stdin.close()
            live.wait(timeout=3)
            zombie.wait(timeout=3)

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
            wrapper.wait(timeout=3)
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
                wrapper.wait(timeout=3)
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
        self.assertEqual(wrapper.wait(timeout=3), 0)
        original = {'finished': False, 'state': 'staging-running'}
        lab.write_json(self.evidence / 'staging-reconciliation.json', original)
        report = self.recover(abandoned=False)
        self.assertEqual(report['state'], 'staging-finished')
        self.assertEqual(lab.read_json(self.evidence / 'staging-reconciliation.json'), original)
        self.assertEqual(lab.read_json(self.evidence / 'staging-recovery.json'), report)

    def test_missing_controller_receipt_reconstructs_from_guest_identity(self):
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=3), 0)
        (self.evidence / 'staging-failed.json').unlink()
        report = self.recover(abandoned=False)
        self.assertEqual(report['state'], 'staging-finished')
        self.assertEqual(report['process']['pid'], wrapper.pid)
        self.assertEqual(lab.read_json(self.evidence / 'staging-recovery.json'), report)

    def test_wrapper_records_its_own_new_session(self):
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=3), 0)
        saved = lab.read_json(self.remote / 'staging-process.json')
        self.assertEqual(saved['sessionId'], wrapper.pid)
        self.assertNotEqual(saved['sessionId'], os.getsid(0))
        self.assertEqual(saved, {key: lab.read_json(self.remote / 'staging-result.json')[key]
                               for key in ('pid', 'startTicks', 'sessionId')})

    def test_pidfd_enosys_falls_back_to_proc(self):
        wrapper = subprocess.Popen([sys.executable, str(STAGING), 'run', str(self.remote), 'true'])
        self.assertEqual(wrapper.wait(timeout=3), 0)
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
                        env={**os.environ, 'HOME': directory}, check=True, timeout=3).stdout
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
                        deadline = time.monotonic() + 5
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
                    _, error = controller.communicate(timeout=10)
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
                                env={**os.environ, 'HOME': directory}, check=True, timeout=5).stdout
                        with patch.object(lab, 'verify_owned_guest'), patch.object(lab, 'ssh', side_effect=ssh):
                            self.assertTrue(lab.recover_staging(home, JOB)['finished'])
                finally:
                    (remote / 'finish').touch()
                    if controller.poll() is None:
                        controller.kill()
                    controller.communicate(timeout=5)
                    if (remote / 'staging-process.json').exists():
                        wait_for(lambda: dead(lab.read_json(remote / 'staging-process.json')['pid']))


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
orig_write = lab.write_json
def write_json(path, value):
    if phase == 'receipt' and str(path).endswith('staging-failed.json'):
        print('second-window', flush=True); time.sleep(30)
    return orig_write(path, value)
def get(*a, **kw):
    if phase == 'get':
        print('second-window', flush=True); time.sleep(30)
orig_rec = lab.reconcile_staging
def rec(*a, **kw):
    if phase == 'reconcile':
        print('second-window', flush=True); time.sleep(30)
    return orig_rec(*a, **kw)
with patch.object(lab, 'ssh', side_effect=ssh), patch.object(lab, 'put'), patch.object(lab, 'get', side_effect=get), \
        patch.object(lab, 'stage_install_helper', side_effect=stage), patch.object(lab, 'write_json', side_effect=write_json), \
        patch.object(lab, 'reconcile_staging', side_effect=rec):
    lab.install_package(home, 'synthetic.deb')
'''


if __name__ == '__main__':
    unittest.main()
