"""Real AF_UNIX backlog regressions for surface input and endpoint checks."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
from unittest.mock import patch

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.lab import lab
from tools.lab.surface_driver.run import verify_guest_endpoint


@contextmanager
def full_listener(*, drain=True, respond=True, drop_input_reply=False):
    # Linux permits one queued connection with backlog=0. QEMU's single
    # monitor can similarly lag behind short-lived peer-check connections.
    with tempfile.TemporaryDirectory(prefix='sd-qmp-') as directory, \
         socket.socket(socket.AF_UNIX) as listener, socket.socket(socket.AF_UNIX) as queued:
        path = Path(directory) / 'qmp.sock'
        listener.bind(str(path))
        listener.listen(0)
        listener.settimeout(1)
        queued.connect(str(path))
        congested = threading.Event()
        attempts, requests, errors = [], [], []

        class Client(socket.socket):
            def connect(self, address):
                attempts.append(self)
                try:
                    return super().connect(address)
                except BlockingIOError:
                    congested.set()
                    raise

        def serve():
            try:
                if not congested.wait(1):
                    raise AssertionError('client never encountered the full real socket queue')
                if not drain:
                    return
                with listener.accept()[0]:
                    pass
                with listener.accept()[0] as peer:
                    if not respond:
                        return
                    peer.sendall(b'{"QMP":{"version":{}}}\n')
                    with peer.makefile('rb') as stream:
                        for line in stream:
                            request = json.loads(line)
                            requests.append(request)
                            if drop_input_reply and request['execute'] == 'input-send-event':
                                return
                            peer.sendall(json.dumps({'return': {}, 'id': request['id']}).encode()+b'\n')
            except BaseException as error:
                errors.append(error)

        worker = threading.Thread(target=serve)
        worker.start()
        try:
            with patch.object(lab.socket, 'socket', Client):
                yield path, attempts, requests, congested
        finally:
            worker.join(3)
            assert not worker.is_alive()
            # A baseline client fails at connect, before the server's accept.
            if errors and sys.exc_info()[0] is None:
                raise errors[0]


def test_surface_qmp_retries_full_unix_backlog_before_dispatch():
    with full_listener() as (path, attempts, requests, congested):
        with lab.QMP(path, timeout=1) as qmp:
            qmp.execute('input-send-event', {'events': [
                {'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': 'shift'}}}]})
        assert congested.is_set()
        assert len(attempts) >= 2
        assert [r['execute'] for r in requests] == ['qmp_capabilities', 'input-send-event']
        assert all(peer.fileno() == -1 for peer in attempts)


def test_surface_endpoint_check_retries_same_full_backlog():
    with full_listener(respond=False) as (path, attempts, _, congested):
        pid = os.getpid()
        verify_guest_endpoint(SimpleNamespace(run=path.parent), pid,
                              Path(f'/proc/{pid}/cmdline').read_bytes())
        assert congested.is_set()
        assert len(attempts) >= 2
        assert all(peer.fileno() == -1 for peer in attempts)


def test_surface_qmp_full_backlog_deadline_closes_every_attempt():
    with full_listener(drain=False) as (path, attempts, requests, congested):
        start = time.monotonic()
        with pytest.raises(TimeoutError):
            lab.QMP(path, timeout=.05)
        assert time.monotonic() - start < 1
        assert congested.is_set()
        assert len(attempts) >= 2
        assert not requests
        assert all(peer.fileno() == -1 for peer in attempts)


def test_surface_qmp_never_replays_input_after_uncertain_dispatch():
    with full_listener(drop_input_reply=True) as (path, attempts, requests, _):
        with lab.QMP(path, timeout=1) as qmp:
            with pytest.raises(lab.LabError, match='QMP closed'):
                qmp.execute('input-send-event', {'events': []})
        assert [r['execute'] for r in requests] == ['qmp_capabilities', 'input-send-event']
        assert all(peer.fileno() == -1 for peer in attempts)


def test_surface_qmp_failed_handshake_closes_connection():
    with full_listener(respond=False) as (path, attempts, _, _):
        with pytest.raises(lab.LabError, match='QMP closed'):
            lab.QMP(path, timeout=1)
        assert all(peer.fileno() == -1 for peer in attempts)


if __name__ == '__main__':
    raise SystemExit(pytest.main([__file__]))
