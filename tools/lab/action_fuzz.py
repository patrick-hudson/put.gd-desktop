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

SOCKET_LIMIT = 107
TOKEN = 'putgd-synthetic-uploader-token-not-real'
# What a clean sanitizer run never writes: any of these in a log is a finding.
BAD_MARKERS = ('AddressSanitizer', 'LeakSanitizer', 'runtime error:', 'ThreadSanitizer', 'UndefinedBehaviorSanitizer',
               'Assertion', 'terminate called', 'Segmentation fault', 'QFATAL', 'Fatal:', 'double free', 'SUMMARY:')
# Resident states that are not an answer to the call but the resident's own.
BUSY_CODES = ('initializing', 'journal_busy')


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
        self.port = int(self.process.stdout.readline())
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
    def __init__(self, run, build):
        self.run, self.build = run, Path(build)
        self.root = run / 'r'
        self.runtime = self.root / 'rt'
        self.socket_path = str(self.runtime / 'putgd' / 'session.sock')
        if len(self.socket_path.encode()) > SOCKET_LIMIT:
            raise SystemExit('socket path %s is %d bytes (limit %d): use a shorter --run-dir' % (self.socket_path, len(self.socket_path.encode()), SOCKET_LIMIT))
        self.san = run / 'san'
        self.process = None
        self.starts = 0
        self.sequence = 0
        self.lock = threading.Lock()

    def environment(self):
        for name in ('config', 'data', 'state', 'cache', 'rt', 'home'):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        os.chmod(self.runtime, 0o700)
        self.san.mkdir(exist_ok=True)
        env = {k: os.environ[k] for k in ('PATH', 'DBUS_SESSION_BUS_ADDRESS', 'LANG', 'XDG_DATA_DIRS') if k in os.environ}
        env.update(HOME=str(self.root / 'home'), XDG_CONFIG_HOME=str(self.root / 'config'), XDG_DATA_HOME=str(self.root / 'data'),
                   XDG_STATE_HOME=str(self.root / 'state'), XDG_CACHE_HOME=str(self.root / 'cache'), XDG_RUNTIME_DIR=str(self.runtime),
                   QT_QPA_PLATFORM='offscreen', PUTGD_STARTED_FOR_UPLOAD='1',
                   ASAN_OPTIONS='halt_on_error=1:print_stacktrace=1:detect_leaks=1:detect_stack_use_after_return=1:strict_init_order=1:abort_on_error=0:log_path=%s/asan' % self.san,
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:log_path=%s/ubsan' % self.san,
                   LSAN_OPTIONS='log_path=%s/lsan' % self.san)
        return env

    def start_nowait(self):
        """Start the resident and return at once, for a kill during its recovery."""
        self.starts += 1
        log = open(self.run / 'logs' / ('resident-%d.log' % self.starts), 'w')
        self.process = subprocess.Popen([str(self.build / 'putgd'), '--background'], env=self.environment(), stdin=subprocess.DEVNULL, stdout=log,
                                        stderr=subprocess.STDOUT, start_new_session=True)

    def start(self):
        if self.process is not None and self.process.poll() is None:
            raise Finding('driver', 'start with a live resident')
        self.starts += 1
        log = open(self.run / 'logs' / ('resident-%d.log' % self.starts), 'w')
        self.process = subprocess.Popen([str(self.build / 'putgd'), '--background'], env=self.environment(), stdin=subprocess.DEVNULL, stdout=log,
                                        stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise Finding('startup', 'the resident exited %s while starting' % self.process.returncode)
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
        end = time.monotonic() + 60
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
        client.settimeout(timeout)
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
        return reply if raw else reply.get('result')

    def alive(self):
        return self.process is not None and self.process.poll() is None

    def children(self):
        """PIDs of the resident and everything it started, from /proc: children of every thread (a worker thread's child is not listed under the
        main thread), and every process left in the resident's session, such as a vfork-fallback child that waits for a parent that is gone."""
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
        return sorted(pids)

    def session_members(self):
        found = []
        if self.process is None:
            return found
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            try:
                stat = (entry / 'stat').read_text()
                fields = stat[stat.rindex(')') + 2:].split()
                if int(fields[3]) == self.process.pid:  # session id
                    found.append(int(entry.name))
            except (OSError, ValueError, IndexError):
                pass
        return found

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
        """SIGKILL the resident and its helpers (exact PIDs found from /proc; never a pattern kill)."""
        for _ in range(3):
            pids = self.children()
            for pid in reversed(pids):
                try:
                    os.kill(pid, sig)
                except OSError:
                    pass
            if self.process is not None:
                try:
                    self.process.wait(20)
                except subprocess.TimeoutExpired:
                    raise Finding('hang', 'the resident did not die after SIGKILL')
            # Whatever the resident left behind in its session (a child it started in the last moment) dies too.
            if not self.session_members():
                return
            time.sleep(0.05)
        raise Finding('hang', 'processes of the killed resident are still alive: %s' % self.session_members())

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
            os.kill(self.process.pid, getattr(signal, how))
        try:
            self.process.wait(60)
        except subprocess.TimeoutExpired:
            raise Finding('hang', 'the resident did not exit within 60 s of %s' % how)
        # After SIGTERM or SIGINT the status is the signal's or 0; a sanitizer abort is neither.
        if self.process.returncode not in (0, -signal.SIGTERM, -signal.SIGINT, 128 + signal.SIGTERM, 128 + signal.SIGINT):
            raise Finding('exit', 'the resident exited with %s after %s' % (self.process.returncode, how))
        # Helpers it started leave with it.
        end = time.monotonic() + 10
        while self.session_members() and time.monotonic() < end:
            time.sleep(0.1)
        if self.session_members():
            raise Finding('orphan', 'processes of the resident are still alive 10 s after it exited after %s: %s' % (how, self.session_members()))

    def stacks(self):
        """Symbolized stacks of the resident and its helpers (eu-stack, same user), for a hang report."""
        out = []
        for pid in self.children():
            try:
                name = Path('/proc/%d/comm' % pid).read_text().strip()
                reply = subprocess.run(['eu-stack', '-p', str(pid)], capture_output=True, text=True, timeout=120)
                out.append('=== %s (%d)\n%s%s' % (name, pid, reply.stdout, reply.stderr[-300:]))
            except (OSError, subprocess.TimeoutExpired) as error:
                out.append('=== pid %d: no stack (%s)' % (pid, error))
        return '\n'.join(out)

    def sanitizer_output(self):
        found = sorted(p for p in self.san.glob('*') if p.is_file())
        return found

    def log_problems(self):
        problems = []
        for path in sorted((self.run / 'logs').glob('resident-*.log')):
            try:
                text = path.read_text(errors='replace')
            except OSError:
                continue
            for marker in BAD_MARKERS:
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
        for name in ('logs', 'fixtures', 'saves', 'archive'):
            (self.run / name).mkdir(parents=True, exist_ok=True)
        self.resident = Resident(self.run, options.build)
        self.receiver = None
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
        source = Path(self.options.fixture_server) if self.options.fixture_server else Path(__file__).resolve().parents[2] / 'tests/fixtures/http_uploader_server.py'
        if self.options.destination == 's3':
            self.receiver = S3Receiver(source.with_name('s3_server.py'), self.run)
        else:
            self.receiver = Receiver(source, self.run)
        self.receiver.reset()
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
            profile['settings'].update(endpoint=self.receiver.base, bucket='isolated-fixture', region='eu-west-3', objectPrefix='p', aclMode='omit',
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
                                    'RequestMethod': 'POST', 'RequestURL': self.receiver.base + '/upload/json', 'Headers': {'Authorization': TOKEN},
                                    'Body': 'MultipartFormData', 'FileFormName': 'file', 'URL': '{json:files[0].url}',
                                    'DeletionURL': '{json:files[0].delete}'}))
        os.chmod(sxcu, 0o600)
        settings = self.resident.call('settings.get')
        if any(p.get('provider') == 'custom-http' for p in (settings['document'].get('profiles') or [])):
            self.profile = next(p['id'] for p in settings['document']['profiles'] if p.get('provider') == 'custom-http')
            return
        reply = self.resident.call('settings.importUploader', {'path': str(sxcu), 'expectedRevision': settings['metadata']['observedRevision'], 'credentialMode': 'session',
                                                                'profileId': 'fuzz', 'label': 'Fuzz'})
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
            raise Finding('crash', '%s: %s and the resident is gone (exit %s)' % (method, error, self.resident.process.returncode))
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
        for pid in self.resident.children():
            if pid == self.resident.process.pid:
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
        try:
            if action == 'kill':
                os.kill(pid, signal.SIGKILL)
            else:
                os.kill(pid, signal.SIGSTOP)
                hold = rng.choice((0.5, 3.0, 12.0))
                end = time.monotonic() + hold
                # The resident keeps answering while the helper is frozen.
                while time.monotonic() < end:
                    self.call(rng.choice(('capabilities', 'history.list', 'operation.recent', 'settings.get')), {}, timeout=30)
                    time.sleep(0.3)
                os.kill(pid, signal.SIGCONT)
        except OSError:
            pass

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
        self.shutdown_and_check(self.rng.choice(('ui.quit', 'ui.quit', 'SIGTERM', 'SIGINT')))
        self.resident.start()
        self.import_uploader()
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
            raise Finding('crash', 'the resident is gone (exit %s)' % self.resident.process.returncode)
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
                if not self.active():
                    raise Finding('history', 'History row %s is %r (phase %r) while no operation is active' % (row.get('id'), row.get('uploadStatus'), row.get('phase')))

    def active(self):
        reply = self.resident.call('diagnostics', raw=True)
        return (reply.get('result') or {}).get('activeOperations') if reply.get('ok') else None

    def drain(self, seconds):
        """Wait for every active operation to settle; one that stays active is a stuck upload."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if not self.active():
                return True
            time.sleep(0.5)
        self.log('drain-timeout', seconds=seconds, active=self.active())
        return False

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
            self.receiver.stop()
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

    def restore_permissions(self):
        for path in [self.run / 'archive'] + sorted((self.run / 'archive').glob('*')):
            try:
                os.chmod(path, 0o700)
            except OSError:
                pass

    def run_crash(self):
        self.setup()
        try:
            for self.step_index in range(1, self.options.steps + 1):
                self.crash_round()
                self.sweep()
            self.shutdown_and_check()
        finally:
            self.receiver.stop()

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

    def check_files_after_kill(self):
        """What a SIGKILL leaves on disk: the configuration still parses, the history database passes its own integrity check."""
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
        if database.exists():
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
            if (reply.get('error') or {}).get('code') not in BUSY_CODES:
                raise Finding('recovery', 'operation.recent fails after a kill: %s' % str(reply)[:300])
            time.sleep(0.3)
        holders = []
        for entry in Path('/proc').iterdir():
            if entry.name.isdigit():
                try:
                    for fd in (entry / 'fd').iterdir():
                        if os.readlink(fd).endswith('resident.lock'):
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
        rows = self.rows()
        stored = self.receiver.stored()
        snapshot = self.receiver.snapshot()
        for digest, files in stored.items():
            if len(files) > 1 and digest not in self.expected_duplicates:
                raise Finding('duplicate-upload', 'the receiver stored the same content %d times (sha %s): %s' % (len(files), digest[:12], files))
        by_status = {}
        for row in rows:
            status = row.get('uploadStatus')
            by_status.setdefault(status, []).append(row)
            if status not in self.TERMINAL:
                raise Finding('recovery', 'History row %s is %r (phase %r) after the restart' % (row.get('id'), status, row.get('phase')))
            digest = row.get('sha256')
            # A deletion that was tried (its outcome may be unknown after a lost reply) explains a file that is gone.
            removed = row.get('remoteStatus') not in ('not_requested', None) and self.receiver.remote(row, snapshot) == 'gone'
            if status == 'uploaded' and digest and digest not in self.expected_duplicates and len(stored.get(digest, [])) != 1:
                if not self.row_removed_by_delete(row) and not removed:
                    raise Finding('recovery', 'History row %s is uploaded but the receiver holds it %d times' % (row.get('id'), len(stored.get(digest, []))))
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
            if len(files) > 1:
                self.expected_duplicates.add(digest)
            if len(files) > before_count.get(digest, 0) and before_count.get(digest, 0) == 0 and before_status.get(digest) in ('uploaded',):
                raise Finding('lost-upload', 'History had %s as uploaded with nothing on the server; a retry then stored it' % digest[:12])

    expected_duplicates = set()

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
    (run / 'finding.json').write_text(json.dumps(record, indent=1))
    print('FINDING %s at step %d (seed %d): %s' % (error.kind, fuzzer.step_index, fuzzer.options.seed, str(error.detail)[:600]), flush=True)
    for path in fuzzer.resident.sanitizer_output()[:3]:
        print('--- %s' % path.name)
        print(path.read_text(errors='replace')[:6000])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('run', 'crash'))
    parser.add_argument('--build', required=True, help='the sanitizer build folder holding putgd, putgd-ui and the helpers')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--steps', type=int, default=300)
    parser.add_argument('--check-every', type=int, default=10)
    parser.add_argument('--run-dir', help='a new short private folder (default /tmp/at/<mode>-<seed>)')
    parser.add_argument('--fixture-server', help='tests/fixtures/http_uploader_server.py when not run from the repository')
    parser.add_argument('--destination', choices=('http', 's3'), default='http', help='the loopback destination: the custom uploader or put.gd\'s S3 provider')
    parser.add_argument('--verbose', action='store_true')
    options = parser.parse_args()
    if not options.run_dir:
        options.run_dir = str(Path.home() / 'work' / 'apptest-runs' / ('%s-%d' % (options.mode, options.seed)))
    run = Path(options.run_dir)
    if run.exists():
        raise SystemExit('%s exists: pick a new --run-dir (a failed run is evidence)' % run)
    run.mkdir(parents=True)
    print('seed %d mode %s steps %d run %s' % (options.seed, options.mode, options.steps, run), flush=True)
    fuzzer = Fuzzer(options)
    try:
        fuzzer.run_sequence() if options.mode == 'run' else fuzzer.run_crash()
    except Finding as error:
        write_finding(fuzzer, error)
        if fuzzer.resident.alive():
            fuzzer.resident.kill_tree()
        return 1
    if os.environ.get('ACTION_FUZZ_STATS'):
        with open(os.environ['ACTION_FUZZ_STATS'], 'a') as out:
            out.write(json.dumps({'mode': options.mode, 'seed': options.seed, 'steps': options.steps, 'calls': fuzzer.stats,
                                  'rss': [r for _, r in fuzzer.rss]}) + '\n')
    print('clean: seed %d, %d steps, rss kib samples %s' % (options.seed, options.steps, fuzzer.rss[-6:]), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
