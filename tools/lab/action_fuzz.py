#!/usr/bin/env python3
"""Seeded random action-sequence fuzzer for a sanitizer build of the put.gd resident.

It starts the resident headlessly (offscreen Qt, a private session bus, private XDG roots), points it at a
loopback destination (the custom HTTP uploader fixture, tests/fixtures/http_uploader_server.py, or put.gd's S3 provider on the offline
oracle, tests/fixtures/s3_server.py) and drives its session socket directly.

  run    long random sequences of session methods: fixtures, editor sessions and commands, uploads (also of folders, FIFOs,
         links, vanishing files), History, deletion, operation control, settings, malformed and oversized calls, concurrent
         bursts, outside edits of config.json, an unwritable archive, helpers killed or frozen on their own, graceful restarts
  crash  SIGKILL of the resident and its helpers at random points of uploads, deletions and settings writes (also a second
         kill during the recovery), then a restart and a check that it recovered
  faults the OS and the network say no underneath the resident: an LD_PRELOAD shim (tools/lab/faults/faultshim.c) fails writes,
         fsyncs, renames and opens with ENOSPC, EIO, EACCES, EROFS, EMFILE, short writes and a slow disk on the files of the
         resident's roots; a TCP proxy (tools/lab/faults/fault_proxy.py) between it and the destination resets, cuts, stalls,
         duplicates, throttles and delays; the open-file limit drops under it. A full disk is a shim rule (ENOSPC, EDQUOT): the
         harness mounts and fills nothing. Every few steps the faults are lifted and the whole state is checked

After every step it checks that the resident lives and the sanitizers wrote nothing; every few steps that History and the
settings read back consistently; after every kill that config.json parses, history.sqlite passes its integrity check,
no operation or History row is stuck, no upload is on the server twice or unknown to History, and deletions agree on both
sides. Run it inside a private bus (the whole process tree stays off a desktop):

  dbus-run-session -- python3 -I tools/lab/action_fuzz.py --build build/clang-asan --seed 1 --steps 500 run
  dbus-run-session -- python3 -I tools/lab/action_fuzz.py --build build/clang-asan --seed 1 --steps 60 --destination s3 crash

The seed and every step are printed (and kept in <run-dir>/steps.jsonl), so a failure replays with the same seed and --steps
(timing still varies). A finding stops the run and is kept in <run-dir>/finding.json with the sanitizer logs and, for a
hang, the stacks of every process. Exit 0: clean, 1: a finding. Put --run-dir on disk, not on tmpfs: a run writes copies of
its payloads. Synthetic data and loopback receivers only: no cloud account, no keyring, no Proton.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import random
import secrets
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent / 'faults'))  # python -I does not put the script's folder on the path
import fault_control  # noqa: E402

SOCKET_LIMIT = 107
# Every wait scales with it: rr record --chaos runs several times slower than a plain run (ACTION_FUZZ_SLOW=5), ThreadSanitizer about three.
SLOW = float(os.environ.get('ACTION_FUZZ_SLOW', '1'))
TOKEN = 'putgd-synthetic-uploader-token-not-real'
# What a clean sanitizer run never writes: any of these in a log is a finding.
BAD_MARKERS = ('AddressSanitizer', 'LeakSanitizer', 'runtime error:', 'ThreadSanitizer', 'UndefinedBehaviorSanitizer',
               'Assertion', 'terminate called', 'Segmentation fault', 'QFATAL', 'Fatal:', 'double free', 'SUMMARY:')
# Resident states that are not an answer to the call but the resident's own.
BUSY_CODES = ('initializing', 'journal_busy')
# A journal recovery that failed on an I/O error is retried by the resident (#1576) and answers with that error meanwhile.
RETRYING_CODES = ('history_io',)


class Finding(Exception):
    def __init__(self, kind, detail):
        super().__init__('%s: %s' % (kind, detail))
        self.kind, self.detail = kind, detail


# ---------------------------------------------------------------------------------------------- fixtures

def png(width, height, seed=0):
    rng = random.Random(seed)
    rows = b''.join(b'\x00' + bytes((x * 255 // max(width, 1) + rng.randrange(8)) & 255 if c == 0 else (y * 255 // max(height, 1)) & 255 if c == 1 else (x ^ y) & 255
                                    for x in range(width) for c in range(3)) for y in range(height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


PNG_SIZES = {'small.png': (320, 200), 'tiny.png': (3, 3), 'wide.png': (600, 30), 'tall.png': (30, 600), 'big.png': (700, 500),
             'unicode \u00e9\u4e2d \u202e.png': (320, 200)}


def make_fixtures(folder, rng):
    """Fixture files: (name -> bytes). Unique nonce per use is added by the caller for uploads."""
    folder.mkdir(parents=True, exist_ok=True)
    good = png(320, 200, 1)
    files = {
        'small.png': good,
        'tiny.png': png(1, 1),
        'wide.png': png(1200, 40, 2),
        'tall.png': png(40, 1200, 3),
        'big.png': png(1600, 1200, 4),
        'notes.txt': b'hello put.gd\n' * 50,
        'empty.bin': b'',
        'truncated.png': good[:len(good) // 2],
        'garbage.png': bytes(rng.randrange(256) for _ in range(5000)),
        'fake.jpg': b'\xff\xd8\xff\xe0' + bytes(rng.randrange(256) for _ in range(900)),
        'payload.bin': os.urandom(1024 * 1024),
        # Large enough that an upload is still in flight when the next steps arrive.
        'large.bin': os.urandom(6 * 1024 * 1024),
        # A name the file manager would never send is still a valid path.
        'unicode é中 ‮.png': good,
    }
    paths = {}
    for name, data in files.items():
        path = folder / name
        path.write_bytes(data)
        paths[name] = path
    return paths


# ---------------------------------------------------------------------------------------------- receiver

class Receiver:
    """The loopback upload receiver (the unit tests' fixture) in its own process, alive across resident restarts."""

    def __init__(self, source, run):
        self.process = subprocess.Popen([sys.executable, '-I', str(source)], stdout=subprocess.PIPE, stderr=open(run / 'receiver.log', 'w'),
                                        stdin=subprocess.DEVNULL, start_new_session=True)
        try:
            self.port = int(self.process.stdout.readline())
        except BaseException:  # an interrupt or a broken start: the receiver is in its own session, so it is stopped here
            self.process.kill()
            self.process.wait(5)
            raise
        self.base = 'http://127.0.0.1:%d' % self.port

    def post(self, path, body):
        request = urllib.request.Request(self.base + path, data=json.dumps(body).encode(), method='POST')
        with urllib.request.urlopen(request, timeout=10) as reply:
            return reply.read()

    def state(self):
        with urllib.request.urlopen(self.base + '/__state', timeout=10) as reply:
            return json.loads(reply.read())

    def reset(self, **options):
        self.post('/__reset', options)

    def rules(self, rules):
        self.post('/__rules', {'rules': rules})

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(5)

    # What the invariants ask of any receiver (the S3 one below answers the same questions).
    def upload_events(self):
        return len([e for e in self.state()['events'] if e.get('call') == 'upload'])

    def delete_events(self):
        return len([e for e in self.state()['events'] if e.get('call') == 'delete'])

    def stored(self):
        """content sha256 -> the receiver's ids for each copy it holds (or held and deleted)."""
        found = {}
        for event in self.state()['events']:
            if event.get('call') == 'upload' and event.get('stored'):
                found.setdefault(event['sha256'], []).append(event['stored'])
        return found

    def names(self):
        """content sha256 -> the file name the receiver was given for it."""
        return {e['sha256']: e.get('fileName') for e in self.state()['events'] if e.get('call') == 'upload' and e.get('stored')}

    def remote(self, row, files=None):
        """'present', 'gone' or None for a History row's file."""
        direct = ((row.get('receipt') or {}).get('directUrl') or '').split('/')
        file_id = direct[4] if len(direct) > 4 and direct[3] == 'f' else None
        entry = (files if files is not None else self.state()['files']).get(file_id) if file_id else None
        if entry is None:
            return None
        return 'gone' if entry.get('deleted') else 'present'

    def snapshot(self):
        return self.state()['files']

    def present(self):
        """content sha256 -> the receiver's ids of the copies it holds now (not deleted)."""
        found = {}
        for file_id, entry in self.state()['files'].items():
            if not entry.get('deleted'):
                found.setdefault(entry.get('sha256'), []).append(file_id)
        return found

    def holds(self, row, digest, base):
        """None when the file the row's receipt names is on this receiver (at `base`, the address put.gd was given) with exactly this
        content, else what is wrong."""
        url = (row.get('receipt') or {}).get('directUrl') or ''
        direct = url.split('/')
        file_id = direct[4] if len(direct) > 4 and direct[3] == 'f' else None
        if not file_id or '/'.join(direct[:3]) != base.rstrip('/'):
            return 'the receipt does not name a file at %s (%s)' % (base, url)
        entry = self.state()['files'].get(file_id)
        if entry is None or entry.get('deleted'):
            return 'the receiver has no file %s' % file_id
        if entry.get('sha256') != digest:
            return 'the receiver\'s file %s has content %s, not the probe\'s %s' % (file_id, str(entry.get('sha256'))[:12], digest[:12])
        return None

    def describe(self, digest):
        return json.dumps([{k: e.get(k) for k in ('fileName', 'fileBytes', 'sha256', 'stored')} for e in self.state()['events'] if e.get('sha256') == digest])

    def random_rule(self, rng):
        return rng.choice(({'call': 'upload', 'status': 500}, {'call': 'upload', 'status': 503, 'store': True}, {'call': 'upload', 'drop': True},
                           {'call': 'upload', 'drop': True, 'store': True}, {'call': 'upload', 'cut': 'length'}, {'call': 'upload', 'cut': 'chunk'},
                           {'call': 'upload', 'early': True, 'status': 413}, {'call': 'delete', 'status': 500}, {'call': 'delete', 'drop': True},
                           {'call': 'file', 'status': 404}))

    def credentials_after_restart(self, fuzzer):
        hint = fuzzer.resident.call('setup.credentialHint', {'profileId': fuzzer.profile, 'expectedRevision': fuzzer.revision()}, raw=True)
        if (hint.get('result') or {}).get('missingValues'):
            return {'values': {'headers/Authorization': TOKEN}}
        return None


class S3Receiver(Receiver):
    """The offline S3 wire oracle (tests/fixtures/s3_server.py): put.gd's own S3 provider, with conditional create and multipart."""

    def __init__(self, source, run):
        super().__init__(source, run)

    def reset(self, **options):
        self.post('/__reset', options)

    def upload_events(self):
        return len([e for e in self.state()['events'] if e['method'] == 'PUT' or (e['method'] == 'POST' and 'uploads' in e['path'])])

    def delete_events(self):
        return len([e for e in self.state()['events'] if e['method'] == 'DELETE' and 'uploadId' not in e['path']])

    @staticmethod
    def sha(version):
        if version.get('blob'):
            return version['blob']
        return hashlib.sha256(base64.b64decode(version.get('body') or '')).hexdigest()

    def stored(self):
        found = {}
        for key, versions in self.state()['objects'].items():
            for version in versions:
                # The oracle's "competitor" rule plants a third party's object under the key: not one of ours.
                if not version.get('marker') and version.get('version') != 'competitor':
                    found.setdefault(self.sha(version), []).append(key)
        return found

    def remote(self, row, files=None):
        key = ((row.get('receipt') or {}).get('objectReference') or {}).get('key')
        if not key:
            return None
        versions = [v for v in (files if files is not None else self.snapshot()).get(key, []) if not v.get('marker')]
        return 'present' if versions else 'gone'

    def snapshot(self):
        return self.state()['objects']

    def present(self):
        return self.stored()  # the oracle's objects are what it holds now

    def holds(self, row, digest, base):
        reference = (row.get('receipt') or {}).get('objectReference') or {}
        key, version_id = reference.get('key'), reference.get('versionId')
        if not key:
            return 'the receipt names no object key'
        versions = [v for v in self.snapshot().get(key, []) if not v.get('marker') and v.get('version') != 'competitor']
        if version_id:  # a versioned receipt names one version: that one must hold the bytes
            versions = [v for v in versions if v.get('version') == version_id]
        if not versions:
            return 'the oracle has no object %s%s' % (key, ' version %s' % version_id if version_id else '')
        if self.sha(versions[-1]) != digest:
            return 'the object %s has content %s, not the probe\'s %s' % (key, self.sha(versions[-1])[:12], digest[:12])
        return None

    def names(self):
        found = {}
        for key, versions in self.state()['objects'].items():
            for version in versions:
                if not version.get('marker') and version.get('version') != 'competitor':
                    found[self.sha(version)] = key.rsplit('/', 1)[-1]
        return found

    def describe(self, digest):
        return json.dumps({'keys': self.stored().get(digest), 'events': [(e['method'], e['path'][-60:], e.get('bodyBytes')) for e in self.state()['events'] if e.get('bodySha256') == digest][:8]})

    def incomplete(self):
        return len(self.state()['uploads'])

    def random_rule(self, rng):
        return rng.choice(({'method': 'PUT', 'status': 500}, {'method': 'PUT', 'status': 503, 'code': 'SlowDown'}, {'method': 'PUT', 'drop': True},
                           {'method': 'PUT', 'query': 'partNumber', 'drop': True}, {'method': 'PUT', 'query': 'partNumber', 'status': 500},
                           {'method': 'POST', 'query': 'uploads', 'status': 500}, {'method': 'POST', 'query': 'uploadId', 'status': 500},
                           {'method': 'POST', 'query': 'uploadId', 'drop': True}, {'method': 'DELETE', 'status': 500}, {'method': 'DELETE', 'drop': True},
                           {'method': 'HEAD', 'status': 500}, {'method': 'PUT', 'delay': 1.0}, {'method': 'PUT', 'competitor': True}))

    def credentials_after_restart(self, fuzzer):
        return {'accessKey': 'PUTGD_SYNTHETIC_ACCESS', 'secretKey': 'putgd-synthetic-secret-not-a-real-key'}


# ---------------------------------------------------------------------------------------------- resident

class Resident:
    def __init__(self, run, build, runtime=None, options=None):
        self.run, self.build = run, Path(build)
        self.options = options
        self.wrapped = bool(options is not None and options.wrap)  # under a recorder such as rr: self.process is the recorder
        self.root = run / 'r'
        self.runtime = Path(runtime) if runtime else self.root / 'rt'
        self.socket_path = str(self.runtime / 'putgd' / 'session.sock')
        if len(self.socket_path.encode()) > SOCKET_LIMIT:
            raise SystemExit('socket path %s is %d bytes (limit %d): use a shorter --run-dir' % (self.socket_path, len(self.socket_path.encode()), SOCKET_LIMIT))
        self.san = run / 'san'
        self.process = None
        self.pidfd = None     # a pidfd of the current resident, opened before it can be reaped: signals never reach a reused PID
        # Every process of this run's resident carries this in its environment (helpers inherit it). A PID is acted on only after a pidfd
        # is open on it and its environment shows the token, so a reused PID or session ID is never mistaken for one of ours.
        self.token = 'PUTGD_FUZZ_RUN=%s' % secrets.token_hex(12)
        self.pids_seen = set()   # every PID of the resident itself, for the shim's no-helper check
        self.retries = {}        # operation id -> accepted operation.retry calls
        self.extra_env = {}   # fault mode: the LD_PRELOAD shim and its rule file
        # Fault mode: start (self.starts) -> [(first byte, end byte or None)] of its log written while the harness held the resident at its
        # open-file limit on purpose. Only there is UBSan's invalid-vptr report expected.
        self.descriptor_windows = {}
        self.judged_logs = set()  # resident logs of an abort the harness caused on purpose (the open-file limit) and judged by its recovery
        self.memory_max = None  # fault mode: a memory cgroup limit (systemd-run --user --scope)
        self.nofile = None    # fault mode: an open-file limit from the start
        self.starts = 0
        self.sequence = 0
        self.lock = threading.Lock()

    def environment(self):
        for name in ('config', 'data', 'state', 'cache', 'home'):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        self.runtime.mkdir(parents=True, exist_ok=True)
        os.chmod(self.runtime, 0o700)
        self.san.mkdir(exist_ok=True)
        env = {k: os.environ[k] for k in ('PATH', 'DBUS_SESSION_BUS_ADDRESS', 'LANG', 'XDG_DATA_DIRS') if k in os.environ}
        env.update(self.extra_env)
        env[self.token.split('=', 1)[0]] = self.token.split('=', 1)[1]
        env.update(HOME=str(self.root / 'home'), XDG_CONFIG_HOME=str(self.root / 'config'), XDG_DATA_HOME=str(self.root / 'data'),
                   XDG_STATE_HOME=str(self.root / 'state'), XDG_CACHE_HOME=str(self.root / 'cache'), XDG_RUNTIME_DIR=str(self.runtime),
                   QT_QPA_PLATFORM='offscreen', PUTGD_STARTED_FOR_UPLOAD='1',
                   ASAN_OPTIONS='halt_on_error=1:print_stacktrace=1:detect_leaks=1:detect_stack_use_after_return=1:strict_init_order=1:abort_on_error=0:log_path=%s/asan%s' % (
                       self.san, ':verify_asan_link_order=0' if 'LD_PRELOAD' in self.extra_env else ''),
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:log_path=%s/ubsan' % self.san,
                   LSAN_OPTIONS='log_path=%s/lsan' % self.san)
        options = self.options
        if options is not None and options.tsan_prefix:
            # ThreadSanitizer build against the instrumented Qt and D-Bus (docs/testing/thread-sanitizer.md): the libraries come from the
            # test prefix, and reports go to files next to the ASan ones so check_sanitizers sees them. No suppressions file: every report
            # is kept, and tools/lab/tsan_summary.py separates verified Qt-only pairs from the rest.
            prefix = Path(options.tsan_prefix)
            env['LD_LIBRARY_PATH'] = ':'.join(str(p) for p in (prefix / 'deps-tsan' / 'lib', prefix / 'qtbase-tsan-build' / 'lib'))
            env['PATH'] = '%s:%s' % (prefix / 'deps-tsan' / 'bin', env['PATH'])
            suppressions = Path(__file__).resolve().parents[2] / 'tests/sanitizer/tsan.supp'
            env['TSAN_OPTIONS'] = 'halt_on_error=0:ignore_noninstrumented_modules=1:history_size=5:second_deadlock_stack=1:suppressions=%s:log_path=%s/tsan' % (suppressions, self.san)
            env['QT_PLUGIN_PATH'] = str(prefix / 'qtbase-tsan-build' / 'plugins')
            # GStreamer's plugin scanner is a distribution binary that would load the instrumented GLib without libtsan and crash.
            env['GST_REGISTRY_FORK'] = 'no'
        for assignment in (options.env if options is not None else []):
            name, _, value = assignment.partition('=')
            env[name] = value
        return env

    def launch(self, log):
        """Popen the resident, wrapped for a memory cgroup limit or an open-file limit when a fault mode asks for one."""
        if self.process is not None and self.returncode() is not None:
            self.reap()  # the previous resident is gone and its session was cleared: release its PID now
        command, env = [str(self.build / 'putgd'), '--background'], self.environment()
        if 'PUTGD_FAULT_EXE' in env:
            # The shim binds only to the PID this marker names; sh sets it to its own PID and execs the resident in place, so it is exact.
            command = ['/bin/sh', '-c', 'PUTGD_FAULT_PID=$$; export PUTGD_FAULT_PID; exec "$0" "$@"'] + command
        if self.nofile:
            command = ['prlimit', '--nofile=%d:%d' % (self.nofile, self.nofile), '--'] + command
        if self.wrapped:
            command = self.options.wrap.split() + command  # a recorder such as `rr record --chaos`: the resident is its tracee
        if self.options is not None and self.options.tsan_prefix:
            command = ['setarch', 'x86_64', '-R'] + command  # GCC's TSan runtime aborts on the host's address randomization
        if self.memory_max:
            # A scope of the real user manager: it needs the real user bus, and the resident its private one, set by env(1) inside.
            inner = ['env'] + ['%s=%s' % (k, v) for k, v in env.items()]
            uid = os.getuid()
            command = ['systemd-run', '--user', '--scope', '--quiet', '-p', 'MemoryMax=%s' % self.memory_max, '-p', 'MemorySwapMax=0', '--'] + inner + command
            env = {'PATH': os.environ.get('PATH', ''), 'XDG_RUNTIME_DIR': '/run/user/%d' % uid, 'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/run/user/%d/bus' % uid}
        # The process and its pidfd are published together: SIGINT and SIGTERM wait until both are set, so the cleanup path never sees a
        # resident without its pidfd, or with the previous one's.
        with signals_deferred():
            process = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                pidfd = os.pidfd_open(process.pid)  # not reaped yet, so the PID is still this process: the pidfd names it for good
            except OSError:
                pidfd = None
            previous, self.process, self.pidfd = self.pidfd, process, pidfd
            if previous is not None:
                os.close(previous)
            self.pids_seen.add(process.pid)
        return process

    def opened_log(self):
        if self.nofile:
            self.descriptor_windows[self.starts] = [(0, None)]  # started at its limit: the whole start is one window

    def start_nowait(self):
        """Start the resident and return at once, for a kill during its recovery."""
        self.starts += 1
        self.opened_log()
        log = open(self.run / 'logs' / ('resident-%d.log' % self.starts), 'w')
        self.process = self.launch(log)

    def start(self):
        if self.alive():
            raise Finding('driver', 'start with a live resident')
        self.starts += 1
        self.opened_log()
        log = open(self.run / 'logs' / ('resident-%d.log' % self.starts), 'w')
        self.process = self.launch(log)
        deadline = time.monotonic() + 90 * SLOW
        while time.monotonic() < deadline:
            if self.returncode() is not None:
                raise Finding('startup', 'the resident exited %s while starting' % self.returncode())
            try:
                reply = self.call('capabilities', timeout=10, raw=True)
                if reply.get('ok'):
                    break
            except (OSError, ValueError, Finding):
                pass  # still starting: the socket may exist and not answer yet
            time.sleep(0.3)
        else:
            raise Finding('hang', 'the resident did not answer capabilities within 90 s')
        # "initializing" resolves on its own; wait for a settled snapshot.
        end = time.monotonic() + 60 * SLOW
        while time.monotonic() < end:
            reply = self.call('settings.get', raw=True)
            if reply.get('ok'):
                return
            if (reply.get('error') or {}).get('code') not in BUSY_CODES:
                return
            time.sleep(0.3)

    def call(self, method, params=None, timeout=60, raw=False):
        """One request on its own connection. Returns the reply dict (raw) or its result, raising Finding on a hang or a protocol break."""
        with self.lock:
            self.sequence += 1
            request_id = 'f%d' % self.sequence
        request = json.dumps({'schemaVersion': 1, 'requestId': request_id, 'session': self.socket_path, 'method': method,
                              'params': {} if params is None else params}, ensure_ascii=False).encode() + b'\n'
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(timeout * SLOW)
        try:
            client.connect(self.socket_path)
            client.sendall(request)
            data = b''
            while not data.endswith(b'\n'):
                chunk = client.recv(1 << 20)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            raise Finding('hang', '%s got no reply in %s s' % (method, timeout))
        finally:
            client.close()
        reply = json.loads(data) if data else None
        if reply is None:
            raise OSError('no reply to %s (connection closed)' % method)
        if reply.get('requestId') != request_id or reply.get('schemaVersion') != 1:
            raise Finding('protocol', 'reply to %s has a wrong envelope: %s' % (method, str(reply)[:200]))
        # A retry with scope "sharing" only finishes share links; it sends nothing again.
        if method == 'operation.retry' and reply.get('ok') and isinstance(params, dict) and isinstance(params.get('id'), str) and 'scope' not in params:
            with self.lock:  # every accepted retry, from any step or burst thread: each may send its files once more
                self.retries[params['id']] = self.retries.get(params['id'], 0) + 1
        return reply if raw else reply.get('result')

    # The resident is reaped only once its session has been cleared (kill_tree, quit, the next launch). Until then its PID, which is also
    # the session ID of every helper, stays taken (a zombie holds it), so a session match cannot be a reused number.

    def open_late(self):
        """Open the resident's pidfd if launch could not (it is not reaped before its session is clear, so its PID is still its own)."""
        if self.pidfd is None and self.process is not None and self.process.returncode is None:
            try:
                self.pidfd = os.pidfd_open(self.process.pid)
            except OSError as error:
                self.pidfd_error = error
        return self.pidfd is not None

    def handle(self):
        """The resident's pidfd. Without one (both opens failed, say on EMFILE) the harness cannot tell its resident from a reused PID
        without reaping it, so it stops: a harness error, never a fallback to poll(), wait() or bare-PID signals."""
        if self.process is not None and self.process.returncode is None and not self.open_late():
            raise Finding('driver', 'no pidfd for the resident (pid %d): %s; the run stops' % (self.process.pid, getattr(self, 'pidfd_error', '?')))
        return self.pidfd

    def returncode(self):
        """The resident's exit status as Popen reports it (negative: the signal), or None while it runs. Never reaps it."""
        if self.process is None or self.process.returncode is not None:
            return None if self.process is None else self.process.returncode
        self.handle()
        try:
            info = os.waitid(os.P_PIDFD, self.pidfd, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        except ChildProcessError:
            return self.process.poll()
        if info is None:
            return None
        return -info.si_status if info.si_code in (os.CLD_KILLED, os.CLD_DUMPED) else info.si_status

    def alive(self):
        return self.process is not None and self.returncode() is None

    def wait_exit(self, seconds):
        """Wait for the resident to exit, without reaping it. True when it has."""
        if self.process is None or self.returncode() is not None:
            return True
        select.select([self.pidfd], [], [], seconds)  # a pidfd turns readable when its process exits
        return self.returncode() is not None

    def reap(self):
        if self.process is not None and self.process.returncode is None and self.pidfd is not None:  # without a pidfd it stays unreaped
            self.process.wait()

    def kill_pinned_session(self, sig):
        """No pidfd: SIGKILL the resident's process group by its ID. The resident leads that group and is not reaped, so the ID is still
        its own; nothing is reaped afterwards, and the run stops with a harness error."""
        try:
            os.killpg(self.process.pid, sig)
        except OSError:
            pass
        raise Finding('driver', 'no pidfd for the resident (pid %d): its process group was killed and the run stops' % self.process.pid)

    def owned(self, pid):
        """A pidfd on `pid` if that process belongs to this run's resident (its environment carries the run token), else None.
        The pidfd is opened first, then the environment read: a PID that was reused before the open fails the token check, and a
        signal sent through the pidfd can only reach the process that was checked."""
        try:
            fd = os.pidfd_open(pid)
        except OSError:
            return None
        try:
            environ = Path('/proc/%d/environ' % pid).read_bytes()
        except PermissionError:
            environ = None  # a helper that made itself non-dumpable (the OCR helper, the clipboard keeper) hides its environment
        except OSError:
            environ = b''
        if environ is None:
            ours = self.session_pinned() and self.session_of(pid) == self.process.pid
        else:
            ours = self.token.encode() in environ.split(b'\0')
        if not ours:
            os.close(fd)
            return None
        return fd

    def session_pinned(self):
        """True while the resident is not reaped: its PID, the session ID, cannot belong to anyone else."""
        return self.process is not None and self.process.returncode is None

    @staticmethod
    def session_of(pid):
        try:
            stat = Path('/proc/%d/stat' % pid).read_text()
            return int(stat[stat.rindex(')') + 2:].split()[3])
        except (OSError, ValueError, IndexError):
            return None

    def children(self):
        """PIDs of the resident and everything it started, from /proc: children of every thread (a worker thread's child is not listed under the
        main thread), and every process left in the resident's session, such as a vfork-fallback child that waits for a parent that is gone.
        Only processes that carry the run token are listed."""
        if not self.alive():
            return self.session_members()
        pids, frontier = set(), [self.process.pid]
        while frontier:
            pid = frontier.pop()
            if pid in pids:
                continue
            pids.add(pid)
            try:
                for task in Path('/proc/%d/task' % pid).iterdir():
                    frontier.extend(int(x) for x in (task / 'children').read_text().split())
            except OSError:
                pass
        pids.update(self.session_members())
        return sorted(pid for pid in pids if self.is_owned(pid))

    def is_owned(self, pid):
        fd = self.owned(pid)
        if fd is None:
            return False
        os.close(fd)
        return True

    def session_members(self):
        """Living processes of the resident's session (the session ID is the resident's first PID) that carry the run token."""
        found = []
        if self.process is None:
            return found
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            try:
                stat = (entry / 'stat').read_text()
                fields = stat[stat.rindex(')') + 2:].split()
                if int(fields[3]) == self.process.pid and fields[0] != 'Z' and self.is_owned(int(entry.name)):  # session id
                    found.append(int(entry.name))
            except (OSError, ValueError, IndexError):
                pass
        return found

    def send(self, pid, sig):
        """Signal one process of the resident's tree through a checked pidfd. Returns False when it is not (or no longer) ours."""
        if self.process is not None and pid == self.process.pid and self.handle() is not None:
            try:
                signal.pidfd_send_signal(self.pidfd, sig)
                return True
            except OSError:
                return False
        fd = self.owned(pid)
        if fd is None:
            return False
        try:
            signal.pidfd_send_signal(fd, sig)
            return True
        except OSError:
            return False
        finally:
            os.close(fd)

    def resident_pid(self):
        """The resident's own process: the one started, or under a recorder (rr) its tracee, whose command line carries --background."""
        if self.wrapped:
            for pid in self.children():
                try:
                    if pid != self.process.pid and b'--background' in Path('/proc/%d/cmdline' % pid).read_bytes().split(b'\0'):
                        return pid
                except OSError:
                    pass
        return self.process.pid

    def tracees(self):
        """Everything of the resident's tree except the recorder itself."""
        return [pid for pid in self.children() if pid != self.process.pid]

    def hold(self, pid):
        """A pidfd on one of this launch's processes, to signal it more than once (stop, then continue) without looking its PID up again."""
        return self.owned(pid)

    def rss_kib(self):
        total = 0
        for pid in self.children():
            try:
                for line in Path('/proc/%d/status' % pid).read_text().splitlines():
                    if line.startswith('VmRSS:'):
                        total += int(line.split()[1])
            except OSError:
                pass
        return total

    def kill_tree(self, sig=signal.SIGKILL):
        """SIGKILL the resident and its helpers (exact PIDs found from /proc, each signalled through a pidfd checked as this launch's; never a
        pattern kill). Under a recorder only the tracees are killed: rr then flushes its trace and exits by itself."""
        if self.process is not None and self.process.returncode is None and not self.open_late():
            self.kill_pinned_session(sig)
        if self.wrapped and self.process is not None and self.alive():
            self.kill_tracees(sig)
        for _ in range(3):
            if self.process is not None and self.handle() is not None:
                try:
                    signal.pidfd_send_signal(self.pidfd, sig)  # the resident itself, even if it no longer passes the environment check
                except OSError:
                    pass
            for pid in reversed(self.children()):
                self.send(pid, sig)
            if self.process is not None and not self.wait_exit(20 * SLOW):
                raise Finding('hang', 'the resident did not die after SIGKILL')
            # Whatever the resident left behind in its session (a child it started in the last moment) dies too.
            if not self.session_members():
                self.reap()
                return
            time.sleep(0.05)
        # A wrapper such as rr leaves a process of its own that ends after the tracees: give it the time a recording needs.
        end = time.monotonic() + (10 * SLOW if self.options is not None and self.options.wrap else 0)
        while self.session_members() and time.monotonic() < end:
            for pid in self.session_members():
                try:
                    os.kill(pid, sig)
                except OSError:
                    pass
            time.sleep(0.2)
        if self.session_members():
            raise Finding('hang', 'processes of the killed resident are still alive: %s' % self.session_members())

    # Under rr a trace is complete once rr has written it: rr removes the `incomplete` marker of its trace folder at the end of recording.
    def kill_tracees(self, sig):
        for _ in range(3):
            for pid in reversed(self.tracees()):
                self.send(pid, sig)
            if self.wait_exit(60 * SLOW):  # rr notices its tracees are gone, finishes the trace and exits
                return
            if not self.tracees():
                break
        # rr itself did not end: SIGTERM asks it to stop recording cleanly before the SIGKILL of kill_tree.
        if self.handle() is not None:
            try:
                signal.pidfd_send_signal(self.pidfd, signal.SIGTERM)
            except OSError:
                pass
        self.wait_exit(30 * SLOW)

    def traces(self):
        """(complete, incomplete) rr trace folders of this run, from _RR_TRACE_DIR."""
        folder = next((a.partition('=')[2] for a in (self.options.env if self.options is not None else []) if a.startswith('_RR_TRACE_DIR=')), None)
        if not folder:
            return [], []
        found = [p for p in sorted(Path(folder).glob('*-*')) if p.is_dir() and not p.is_symlink()]
        return [str(p) for p in found if not (p / 'incomplete').exists()], [str(p) for p in found if (p / 'incomplete').exists()]

    def quit(self, how='ui.quit'):
        """A graceful stop: ui.quit, or the signal a session manager sends. The resident must exit by itself, with its leak check clean."""
        if not self.alive():
            return
        if how == 'ui.quit':
            try:
                self.call('ui.quit', timeout=15, raw=True)
            except (OSError, Finding, ValueError):
                pass
        else:
            self.send(self.resident_pid(), getattr(signal, how))
        if not self.wait_exit(60 * SLOW):
            raise Finding('hang', 'the resident did not exit within 60 s of %s' % how)
        # After SIGTERM or SIGINT the status is the signal's or 0; a sanitizer abort is neither.
        if self.returncode() not in (0, -signal.SIGTERM, -signal.SIGINT, 128 + signal.SIGTERM, 128 + signal.SIGINT):
            raise Finding('exit', 'the resident exited with %s after %s' % (self.returncode(), how))
        # Helpers it started leave with it.
        end = time.monotonic() + 10 * SLOW
        while self.session_members() and time.monotonic() < end:
            time.sleep(0.1)
        if self.session_members():
            raise Finding('orphan', 'processes of the resident are still alive 10 s after it exited after %s: %s' % (how, self.session_members()))
        self.reap()

    def stacks(self):
        """Symbolized stacks of the resident and its helpers (eu-stack, same user), for a hang report."""
        out = []
        for pid in self.children():
            try:
                name = Path('/proc/%d/comm' % pid).read_text().strip()
                if shutil.which('eu-stack'):
                    reply = subprocess.run(['eu-stack', '-p', str(pid)], capture_output=True, text=True, timeout=120)
                else:  # a builder without elfutils: gdb attaches to the resident's own children
                    reply = subprocess.run(['gdb', '-p', str(pid), '-batch', '-ex', 'thread apply all bt'], capture_output=True, text=True, timeout=180)
                out.append('=== %s (%d)\n%s%s' % (name, pid, reply.stdout, reply.stderr[-300:]))
            except (OSError, subprocess.TimeoutExpired) as error:
                out.append('=== pid %d: no stack (%s)' % (pid, error))
        return '\n'.join(out)

    def sanitizer_output(self):
        found = sorted(p for p in self.san.glob('*') if p.is_file() and not self.third_party(p))
        return found

    @staticmethod
    def third_party(path):
        """GStreamer's ORC JIT (the recording helper's probe) maps an executable file in XDG_RUNTIME_DIR and gets SIGBUS when that
        tmpfs is full. A helper dying on that is ORC's, not put.gd's; the resident and the run go on."""
        head = ''
        for _ in range(6):  # a report is written in pieces: wait for its stack before judging it
            try:
                head = path.read_text(errors='replace')[:6000]
            except OSError:
                return False
            if 'ERROR: AddressSanitizer: BUS' not in head or 'orc_compiler_compile_program' in head or 'SUMMARY:' in head:
                break
            time.sleep(0.5)
        return 'ERROR: AddressSanitizer: BUS' in head and 'orc_compiler_compile_program' in head

    # What UBSan says about a valid object when it cannot open the files it checks types with: a "does not point to an object of type"
    # error whose note says the object has an invalid vptr. A report that names a wrong dynamic type ("object is of type") is a real one.
    NOISE_ERROR = b'which does not point to an object of type'
    NOISE_NOTE = b'invalid vptr'

    def log_path(self, start=None):
        return self.run / 'logs' / ('resident-%d.log' % (start or self.starts))

    def log_offset(self):
        try:
            return self.log_path().stat().st_size
        except OSError:
            return 0

    def descriptor_noise_only(self, text, start):
        """True when the resident log of that start has UBSan runtime errors, and every one is the invalid-vptr report written inside a
        window in which the harness held the resident at its open-file limit (a_fault_res, or the whole start under --nofile)."""
        windows = self.descriptor_windows.get(start) or []
        lines = text.encode('utf-8', 'surrogateescape').splitlines(keepends=True)
        found, offset = False, 0
        for index, line in enumerate(lines):
            begin, offset = offset, offset + len(line)
            if b'runtime error:' not in line:
                continue
            found = True
            inside = any(first <= begin and (end is None or begin < end) for first, end in windows)
            notes = []
            for following in lines[index + 1:index + 12]:  # the report's own notes, up to the next report
                if b'runtime error:' in following:
                    break
                notes.append(following)
            if not inside or self.NOISE_ERROR not in line or not any(self.NOISE_NOTE in note for note in notes):
                return False
        return found

    def log_problems(self):
        problems = []
        for path in sorted((self.run / 'logs').glob('resident-*.log')):
            if path.name in self.judged_logs:
                continue
            try:
                text = path.read_bytes().decode('utf-8', 'surrogateescape')  # byte offsets survive the round trip
            except OSError:
                continue
            start = int(path.stem.split('-')[-1]) if path.stem.split('-')[-1].isdigit() else None
            for marker in BAD_MARKERS:
                # Invalid-vptr reports while the harness held the resident at its open-file limit are UBSan's own (it cannot read the files it checks
                # types with); any other runtime error, or one outside such an event, is a finding.
                if marker == 'runtime error:' and self.descriptor_noise_only(text, start):
                    continue
                if marker in text:
                    problems.append((path.name, marker))
        return problems


# ---------------------------------------------------------------------------------------------- the run

def odd_string(rng):
    kinds = [
        lambda: '',
        lambda: 'A' * rng.choice((1, 255, 256, 4096, 65536, 300000)),
        lambda: '\u0000'.join(['a', 'b']),
        lambda: ''.join(chr(rng.choice((0x7f, 0x80, 0x202e, 0x200d, 0x1f600, 0xfffd, 0x10ffff, 0xd7ff))) for _ in range(rng.randrange(1, 40))),
        lambda: '../' * rng.randrange(1, 30) + 'etc/passwd',
        lambda: '%s%n%x' * 20,
        lambda: "'; DROP TABLE operations; --",
        lambda: '{"a":' * 40,
        lambda: '/' + 'x/' * 300,
        lambda: '\n\r\t' * 20,
        lambda: rng.choice(('null', 'true', '0', '-1', '1e999', '[]', '{}')),
    ]
    return rng.choice(kinds)()


def odd_value(rng, depth=0):
    kinds = [lambda: None, lambda: True, lambda: False, lambda: 0, lambda: -1, lambda: 2 ** 31, lambda: 2 ** 63, lambda: -2 ** 63, lambda: 1e300, lambda: 0.5,
             lambda: odd_string(rng), lambda: [], lambda: {}]
    if depth < 3:
        kinds += [lambda: [odd_value(rng, depth + 1) for _ in range(rng.randrange(0, 5))],
                  lambda: {odd_string(rng)[:20]: odd_value(rng, depth + 1) for _ in range(rng.randrange(0, 4))}]
    return rng.choice(kinds)()


# Methods the fuzzer must not call at all: they reach beyond the loopback lab or need a desktop.
FORBIDDEN = {'update.check', 'history.openLink', 'editor.openCode', 'ui.quit', 'diagnostics.bundle', 'history.unlockLinkCredentials',
             'setup.testDestination', 'upload.files', 'action.invoke', 'settings.importDevelopment', 'settings.importUploader'}
KNOWN_METHODS = '''capabilities diagnostics editor.open editor.inspect editor.submit editor.cancel editor.raise editor.apply editor.preview editor.saveCopy editor.copy
editor.recognize editor.selectText editor.copyText editor.copyCode editor.fileDialog text.recognize text.inspect text.cancel history.copyText history.qr
operation.recent operation.inspect operation.cancel operation.retry operation.copy operation.retryLocalSave history.list history.counts history.detail
history.copyLink history.copyImage history.preview history.clear cache.clear logs.clear update.status delete.prepare delete.confirm delete.inspect delete.retry
delete.cancel settings.developmentTemplate settings.get settings.reload settings.validate settings.patch settings.reorderProfiles setup.credentialHint
setup.inspect desktop.setup desktop.setup.recheck ui.show ui.snapshot ui.viewState ui.moveDestination ui.diagnostics ui.history ui.historySaveDiagnostics
ui.customUploader ui.fileDialog scroll.capabilities scroll.inspect scroll.cancel recording.capabilities recording.inspect'''.split()


class Fuzzer:
    def __init__(self, options):
        self.options = options
        self.rng = random.Random(options.seed)
        self.run = Path(options.run_dir)
        self.receiver = None
        self.faults = None
        for name in ('logs', 'fixtures', 'saves', 'archive'):
            (self.run / name).mkdir(parents=True, exist_ok=True)
        self.resident = Resident(self.run, options.build, options=options)
        self.proxy_port = None
        self.fault_classes = set(options.fault_classes.split(',')) if options.mode == 'faults' else set()
        self.fixtures = {}
        self.profile = None
        self.step_index = 0
        self.steps = open(self.run / 'steps.jsonl', 'a')
        self.operations, self.history_ids, self.editors, self.deletes = [], [], [], []
        self.nonce = 0
        self.rss = []
        self.stats = {}
        self.temporary = []
        self.open_editors = set()
        self.multi = None

    # --- logging

    def log(self, action, **detail):
        record = {'step': self.step_index, 't': round(time.monotonic() - self.t0, 2), 'action': action}
        record.update(detail)
        self.steps.write(json.dumps(record, ensure_ascii=False, default=str) + '\n')
        self.steps.flush()
        if self.options.verbose:
            print(json.dumps(record, ensure_ascii=False, default=str)[:300], flush=True)

    # --- setup

    def setup(self):
        self.t0 = time.monotonic()
        self.fixtures = make_fixtures(self.run / 'fixtures', self.rng)
        if self.options.destination == 's3':
            # Over put.gd's multipart threshold (64 MiB), so the multipart journal and its abort are exercised.
            (self.run / 'fixtures' / 'multi.bin').write_bytes(os.urandom(65 * 1024 * 1024))
            self.multi = self.run / 'fixtures' / 'multi.bin'
        if self.options.mode == 'faults':
            os.environ['PUTGD_HTTP_FIXTURE_TRUST_HOST'] = '1'  # the file and deletion addresses stay behind the proxy too
            os.environ['PUTGD_HTTP_FIXTURE_STRICT_BODY'] = '1'  # a request cut by the proxy is not answered as if it had arrived whole
        source = Path(self.options.fixture_server) if self.options.fixture_server else Path(__file__).resolve().parents[2] / 'tests/fixtures/http_uploader_server.py'
        if self.options.destination == 's3':
            self.receiver = S3Receiver(source.with_name('s3_server.py'), self.run)
        else:
            self.receiver = Receiver(source, self.run)
        self.receiver.reset()
        if self.options.mode == 'faults':
            self.setup_faults()
        self.resident.start()
        if self.options.destination == 's3':
            self.add_s3_profile()
        else:
            self.import_uploader()

    def add_s3_profile(self):
        """An S3 destination on the offline oracle, with session credentials (nothing reaches a keyring)."""
        settings = self.resident.call('settings.get')
        document = settings['document']
        existing = [p for p in document.get('profiles') or [] if p.get('provider') == 's3' and p.get('id') == 's3fuzz']
        self.profile = 's3fuzz'
        if not existing:
            profile = json.loads(json.dumps(self.resident.call('settings.developmentTemplate')['document']['profiles'][0]))
            profile.update(id='s3fuzz', label='S3 fuzz', credentials={'mode': 'session', 'reference': None})
            profile['settings'].update(endpoint=self.endpoint(), bucket='isolated-fixture', region='eu-west-3', objectPrefix='p', aclMode='omit',
                                       publicUrlBase=None, requestTimeoutMs=8000, connectTimeoutMs=1000)
            document['profiles'] = (document.get('profiles') or []) + [profile]
            self.resident.call('settings.save', {'document': document, 'expectedRevision': settings['metadata']['observedRevision']})
            settings = self.resident.call('settings.get')
            self.resident.call('settings.patch', {'patch': {'archive': {'keepUploads': True, 'root': str(self.run / 'archive')},
                                                           'preferences': {'notifications': False}},
                                                  'expectedRevision': settings['metadata']['observedRevision']})
        self.credentials_check()

    def import_uploader(self):
        sxcu = self.run / 'fuzz.sxcu'
        sxcu.write_text(json.dumps({'Version': '17.0.0', 'Name': 'fuzz receiver', 'DestinationType': 'ImageUploader, TextUploader, FileUploader',
                                    'RequestMethod': 'POST', 'RequestURL': self.endpoint() + '/upload/json', 'Headers': {'Authorization': TOKEN},
                                    'Body': 'MultipartFormData', 'FileFormName': 'file', 'URL': '{json:files[0].url}',
                                    'DeletionURL': '{json:files[0].delete}'}))
        os.chmod(sxcu, 0o600)
        settings = self.resident.call('settings.get')
        if any(p.get('provider') == 'custom-http' for p in (settings['document'].get('profiles') or [])):
            self.profile = next(p['id'] for p in settings['document']['profiles'] if p.get('provider') == 'custom-http')
            return
        reply = self.resident.call('settings.importUploader', {'path': str(sxcu), 'expectedRevision': settings['metadata']['observedRevision'], 'credentialMode': 'session',
                                                                'profileId': 'fuzz', 'label': 'Fuzz'})
        if not isinstance(reply, dict) or 'profileId' not in reply:
            raise Finding('setup', 'settings.importUploader did not import the receiver profile: %s' % str(reply)[:300])
        self.profile = reply['profileId']
        settings = self.resident.call('settings.get')
        self.resident.call('settings.patch', {'patch': {'archive': {'keepUploads': True, 'root': str(self.run / 'archive')},
                                                       'preferences': {'notifications': False}},
                                              'expectedRevision': settings['metadata']['observedRevision']})
        self.session_credentials_lost = True

    def reload_credentials_after_restart(self):
        """Session-mode credentials die with the resident: store them again like the Settings window's Use for this session."""
        hint = self.resident.call('setup.credentialHint', {'profileId': self.profile, 'expectedRevision': self.revision()}, raw=True)
        if (hint.get('result') or {}).get('missingValues'):
            self.log('credentials-missing-after-restart', hint=hint.get('result'))

    def revision(self):
        return self.resident.call('settings.get')['metadata']['observedRevision']

    # --- one step

    def new_upload_file(self, name=None):
        name = name or (self.rng.choice(list(self.fixtures)) if self.rng.random() > 0.03 else 'large.bin')
        if self.multi is not None and name == 'multi.bin':
            self.fixtures['multi.bin'] = self.multi
        self.nonce += 1
        source = self.fixtures[name]
        suffix = ''.join(c for c in name if c.isprintable())[-24:]
        path = self.run / 'fixtures' / ('u%d-%s' % (self.nonce, suffix.replace('/', '_')))
        # Every upload's content is distinct, so the receiver can count a second send. put.gd re-encodes a PNG,
        # which drops a tail, so a valid image gets its own pixels instead.
        if name in PNG_SIZES:
            path.write_bytes(png(*PNG_SIZES[name], seed=self.nonce))
        else:
            path.write_bytes(source.read_bytes() + b'\n#%d\n' % self.nonce)
        self.temporary.append((self.step_index, path))
        return path

    def sweep(self):
        """Like a user tidying up: the sources of finished uploads, and now and then local copies in the archive, are deleted (exact files)."""
        keep = []
        for step, path in self.temporary:
            if step + 6 < self.step_index:
                try:
                    path.unlink()
                except OSError:
                    pass
            else:
                keep.append((step, path))
        self.temporary = keep
        if self.multi is not None:
            # Large local copies are not kept around.
            for path in list((self.run / 'archive').rglob('*')):
                try:
                    if path.is_file() and path.stat().st_size > 8 * 1024 * 1024:
                        path.unlink()
                except OSError:
                    pass
        if self.step_index % 40 == 0:
            for path in sorted((self.run / 'archive').rglob('*'))[:400]:
                if path.is_file() and self.rng.random() < 0.7:
                    try:
                        path.unlink()
                    except OSError:
                        pass
            for path in sorted((self.run / 'saves').glob('*')):
                if path.is_file():
                    path.unlink()

    def call(self, method, params=None, timeout=60, expect=True):
        """A call from a step: records the exchange; a transport loss with a live resident is a finding."""
        try:
            reply = self.resident.call(method, params, timeout=timeout, raw=True)
        except OSError as error:
            if self.resident.alive():
                raise Finding('transport', '%s: %s while the resident lives' % (method, error))
            raise Finding('crash', '%s: %s and the resident is gone (exit %s)' % (method, error, self.resident.returncode()))
        error = reply.get('error') or {}
        if method == 'editor.open' and reply.get('ok') and (reply.get('result') or {}).get('operationId'):
            self.open_editors.add(reply['result']['operationId'])
        if method in ('editor.cancel', 'editor.submit') and reply.get('ok') and isinstance(params, dict):
            self.open_editors.discard(params.get('id'))
        key = '%s %s' % (method, 'ok' if reply.get('ok') else error.get('code'))
        self.stats[key] = self.stats.get(key, 0) + 1
        self.log('call', method=method, params=json.dumps(params, ensure_ascii=False, default=str)[:200], ok=reply.get('ok'), code=error.get('code'))
        return reply

    def pick(self, items):
        return self.rng.choice(items) if items else None

    def act(self, name):
        getattr(self, 'a_' + name)()

    ACTIONS = [('upload', 10), ('upload_cancel', 3), ('editor_open', 3), ('editor_session', 8), ('editor_apply', 6), ('editor_view', 5), ('editor_end', 4), ('history', 8),
               ('detail', 6), ('delete', 4), ('settings', 6), ('operations', 6), ('garbage', 8), ('ui', 6), ('burst', 4), ('receiver_rule', 3),
               ('misc', 5), ('text', 2), ('restart', 1), ('edit_action', 2), ('config_chaos', 3), ('archive_chaos', 3), ('helper_chaos', 4), ('upload_weird', 5)]

    def a_upload(self):
        paths = [str(self.new_upload_file()) for _ in range(self.rng.choice((1, 1, 2, 3, 5)))]
        reply = self.call('upload.files', {'paths': paths, 'profileId': self.profile})
        if reply.get('ok'):
            self.operations.append(((reply['result'] or {}).get('operation') or {}).get('id'))
            self.operations = [o for o in self.operations if o][-40:]

    def a_upload_cancel(self):
        self.a_upload()
        time.sleep(self.rng.choice((0, 0, 0.02, 0.1, 0.4)))
        if self.operations:
            self.call('operation.cancel', {'id': self.operations[-1]})

    def a_editor_open(self):
        name = self.rng.choice(list(self.fixtures))
        reply = self.call('editor.open', {'source': 'file', 'path': str(self.fixtures[name])})
        if reply.get('ok'):
            identifier = (reply['result'] or {}).get('operationId')
            if identifier:
                self.editors.append(identifier)
                self.editors = self.editors[-6:]

    def a_editor_session(self):
        """A whole editing session: open (closing a previous one), wait until it is ready, edit, view, then submit, cancel or leave it."""
        rng = self.rng
        if self.editors and rng.random() < 0.8:
            self.call('editor.cancel', {'id': self.editors.pop()})
            time.sleep(rng.choice((0, 0.05, 0.3)))
        # Mostly images the editor can open, so the edit commands run; now and then something it must refuse.
        valid = ['small.png', 'tiny.png', 'wide.png', 'tall.png', 'big.png', 'unicode \u00e9\u4e2d \u202e.png']
        name = rng.choice(valid) if rng.random() < 0.85 else rng.choice([n for n in self.fixtures if n not in ('payload.bin', 'large.bin', 'multi.bin')])
        reply = self.call('editor.open', {'source': 'file', 'path': str(self.fixtures[name])})
        if not reply.get('ok'):
            return
        identifier = (reply['result'] or {}).get('operationId')
        if not identifier:
            return
        self.editors.append(identifier)
        end = time.monotonic() + rng.choice((0, 0.2, 3, 15))
        while time.monotonic() < end:
            inspected = self.call('editor.inspect', {'id': identifier})
            if inspected.get('ok') or (inspected.get('error') or {}).get('code') != 'editor_starting':
                break
            time.sleep(0.2)
        for _ in range(rng.randrange(1, 8)):
            choice = rng.randrange(5)
            if choice <= 1:
                self.a_editor_apply_to(identifier)
            elif choice == 2:
                self.call('editor.preview', {'id': identifier, 'maxEdge': rng.choice((64, 400, 1600))})
            elif choice == 3:
                self.call('editor.saveCopy', {'id': identifier, 'path': str(self.run / 'saves' / ('s%d.png' % rng.randrange(4))), 'overwrite': rng.choice((True, False))})
            else:
                self.call('editor.inspect', {'id': identifier})
        ending = rng.choice(('submit', 'cancel', 'cancel', 'leave', 'burst'))
        if ending == 'submit':
            reply = self.call('editor.submit', {'id': identifier})
            if reply.get('ok'):
                self.editors.remove(identifier)
        elif ending == 'cancel':
            self.call('editor.cancel', {'id': identifier})
            self.editors.remove(identifier)
        elif ending == 'burst':
            # Submit and cancel race, with a late edit behind them.
            threads = [threading.Thread(target=lambda m=m: self.resident.call(m, {'id': identifier}, timeout=90, raw=True)) for m in ('editor.submit', 'editor.cancel', 'editor.preview')]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(120)
            self.editors.remove(identifier)
            self.a_editor_apply_to(identifier)

    def a_editor_apply_to(self, identifier):
        self.editors.append(identifier) if identifier not in self.editors else None
        keep = self.editors[:]
        self.editors[:] = [identifier]
        try:
            self.a_editor_apply()
        finally:
            self.editors[:] = keep

    def random_shape(self):
        rng = self.rng

        def point():
            return [rng.choice((rng.randrange(-50, 400), rng.randrange(0, 200), 0, -1, 2 ** 31, 10 ** 12)), rng.randrange(-50, 300)]

        def rect():
            return [rng.randrange(-20, 300), rng.randrange(-20, 200), rng.choice((0, 1, 5, 80, 500, -3, 10 ** 9)), rng.choice((0, 1, 5, 50, 400, -3))]
        kind = rng.choice(('redact', 'blur', 'arrow', 'line', 'rectangle', 'ellipse', 'pen', 'step', 'highlight', 'text', 'bogus'))
        shape = {'kind': kind}
        color = rng.choice(('#FF0000', '#00ff00', 'red', '', '#12345', None))
        if kind in ('redact', 'rectangle', 'ellipse', 'blur'):
            shape['rect'] = rect()
        if kind in ('arrow', 'line'):
            shape['from'], shape['to'] = point(), point()
        if kind == 'pen':
            shape['points'] = [point() for _ in range(rng.choice((0, 1, 2, 30, 2001)))]
        if kind == 'step':
            shape['at'] = point()
            shape['number'] = rng.choice((1, 5, 999, 1000, 0))
        if kind == 'highlight':
            if rng.random() < 0.5:
                shape['rect'] = rect()
            else:
                shape['at'] = point()
                shape['snap'] = rng.choice(('word', 'line', 'text', 'none', 'x'))
        if kind == 'text':
            shape['at'] = point()
            shape['text'] = rng.choice(('hello', odd_string(rng)[:600], 'Ḿ̂' * 30, '‮text', ''))
            shape['fontPixels'] = rng.choice((8, 24, 200, 7, 201))
        if kind == 'blur':
            shape['strength'] = rng.choice((2, 64, 1, 65))
            shape['style'] = rng.choice(('blur', 'pixelate', 'x'))
        if color and rng.random() < 0.6:
            shape['color'] = color
        if rng.random() < 0.3:
            shape['width'] = rng.choice((1, 64, 0, 65, 3))
        return shape

    def a_editor_apply(self):
        identifier = self.pick(self.editors) or 'no-such-editor'
        commands = []
        for _ in range(self.rng.choice((1, 1, 2, 5, 20, 101))):
            op = self.rng.choice(('add', 'add', 'add', 'update', 'remove', 'crop', 'resetCrop', 'undo', 'redo', 'redactText', 'zzz'))
            command = {'op': op}
            if op == 'add':
                command['shape'] = self.random_shape()
            elif op == 'update':
                command['shapeId'] = self.rng.choice((1, 2, 3, 'x', 0, 2 ** 40))
                command['shape'] = self.random_shape()
            elif op == 'remove':
                command['shapeId'] = self.rng.choice((1, 2, 3, 'x', -1))
            elif op == 'crop':
                command['rect'] = [self.rng.randrange(-5, 100), self.rng.randrange(-5, 100), self.rng.randrange(-5, 300), self.rng.randrange(-5, 300)]
            elif op == 'redactText':
                command['categories'] = self.rng.sample(['email', 'phone', 'ip', 'card', 'key', 'bad'], self.rng.randrange(0, 4))
            commands.append(command)
        self.call('editor.apply', {'id': identifier, 'commands': commands})

    def a_editor_view(self):
        identifier = self.pick(self.editors) or 'no-such-editor'
        choice = self.rng.randrange(7)
        if choice == 0:
            self.call('editor.inspect', {'id': identifier})
        elif choice == 1:
            self.call('editor.preview', {'id': identifier, 'maxEdge': self.rng.choice((64, 1600, 63, 1601, 400))})
        elif choice == 2:
            name = self.rng.choice(('a.png', 'b.png', '../x.png', 'c.txt', 'rel.png', 'd' * 300 + '.png'))
            path = str(self.run / 'saves' / name) if name != 'rel.png' else 'rel.png'
            self.call('editor.saveCopy', {'id': identifier, 'path': path, 'overwrite': self.rng.choice((True, False, None))})
        elif choice == 3:
            self.call('editor.copy', {'id': identifier})
        elif choice == 4:
            self.call('editor.recognize', {'id': identifier})
        elif choice == 5:
            self.call('editor.raise', {'id': identifier})
        else:
            self.call('editor.fileDialog', {'id': identifier, 'action': self.rng.choice(('open', 'inspect', 'cancel', 'zz'))})

    def a_editor_end(self):
        identifier = self.pick(self.editors) or 'no-such-editor'
        method = self.rng.choice(('editor.submit', 'editor.cancel', 'editor.submit'))
        reply = self.call(method, {'id': identifier})
        if reply.get('ok') and identifier in self.editors:
            self.editors.remove(identifier)
        if method == 'editor.submit' and reply.get('ok'):
            self.operations.append(identifier)

    def a_history(self):
        rng = self.rng
        params = {}
        for key, values in (('limit', (1, 50, 200, 201, 0, -1)), ('offset', (0, 1, 7, 10 ** 6, -1)), ('search', ('small', '%', '_', "'", odd_string(rng))),
                            ('category', ('images', 'videos', 'text+other', 'failed', 'x')), ('uploadStatus', ('uploaded', 'failed+unknown', 'failed', 'unknown', 'x')),
                            ('mime', ('image/png', 'text/plain')), ('from', ('2020-01-01T00:00:00.000Z', 'x')), ('to', ('2099-01-01T00:00:00.000Z',))):
            if rng.random() < 0.3:
                params[key] = rng.choice(values)
        method = rng.choice(('history.list', 'history.list', 'history.counts'))
        reply = self.call(method, params)
        if reply.get('ok') and method == 'history.list':
            for row in (reply['result'] or {}).get('records') or []:
                if isinstance(row, dict) and row.get('id'):
                    self.history_ids.append(row['id'])
            self.history_ids = self.history_ids[-200:]

    def a_detail(self):
        identifier = self.pick(self.history_ids) or odd_string(self.rng)
        method = self.rng.choice(('history.detail', 'history.copyLink', 'history.copyImage', 'history.preview', 'history.qr', 'history.copyText'))
        params = {'id': identifier}
        if method == 'history.qr':
            params['action'] = self.rng.choice(('show', 'copy', 'save', 'x'))
        self.call(method, params, timeout=90)

    def a_delete(self):
        ids = self.rng.sample(self.history_ids, min(len(self.history_ids), self.rng.randrange(0, 4))) if self.history_ids else []
        if self.rng.random() < 0.15:
            ids.append(odd_string(self.rng))
        prepared = self.call('delete.prepare', {'ids': ids})
        result = prepared.get('result') or {}
        if not prepared.get('ok'):
            return
        acknowledgement = result.get('acknowledgement')
        if self.rng.random() < 0.15:
            acknowledgement = odd_string(self.rng)
        confirm_ids = ids if self.rng.random() > 0.1 else list(reversed(ids)) + ['x']
        confirmed = self.call('delete.confirm', {'ids': confirm_ids, 'acknowledgement': acknowledgement}, timeout=90)
        operation = (confirmed.get('result') or {}).get('deleteOperationId')
        if operation:
            self.deletes.append(operation)
            self.deletes = self.deletes[-10:]
            if self.rng.random() < 0.3:
                self.call('delete.cancel', {'id': operation})
            elif self.rng.random() < 0.3:
                self.call('delete.retry', {'id': operation})
            else:
                self.call('delete.inspect', {'id': operation})

    def a_settings(self):
        rng = self.rng
        choice = rng.randrange(6)
        if choice == 0:
            self.call('settings.get')
        elif choice == 1:
            self.call('settings.reload')
        elif choice == 2:
            document = self.resident.call('settings.get')['document']
            if rng.random() < 0.5:
                document['retention'] = odd_value(rng)
            self.call('settings.validate', {'document': document})
        elif choice == 3:
            self.call('settings.reorderProfiles', {'order': [self.profile] + ([] if rng.random() < 0.8 else ['x']), 'expectedRevision': self.revision()})
        else:
            patch = rng.choice((
                {'retention': {'recentActivityLimit': rng.choice((1, 10, 77, 500, 0, -1, 10 ** 9))}},
                {'limits': {'maxFileBytes': rng.choice((None, 'auto', 1000, 5 * 1024 * 1024, 0, -5, 'x'))}},
                {'limits': {'pendingArtifactBytes': rng.choice((None, 'auto', 10 ** 7))}},
                {'preferences': {'notifications': rng.choice((True, False))}},
                {'preferences': odd_value(rng)},
                {odd_string(rng)[:30]: odd_value(rng)},
                {'archive': {'keepUploads': rng.choice((True, False))}},
            ))
            revision = self.revision() if rng.random() > 0.2 else 'stale-revision'
            self.call('settings.patch', {'patch': patch, 'expectedRevision': revision})

    def a_operations(self):
        identifier = self.pick(self.operations) or 'no-such-op'
        if self.rng.random() < 0.08:
            identifier = odd_string(self.rng)
        method = self.rng.choice(('operation.inspect', 'operation.inspect', 'operation.recent', 'operation.cancel', 'operation.retry', 'operation.copy',
                                  'operation.retryLocalSave'))
        params = {'id': identifier} if method != 'operation.recent' else {}
        if method == 'operation.retry' and self.rng.random() < 0.3:
            params['scope'] = 'sharing'
        if method == 'operation.cancel' and self.rng.random() < 0.3:
            params['itemId'] = odd_string(self.rng)
        self.call(method, params, timeout=90)

    def a_garbage(self):
        method = self.rng.choice([m for m in KNOWN_METHODS if m not in FORBIDDEN] + ['no.such.method', odd_string(self.rng)[:60]])
        shape = self.rng.randrange(4)
        if shape == 0:
            params = None
        elif shape == 1:
            params = {odd_string(self.rng)[:24]: odd_value(self.rng) for _ in range(self.rng.randrange(0, 4))}
        elif shape == 2:
            params = odd_value(self.rng)
        else:
            params = {'id': odd_value(self.rng), 'ids': odd_value(self.rng), 'path': odd_value(self.rng), 'profileId': odd_value(self.rng),
                      'action': odd_value(self.rng), 'view': odd_value(self.rng)}
        self.call(method, params, timeout=90)

    def a_ui(self):
        rng = self.rng
        choice = rng.randrange(5)
        if choice == 0:
            self.call('ui.show', {'view': rng.choice(('settings', 'history', 'activity', 'destinations', 'about', 'updates', 'editFile', 'zz'))}, timeout=60)
        elif choice == 1:
            self.call('ui.history', {'action': rng.choice(('inspect', 'select', 'clearSelection', 'openDetails', 'closeDetails', 'toggleDetails', 'nextPage', 'previousPage',
                                                           'goToPage', 'setFilter', 'removeFilter', 'clearFilters', 'showCategory', 'setView', 'copySelectedLinks',
                                                           'finishShareLink', 'deleteSelected', 'cancelConfirmation', 'retryLoading', 'expandPreview', 'closePreview',
                                                           'cancelAll', 'zz')),
                                     'ids': self.rng.sample(self.history_ids, min(2, len(self.history_ids))), 'page': rng.choice((1, 2, 0, 99)),
                                     'key': rng.choice(('from', 'to', 'mime', 'uploadStatus', 'includeCanceled', 'x')), 'value': rng.choice(('2026-11-01', 'image/png', 'true', 'x')),
                                     'category': rng.choice(('all', 'images', 'failed', 'zz')), 'view': rng.choice(('list', 'gallery', 'zz')),
                                     'scope': rng.choice(('filters', 'all'))}, timeout=60)
        elif choice == 2:
            self.call('ui.diagnostics', {'action': rng.choice(('inspect', 'setDebugLogging', 'clearLogs', 'openPackage', 'setPackage', 'closePackage', 'zz')),
                                         'enabled': rng.choice((True, False)), 'includeNames': rng.choice((True, False))}, timeout=60)
        elif choice == 3:
            self.call('ui.customUploader', {'action': rng.choice(('inspect', 'openImport', 'closeImport', 'zz')), 'profileId': self.profile}, timeout=60)
        else:
            self.call(rng.choice(('ui.viewState', 'ui.snapshot', 'ui.moveDestination', 'ui.fileDialog')), rng.choice(({}, {'direction': 'up'}, {'dialog': 'outputFolder', 'action': 'inspect'})), timeout=60)

    def a_burst(self):
        count = self.rng.randrange(2, 9)
        methods = [self.rng.choice((('history.list', {'limit': 50}), ('settings.get', {}), ('operation.recent', {}), ('capabilities', {}), ('diagnostics', {}),
                                    ('history.counts', {}), ('upload.files', {'paths': [str(self.new_upload_file('small.png'))], 'profileId': self.profile}),
                                    ('editor.open', {'source': 'file', 'path': str(self.fixtures['small.png'])}),
                                    ('ui.show', {'view': 'history'}), ('settings.patch', {'patch': {'retention': {'recentActivityLimit': 50}}, 'expectedRevision': 'x'}))) for _ in range(count)]
        results = [None] * count

        def worker(index):
            method, params = methods[index]
            try:
                results[index] = self.resident.call(method, params, timeout=90, raw=True)
            except (OSError, Finding) as error:
                results[index] = error
        threads = [threading.Thread(target=worker, args=(i,)) for i in range(count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(120)
        for (method, _), result in zip(methods, results):
            if isinstance(result, Finding):
                raise result
            if isinstance(result, OSError) and self.resident.alive():
                raise Finding('transport', 'burst %s: %s while the resident lives' % (method, result))
            if isinstance(result, dict) and result.get('ok') and method == 'upload.files':
                self.operations.append(((result['result'] or {}).get('operation') or {}).get('id'))
            if isinstance(result, dict) and result.get('ok') and method == 'editor.open' and (result['result'] or {}).get('operationId'):
                self.editors.append(result['result']['operationId'])
                self.open_editors.add(result['result']['operationId'])  # the epoch check and settle() cancel it by id, not through the bounded activity list
        self.log('burst', methods=[m for m, _ in methods], oks=[r.get('ok') if isinstance(r, dict) else str(r) for r in results])

    def a_receiver_rule(self):
        rule = self.receiver.random_rule(self.rng)
        rule['remaining'] = self.rng.choice((1, 2, 5))
        self.receiver.rules([rule])
        self.log('receiver-rule', rule=rule)

    def a_misc(self):
        self.call(self.rng.choice(('capabilities', 'diagnostics', 'update.status', 'desktop.setup', 'settings.developmentTemplate', 'cache.clear', 'logs.clear',
                                   'history.clear', 'scroll.capabilities', 'recording.capabilities', 'recording.inspect', 'scroll.inspect')), {}, timeout=90)

    def a_text(self):
        self.call(self.rng.choice(('text.recognize', 'text.inspect', 'text.cancel')),
                  {'source': 'file', 'path': str(self.fixtures[self.rng.choice(list(self.fixtures))]), 'id': self.pick(self.operations) or 'x'}, timeout=90)

    def a_edit_action(self):
        self.call('action.invoke', {'action': self.rng.choice(('edit', 'clipboard', 'desktop', 'scroll', 'text', 'region', 'window', 'monitor', 'repeat', 'record')),
                                    'source': 'file', 'path': str(self.fixtures['small.png']), 'mode': self.rng.choice(('open', 'toggle'))}, timeout=60)

    def a_upload_weird(self):
        """upload.files with what a file manager can hand over besides a plain file: folders, FIFOs, links, unreadable or vanishing files, odd names."""
        rng = self.rng
        folder = self.run / 'weird'
        folder.mkdir(exist_ok=True)
        self.nonce += 1
        n = self.nonce
        kinds = rng.sample(('dir', 'fifo', 'symlink', 'loop', 'unreadable', 'vanish', 'grow', 'newline', 'longname', 'dup', 'missing', 'relative', 'dotdot', 'empty', 'many'), rng.randrange(1, 4))
        paths = []
        later = []
        for kind in kinds:
            if kind == 'dir':
                path = folder / ('dir%d' % n)
                path.mkdir(exist_ok=True)
                (path / 'inner.txt').write_text('inner')
            elif kind == 'fifo':
                path = folder / ('fifo%d' % n)
                os.mkfifo(path)
            elif kind == 'symlink':
                path = folder / ('link%d.png' % n)
                path.symlink_to(self.new_upload_file('small.png'))
            elif kind == 'loop':
                path = folder / ('loop%d' % n)
                path.symlink_to(path.name)
            elif kind == 'unreadable':
                path = folder / ('secret%d.txt' % n)
                path.write_text('x' * 100)
                os.chmod(path, 0)
            elif kind in ('vanish', 'grow'):
                path = folder / ('moving%d.bin' % n)
                path.write_bytes(os.urandom(2 * 1024 * 1024))
                later.append((kind, path))
            elif kind == 'newline':
                path = folder / ('line\nbreak%d.txt' % n)
                path.write_text('x')
            elif kind == 'longname':
                path = folder / (('n' * 240) + '%d.txt' % n)
                path.write_text('x')
            elif kind == 'dup':
                path = self.new_upload_file('notes.txt')
                paths.append(str(path))
            elif kind == 'missing':
                path = folder / ('missing%d' % n)
            elif kind == 'relative':
                path = Path('relative.png')
            elif kind == 'dotdot':
                path = folder / '..' / 'weird' / ('x%d.txt' % n)
                (folder / ('x%d.txt' % n)).write_text('x')
            elif kind == 'empty':
                path = folder / ('empty%d' % n)
                path.write_bytes(b'')
            else:
                path = None
                paths.extend(str(folder / ('nothing%d-%d' % (n, i))) for i in range(rng.choice((50, 1000, 10001))))
            if path is not None:
                paths.append(str(path))
        reply = self.call('upload.files', {'paths': paths, 'profileId': self.profile}, timeout=130)
        if reply.get('ok'):
            self.operations.append(((reply['result'] or {}).get('operation') or {}).get('id'))
        for kind, path in later:
            time.sleep(rng.choice((0, 0.01, 0.2)))
            try:
                if kind == 'vanish':
                    path.unlink()
                else:
                    with open(path, 'ab') as handle:
                        handle.write(os.urandom(500000))
            except OSError:
                pass
        # Unreadable files and links are put back to something the sweep can delete.
        for path in sorted(folder.glob('secret*')):
            try:
                os.chmod(path, 0o600)
            except OSError:
                pass

    def a_helper_chaos(self):
        """A helper (the UI, an upload worker, a recognizer) is killed or frozen by itself while the resident lives; the resident must cope."""
        rng = self.rng
        if rng.random() < 0.6:
            for _ in range(rng.randrange(1, 3)):
                self.a_upload()
        if rng.random() < 0.4:
            self.call('ui.show', {'view': rng.choice(('history', 'settings'))}, timeout=60)
        time.sleep(rng.choice((0, 0.05, 0.3, 1.0)))
        helpers = []
        resident = self.resident.resident_pid()
        for pid in self.resident.children():
            if pid in (self.resident.process.pid, resident):  # the resident (also as a recorder's tracee) and the recorder are not helpers
                continue
            try:
                helpers.append((pid, Path('/proc/%d/comm' % pid).read_text().strip()))
            except OSError:
                pass
        if not helpers:
            self.log('helper-chaos', helpers=0)
            return
        pid, name = rng.choice(helpers)
        action = rng.choice(('kill', 'kill', 'freeze'))
        self.log('helper-chaos', helper=name, pid=pid, what=action)
        # One pidfd, checked once, for every signal to this helper: a SIGCONT after a long pause reaches the helper that was stopped or nobody.
        handle = self.resident.hold(pid)
        if handle is None:
            return
        try:
            if action == 'kill':
                signal.pidfd_send_signal(handle, signal.SIGKILL)
            else:
                signal.pidfd_send_signal(handle, signal.SIGSTOP)
                hold = rng.choice((0.5, 3.0, 12.0))
                end = time.monotonic() + hold
                try:
                    # The resident keeps answering while the helper is frozen.
                    while time.monotonic() < end:
                        self.call(rng.choice(('capabilities', 'history.list', 'operation.recent', 'settings.get')), {}, timeout=30)
                        time.sleep(0.3)
                finally:
                    signal.pidfd_send_signal(handle, signal.SIGCONT)
        except OSError:
            pass
        finally:
            os.close(handle)

    def config_path(self):
        return self.resident.root / 'config' / 'putgd' / 'config.json'

    def a_config_chaos(self):
        """Someone edits config.json behind the resident's back (an editor, a sync tool, a crash): reload must refuse or accept it cleanly, and the good file comes back."""
        rng = self.rng
        path = self.config_path()
        good = path.read_bytes() if path.exists() else None
        if good is None:
            return
        variants = [
            lambda: good[:rng.randrange(0, len(good))],
            lambda: good + b'\n{"trailing":',
            lambda: b'',
            lambda: b'\xff\xfe' + good,
            lambda: bytes(rng.randrange(256) for _ in range(rng.choice((1, 64, 5000)))),
            lambda: json.dumps({'schemaVersion': rng.choice((0, 1, 999, 'x', None)), 'profiles': odd_value(rng)}).encode(),
            lambda: good.replace(b'"fuzz"', b'"fuzz"', 1).replace(b'"profiles":[', b'"profiles":[null,', 1),
            lambda: b'[' * 5000,
            lambda: good.replace(b'"recentActivityLimit"', b'"recentActivityLimit":-5,"recentActivityLimit"', 1),
        ]
        bad = rng.choice(variants)()
        how = rng.choice(('overwrite', 'replace', 'delete'))
        self.log('config-chaos', how=how, bytes=len(bad))
        if how == 'overwrite':
            path.write_bytes(bad)
        elif how == 'replace':
            temporary = path.with_name('config.json.chaos')
            temporary.write_bytes(bad)
            os.replace(temporary, path)
        else:
            path.unlink()
        time.sleep(rng.choice((0, 0.05, 0.4)))
        for _ in range(rng.randrange(1, 4)):
            method = rng.choice(('settings.reload', 'settings.get', 'upload.files', 'settings.patch', 'history.list', 'diagnostics'))
            params = {'paths': [str(self.new_upload_file('small.png'))], 'profileId': self.profile} if method == 'upload.files' else \
                {'patch': {'retention': {'recentActivityLimit': 33}}, 'expectedRevision': self.revision_or_stale()} if method == 'settings.patch' else {}
            self.call(method, params, timeout=90)
        # Put the good file back, as the user does, and the resident must accept it again.
        path.write_bytes(good)
        time.sleep(rng.choice((0.05, 0.5)))
        reloaded = self.call('settings.reload', {}, timeout=90)
        if not reloaded.get('ok'):
            raise Finding('settings', 'settings.reload refuses the original file after it was restored: %s' % str(reloaded.get('error'))[:300])
        got = self.resident.call('settings.get', raw=True)
        if not got.get('ok') or (got['result']['metadata'].get('validation') or {}).get('ok') is False:
            raise Finding('settings', 'settings.get is not valid after the file was restored: %s' % str(got)[:300])

    def revision_or_stale(self):
        reply = self.resident.call('settings.get', raw=True)
        return reply['result']['metadata']['observedRevision'] if reply.get('ok') else 'stale'

    def a_archive_chaos(self):
        """The local-copy folder turns unwritable or vanishes under uploads and local-save retries."""
        rng = self.rng
        archive = self.run / 'archive'
        choice = rng.randrange(4)
        self.log('archive-chaos', choice=choice)
        if choice == 0:
            os.chmod(archive, 0o500)
        elif choice == 1:
            os.chmod(archive, 0o700)
        elif choice == 2:
            for sub in sorted(archive.glob('*')):
                if sub.is_dir():
                    os.chmod(sub, rng.choice((0o500, 0o700)))
        else:
            os.chmod(archive, 0o700)
            for sub in sorted(archive.glob('*')):
                if sub.is_dir():
                    os.chmod(sub, 0o700)
        for _ in range(rng.randrange(1, 3)):
            self.a_upload()
        method = rng.choice(('operation.retryLocalSave', 'history.copyImage', 'operation.retry', 'history.preview'))
        self.call(method, {'id': self.pick(self.operations) or self.pick(self.history_ids) or 'x'}, timeout=90)

    def a_restart(self):
        """A graceful restart in the middle of a sequence: ui.quit or a termination signal, then start again with the same roots."""
        if self.faults and self.options.clean_restarts:
            self.faults.write_spec([])  # the start itself is not under a file-system fault
        self.shutdown_and_check(self.rng.choice(('ui.quit', 'ui.quit', 'SIGTERM', 'SIGINT')))
        self.resident.start()
        self.add_s3_profile() if self.options.destination == 's3' else self.import_uploader()
        self.credentials_check()

    def credentials_check(self):
        """Session-mode credentials die with the resident: put them back the way the Settings window does, so uploads keep going."""
        credentials = self.receiver.credentials_after_restart(self)
        if credentials:
            stored = self.resident.call('setup.storeCredentials', {'profileId': self.profile, 'expectedRevision': self.revision(), 'credentials': credentials}, raw=True)
            self.log('credentials-reload', ok=stored.get('ok'), error=(stored.get('error') or {}).get('code'))
            setup = (stored.get('result') or {}).get('setupId')
            end = time.monotonic() + 20
            while setup and time.monotonic() < end:
                state = (self.resident.call('setup.inspect', {'id': setup}, raw=True).get('result') or {}).get('state')
                if state in ('completed', 'failed'):
                    break
                time.sleep(0.1)

    def shutdown_and_check(self, how='ui.quit'):
        self.drain(10)
        self.resident.quit(how)
        self.check_sanitizers()

    # --- invariants

    def check_sanitizers(self):
        found = self.resident.sanitizer_output()
        if found:
            raise Finding('sanitizer', '%s' % ', '.join(p.name for p in found))
        problems = self.resident.log_problems()
        if problems:
            raise Finding('log', 'the resident log shows %s' % problems[:3])

    def check_alive(self):
        if not self.resident.alive():
            raise Finding('crash', 'the resident is gone (exit %s)' % self.resident.returncode())
        # A helper that vanished from the tree is not visible; a dead putgd-ui reports through ui.* calls instead.

    def deep_check(self):
        settings = self.resident.call('settings.get', raw=True)
        if settings.get('ok'):
            metadata = settings['result']['metadata']
            validation = metadata.get('validation') or {}
            if validation.get('ok') is False:
                raise Finding('settings', 'settings.get reports validation not ok: %s' % str(validation)[:300])
        quiet = not self.active()
        listing = self.resident.call('history.list', {'limit': 200}, raw=True)
        counts = self.resident.call('history.counts', {}, raw=True)
        if quiet and not self.active() and listing.get('ok') and counts.get('ok'):
            total = (counts['result'] or {}).get('total')
            rows = listing['result']
            list_total = rows.get('total') if isinstance(rows, dict) else None
            if total is not None and list_total is not None and total != list_total:
                raise Finding('history', 'history.counts total %s differs from history.list total %s' % (total, list_total))
            if quiet and not self.active():
                self.cross_check_history(counts['result'])
        diagnostics = self.resident.call('diagnostics', raw=True)
        if not diagnostics.get('ok') and (diagnostics.get('error') or {}).get('code') not in BUSY_CODES:
            raise Finding('diagnostics', 'diagnostics failed: %s' % str(diagnostics)[:300])
        self.rss.append((self.step_index, self.resident.rss_kib()))

    def cross_check_history(self, counts):
        """History's counts, its filtered lists and its rows must agree while nothing is changing them."""
        categories = counts.get('categories') or {}
        for category in ('images', 'videos', 'text+other'):
            reply = self.resident.call('history.list', {'limit': 1, 'category': category}, raw=True)
            if reply.get('ok') and not self.active() and reply['result'].get('total') != categories.get(category):
                again = self.resident.call('history.counts', {}, raw=True)
                if again.get('ok') and not self.active() and (again['result'].get('categories') or {}).get(category) == categories.get(category):
                    raise Finding('history', 'history.list category %s has total %s but history.counts says %s' % (category, reply['result'].get('total'), categories.get(category)))
        failed = self.resident.call('history.list', {'limit': 1, 'uploadStatus': 'failed+unknown'}, raw=True)
        if failed.get('ok') and not self.active() and failed['result'].get('total') != categories.get('failed'):
            again = self.resident.call('history.counts', {}, raw=True)
            if again.get('ok') and not self.active() and (again['result'].get('categories') or {}).get('failed') == categories.get('failed'):
                raise Finding('history', 'history.list failed+unknown has total %s but history.counts says failed %s' % (failed['result'].get('total'), categories.get('failed')))
        for row in self.rows():
            if row.get('uploadStatus') not in self.TERMINAL:
                if not self.active() and not self.known_stuck(row):
                    raise Finding('history', 'History row %s is %r (phase %r) while no operation is active' % (row.get('id'), row.get('uploadStatus'), row.get('phase')))

    def active(self):
        reply = self.resident.call('diagnostics', raw=True)
        return (reply.get('result') or {}).get('activeOperations') if reply.get('ok') else None

    def drain(self, seconds):
        """Wait for every active operation to settle; one that stays active is a stuck upload."""
        end = time.monotonic() + seconds * SLOW
        while time.monotonic() < end:
            if not self.active():
                return True
            time.sleep(0.5)
        self.log('drain-timeout', seconds=seconds, active=self.active())
        return False


    # --- fault mode: the OS and the network say no

    def endpoint(self):
        """Where put.gd sends its uploads: the fault proxy in front of the receiver in fault mode, else the receiver itself."""
        return 'http://127.0.0.1:%d' % self.proxy_port if self.proxy_port else self.receiver.base

    def setup_faults(self):
        options = self.options
        self.faults = fault_control.FaultControl(self.run, random.Random(options.seed ^ 0x5eed), self.fault_classes, Path(__file__).resolve().parent / 'faults',
                                                 self.resident.build / 'putgd', log=self.log, seed=options.seed,
                                                 net_kinds=options.net_kinds.split(',') if options.net_kinds else None)
        self.resident.extra_env = self.faults.environment()
        if options.nofile:
            self.resident.nofile = options.nofile
        if options.memory_max:
            self.resident.memory_max = options.memory_max
        if 'net' in self.fault_classes:
            self.proxy_port = self.faults.start_proxy(self.receiver.port)

    FAULT_ACTIONS = [('upload', 12), ('upload_cancel', 2), ('history', 4), ('detail', 3), ('delete', 6), ('settings', 8), ('operations', 6), ('burst', 3),
                     ('receiver_rule', 4), ('restart', 1), ('editor_open', 1), ('editor_session', 2), ('editor_apply', 1), ('editor_end', 1), ('cache_clear', 1),
                     ('upload_weird', 2), ('fault_fs', 6), ('fault_net', 6), ('fault_disk_full', 3), ('fault_res', 1), ('fault_clear', 4)]

    def a_cache_clear(self):
        self.call('cache.clear', {}, timeout=120)

    def a_fault_fs(self):
        if 'fs' in self.fault_classes:
            self.faults.write_spec(self.faults.random_fs_rules())

    def a_fault_net(self):
        if 'net' in self.fault_classes:
            self.faults.set_net(self.faults.random_net_profile())

    def a_fault_disk_full(self):
        """The disk fills up: writes, truncates and allocations under the resident's roots fail with ENOSPC or EDQUOT, on top of the
        rules in force. A shim rule, not a filled filesystem: nothing on the host runs out of space."""
        if 'fs' not in self.fault_classes:
            return
        rng = self.faults.rng
        error = rng.choice(('ENOSPC', 'ENOSPC', 'EDQUOT'))
        target = fault_control.FS_TARGETS[rng.choice(('everything', 'everything', 'database', 'data', 'archive', 'state', 'cache'))]
        skip = rng.choice((0, 0, 3, 20))
        self.faults.write_spec(self.faults.rules + ['rule write,truncate,fallocate %s %s %s%s' % (target, rng.choice((0.3, 1.0, 1.0)), error, ' skip=%d' % skip if skip else '')])

    def a_fault_res(self):
        """The resident hits its open-file limit for a moment, with an upload and a deletion starting under it, then gets its limit back."""
        if 'res' not in self.fault_classes or not self.resident.alive():
            return
        rng = self.faults.rng
        window, start = [self.resident.log_offset(), None], self.resident.starts
        self.resident.descriptor_windows.setdefault(start, []).append(window)
        if not self.faults.lower_nofile(self.resident.process.pid, rng.choice((0, 1, 3, 8, 30)), start=start):
            self.resident.descriptor_windows[start].remove(window)
            return
        paths = [str(self.new_upload_file()) for _ in range(rng.choice((1, 2, 4)))]
        threads = [threading.Thread(target=lambda: self.resident.call('upload.files', {'paths': paths, 'profileId': self.profile}, timeout=120, raw=True) and None, daemon=True)]
        for thread in threads:
            thread.start()
        time.sleep(rng.choice((0.3, 1.0, 2.5)))
        self.lift(self.faults.restore_nofile, self.resident_pids())  # raises a finding unless the original limit is back
        window[1] = self.resident.log_offset()  # closed only now that the original limit is verified
        for thread in threads:
            thread.join(150)

    def a_fault_clear(self):
        """Lift one kind of fault, or all of them, the way a disk gets space again or a network comes back."""
        rng = self.faults.rng
        which = rng.choice(('fs', 'net', 'fs', 'all'))
        if which in ('fs', 'all'):
            self.faults.write_spec([])
        if which in ('net', 'all') and self.faults.proxy:
            self.lift(self.faults.proxy.clear)
        if which == 'all':
            self.lift(self.faults.restore_nofile, self.resident_pids())

    def lift(self, clear, *args):
        """Lift faults; a proxy that cannot end a faulted connection would let an old fault reach the recovery check, so that is a finding."""
        try:
            clear(*args)
        except (fault_control.fault_proxy.ProxyStuck, fault_control.NofileNotRestored) as error:
            raise Finding('driver', str(error))

    def resident_pids(self):
        return [self.resident.resident_pid()] if self.resident.alive() else []

    def run_faults(self):
        """The fault loop. Setup and every step run inside main()'s one cleanup path (Fuzzer.cleanup)."""
        self.setup()
        names = [name for name, weight in self.FAULT_ACTIONS for _ in range(weight)]
        try:
            for self.step_index in range(1, self.options.steps + 1):
                action = self.rng.choice(names)
                self.log('step', name=action)
                try:
                    self.act(action)
                    time.sleep(self.rng.choice((0, 0, 0, 0, 0.02, 0.1, 0.5)))
                    self.sweep()
                    self.check_alive()
                except Finding as error:
                    if error.kind != 'crash' or not self.emfile_abort():
                        raise
                    self.recover_from_abort()
                    continue
                self.check_sanitizers()
                if self.step_index % self.options.epoch == 0 and self.step_index < self.options.steps:
                    self.guarded_epoch_check()
            self.guarded_epoch_check(final=True)
            self.shutdown_and_check()
        except Finding:
            self.keep_receiver_state()
            raise

    def guarded_epoch_check(self, final=False):
        """The epoch check, with a resident that died on its own (an OOM kill, an open-files abort) judged by its restart like in a step."""
        for attempt in range(3):
            try:
                if not self.resident.alive():
                    self.check_alive()
                return self.epoch_check(final)
            except (Finding, OSError) as error:
                crashed = isinstance(error, OSError) or error.kind == 'crash'
                if not crashed or self.resident.alive() or not self.emfile_abort():
                    raise
                self.recover_from_abort()
        raise Finding('epoch', 'the resident aborted during each of 3 epoch checks in a row; no epoch check completed')

    def emfile_abort(self):
        """Qt and GLib abort when a thread cannot get its wake-up pipe (GLib-ERROR, 'Too many open files'): that is what running out of file
        descriptors does to any Qt program, not a defect to report. What matters is the restart."""
        if self.resident.alive():
            return False
        log = self.resident.log_path()
        text = log.read_bytes().decode('utf-8', 'surrogateescape') if log.exists() else ''
        if self.options.memory_max and self.resident.returncode() == -signal.SIGKILL:
            # The memory cgroup's OOM killer took it (--memory-max): the same as a SIGKILL, checked by the recovery that follows.
            self.log('oom-kill', limit=self.options.memory_max)
            self.resident.judged_logs.add(log.name)
            return True
        # Only an abort during a descriptor exhaustion the harness caused is expected, and only with nothing else wrong in its log.
        start = self.resident.starts
        if start not in self.resident.descriptor_windows:
            return False
        runtime_errors = 'runtime error:' in text
        if runtime_errors and not self.resident.descriptor_noise_only(text, start):
            return False
        # UBSan, halting, reads files to check object types: with no descriptor left it reports valid objects as invalid and exits 1.
        sanitizer_without_descriptors = self.resident.returncode() == 1 and runtime_errors
        if 'Too many open files' not in text and not sanitizer_without_descriptors:
            return False
        self.log('emfile-abort', exit=self.resident.returncode(), first_runtime_errors=[l for l in text.splitlines() if 'runtime error' in l][:3])
        self.resident.judged_logs.add(log.name)
        return True

    def recover_from_abort(self):
        """The resident is gone: do what a restart after a crash must do, and check it like a restart after a kill."""
        self.lift(self.faults.clear_all, [])
        self.resident.kill_tree()
        self.check_files_after_kill()
        self.resident.start()
        self.credentials_check()
        self.delete_operation = None
        self.verify_recovery('abort')

    def keep_receiver_state(self):
        """A finding is judged against what the server saw: keep its events and files before the receiver goes away."""
        try:
            state = self.receiver.state()
            (self.run / 'receiver-state.json').write_text(json.dumps({'events': state.get('events'), 'files': state.get('files'), 'uploads': state.get('uploads')}, default=str)[:20_000_000])
        except (OSError, ValueError):
            pass

    def epoch_check(self, final=False):
        """Lift every fault and require the whole state to be sound: nothing stuck, the files intact, History agreeing with the server, and a
        new upload, a settings write and a History write all working again (a full disk recovers once space is back)."""
        self.log('epoch-check', final=final)
        self.lift(self.faults.clear_all, self.resident_pids())
        self.receiver.rules([])
        time.sleep(0.3)
        self.wait_journal(120)  # a startup recovery that failed under a fault must finish once the fault is gone
        # An editor session waits for its user for ever: close them (the ones this run opened and any other in the activity list); the uploads are what is checked.
        recent = (self.resident.call('operation.recent', raw=True).get('result') or {}).get('operations', [])
        editing = {o.get('id') for o in recent if o.get('action') == 'edit' and o.get('state') not in ('completed', 'failed', 'cancelled', 'interrupted', 'partial')}
        for identifier in sorted(self.open_editors | editing):
            self.call('editor.cancel', {'id': identifier}, timeout=60)
        if not self.drain(120):
            self.settle()
        self.check_alive()
        self.check_sanitizers()
        self.check_files_after_kill(live=True)
        self.check_no_helper_faulted()
        self.verify_unknown_kept()
        self.verify_rows()
        self.probe_recovery()
        self.check_sanitizers()
        self.deep_check()
        codes = {}
        for row in self.rows():
            if row.get('uploadStatus') in ('failed', 'unknown'):
                key = '%s %s' % (row.get('uploadStatus'), (row.get('error') or {}).get('code') or '-')
                codes[key] = codes.get(key, 0) + 1
        self.log('epoch-ok', rss=self.resident.rss_kib(), error_codes=codes)  # the codes a user is shown, to be read for generic or misleading ones

    def check_no_helper_faulted(self):
        """The shim binds to the resident's executable and PID: a fault in any other process (an image, editor or upload helper) would be
        blamed on the resident, so it is a harness defect."""
        foreign = {pid: comm for pid, comm in self.faults.faulted_pids().items() if pid not in self.resident.pids_seen}
        if foreign:
            raise Finding('driver', 'the fault shim injected into processes that are not the resident: %s (resident PIDs %s)' % (foreign, sorted(self.resident.pids_seen)))

    def verify_unknown_kept(self):
        """An upload that was unknown and whose copy is on the server must still say so (#1572): never plain failed, cancelled or vanished.
        "On the server" is what the receiver holds now: a copy that was uploaded and then deleted is no longer a reason to warn."""
        stored = self.receiver.present()
        for row in self.rows():
            digest = row.get('sha256')
            if 'unknown' in self.seen.get(row.get('id'), ()) and row.get('uploadStatus') in ('failed', 'cancelled', 'canceled') and stored.get(digest) \
                    and not (row.get('receipt') or {}).get('directUrl'):
                raise Finding('unknown-lost', 'History row %s was unknown and is now %s while the server holds its content %s: the warning that a copy may be on the server is gone\nrow: %s' % (
                    row.get('id'), row.get('uploadStatus'), stored[digest], json.dumps({k: row.get(k) for k in ('id', 'operationId', 'uploadStatus', 'remoteStatus', 'phase', 'error')}, default=str)))

    def probe_recovery(self):
        """With every fault lifted the app must work: a fresh upload ends up on the server and in History as uploaded; a settings write succeeds."""
        before = self.receiver.upload_events()
        # The run's own settings steps may have set a small limit (a_settings): back to the defaults, so only a fault can fail the probe.
        self.call('settings.patch', {'patch': {'limits': {'pendingArtifactBytes': 'auto', 'maxFileBytes': 'auto'}}, 'expectedRevision': self.revision()})
        path = self.new_upload_file('notes.txt')
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        reply = self.call('upload.files', {'paths': [str(path)], 'profileId': self.profile}, timeout=90)
        if not reply.get('ok'):
            raise Finding('not-recovered', 'upload.files fails after every fault was lifted: %s' % str(reply.get('error'))[:300])
        operation = ((reply.get('result') or {}).get('operation') or {}).get('id')
        if not self.drain(90):
            raise Finding('not-recovered', 'the probe upload %s stays active 90 s after every fault was lifted' % operation)
        rows = [row for row in self.rows() if row.get('operationId') == operation]
        if rows and all((row.get('error') or {}).get('code') == 'credential_setup_required' for row in rows):
            # Session-mode credentials live in memory only; whatever dropped them (a restart, a helper) is not the fault under test.
            self.log('probe-credentials-lost')
            self.credentials_check()
            return self.probe_recovery()
        if not rows or any(row.get('uploadStatus') != 'uploaded' for row in rows):
            raise Finding('not-recovered', 'the probe upload %s is %s after every fault was lifted (server saw %d new uploads): %s' % (
                operation, [(r.get('uploadStatus'), r.get('error')) for r in rows], self.receiver.upload_events() - before, json.dumps(rows, default=str)[:600]))
        # "uploaded" is a claim: the receipt must name a file the receiver stored, with the probe's bytes.
        for row in rows:
            wrong = self.receiver.holds(row, digest, self.endpoint())
            if wrong:
                raise Finding('not-recovered', 'the probe upload %s is uploaded in History but %s: %s' % (operation, wrong, json.dumps(row, default=str)[:600]))
        # A lock file a faulted unlink left behind is stale after 30 s (SettingsService), so config_busy may last that long and no longer.
        end = time.monotonic() + 45
        while True:
            patched = self.call('settings.patch', {'patch': {'retention': {'recentActivityLimit': 100 + self.step_index % 50}}, 'expectedRevision': self.revision()})
            if patched.get('ok') or (patched.get('error') or {}).get('code') != 'config_busy' or time.monotonic() > end:
                break
            time.sleep(2)
        if not patched.get('ok'):
            raise Finding('not-recovered', 'settings.patch fails after every fault was lifted: %s' % str(patched.get('error'))[:300])

    # --- the loop

    def run_sequence(self):
        self.setup()
        names = [name for name, weight in self.ACTIONS for _ in range(weight)]
        try:
            for self.step_index in range(1, self.options.steps + 1):
                action = self.rng.choice(names)
                self.log('step', name=action)
                self.act(action)
                time.sleep(self.rng.choice((0, 0, 0, 0, 0.02, 0.1, 0.5)))
                self.sweep()
                self.check_alive()
                self.check_sanitizers()
                if self.step_index % self.options.check_every == 0:
                    self.deep_check()
            self.settle()
            self.deep_check()
            self.shutdown_and_check()
        finally:
            self.restore_permissions()

    def settle(self):
        """End of a sequence: cancel what is open and require every operation to end. An operation that cannot be cancelled and never ends is stuck."""
        if self.drain(30):
            return
        # operation.recent is bounded by the retention setting, so every editor this run opened is cancelled by its own id too.
        for identifier in sorted(self.open_editors):
            for _ in range(20):
                reply = self.call('editor.cancel', {'id': identifier}, timeout=60)
                if reply.get('ok') or (reply.get('error') or {}).get('code') != 'editor_starting':
                    break
                time.sleep(0.5)
            self.call('operation.cancel', {'id': identifier}, timeout=60)
        recent = (self.resident.call('operation.recent', raw=True).get('result') or {}).get('operations', [])
        for operation in recent:
            if operation.get('state') in ('completed', 'failed', 'cancelled', 'interrupted', 'partial'):
                continue
            identifier = operation.get('id')
            if operation.get('action') == 'edit':
                self.call('editor.cancel', {'id': identifier}, timeout=60)
            self.call('operation.cancel', {'id': identifier}, timeout=60)
        if not self.drain(60):
            reply = self.resident.call('operation.recent', raw=True)
            active = [(o.get('id'), o.get('state'), o.get('phase'), o.get('action')) for o in (reply.get('result') or {}).get('operations', []) if o.get('state') not in ('completed', 'failed', 'cancelled', 'interrupted', 'partial')]
            raise Finding('stuck', '%s operations are still active 60 s after every operation and editor was cancelled: %s' % (self.active(), active[:6]))

    def cleanup(self):
        """The one way out of every run, however it ends (a clean finish, a finding, any exception, SIGINT or SIGTERM): the resident and its
        helpers, the receiver and the proxy stop. Safe to call twice; a second signal does not cut it short. Every step runs whatever the
        one before it met: a harness finding (no pidfd for the resident, a tree that would not die) is kept, the resident's process group
        is still killed and the receiver still stopped, and the first such finding is raised once everything is done."""
        previous = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
        met = []
        try:
            if self.resident.process is not None:
                try:
                    running = self.resident.alive() or bool(self.resident.session_members())
                except Finding as error:  # no pidfd: liveness is unknown, so the tree is killed anyway (kill_tree kills the pinned group)
                    met.append(error)
                    running = True
                if running:
                    try:
                        self.resident.kill_tree()
                    except Finding as error:
                        met.append(error)
            for stop in ((self.receiver.stop if self.receiver else None), (self.faults.stop if self.faults else None), self.restore_permissions):
                if stop is None:
                    continue
                try:
                    stop()
                except Exception as error:  # noqa: BLE001 - the next step runs whatever this one met
                    met.append(Finding('driver', 'cleanup: %s failed: %s' % (getattr(stop, '__qualname__', stop), error)))
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)
        for error in met:
            print('cleanup: %s' % error, file=sys.stderr, flush=True)
        if met:
            raise met[0]

    def restore_permissions(self):
        for path in [self.run / 'archive'] + sorted((self.run / 'archive').glob('*')):
            try:
                os.chmod(path, 0o700)
            except OSError:
                pass

    def run_crash(self):
        self.setup()
        for self.step_index in range(1, self.options.steps + 1):
            self.crash_round()
            self.sweep()
        self.shutdown_and_check()

    def wait_milestone(self, base_events, kind, extra):
        """Wait for the upload to reach a point a kill should land at; returns after at most 8 s."""
        end = time.monotonic() + 8
        if kind == 'delay':
            time.sleep(extra)
            return
        seen = set()
        while time.monotonic() < end:
            if kind == 'events' and self.receiver.upload_events() - base_events >= extra:
                return
            if kind == 'phase':
                for operation in (self.resident.call('operation.recent', raw=True).get('result') or {}).get('operations', [])[-1:]:
                    seen.add((operation.get('state'), operation.get('phase')))
                if len(seen) >= extra:
                    return
            time.sleep(0.01 if self.options.destination != 's3' else 0.05)

    def crash_round(self):
        rng = self.rng
        scenario = rng.choice(('upload', 'upload', 'upload', 'delete', 'delete', 'settings', 'settings_async', 'editor'))
        self.log('crash-round', scenario=scenario)
        self.delete_operation = None
        self.deleting = set()
        base_events = self.receiver.upload_events()
        if scenario == 'upload':
            names = ['payload.bin', 'big.png', 'small.png', 'notes.txt', 'wide.png'] + (['multi.bin'] * 2 if self.multi is not None else [])
            paths = [self.new_upload_file(rng.choice(names)) for _ in range(rng.choice((1, 3, 8)))]
            if rng.random() < 0.3:
                self.a_receiver_rule()
            self.call('upload.files', {'paths': [str(p) for p in paths], 'profileId': self.profile})
            self.wait_milestone(base_events, rng.choice(('delay', 'events', 'events', 'phase', 'phase')), rng.choice((1, 2, 3, 0.05, 0.3, 1.0)))
        elif scenario == 'delete':
            listing = self.call('history.list', {'limit': 50, 'uploadStatus': 'uploaded'})
            ids = [row['id'] for row in (listing.get('result') or {}).get('records') or [] if isinstance(row, dict) and row.get('id')]
            if not ids:
                self.log('no-uploaded-rows-to-delete')
                return
            chosen = rng.sample(ids, min(len(ids), rng.choice((1, 3, 6))))
            if rng.random() < 0.3:
                self.a_receiver_rule()
            prepared = self.call('delete.prepare', {'ids': chosen})
            if prepared.get('ok'):
                before = self.receiver.delete_events()
                confirmed = self.call('delete.confirm', {'ids': chosen, 'acknowledgement': prepared['result'].get('acknowledgement')})
                self.deleting = set(chosen)
                self.delete_operation = (confirmed.get('result') or {}).get('deleteOperationId')
                end = time.monotonic() + 4
                while time.monotonic() < end and self.receiver.delete_events() <= before:
                    time.sleep(0.005 if self.options.destination != 's3' else 0.05)
            time.sleep(rng.choice((0, 0.005, 0.02, 0.1)))
        elif scenario == 'settings':
            for _ in range(rng.randrange(1, 6)):
                self.call('settings.patch', {'patch': {'retention': {'recentActivityLimit': rng.randrange(10, 300)}}, 'expectedRevision': self.revision()})
            time.sleep(rng.choice((0, 0.001, 0.01)))
        elif scenario == 'settings_async':
            # Writes in flight when the kill arrives: the saved file must never be half written.
            revision = self.revision()
            def patch(n):
                try:
                    self.resident.call('settings.patch', {'patch': {'retention': {'recentActivityLimit': 10 + n}}, 'expectedRevision': revision}, timeout=30, raw=True)
                except (OSError, Finding, ValueError):
                    pass  # the kill is the point
            threads = [threading.Thread(target=patch, args=(n,)) for n in range(rng.randrange(2, 12))]
            for thread in threads:
                thread.daemon = True
                thread.start()
            time.sleep(rng.choice((0, 0.002, 0.01, 0.03, 0.08)))
        else:
            opened = self.call('editor.open', {'source': 'file', 'path': str(self.fixtures['big.png'])})
            if opened.get('ok'):
                identifier = (opened['result'] or {}).get('operationId')
                time.sleep(rng.choice((0.1, 0.5, 1.5)))
                if identifier:
                    self.call('editor.submit', {'id': identifier})
                    time.sleep(rng.choice((0, 0.05, 0.3)))
        self.resident.kill_tree()
        self.log('killed')
        self.check_files_after_kill()
        if rng.random() < 0.25:
            # A second kill while the restart is still recovering the journal.
            self.resident.start_nowait()
            time.sleep(rng.choice((0.05, 0.3, 0.8, 1.5, 2.5)))
            self.resident.kill_tree()
            self.log('killed-during-recovery')
            self.check_files_after_kill()
        self.resident.start()
        self.credentials_check()
        self.verify_recovery(scenario)

    def check_files_after_kill(self, live=False):
        """What a SIGKILL leaves on disk: the configuration still parses, the history database passes its own integrity check.
        live: the resident is running (the fault mode's epoch check). Its database is then read through SQLite's backup API, which takes
        a consistent snapshot under SQLite's own locking; a stopped resident's files are copied as they are, journal and all."""
        path = self.config_path()
        if path.exists():
            try:
                json.loads(path.read_bytes())
            except ValueError as error:
                raise Finding('config', 'config.json does not parse after a kill: %s (%d bytes)' % (error, path.stat().st_size))
        for stray in sorted(path.parent.glob('*')):
            if stray.name not in ('config.json',) and not stray.name.endswith(('.lock',)):
                self.log('stray-config-file', name=stray.name)
        database = self.resident.root / 'data' / 'putgd' / 'history.sqlite'
        if database.exists() and live:
            self.check_live_database(database)
        elif database.exists():
            import sqlite3
            # A killed WAL database needs write access to recover, so the check runs on a copy and never touches the resident's files.
            scratch = self.run / 'dbcheck'
            scratch.mkdir(exist_ok=True)
            copies = []
            for suffix in ('', '-wal', '-shm', '-journal'):  # a hot rollback journal belongs to the database it guards
                source = Path(str(database) + suffix)
                if source.exists():
                    target = scratch / ('history.sqlite' + suffix)
                    shutil.copy2(source, target)
                    copies.append(target)
            connection = sqlite3.connect(str(scratch / 'history.sqlite'), timeout=10)
            try:
                result = [row[0] for row in connection.execute('PRAGMA integrity_check')]
            except sqlite3.DatabaseError as error:
                raise Finding('history', 'history.sqlite is not readable after a kill: %s' % error)
            finally:
                connection.close()
                for target in copies:
                    try:
                        target.unlink()
                    except OSError:
                        pass
            if result != ['ok']:
                raise Finding('history', 'history.sqlite integrity_check after a kill: %s' % result[:5])

    LIVE_DB_DEADLINE = 60  # seconds for one consistent snapshot of the live database

    def check_live_database(self, database):
        """A consistent snapshot of the running resident's database through SQLite's backup API, then integrity_check on it. The backup
        retries a busy or locked step by itself, without end: a monotonic deadline in its progress callback stops it, and a database that
        gives no snapshot within the deadline is a finding (a resident that holds its write lock that long blocks every History call)."""
        import sqlite3
        scratch = self.run / 'dbcheck'
        scratch.mkdir(exist_ok=True)
        target = scratch / 'live-snapshot.sqlite'
        deadline = time.monotonic() + self.LIVE_DB_DEADLINE

        class Overdue(Exception):
            pass

        def progress(status, remaining, total):
            if time.monotonic() > deadline:
                raise Overdue()

        last, result = 'the deadline passed', None
        while time.monotonic() < deadline:
            try:
                target.unlink()
            except OSError:
                pass
            try:
                source = sqlite3.connect('file:%s?mode=ro' % database, uri=True, timeout=max(0.1, min(5, deadline - time.monotonic())))
                try:
                    copy = sqlite3.connect(str(target))
                    try:
                        source.backup(copy, pages=64, progress=progress, sleep=0.1)
                        result = [row[0] for row in copy.execute('PRAGMA integrity_check')]
                    finally:
                        copy.close()
                finally:
                    source.close()
                break
            except Overdue:
                last = 'the backup did not finish'
                break
            except sqlite3.OperationalError as error:
                if 'locked' not in str(error) and 'busy' not in str(error):
                    raise Finding('history', 'history.sqlite cannot be read while the resident runs: %s' % error)
                last = str(error)
                time.sleep(0.5)  # the resident holds a write lock: try again within the deadline
            except sqlite3.DatabaseError as error:
                raise Finding('history', 'history.sqlite is not readable: %s' % error)
        if result is None:
            raise Finding('history', 'no consistent snapshot of the live history.sqlite within %d s (%s)' % (self.LIVE_DB_DEADLINE, last))
        try:
            target.unlink()
        except OSError:
            pass
        if result != ['ok']:
            raise Finding('history', 'history.sqlite integrity_check (a backup of the live database): %s' % result[:5])

    TERMINAL = ('uploaded', 'failed', 'unknown', 'stored', 'cancelled', 'canceled', 'local_only', 'skipped', 'conflict')

    def rows(self):
        found, offset = [], 0
        while True:
            reply = self.resident.call('history.list', {'limit': 200, 'offset': offset}, raw=True)
            if not reply.get('ok'):
                raise Finding('recovery', 'history.list fails after a kill: %s' % str(reply)[:300])
            batch = reply['result'].get('records') or []
            for row in batch:
                self.seen.setdefault(row.get('id'), set()).add(row.get('uploadStatus'))
                self.seen_digest.setdefault(row.get('sha256'), set()).add(row.get('uploadStatus'))
                self.digest_operations.setdefault(row.get('sha256'), set()).add(row.get('operationId'))
                self.digest_rows.setdefault(row.get('sha256'), set()).add(row.get('id'))
            found.extend(batch)
            if len(batch) < 200:
                return found
            offset += 200

    def wait_journal(self, seconds=90):
        """Journal recovery after a restart finishes on its own; one that does not is a stuck resident."""
        end = time.monotonic() + seconds
        started = time.monotonic()
        while time.monotonic() < end:
            reply = self.resident.call('operation.recent', raw=True)
            if reply.get('ok'):
                if time.monotonic() - started > 20:
                    self.log('slow-journal-recovery', seconds=round(time.monotonic() - started, 1))
                return
            if (reply.get('error') or {}).get('code') not in BUSY_CODES + RETRYING_CODES:
                raise Finding('recovery', 'operation.recent fails after a kill: %s' % str(reply)[:300])
            time.sleep(0.3)
        holders = []
        lock = os.path.realpath(self.resident.root / 'data' / 'putgd' / 'resident.lock')  # this run's lock, not another run's on the host
        for entry in Path('/proc').iterdir():
            if entry.name.isdigit():
                try:
                    for fd in (entry / 'fd').iterdir():
                        if os.readlink(fd) == lock:
                            holders.append((int(entry.name), (entry / 'comm').read_text().strip()))
                except OSError:
                    pass
        raise Finding('hang', 'journal recovery still initializing %d s after a restart; processes holding resident.lock: %s; resident pid %s; lock file: %s' % (
            seconds, holders, self.resident.process.pid, (self.resident.root / 'data' / 'putgd' / 'resident.lock').read_text().split() if (self.resident.root / 'data' / 'putgd' / 'resident.lock').exists() else None))

    def verify_recovery(self, scenario):
        self.wait_journal()
        if not self.drain(90):
            raise Finding('recovery', 'an operation stays active 90 s after the restart: %s active' % self.active())
        settings = self.resident.call('settings.get', raw=True)
        if not settings.get('ok') or (settings['result']['metadata'].get('validation') or {}).get('ok') is False:
            raise Finding('recovery', 'settings are not readable after a kill: %s' % str(settings)[:300])
        recent = self.resident.call('operation.recent', raw=True)
        if not recent.get('ok'):
            raise Finding('recovery', 'operation.recent fails after a kill: %s' % str(recent)[:300])
        for operation in recent['result'].get('operations') or []:
            if operation.get('state') in ('accepted', 'acquiring', 'running', 'uploading', 'publishing'):
                raise Finding('recovery', 'operation %s is %s/%s after the restart with nothing active' % (operation.get('id'), operation.get('state'), operation.get('phase')))
        rows, stored, snapshot, by_status = self.verify_rows()
        self.log('recovered', rows={k: len(v) for k, v in by_status.items()}, stored=len(stored))
        if self.delete_operation:
            self.finish_deletion(rows)
        # Retry what the kill interrupted: every retried file must end up on the server once or be reported.
        if self.rng.random() < 0.5 and self.operations_to_retry():
            self.retry_interrupted({row.get('sha256'): row.get('uploadStatus') for row in rows}, {d: len(v) for d, v in stored.items()})
        if self.options.destination == 's3' and self.rng.random() < 0.25:
            self.check_multipart_cleanup()
        self.check_sanitizers_after_kill()
        self.deep_check()

    def known_stuck(self, row):
        """--known-journal-stuck: a row left 'uploading' after the journal refused a write (#1579) is counted once and no longer ends the run."""
        if not self.options.known_journal_stuck or not self.faults:
            return False
        if self.faults.nofile_events or any('history.sqlite' in line for line in self.faults.tail(100000)):
            if row.get('id') not in self.stuck_rows:
                self.stuck_rows.add(row.get('id'))
                self.log('known-stuck-row', row=row.get('id'), status=row.get('uploadStatus'), issue=1579)
            return True
        return False

    stuck_rows = set()

    def allowed_copies(self, digest):
        """How many copies of this content the server may hold, counted from the upload attempts the run saw: one per History row that ever
        had it (the same file twice in a call, or the same bytes under two names, are two rows), one more per accepted operation.retry of an
        operation holding it, and one more per row that was ever unknown (its first attempt may have landed; the custom uploader cannot look
        a file up before sending it again)."""
        rows = self.digest_rows.get(digest, set())
        retries = sum(self.resident.retries.get(operation, 0) for operation in self.digest_operations.get(digest, ()))
        unknown = sum(1 for row in rows if 'unknown' in self.seen.get(row, ()))
        return len(rows) + retries + unknown

    def verify_rows(self):
        """History against the receiver, with nothing active: no row stuck, nothing uploaded that is not on the server, nothing on the
        server that History does not know, no unexplained second copy, deletions agreeing. Returns (rows, stored, snapshot, by_status)."""
        rows = self.rows()
        stored = self.receiver.stored()
        present = self.receiver.present()
        snapshot = self.receiver.snapshot()
        for digest, files in stored.items():
            allowed = max(1, self.allowed_copies(digest))  # content no row knows is an orphan, reported below
            if len(files) > allowed:
                raise Finding('duplicate-upload', 'the receiver stored the same content %d times (sha %s), more than the %d upload attempts the run made explain: %s' % (
                    len(files), digest[:12], allowed, files))
        by_status = {}
        for row in rows:
            status = row.get('uploadStatus')
            by_status.setdefault(status, []).append(row)
            if status not in self.TERMINAL:
                if self.known_stuck(row):
                    continue
                raise Finding('recovery', 'History row %s is %r (phase %r) after the restart' % (row.get('id'), status, row.get('phase')))
            digest = row.get('sha256')
            # A deletion that was tried (its outcome may be unknown after a lost reply) explains a file that is gone.
            removed = row.get('remoteStatus') not in ('not_requested', None) and self.receiver.remote(row, snapshot) == 'gone'
            # Presence is checked on its own, against what the server holds now: however many copies attempts explain, an uploaded row needs
            # one that is still there.
            if status == 'uploaded' and digest and not present.get(digest) and not self.row_removed_by_delete(row) and not removed:
                raise Finding('recovery', 'History row %s is uploaded but the receiver holds no copy of it' % row.get('id'))
            # A retry that fails turns an unknown outcome into "failed" while the first attempt's copy is on the server: not a loss.
            if status in ('stored', 'failed') and digest and stored.get(digest) and 'unknown' not in self.seen.get(row.get('id'), ()) and not row.get('receipt', {}).get('directUrl'):
                raise Finding('lost-upload', 'History row %s is %s but the receiver stored its content (%s): the upload is on the server and not in History as uploaded or unknown\nrow: %s\nevents: %s' % (
                    row.get('id'), status, stored[digest], json.dumps({k: row.get(k) for k in ('id', 'operationId', 'uploadStatus', 'remoteStatus', 'phase', 'name', 'sha256', 'bytes', 'error', 'uploadJournal')}, default=str),
                    self.receiver.describe(digest)))
        for row in rows:
            remote = self.receiver.remote(row, snapshot)
            if remote is None:
                continue
            if row.get('remoteStatus') in ('removed', 'already_missing') and remote != 'gone':
                raise Finding('delete', 'History row %s says its upload was removed but the receiver still serves it' % row.get('id'))
            if remote == 'gone' and row.get('remoteStatus') == 'not_requested':
                raise Finding('delete', 'the receiver deleted the file of History row %s but the row never asked for a deletion' % row.get('id'))
        known = {row.get('sha256') for row in rows}
        # A row caught mid-upload still shows the spooled file's digest while the server holds the re-encoded image it was sent: same name.
        known_names = {row.get('name') for row in rows}
        server_names = self.receiver.names()
        orphans = [d for d in stored if d not in known and server_names.get(d) not in known_names]
        if orphans:
            raise Finding('lost-upload', 'the receiver holds %d uploads no History row knows (%s)\nrows: %s\nevents: %s' % (
                len(orphans), [d[:12] for d in orphans[:3]],
                json.dumps([{k: row.get(k) for k in ('id', 'operationId', 'uploadStatus', 'name', 'sha256', 'bytes')} for row in rows]),
                [self.receiver.describe(d) for d in orphans]))
        return rows, stored, snapshot, by_status

    def journal_ids(self):
        found = set()
        for row in self.rows():
            journal = row.get('uploadJournal') or {}
            if isinstance(journal, dict) and journal.get('uploadId'):
                found.add(journal['uploadId'])
        return found

    def check_multipart_cleanup(self):
        """A multipart upload open on the server must belong to a journal put.gd still keeps (an interrupted item resumes it). One that cache.clear
        took the journal of without aborting it can never be aborted, and cache.clear must not report that it finished clean."""
        open_before = set(self.receiver.state()['uploads'])  # {id: {'key', 'parts', ...}}
        named_before = self.journal_ids()
        operations_before = [(o.get('id'), o.get('state')) for o in (self.resident.call('operation.recent', raw=True).get('result') or {}).get('operations', [])]
        reply = self.call('cache.clear', {}, timeout=120)
        if not reply.get('ok'):
            return
        result = reply['result'] or {}
        uploads_after = self.receiver.state()['uploads']
        open_after = set(uploads_after)
        named_after = self.journal_ids()
        # An upload with no part costs nothing and holds nothing; one with parts holds the user's bytes on the server for good.
        leaked = {i for i in open_after - named_after if uploads_after[i].get('parts')}
        empty = len(open_after - named_after) - len(leaked)
        dropped = leaked & named_before
        self.log('cache-clear', open_before=len(open_before), named_before=len(open_before & named_before), open_after=len(open_after), leaked=len(leaked),
                 dropped=len(dropped), without_parts=empty, aborted=result.get('abortedUploads'), removed=result.get('removedOperations'))
        if leaked and result.get('finished') and not result.get('incompleteUploads') and not result.get('incompleteUploadNotice'):
            raise Finding('multipart', 'cache.clear (removed %s operations, aborted %s) left %d multipart uploads open that no kept journal names, %d of which a journal named just before it: %s\noperations before: %s\nresult: %s' % (
                result.get('removedOperations'), result.get('abortedUploads'), len(leaked), len(dropped), sorted(leaked)[:3], operations_before[-12:], json.dumps(result)[:400]))

    deleting = set()
    seen = {}
    seen_digest = {}
    digest_operations = {}
    digest_rows = {}

    def finish_deletion(self, rows):
        """The kill came during a deletion: inspect it, retry it, and require every chosen row removed on both sides."""
        inspected = self.call('delete.inspect', {'id': self.delete_operation}, timeout=60)
        state = ((inspected.get('result') or {}).get('deletion') or inspected.get('result') or {}).get('state')
        self.log('delete-after-restart', state=state, ok=inspected.get('ok'), code=(inspected.get('error') or {}).get('code'))
        if state not in ('completed', 'cancelled'):
            retried = self.call('delete.retry', {'id': self.delete_operation}, timeout=90)
            self.log('delete-retry', ok=retried.get('ok'), code=(retried.get('error') or {}).get('code'))
            end = time.monotonic() + 60
            while time.monotonic() < end:
                inspected = self.call('delete.inspect', {'id': self.delete_operation}, timeout=60)
                state = ((inspected.get('result') or {}).get('deletion') or inspected.get('result') or {}).get('state')
                if state in ('completed', 'cancelled', 'failed', 'partial'):
                    break
                time.sleep(0.3)
            else:
                raise Finding('delete', 'the deletion %s is still %s 60 s after a retry' % (self.delete_operation, state))
        snapshot = self.receiver.snapshot()
        items = {item.get('historyId'): item.get('status') for item in ((inspected.get('result') or {}).get('deletion') or inspected.get('result') or {}).get('items') or []}
        for row in self.rows():
            if row.get('id') not in self.deleting:
                continue
            gone = self.receiver.remote(row, snapshot) == 'gone'
            item = items.get(row.get('id'))
            if item is not None and row.get('remoteStatus') != item:
                raise Finding('delete', 'deletion %s reports %s for row %s but History shows remoteStatus %s' % (self.delete_operation, item, row.get('id'), row.get('remoteStatus')))
            if item in ('removed', 'already_missing') and not gone:
                raise Finding('delete', 'deletion %s reports %s for row %s but the receiver still serves the file' % (self.delete_operation, item, row.get('id')))

    def row_removed_by_delete(self, row):
        return row.get('id') in self.deleting

    def operations_to_retry(self):
        wanted = {row.get('operationId') for row in self.rows() if row.get('uploadStatus') in ('stored', 'unknown', 'failed')}
        recent = self.resident.call('operation.recent', raw=True)
        return [o for o in (recent.get('result') or {}).get('operations', []) if o.get('id') in wanted]

    def retry_interrupted(self, before_status, before_count):
        for operation in self.operations_to_retry()[-3:]:
            reply = self.call('operation.retry', {'id': operation['id']}, timeout=90)
            self.log('retry', operation=operation['id'], ok=reply.get('ok'), code=(reply.get('error') or {}).get('code'))
        if not self.drain(90):
            raise Finding('recovery', 'a retry stays active 90 s')
        stored = self.receiver.stored()
        statuses = {row.get('sha256'): row.get('uploadStatus') for row in self.rows()}
        for digest, files in stored.items():
            # An unknown outcome may legitimately send a second copy (the custom uploader cannot look a file up);
            # a file History already showed as uploaded, or never sent, must not.
            if len(files) > before_count.get(digest, 0) and before_count.get(digest, 0) >= 1 and 'unknown' not in self.seen_digest.get(digest, ()):
                raise Finding('duplicate-upload', 'a retry stored content %s %d times although History had it as %r (%s)\nrows: %s\nevents: %s' % (
                    digest[:12], len(files), before_status.get(digest), files,
                    json.dumps([{k: row.get(k) for k in ('id', 'operationId', 'uploadStatus', 'remoteStatus', 'name', 'sha256', 'publicationId')} for row in self.rows() if row.get('sha256') == digest]),
                    self.receiver.describe(digest)))
            if len(files) > before_count.get(digest, 0) and before_count.get(digest, 0) == 0 and before_status.get(digest) in ('uploaded',):
                raise Finding('lost-upload', 'History had %s as uploaded with nothing on the server; a retry then stored it' % digest[:12])

    def check_sanitizers_after_kill(self):
        # A SIGKILLed process leaves no report; anything that is there came from a process that ran its own checks.
        found = self.resident.sanitizer_output()
        if found:
            raise Finding('sanitizer', '%s' % ', '.join(p.name for p in found))
        problems = self.resident.log_problems()
        if problems:
            raise Finding('log', 'the resident log shows %s' % problems[:3])


def write_finding(fuzzer, error):
    run = fuzzer.run
    record = {'seed': fuzzer.options.seed, 'mode': fuzzer.options.mode, 'step': fuzzer.step_index, 'kind': error.kind, 'detail': str(error.detail)[:4000],
              'sanitizer_files': [p.name for p in fuzzer.resident.sanitizer_output()],
              'rss_samples': fuzzer.rss[-20:]}
    if error.kind in ('hang', 'transport') and fuzzer.resident.alive():
        (run / 'stacks.txt').write_text(fuzzer.resident.stacks())
    if error.kind in ('hang', 'transport', 'stuck') and fuzzer.resident.alive():
        # What the resident says about its operations at the moment of the finding.
        try:
            (run / 'state.json').write_text(json.dumps({'operation.recent': fuzzer.resident.call('operation.recent', {}, timeout=20, raw=True),
                                                        'ui.snapshot': fuzzer.resident.call('ui.snapshot', {}, timeout=20, raw=True)}, indent=1)[:200000])
        except (OSError, ValueError, Finding):
            pass
    if getattr(fuzzer, 'faults', None):
        record['fault_rules'] = fuzzer.faults.rules
        record['fault_log_tail'] = fuzzer.faults.tail(60)
    (run / 'finding.json').write_text(json.dumps(record, indent=1))
    print('FINDING %s at step %d (seed %d): %s' % (error.kind, fuzzer.step_index, fuzzer.options.seed, str(error.detail)[:600]), flush=True)
    for path in fuzzer.resident.sanitizer_output()[:3]:
        print('--- %s' % path.name)
        print(path.read_text(errors='replace')[:6000])


class Interrupted(BaseException):
    """SIGINT or SIGTERM: not a finding; main() cleans up and exits 128 + the signal."""

    def __init__(self, signum):
        super().__init__(signum)
        self.signum = signum


DEFERRAL = {'depth': 0, 'pending': None}


def interrupted(signum, frame):
    if DEFERRAL['depth']:
        DEFERRAL['pending'] = signum  # raised when the section that must not be cut in half ends
        return
    raise Interrupted(signum)


class signals_deferred:
    """SIGINT and SIGTERM inside this block are raised when it ends, so a step that must happen whole (start a process and publish its
    pidfd) is never cut in half."""

    def __enter__(self):
        DEFERRAL['depth'] += 1

    def __exit__(self, *exc):
        DEFERRAL['depth'] -= 1
        if not DEFERRAL['depth'] and DEFERRAL['pending'] is not None:
            signum, DEFERRAL['pending'] = DEFERRAL['pending'], None
            raise Interrupted(signum)
        return False


def cleanup_finding(fuzzer, error, status):
    """A finding met while cleaning up: the run's finding when it had none, else noted in the one it has. Returns the exit status."""
    finding = fuzzer.run / 'finding.json'
    if status is None or not finding.exists():
        write_finding(fuzzer, error)
        return 1 if status is None else status
    record = json.loads(finding.read_text())
    record['cleanup_finding'] = '%s: %s' % (error.kind, str(error.detail)[:2000])
    finding.write_text(json.dumps(record, indent=1))
    print('FINDING %s during cleanup: %s' % (error.kind, str(error.detail)[:600]), flush=True)
    return status


def report_traces(fuzzer):
    """After cleanup (rr finishes a trace only once its tracees are gone): which traces rr completed, and so can be replayed."""
    complete, incomplete = fuzzer.resident.traces()
    print('rr traces: %d complete (replayable) %s; %d incomplete %s' % (len(complete), complete, len(incomplete), incomplete), flush=True)
    finding = fuzzer.run / 'finding.json'
    if finding.exists():
        record = json.loads(finding.read_text())
        record['rr_traces'] = {'complete': complete, 'incomplete': incomplete}
        finding.write_text(json.dumps(record, indent=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('run', 'crash', 'faults'))
    parser.add_argument('--build', required=True, help='the sanitizer build folder holding putgd, putgd-ui and the helpers')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--steps', type=int, default=300)
    parser.add_argument('--check-every', type=int, default=10)
    parser.add_argument('--run-dir', help='a new short private folder (default /tmp/at/<mode>-<seed>)')
    parser.add_argument('--fixture-server', help='tests/fixtures/http_uploader_server.py when not run from the repository')
    parser.add_argument('--destination', choices=('http', 's3'), default='http', help='the loopback destination: the custom uploader or put.gd\'s S3 provider')
    parser.add_argument('--tsan-prefix', help='the folder holding deps-tsan and qtbase-tsan-build (docs/testing/thread-sanitizer.md): run a ThreadSanitizer build')
    parser.add_argument('--wrap', help='a command prefix for the resident, such as "rr record --chaos" (put an absolute _RR_TRACE_DIR in --env)')
    parser.add_argument('--env', action='append', default=[], help='NAME=VALUE for the resident (repeatable)')
    parser.add_argument('--verbose', action='store_true')
    faults = parser.add_argument_group('faults mode')
    faults.add_argument('--fault-classes', default='fs,net', help='comma list of fs (the LD_PRELOAD shim), net (the proxy), res (the open-file limit)')
    faults.add_argument('--clean-restarts', action='store_true', help='lift file-system faults before a restart, so a start under a fault (a known class of bugs) does not end every run')
    faults.add_argument('--net-kinds', help='only these proxy faults (reset_before, reset_request, reset_after_request, cut_response, stall, stall_response, dup_response, slow, latency)')
    faults.add_argument('--known-journal-stuck', action='store_true', help='count, and do not stop on, rows left uploading after a journal write fault (#1579)')
    faults.add_argument('--epoch', type=int, default=40, help='steps between lifting every fault and checking the whole state')
    faults.add_argument('--nofile', type=int, help='start the resident with this open-file limit')
    faults.add_argument('--memory-max', help='start the resident in a systemd-run --user scope with this MemoryMax (e.g. 600M)')
    options = parser.parse_args()
    # SIGINT and SIGTERM end the run through Fuzzer.cleanup: the resident and the receiver run in sessions of their own, so a Ctrl+C never
    # reaches them and only the harness can stop them. A handler (unlike an inherited SIG_IGN, which a job started with & from a shell
    # without job control has) is reset at exec, so the resident still gets the default action of the signals a graceful-stop step sends.
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGQUIT, signal.SIG_DFL)  # a background shell job inherits SIGQUIT ignored, and the resident would too
    if not options.run_dir:
        options.run_dir = str(Path.home() / 'work' / 'apptest-runs' / ('%s-%d' % (options.mode, options.seed)))
    # Absolute before anything uses it: the resident's XDG_RUNTIME_DIR and the socket-length check need the real path, and a recorder
    # (rr) or a caller with another working folder would resolve a relative one elsewhere.
    options.run_dir = os.path.abspath(options.run_dir)
    if options.wrap and options.mode == 'faults':
        raise SystemExit('--wrap and the faults mode do not combine: the fault shim would load into the recorder')
    run = Path(options.run_dir)
    if run.exists():
        raise SystemExit('%s exists: pick a new --run-dir (a failed run is evidence)' % run)
    run.mkdir(parents=True)
    print('seed %d mode %s steps %d run %s' % (options.seed, options.mode, options.steps, run), flush=True)
    fuzzer, status = None, None
    try:
        fuzzer = Fuzzer(options)
        {'run': fuzzer.run_sequence, 'crash': fuzzer.run_crash, 'faults': fuzzer.run_faults}[options.mode]()
    except Finding as error:
        write_finding(fuzzer, error)
        status = 1
    except Interrupted as stop:
        print('interrupted by %s at step %d: stopping the resident and the receiver' % (signal.Signals(stop.signum).name, fuzzer.step_index if fuzzer else 0), flush=True)
        status = 128 + stop.signum
    finally:
        if fuzzer:
            try:
                fuzzer.cleanup()
            except Finding as error:  # cleanup did everything first, then raised what it met: a harness finding of its own
                status = cleanup_finding(fuzzer, error, status)
            if fuzzer.resident.wrapped:
                report_traces(fuzzer)
    if status is not None:
        return status
    if os.environ.get('ACTION_FUZZ_STATS'):
        with open(os.environ['ACTION_FUZZ_STATS'], 'a') as out:
            out.write(json.dumps({'mode': options.mode, 'seed': options.seed, 'steps': options.steps, 'calls': fuzzer.stats,
                                  'rss': [r for _, r in fuzzer.rss]}) + '\n')
    print('clean: seed %d, %d steps, rss kib samples %s' % (options.seed, options.steps, fuzzer.rss[-6:]), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
