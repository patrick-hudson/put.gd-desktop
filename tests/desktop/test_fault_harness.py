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
                      row_retry_credit={'row1': 1, 'row2': 1}, resident=types.SimpleNamespace(retries={'op1': 1}))
        self.assertEqual(action_fuzz.Fuzzer.allowed_copies(bound(fake, *ACCOUNTING), 'd'), 4)

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
                      row_retry_credit={'row1': 1}, resident=types.SimpleNamespace(retries={'op1': 1}))
        self.assertEqual(action_fuzz.Fuzzer.allowed_copies(bound(fake, *ACCOUNTING), 'd'), 2)


def bound(fake, *names):
    """Bind the Fuzzer methods `names` to a fake, as far as the class has them (so a test also runs against an older harness)."""
    for name in names:
        if hasattr(action_fuzz.Fuzzer, name):
            setattr(fake, name, getattr(action_fuzz.Fuzzer, name).__get__(fake))
    return fake


ACCOUNTING = ('allowed_copies', 'row_allowance', 'refused', 'copies_explained_without_refused', 'credit_retry', 'lost_to_refusal')


class PerRowRecovery(unittest.TestCase):
    def recovery(self, rows, stored, retries=None, seen=None):
        receiver = types.SimpleNamespace(stored=lambda: stored, present=lambda: stored, snapshot=lambda: {}, remote=lambda row, files=None: None,
                                         names=lambda: {}, describe=lambda digest: '[]')
        fake = fuzzer(rows=lambda: rows, receiver=receiver, TERMINAL=action_fuzz.Fuzzer.TERMINAL, known_stuck=lambda row: False,
                      row_removed_by_delete=lambda row: False, seen=seen or {}, seen_digest={}, deleting=set(),
                      digest_rows={'d': {row['id'] for row in rows}}, digest_operations={'d': {row['operationId'] for row in rows}},
                      row_operations={row['id']: row['operationId'] for row in rows}, resident=types.SimpleNamespace(retries=retries or {}),
                      row_status={row['id']: row['uploadStatus'] for row in rows},
                      row_retry_credit={row['id']: (retries or {}).get(row['operationId'], 0) for row in rows if row['uploadStatus'] != 'uploaded'})
        return bound(fake, *ACCOUNTING)

    def test_review_s2_one_row_of_a_duplicate_batch_refused_is_no_lost_upload(self):
        """Same file twice in one call: one row uploaded, the other refused; the one copy on the server is the uploaded row's."""
        rows = [{'id': 'row1', 'operationId': 'op1', 'uploadStatus': 'uploaded', 'sha256': 'd', 'receipt': {}},
                {'id': 'row2', 'operationId': 'op1', 'uploadStatus': 'failed', 'sha256': 'd', 'receipt': {}}]
        action_fuzz.Fuzzer.verify_rows(self.recovery(rows, {'d': ['f1']}))  # no Finding

    def test_a_refused_row_whose_content_is_on_the_server_is_still_a_lost_upload(self):
        rows = [{'id': 'row1', 'operationId': 'op1', 'uploadStatus': 'failed', 'sha256': 'd', 'receipt': {}}]
        with self.assertRaises(action_fuzz.Finding) as caught:
            action_fuzz.Fuzzer.verify_rows(self.recovery(rows, {'d': ['f1']}))
        self.assertEqual(caught.exception.kind, 'lost-upload')

    def test_review_s2_an_unknown_row_retried_once_may_not_leave_three_copies(self):
        rows = [{'id': 'row1', 'operationId': 'op1', 'uploadStatus': 'unknown', 'sha256': 'd', 'receipt': {}}]
        fake = self.recovery(rows, {'d': ['f1', 'f2', 'f3']}, retries={'op1': 1}, seen={'row1': {'unknown'}})
        fake.seen_digest = {'d': {'unknown'}}
        fake.operations_to_retry = lambda: [{'id': 'op1'}]
        fake.call = lambda method, params=None, timeout=60: {'ok': True, 'result': {'operationId': 'op1', 'retryAccepted': True}}
        fake.drain = lambda seconds: True
        with self.assertRaises(action_fuzz.Finding) as caught:
            action_fuzz.Fuzzer.retry_interrupted(fake, {'d': 'unknown'}, {'d': 1})
        self.assertEqual(caught.exception.kind, 'duplicate-upload')
        fake.receiver.stored = lambda: {'d': ['f1', 'f2']}  # two copies: the first send and the retry
        action_fuzz.Fuzzer.retry_interrupted(fake, {'d': 'unknown'}, {'d': 1})

    def test_a_retry_never_earns_credit_for_a_row_that_was_already_uploaded(self):
        """An uploaded row plus an accepted retry of its operation that stores a second copy is the duplicate this harness exists for."""
        rows = [{'id': 'row1', 'operationId': 'op1', 'uploadStatus': 'uploaded', 'sha256': 'd', 'receipt': {}},
                {'id': 'row2', 'operationId': 'op1', 'uploadStatus': 'failed', 'sha256': 'e', 'receipt': {}}]
        fake = self.recovery(rows, {'d': ['f1', 'f2'], 'e': ['g1']}, retries={'op1': 1})
        if hasattr(fake, 'credit_retry'):  # absent in a harness without per-row credit, which counted the retry for every row
            fake.row_retry_credit = {}
            fake.credit_retry('op1')  # the retry is accepted while row1 is uploaded and row2 failed
            self.assertEqual(fake.row_retry_credit, {'row2': 1})
        fake.digest_rows = {'d': {'row1'}, 'e': {'row2'}}
        fake.operations_to_retry = lambda: [{'id': 'op1'}]
        fake.call = lambda method, params=None, timeout=60: {'ok': True, 'result': {'operationId': 'op1', 'retryAccepted': True}}
        fake.drain = lambda seconds: True
        with self.assertRaises(action_fuzz.Finding) as caught:
            action_fuzz.Fuzzer.retry_interrupted(fake, {'d': 'uploaded', 'e': 'failed'}, {'d': 1, 'e': 0})
        self.assertEqual(caught.exception.kind, 'duplicate-upload')
        self.assertIn(' d', ' ' + caught.exception.detail.split('stored content ')[1][:2])


    def test_review_s2_a_retried_row_still_failed_with_its_content_stored_is_a_lost_upload(self):
        """One row failed (never unknown, no receipt), retried once: the receiver now holds its content but History still says failed."""
        rows = [{'id': 'row1', 'operationId': 'op1', 'uploadStatus': 'failed', 'sha256': 'd', 'receipt': {}}]
        fake = self.recovery(rows, {'d': ['f1']}, retries={'op1': 1}, seen={'row1': {'failed'}})
        fake.operations_to_retry = lambda: [{'id': 'op1'}]
        fake.call = lambda method, params=None, timeout=60: {'ok': True, 'result': {'operationId': 'op1', 'retryAccepted': True}}
        fake.drain = lambda seconds: True
        with self.assertRaises(action_fuzz.Finding) as caught:
            action_fuzz.Fuzzer.retry_interrupted(fake, {'d': 'failed'}, {'d': 0})
        self.assertEqual(caught.exception.kind, 'lost-upload')

    def test_review_s5_a_status_read_by_the_history_action_stops_later_credit(self):
        """op1 has an unknown row (content d) and a failed one (e). A retry resolves the unknown row; the randomized History action is
        what lists it as uploaded; a later retry of the failed sibling sends nothing for d, so a third copy of d is a duplicate."""
        import random
        import socket
        import threading
        rows = {'row1': {'id': 'row1', 'operationId': 'op1', 'uploadStatus': 'unknown', 'sha256': 'd', 'receipt': {}},
                'row2': {'id': 'row2', 'operationId': 'op1', 'uploadStatus': 'failed', 'sha256': 'e', 'receipt': {}}}
        with tempfile.TemporaryDirectory(dir='/tmp') as folder:
            path = os.path.join(folder, 's')
            server = socket.socket(socket.AF_UNIX)
            server.bind(path)
            server.listen(8)

            def serve():
                while True:
                    try:
                        connection, _ = server.accept()
                    except OSError:
                        return
                    request = json.loads(connection.makefile().readline())
                    result = {'operationId': 'op1', 'retryAccepted': True} if request['method'] == 'operation.retry' else \
                        {'records': [dict(row) for row in rows.values()], 'total': len(rows)}
                    connection.sendall(json.dumps({'schemaVersion': 1, 'requestId': request['requestId'], 'ok': True, 'result': result}).encode() + b'\n')
                    connection.close()
            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            resident = action_fuzz.Resident.__new__(action_fuzz.Resident)
            resident.socket_path, resident.lock, resident.sequence, resident.retries = path, threading.Lock(), 0, {}
            fake = fuzzer(resident=resident, seen={}, seen_digest={}, digest_rows={}, digest_operations={}, row_operations={}, row_status={},
                          row_retry_credit={}, history_ids=[], rng=random.Random(0))
            bound(fake, *ACCOUNTING, 'note_rows')
            resident.on_retry = fake.credit_retry
            resident.on_rows = getattr(fake, 'note_rows', None)
            fake.call = lambda method, params=None, timeout=60: resident.call(method, params, timeout=timeout, raw=True)
            # The rows as the harness first lists them (rows() in a real run; the same reply path here).
            if not hasattr(fake, 'note_rows'):  # a harness without the shared hook updated its caches in rows() only
                action_fuzz.Fuzzer.rows(bound(fake, 'rows'))
            else:
                fake.call('history.list', {'limit': 200, 'offset': 0})
            fake.call('operation.retry', {'id': 'op1'})  # resolves row1 and leaves row2 failed
            rows['row1']['uploadStatus'] = 'uploaded'
            fake.rng.choice = lambda values: 'history.list' if 'history.counts' in values else values[0]  # the History action lists rows
            fake.rng.random = lambda: 1.0
            action_fuzz.Fuzzer.a_history(fake)
            self.assertEqual(fake.row_status.get('row1'), 'uploaded')
            fake.call('operation.retry', {'id': 'op1'})  # for the failed sibling
            server.close()
        self.assertEqual(fake.row_retry_credit, {'row1': 1, 'row2': 2})
        self.assertEqual(action_fuzz.Fuzzer.allowed_copies(fake, 'd'), 2)  # first send and the first retry; the third copy is a duplicate

class ReceiverSnapshot(unittest.TestCase):
    def keep(self, state):
        folder = tempfile.mkdtemp()
        fake = fuzzer(run=Path(folder), receiver=types.SimpleNamespace(state=lambda: state))
        action_fuzz.Fuzzer.keep_receiver_state(fake)
        return Path(folder)

    def test_review_s4_request_events_are_saved_without_their_bodies(self):
        import base64
        body = base64.b64encode(os.urandom(1 << 20)).decode()
        events = [{'method': 'PUT', 'path': '/isolated-fixture/p/%d.bin' % n, 'body': body, 'bodyBytes': 1 << 20, 'bodySha256': 'sha%d' % n} for n in range(16)]
        folder = self.keep({'events': events, 'objects': {}, 'uploads': {}})
        text = (folder / 'receiver-state.json').read_text()
        kept = json.loads(text)
        self.assertLess(len(text), 20000)
        self.assertEqual(kept['events'][3], {'method': 'PUT', 'path': '/isolated-fixture/p/3.bin', 'bodyBytes': 1 << 20, 'bodySha256': 'sha3'})

    def test_a_failed_write_leaves_no_file(self):
        class Unwritable:
            def __str__(self):
                raise ValueError('cannot be written')
        folder = self.keep({'events': [{'method': 'PUT', 'note': 'x' * 100000}, {'method': 'PUT', 'odd': Unwritable()}], 'objects': {}, 'uploads': {}})
        self.assertEqual(sorted(p.name for p in folder.iterdir()), [])


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
