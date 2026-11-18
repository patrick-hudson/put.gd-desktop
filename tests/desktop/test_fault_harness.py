"""Fault-harness checks whose failure would be silent: a batch that reads as clean, faults that never lift, evidence lost, a crash or a
duplicate excused. Each test names the defect it guards against."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import types
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'lab'))
import action_fuzz  # noqa: E402


class FaultRun(unittest.TestCase):
    def test_256_findings_do_not_read_as_success(self):
        """An exit status is 8 bits: a count of 256 findings used to exit 0."""
        with tempfile.TemporaryDirectory() as folder:
            stub = Path(folder) / 'bin'
            stub.mkdir()
            (stub / 'dbus-run-session').write_text('#!/bin/sh\nexit 1\n')  # every seed "finds" something
            (stub / 'dbus-run-session').chmod(0o755)
            env = dict(os.environ, PATH='%s:%s' % (stub, os.environ['PATH']))
            done = subprocess.run(['bash', str(ROOT / 'tools/lab/faults/faultrun.sh'), 'build', '%s/out' % folder, '1', '256', '1', 'http', 'fs'],
                                  env=env, capture_output=True, text=True, timeout=600)
        self.assertEqual(done.returncode, 1, done.stdout[-500:] + done.stderr[-500:])
        self.assertIn('256 seeds with findings', done.stdout)


@unittest.skipUnless(shutil.which('gcc') and shutil.which('sh'), 'needs gcc to build the shim')
class ShimReload(unittest.TestCase):
    CHILD = textwrap.dedent('''
        import os, resource, sys, time
        victim = sys.argv[1]; spec = sys.argv[2]
        fd = os.open(victim, os.O_WRONLY | os.O_CREAT, 0o600)
        def write():
            try:
                os.write(fd, b'x'); return 'ok'
            except OSError as error:
                return error.strerror
        time.sleep(0.06); print('faulted:', write())
        open(spec + '.new', 'w').write('seed 1\\n'); os.replace(spec + '.new', spec)  # an empty spec: lift every fault
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        held = len(os.listdir('/proc/self/fd'))
        resource.setrlimit(resource.RLIMIT_NOFILE, (held - 1, hard))  # the next open fails with EMFILE
        time.sleep(0.06); print('during exhaustion:', write())
        resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))
        time.sleep(0.06); print('after:', write())
    ''')

    def test_a_reload_that_hit_emfile_is_retried(self):
        """The shim used to record the new spec's mtime before opening it: an EMFILE then meant the empty spec was never read."""
        with tempfile.TemporaryDirectory() as folder:
            shim = Path(folder) / 'shim.so'
            subprocess.run(['gcc', '-O2', '-shared', '-fPIC', '-o', str(shim), str(ROOT / 'tools/lab/faults/faultshim.c'), '-ldl', '-lpthread'],
                           check=True, capture_output=True)
            spec, victim, child = Path(folder) / 'spec', Path(folder) / 'victim', Path(folder) / 'child.py'
            spec.write_text('seed 1\nrule write victim 1.0 ENOSPC\n')
            child.write_text(self.CHILD)
            python = os.path.realpath(sys.executable)
            env = dict(os.environ, LD_PRELOAD=str(shim), PUTGD_FAULT_SPEC=str(spec), PUTGD_FAULT_EXE=python)
            done = subprocess.run(['sh', '-c', 'PUTGD_FAULT_PID=$$; export PUTGD_FAULT_PID; exec "$0" "$@"', python, '-I', str(child), str(victim), str(spec)],
                                  env=env, capture_output=True, text=True, timeout=60)
        lines = done.stdout.splitlines()
        self.assertIn('faulted: No space left on device', lines, done.stdout + done.stderr)
        self.assertIn('after: ok', lines, done.stdout + done.stderr)


def fuzzer(**fields):
    fake = types.SimpleNamespace(log=lambda *a, **k: None, **fields)
    return fake


class Fuzzer(unittest.TestCase):
    def test_probe_recovery_gives_up_on_credentials_with_a_finding(self):
        """A probe that keeps needing credentials used to recurse until RecursionError."""
        row = {'operationId': 'op1', 'error': {'code': 'credential_setup_required'}}
        fake = fuzzer(profile='fuzz', receiver=types.SimpleNamespace(upload_events=lambda: 0), revision=lambda: 'r', new_upload_file=lambda name: Path(__file__),
                      call=lambda method, params=None, timeout=60: {'ok': True, 'result': {'operation': {'id': 'op1'}}},
                      drain=lambda seconds: True, rows=lambda: [row], credentials_check=lambda: None,
                      PROBE_CREDENTIAL_REFRESHES=2)
        fake.probe_recovery = lambda *args, **kwargs: action_fuzz.Fuzzer.probe_recovery(fake, *args, **kwargs)
        with self.assertRaises(action_fuzz.Finding) as caught:
            fake.probe_recovery()
        self.assertEqual(caught.exception.kind, 'not-recovered')

    def test_a_finding_keeps_the_s3_oracles_objects(self):
        """The saved receiver state used to leave out the S3 oracle's objects and versions."""
        with tempfile.TemporaryDirectory() as folder:
            state = {'events': [], 'objects': {'p/a.txt': [{'version': '1', 'blob': 'abc'}]}, 'uploads': {}}
            fake = fuzzer(run=Path(folder), receiver=types.SimpleNamespace(state=lambda: state))
            action_fuzz.Fuzzer.keep_receiver_state(fake)
            kept = json.loads((Path(folder) / 'receiver-state.json').read_text())
        self.assertEqual(kept['objects'], {'p/a.txt': [{'version': '1', 'marker': False, 'etag': None, 'size': None, 'sha256': 'abc'}]})

    def test_review_s4_large_s3_objects_are_saved_as_metadata_in_complete_json(self):
        """Three 6 MiB bodies inline used to hit the 20M-character cap and cut the JSON."""
        import base64
        import hashlib
        body = os.urandom(6 << 20)
        objects = {'p/%d.bin' % n: [{'version': str(n), 'body': base64.b64encode(body).decode(), 'etag': '"e%d"' % n, 'marker': False, 'metadata': {}}]
                   for n in range(3)}
        with tempfile.TemporaryDirectory() as folder:
            fake = fuzzer(run=Path(folder), receiver=types.SimpleNamespace(state=lambda: {'events': [], 'objects': objects, 'uploads': {}}))
            action_fuzz.Fuzzer.keep_receiver_state(fake)
            text = (Path(folder) / 'receiver-state.json').read_text()
        kept = json.loads(text)  # a complete document
        self.assertLess(len(text), 10000)
        self.assertEqual(kept['objects']['p/1.bin'], [{'version': '1', 'marker': False, 'etag': '"e1"', 'size': 6 << 20, 'sha256': hashlib.sha256(body).hexdigest()}])

    def test_review_s2_one_retry_may_resend_every_row_of_its_operation(self):
        """The same file twice in one call (two rows of one operation), retried once: four sends, so four copies may be stored."""
        fake = fuzzer(digest_rows={'d': {'row1', 'row2'}}, digest_operations={'d': {'op1'}}, row_operations={'row1': 'op1', 'row2': 'op1'}, seen={},
                      resident=types.SimpleNamespace(retries={'op1': 1}))
        self.assertEqual(action_fuzz.Fuzzer.allowed_copies(fake, 'd'), 4)

    def test_review_s3_only_an_accepted_retry_earns_credit(self):
        """A copy-only success of operation.retry sends nothing again; only result.retryAccepted earns a retry's extra copy."""
        import socket
        import threading
        def answer(result):
            with tempfile.TemporaryDirectory(dir='/tmp') as folder:
                path = os.path.join(folder, 's')
                server = socket.socket(socket.AF_UNIX)
                server.bind(path)
                server.listen(1)
                def serve():
                    connection, _ = server.accept()
                    request = json.loads(connection.makefile().readline())
                    connection.sendall(json.dumps({'schemaVersion': 1, 'requestId': request['requestId'], 'ok': True, 'result': result}).encode() + b'\n')
                    connection.close()
                thread = threading.Thread(target=serve)
                thread.start()
                resident = action_fuzz.Resident.__new__(action_fuzz.Resident)
                resident.socket_path, resident.lock, resident.sequence, resident.retries = path, threading.Lock(), 0, {}
                resident.call('operation.retry', {'id': 'op1'}, timeout=10, raw=True)
                thread.join()
                server.close()
                return resident.retries
        self.assertEqual(answer({'operationId': 'op1'}), {})  # copied, not re-sent
        self.assertEqual(answer({'operationId': 'op1', 'retryAccepted': True}), {'op1': 1})

    def test_an_unknown_row_without_a_send_allows_no_extra_copy(self):
        """One row, unknown once, retried once: two sends, so two copies at most (it used to allow three)."""
        fake = fuzzer(digest_rows={'d': {'row1'}}, digest_operations={'d': {'op1'}}, row_operations={'row1': 'op1'}, seen={'row1': {'unknown', 'failed'}},
                      resident=types.SimpleNamespace(retries={'op1': 1}))
        self.assertEqual(action_fuzz.Fuzzer.allowed_copies(fake, 'd'), 2)


class S3Holds(unittest.TestCase):
    def holds(self, versions):
        receiver = action_fuzz.S3Receiver.__new__(action_fuzz.S3Receiver)
        receiver.snapshot = lambda: {'p/a.txt': versions}
        row = {'receipt': {'objectReference': {'key': 'p/a.txt'}}}  # an unversioned receipt
        return receiver.holds(row, 'own', 'http://127.0.0.1:1')

    def test_an_unversioned_receipt_is_checked_against_what_an_unversioned_read_returns(self):
        own = {'version': '1', 'blob': 'own', 'marker': False}
        self.assertIsNone(self.holds([own]))
        self.assertIsNotNone(self.holds([own, {'version': '2', 'marker': True}]))  # the read finds a delete marker: 404
        self.assertIsNotNone(self.holds([own, {'version': 'competitor', 'blob': 'theirs', 'marker': False}]))  # the read returns theirs


class EmfileAbort(unittest.TestCase):
    def judge(self, text, windows, status=-5):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            (run / 'logs').mkdir()
            (run / 'logs' / 'resident-1.log').write_bytes(text)
            resident = action_fuzz.Resident.__new__(action_fuzz.Resident)
            resident.run, resident.starts, resident.descriptor_windows, resident.judged_logs = run, 1, {1: windows}, set()
            resident.alive, resident.returncode = (lambda: False), (lambda: status)
            fake = fuzzer(resident=resident, options=types.SimpleNamespace(memory_max=None), faults=None, GIVE_UP=getattr(action_fuzz.Fuzzer, 'GIVE_UP', ()))
            fake.exhaustion_ended_it = lambda text, start: action_fuzz.Fuzzer.exhaustion_ended_it(fake, text, start)
            return action_fuzz.Fuzzer.emfile_abort(fake)

    EXHAUSTED = b'(process:42): GLib-ERROR **: Creating pipes for GWakeup: Too many open files\n'

    def test_an_abort_while_descriptors_ran_out_is_excused(self):
        self.assertTrue(self.judge(b'started\n' + self.EXHAUSTED, [(0, None)]))  # GLib's g_error ends in a breakpoint trap (-5)
        qt = b'QEventDispatcherUNIXPrivate(): Can not continue without a thread pipe\n'
        self.assertTrue(self.judge(b'started\n' + qt, [(0, None)], status=-6))  # Qt's qFatal aborts (-6)

    def test_a_give_up_line_with_another_exit_excuses_nothing(self):
        self.assertFalse(self.judge(b'started\n' + self.EXHAUSTED, [(0, None)], status=-11))

    def test_review_s1_a_handled_emfile_warning_does_not_excuse_a_silent_abort(self):
        warning = b'QSocketNotifier: Can not create notifier: Too many open files\n'
        self.assertFalse(self.judge(b'started\n' + warning + b'recovered\n', [(0, None)], status=-6))

    def test_an_earlier_exhaustion_does_not_excuse_a_later_crash(self):
        text = b'started\n' + self.EXHAUSTED + b'restored\n' + b"terminate called after throwing an instance of 'std::logic_error'\n"
        self.assertFalse(self.judge(text, [(0, len(b'started\n') + len(self.EXHAUSTED))]))

    def test_an_exhaustion_outside_the_harness_windows_excuses_nothing(self):
        self.assertFalse(self.judge(b'started\n' + self.EXHAUSTED, [(0, 4)]))


if __name__ == '__main__':
    unittest.main()
