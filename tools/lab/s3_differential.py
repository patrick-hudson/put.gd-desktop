#!/usr/bin/env python3
"""Differential test of put.gd's S3 provider across S3-compatible servers.

The same seeded sequence of uploads, deletes and existence checks runs through the real resident (`putgd --background`, headless,
private bus and XDG roots, session credentials: no desktop, no keyring) against several servers. For every case it records what
put.gd reported (operation state, item status, error code and details, receipt key, link) and, independently of put.gd, what each
server holds under the case's prefix (key, size, sha256, content type, user metadata, ETag) and what is left over (incomplete
multipart uploads, stray keys). `report` then lines the servers up per case and marks where put.gd's behaviour differs.

  python3 -I tools/lab/s3_differential.py --build build/release --out RUN run --servers oracle,minio,... [--cases a,b]
  python3 -I tools/lab/s3_differential.py --out RUN report

Servers: `oracle` (tests/fixtures/s3_server.py) starts by itself; the containers come from tools/lab/s3_servers.py (start them
first); `aws` and `r2` read ~/Documents/creds.txt in memory (key set 2 for AWS) and use a unique prefix they remove again.
A cloud run refuses a bucket whose versioning is enabled or suspended, before it writes anything. Cleanup covers each case's prefix and its
leading-slash twin ('/<prefix>'), checks every delete and multipart abort, and the command exits 1 when anything of the run is left.
Every request of the local servers goes through the fault proxy (tools/lab/s3_fault_proxy.py), which also records what put.gd sent.

Run it on a build server, under `dbus-run-session`, with --out on disk. Synthetic payloads only. Credentials never reach a
command line, a log or the results.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import sys
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import action_fuzz as af  # noqa: E402  (the resident driver and the offline oracle receiver)
import s3_servers as servers  # noqa: E402
from s3_fault_proxy import Proxy, Rule  # noqa: E402

MIB = 1 << 20
MULTIPART_THRESHOLD = 64 * MIB
TERMINAL_UPLOAD = ('uploaded', 'failed', 'unknown', 'cancelled', 'skipped', 'refused', 'rejected')
TERMINAL_STATES = ('completed', 'partial', 'failed', 'cancelled', 'unknown', 'done', 'skipped')
ORACLE_ACCESS, ORACLE_SECRET = 'PUTGD_SYNTHETIC_ACCESS', 'putgd-synthetic-secret-not-a-real-key'


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        while True:
            block = handle.read(MIB)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def make_file(path, size, seed=0):
    """Deterministic incompressible content of `size` bytes: 1 MiB of seeded noise, each block perturbed by its index."""
    rng = random.Random(seed)
    block = bytes(rng.getrandbits(8) for _ in range(4096)) * 256
    with open(path, 'wb') as handle:
        written, index = 0, 0
        while written < size:
            part = bytearray(block[:min(MIB, size - written)])
            if part:
                part[0:8] = index.to_bytes(8, 'big')
            handle.write(part)
            written += len(part)
            index += 1
    return path


# ---------------------------------------------------------------------------------------------- targets

class Target:
    """One server: how put.gd reaches it, and how the harness sees what it stores (independently of put.gd)."""
    name = ''
    cloud = False
    proxied = True
    region = 'us-east-1'
    bucket = servers.BUCKET
    root_prefix = 'd'
    capabilities = set()

    def settings(self, **extra):
        raise NotImplementedError

    # backend: what is stored, independently of put.gd
    #
    # A prefix names a case's namespace and, next to it, the same text with a leading slash: put.gd can send a key that starts with '/'
    # (objectPrefix '/x/'), which S3 stores as a different key from 'x/...'. Every listing, cleanup and leftover check covers both.
    @staticmethod
    def namespaces(prefix):
        return [prefix, '/' + prefix]

    def stored(self, prefix):
        return [entry for namespace in self.namespaces(prefix) for entry in self.stored_in(namespace)]

    def raw_keys(self, prefix):
        return [entry for namespace in self.namespaces(prefix) for entry in self.raw_keys_in(namespace)]

    def uploads(self, prefix):
        """Incomplete multipart uploads under the prefix (and its leading-slash twin); None when the server cannot list them."""
        found = [self.uploads_in(namespace) for namespace in self.namespaces(prefix)]
        return None if any(entry is None for entry in found) else [upload for entries in found for upload in entries]

    def stored_in(self, prefix):
        raise NotImplementedError

    def raw_keys_in(self, prefix):
        raise NotImplementedError

    def uploads_in(self, prefix):
        raise NotImplementedError

    def seed(self, key, body, headers=None):
        raise NotImplementedError

    def remove(self, key):
        raise NotImplementedError

    def head(self, key):
        raise NotImplementedError

    def abort(self, key, upload):
        raise NotImplementedError

    def require_unversioned(self):
        """Cloud runs write only into a bucket that never keeps versions: cleanup deletes current objects, and on a versioned bucket that
        would leave versions and delete markers behind. Refused before anything is written."""
        raise NotImplementedError

    def cleanup(self, prefix):
        """Delete exactly what is under this case's prefix (and its leading-slash twin), abort its incomplete uploads and check that both
        are really gone. Raises with what could not be removed."""
        failed = []
        for key, _size, _etag in self.raw_keys(prefix):
            status = self.remove(key)
            if status not in (200, 204):
                failed.append(('delete', key[-60:], status))
        listed = self.uploads(prefix)
        if listed is None:
            failed.append(('uploads-unlisted', prefix[-40:], None))
        for key, upload in listed or []:
            status = self.abort(key, upload)
            # 404 is an upload that is already gone (NoSuchUpload); any other answer means it may still be there.
            if status not in (200, 204, 404):
                failed.append(('abort', key[-60:], status))
        remaining_keys = self.raw_keys(prefix)
        remaining_uploads = self.uploads(prefix)
        if remaining_uploads is None:
            failed.append(('uploads-unlisted-after-cleanup', prefix[-40:], None))
            remaining_uploads = []
        if remaining_keys:
            failed.append(('still-stored', [k[-60:] for k, _s, _e in remaining_keys[:3]], len(remaining_keys)))
        if remaining_uploads:
            failed.append(('still-incomplete', [k[-60:] for k, _u in remaining_uploads[:3]], len(remaining_uploads)))
        if failed:
            raise RuntimeError('cleanup of %s left %d problem(s): %s' % (prefix[-40:], len(failed), failed[:3]))

    def credentials(self):
        raise NotImplementedError

    def close(self):
        pass


class RawTarget(Target):
    """A server reached with the raw SigV4 client of s3_servers.py."""

    def __init__(self, name, endpoint, region, bucket, access, secret, upstream=None, cloud=False, listing=True, root_prefix='d'):
        self.name, self.region, self.bucket, self.cloud, self.root_prefix = name, region, bucket, cloud, root_prefix
        self.endpoint = endpoint
        self.access, self.secret = access, secret
        self.raw = servers.RawS3(endpoint, region, access, secret, bucket)
        self.proxy = None
        self.proxied = not cloud
        if self.proxied:
            parts = urllib.parse.urlsplit(endpoint)
            self.proxy = Proxy(parts.hostname, parts.port)
        self.listing = listing

    @property
    def put_endpoint(self):
        return 'http://127.0.0.1:%d' % self.proxy.port if self.proxy else self.endpoint

    def credentials(self):
        return {'accessKey': self.access, 'secretKey': self.secret}

    def settings(self, **extra):
        base = {'endpoint': self.put_endpoint, 'bucket': self.bucket, 'region': self.region, 'objectPrefix': '', 'aclMode': 'omit', 'addressing': 'path',
                'publicUrlBase': None, 'requestTimeoutMs': 30000, 'connectTimeoutMs': 3000, 'storageClass': 'STANDARD',
                'lowSpeedBytesPerSecond': 1, 'lowSpeedTimeSeconds': 30}
        base.update(extra)
        return base

    def raw_keys_in(self, prefix):
        return self.raw.list_keys(prefix)

    def stored_in(self, prefix):
        found = []
        cache = self.__dict__.setdefault('_cache', {})
        for key, size, etag in self.raw.list_keys(prefix):
            reply = self.raw.head(key)
            known = cache.get((key, etag, size))
            if known is None:
                body = self.raw.get(key)
                known = hashlib.sha256(body.body).hexdigest()
                cache[(key, etag, size)] = known
            found.append({'key': key, 'size': size, 'etag': etag, 'sha256': known, 'content_type': reply.headers.get('content-type', ''),
                          'metadata': {k[len('x-amz-meta-'):]: v for k, v in reply.headers.items() if k.startswith('x-amz-meta-')}})
        return found

    def uploads_in(self, prefix):
        return self.raw.list_uploads(prefix)

    def abort(self, key, upload):
        return self.raw.abort(key, upload).status

    def require_unversioned(self):
        if not self.cloud:
            return  # a throwaway local server: its data goes away with the container
        status = self.raw.versioning()
        if status is None:
            raise SystemExit('%s: cannot read the bucket versioning state, so cleanup cannot be proven complete; refusing to write' % self.name)
        if status:
            raise SystemExit('%s: bucket versioning is %s; the harness deletes current objects only, which would leave versions and delete markers. Refusing to write' % (self.name, status))

    def seed(self, key, body, headers=None):
        reply = self.raw.put(key, body, headers)
        return reply.status

    def remove(self, key):
        return self.raw.delete(key).status

    def head(self, key):
        return self.raw.head(key)

    def restricted(self):
        """A key that may write but not list: Garage (key permissions) and AWS key set 1 (no s3:ListBucket) have one."""
        if self.name == 'garage':
            import subprocess
            done = subprocess.run(['podman', 'exec', 's3diff-garage', '/garage', 'key', 'create', 'putgd-wo'], capture_output=True, text=True)
            text = done.stdout
            key_id = re.search(r'Key ID:\s+(\S+)', text)
            secret = re.search(r'Secret key:\s+(\S+)', text)
            if not key_id or not secret:
                return None
            subprocess.run(['podman', 'exec', 's3diff-garage', '/garage', 'bucket', 'allow', '--write', servers.BUCKET, '--key', 'putgd-wo'], capture_output=True, text=True)
            return {'accessKey': key_id.group(1), 'secretKey': secret.group(1)}
        if self.name == 'aws1':
            return self.credentials()
        return None

    def close(self):
        if self.proxy:
            self.proxy.stop()


class OracleTarget(RawTarget):
    """tests/fixtures/s3_server.py: no listing API, so everything is read from its state document."""

    def __init__(self, run):
        self.receiver = af.S3Receiver(HERE.parent.parent / 'tests/fixtures/s3_server.py', run)
        self.receiver.reset()
        super().__init__('oracle', self.receiver.base, 'eu-west-3', 'isolated-fixture', ORACLE_ACCESS, ORACLE_SECRET)

    def state(self):
        return self.receiver.state()

    def stored_in(self, prefix):
        found = []
        for key, versions in self.state()['objects'].items():
            live = [v for v in versions if not v.get('marker')]
            if key.startswith(prefix) and live:
                value = live[-1]
                body = base64.b64decode(value['body']) if not value.get('blob') else b''
                found.append({'key': key, 'size': len(body) if not value.get('blob') else value.get('size'), 'etag': value['etag'].strip('"'),
                              'sha256': hashlib.sha256(body).hexdigest() if not value.get('blob') else value['blob'],
                              'content_type': value.get('contentType', ''), 'metadata': value.get('metadata', {})})
        return found

    def raw_keys_in(self, prefix):
        return [(e['key'], e['size'], e['etag']) for e in self.stored_in(prefix)]

    def uploads_in(self, prefix):
        return [(u['key'], uid) for uid, u in self.state()['uploads'].items() if u['key'].startswith(prefix)]

    def close(self):
        RawTarget.close(self)
        self.receiver.stop()


def cloud_credentials():
    """Parse ~/Documents/creds.txt in memory: {'aws2': (key, secret), 'aws1': ..., 'r2': (key, secret, endpoint)}. Never printed."""
    text = (Path.home() / 'Documents/creds.txt').read_text()
    lines = [line.rstrip('\n') for line in text.split('\n')]
    out = {}
    labelled = []
    for index, line in enumerate(lines):
        if line.startswith('Access Key ID:') and index + 1 < len(lines) and lines[index + 1].startswith('Secret Access Key:'):
            labelled.append((line.split(':', 1)[1].strip(), lines[index + 1].split(':', 1)[1].strip(), index))
    if labelled:
        out['aws1'] = labelled[0][:2]
    for line in lines:
        parts = line.split()
        if len(parts) == 2 and re.fullmatch(r'AKIA[A-Z0-9]{16}', parts[0]) and len(parts[1]) == 40:
            out['aws2'] = (parts[0], parts[1])
    # R2 is the labelled block that follows the Cloudflare heading.
    cloudflare = next((i for i, line in enumerate(lines) if 'Cloudflare' in line), None)
    if cloudflare is not None:
        for key, secret, index in labelled:
            if index > cloudflare:
                endpoint = next((l.split('-', 1)[1].strip() for l in lines[index:] if l.startswith('Endpoint')), '')
                endpoint = endpoint.replace('https:', 'https://', 1) if endpoint.startswith('https:') and not endpoint.startswith('https://') else endpoint
                out['r2'] = (key, secret, endpoint)
    return out


class CloudTarget(RawTarget):
    def __init__(self, name, endpoint, region, bucket, access, secret, root_prefix, listing=True):
        super().__init__(name, endpoint, region, bucket, access, secret, cloud=True, listing=listing, root_prefix=root_prefix)


def build_targets(names, run, run_id):
    found = {}
    for name in names:
        if name == 'oracle':
            found[name] = OracleTarget(run)
        elif name in servers.SERVERS:
            spec = servers.SERVERS[name]
            found[name] = RawTarget(name, 'http://127.0.0.1:%d' % spec['port'], spec['region'], servers.BUCKET, servers.ACCESS, servers.SECRET)
        elif name in ('aws', 'aws1', 'r2'):
            creds = cloud_credentials()
            if name == 'aws':
                key, secret = creds['aws2']
                found[name] = CloudTarget('aws', 'https://s3.us-east-2.amazonaws.com', 'us-east-2', 'put.gd', key, secret, 'acceptance/s3diff-' + run_id)
            elif name == 'aws1':
                key, secret = creds['aws1']
                found[name] = CloudTarget('aws1', 'https://s3.us-east-2.amazonaws.com', 'us-east-2', 'put.gd', key, secret, 'acceptance/s3diff-' + run_id, listing=False)
                # put.gd runs with key set 1 (no ListBucket); the harness looks at the bucket with set 2.
                found[name].raw = servers.RawS3('https://s3.us-east-2.amazonaws.com', 'us-east-2', creds['aws2'][0], creds['aws2'][1], 'put.gd')
            else:
                key, secret, endpoint = creds['r2']
                bucket = os.environ.get('S3DIFF_R2_BUCKET') or r2_bucket(key, secret, endpoint)
                found[name] = CloudTarget('r2', endpoint, 'auto', bucket, key, secret, 's3diff-' + run_id)
        else:
            raise SystemExit('unknown server %s' % name)
    return found


def r2_bucket(key, secret, endpoint):
    """The only bucket the R2 key can see (ListBuckets); the name is not a secret."""
    client = servers.RawS3(endpoint, 'auto', key, secret, '')
    reply = client.request('GET', bucket=False)
    names = re.findall(r'<Name>([^<]+)</Name>', reply.body.decode(errors='replace'))
    if not names:
        raise SystemExit('R2: no bucket visible to the key (set S3DIFF_R2_BUCKET)')
    return names[0]


# ---------------------------------------------------------------------------------------------- the lab

class Lab:
    def __init__(self, options, target, run_id):
        self.options, self.target, self.run_id = options, target, run_id
        self.root = Path(options.out) / target.name
        self.root.mkdir(parents=True, exist_ok=True)
        self.work = Path(options.work) / target.name
        self.work.mkdir(parents=True, exist_ok=True)
        (self.work / 'logs').mkdir(exist_ok=True)
        self.resident = af.Resident(self.work, options.build)
        self.profile = 'diff'
        self.case_index = 0
        self.states_seen = set()

    # --- resident

    def start(self):
        self.resident.start()
        settings = self.resident.call('settings.get')
        template = json.loads(json.dumps(self.resident.call('settings.developmentTemplate')['document']['profiles'][0]))
        template.update(id=self.profile, label='S3 differential', credentials={'mode': 'session', 'reference': None})
        template['settings'] = self.target.settings()
        document = settings['document']
        document['profiles'] = [template]
        saved = self.resident.call('settings.save', {'document': document, 'expectedRevision': settings['metadata']['observedRevision']}, raw=True)
        if not saved.get('ok'):
            raise RuntimeError('settings.save refused: %s' % json.dumps(saved.get('error'))[:400])
        self.patch({'archive': {'keepUploads': True, 'root': str(self.work / 'archive')}, 'preferences': {'notifications': False}})
        self.store_credentials()

    def revision(self):
        return self.resident.call('settings.get')['metadata']['observedRevision']

    def patch(self, patch):
        reply = self.resident.call('settings.patch', {'patch': patch, 'expectedRevision': self.revision()}, raw=True)
        if not reply.get('ok'):
            raise RuntimeError('settings.patch refused: %s' % json.dumps(reply.get('error'))[:300])

    def store_credentials(self):
        reply = self.resident.call('setup.storeCredentials', {'profileId': self.profile, 'expectedRevision': self.revision(), 'credentials': self.target.credentials()}, raw=True)
        setup = (reply.get('result') or {}).get('setupId')
        end = time.monotonic() + 30
        while setup and time.monotonic() < end:
            state = (self.resident.call('setup.inspect', {'id': setup}, raw=True).get('result') or {}).get('state')
            if state in ('completed', 'failed'):
                if state == 'failed':
                    raise RuntimeError('storing credentials failed')
                return
            time.sleep(0.1)

    def set_profile(self, **settings):
        """Replace the profile's settings (the whole object) and keep the stored session credentials."""
        current = self.resident.call('settings.get')
        document = current['document']
        document['profiles'][0]['settings'] = self.target.settings(**settings)
        reply = self.resident.call('settings.save', {'document': document, 'expectedRevision': current['metadata']['observedRevision']}, raw=True)
        if not reply.get('ok'):
            return reply.get('error')
        self.store_credentials()
        return None

    def set_policy(self, policy):
        self.patch({'naming': {'collisionPolicy': policy}})

    # --- driving

    def upload(self, paths, timeout=900, cancel_after=None):
        """upload.files, then wait for a terminal operation. Returns (reply error or None, operation or None, history rows by item id)."""
        reply = self.resident.call('upload.files', {'paths': [str(p) for p in paths], 'profileId': self.profile}, timeout=130, raw=True)
        if not reply.get('ok'):
            return reply.get('error'), None
        operation_id = reply['result']['operationId']
        end = time.monotonic() + timeout
        cancelled = False
        started = time.monotonic()
        while time.monotonic() < end:
            if cancel_after is not None and not cancelled and time.monotonic() - started >= cancel_after:
                self.resident.call('operation.cancel', {'id': operation_id}, raw=True)
                cancelled = True
            inspect = self.resident.call('operation.inspect', {'id': operation_id}, raw=True)
            operation = inspect.get('result') or {}
            self.states_seen.add(operation.get('state'))
            items = operation.get('items') or []
            # Only an explicit terminal state ends the wait: an upload passes through 'stored' (local copy saved) before it is sent, and a
            # cancelled operation keeps items that were stored locally and never uploaded.
            busy = operation.get('state') not in TERMINAL_STATES
            if not busy:
                return None, operation
            time.sleep(0.25)
        return {'code': 'harness_timeout', 'message': 'no terminal state in %s s' % timeout}, None

    def delete(self, item_ids, timeout=180):
        """delete.prepare then delete.confirm for exact history IDs; returns (prepare reply, final deletion record)."""
        prepared = self.resident.call('delete.prepare', {'ids': item_ids}, timeout=120, raw=True)
        if not prepared.get('ok'):
            return prepared, None
        result = prepared['result']
        confirmed = self.resident.call('delete.confirm', {'ids': item_ids, 'acknowledgement': result.get('acknowledgement')}, timeout=120, raw=True)
        if not confirmed.get('ok'):
            return prepared, {'confirmError': confirmed.get('error')}
        deletion_id = (confirmed['result'] or {}).get('deleteOperationId')
        end = time.monotonic() + timeout
        last = confirmed['result']
        while deletion_id and time.monotonic() < end:
            inspect = self.resident.call('delete.inspect', {'id': deletion_id}, raw=True)
            last = inspect.get('result') or last
            state = (last.get('deletion') or last).get('state')
            if state not in (None, 'accepted', 'running', 'active', 'queued', 'pending', 'confirmed', 'deleting', 'prepared'):
                break
            time.sleep(0.3)
        return prepared, last

    def stop(self):
        try:
            self.resident.quit()
        except Exception:  # noqa: BLE001
            pass
        for log in sorted((self.work / 'logs').glob('resident-*.log')):
            if 'AddressSanitizer' in log.read_text(errors='replace'):
                print('SANITIZER output in %s' % log)


# ---------------------------------------------------------------------------------------------- recording

def summarize_item(item):
    receipt = item.get('receipt') or {}
    reference = receipt.get('objectReference') or item.get('objectReference') or {}
    error = item.get('error') or {}
    details = error.get('details') or {}
    return {
        'name': (item.get('artifact') or {}).get('name'),
        'candidate': item.get('candidate'),
        'uploadStatus': item.get('uploadStatus'),
        'phase': item.get('phase'),
        'remoteStatus': item.get('remoteStatus'),
        'errorCode': error.get('code') or '',
        'httpStatus': details.get('httpStatus'),
        'serviceCode': details.get('serviceCode'),
        'requestStage': details.get('requestStage'),
        'key': reference.get('key') or '',
        'directUrl': receipt.get('directUrl') or '',
        'receiptEtag': receipt.get('concurrencyToken') or item.get('concurrencyToken') or '',
        'sha256': (item.get('artifact') or {}).get('sha256'),
        'bytes': (item.get('artifact') or {}).get('bytes'),
        'message': (error.get('message') or '')[:160],
        'curlCode': details.get('curlCode'),
        'sdkErrorType': details.get('sdkErrorType'),
        'elapsedMs': details.get('elapsedMs'),
    }


class Recorder:
    def __init__(self, lab, case_name, prefix):
        self.lab, self.case, self.prefix = lab, case_name, prefix
        self.data = {'case': case_name, 'server': lab.target.name, 'prefix': prefix, 'notes': [], 'ops': [], 'stored': None, 'uploads': None}
        self.violations = []

    def note(self, text):
        self.data['notes'].append(text)

    def flag(self, code, text):
        """A behaviour that differs from what put.gd's docs promise (checked against what the server really holds)."""
        self.violations.append({'code': code, 'text': text})

    def finish(self):
        target = self.lab.target
        try:
            self.data['stored'] = [{**e, 'key': e['key']} for e in target.stored(self.prefix)]
            self.data['uploads'] = target.uploads(self.prefix)
        except Exception as error:  # noqa: BLE001
            self.data['stored_error'] = repr(error)[:200]
        if target.proxy:
            log, rules = target.proxy.snapshot()
            self.data['requests'] = [{k: v for k, v in e.items() if k != 'time'} for e in log]
            self.data['rules'] = rules
        self.data['violations'] = self.violations
        return self.data


def relative(prefix, key):
    """A key relative to the case prefix; one in the leading-slash twin namespace keeps its slash ('/ls/x.txt'), so it stays distinct."""
    if key.startswith(prefix):
        return key[len(prefix):]
    if key.startswith('/' + prefix):
        return '/' + key[len(prefix) + 1:]
    return key


# ---------------------------------------------------------------------------------------------- cases

CASES = {}


def case(name, doc, needs=()):
    def register(function):
        CASES[name] = (function, doc, needs)
        return function
    return register


def case_prefix(lab, name):
    return '%s/%s/%s/' % (lab.target.root_prefix, lab.run_id, name)


def uploaded_items(record_ops):
    return [item for op in record_ops for item in (op.get('items') or [])]


def run_upload(lab, rec, files, **kwargs):
    error, operation = lab.upload(files, **kwargs)
    summary = {'error': error, 'state': (operation or {}).get('state'), 'items': [summarize_item(i) for i in (operation or {}).get('items') or []],
               'raw_items': (operation or {}).get('items') or []}
    rec.data['ops'].append({k: v for k, v in summary.items() if k != 'raw_items'})
    rec.raw = getattr(rec, 'raw', [])
    rec.raw.append(operation)
    return summary


def check_integrity(rec, files_by_name, lab, expect_stored=True):
    """Every key's last reported upload must be stored under its receipt key with exactly those bytes; failed items must have stored nothing."""
    stored = {relative(rec.prefix, e['key']): e for e in rec.lab.target.stored(rec.prefix)}
    last = {}
    for op in rec.data['ops']:
        for item in op['items']:
            if item['key']:
                last[relative(rec.prefix, item['key'])] = item
            elif item['uploadStatus'] == 'uploaded':
                rec.flag('uploaded_without_key', 'item %r reported uploaded with no object key' % item['name'])
    for key, item in last.items():
        entry = stored.get(key)
        if item['uploadStatus'] == 'uploaded':
            if entry is None:
                rec.flag('uploaded_but_missing', 'item %r reported uploaded under %r but the server holds no such key' % (item['name'], key))
            elif item.get('sha256') and entry['sha256'] != item['sha256']:
                rec.flag('uploaded_wrong_bytes', 'item %r stored under %r with a different SHA-256 (%s vs %s)' % (item['name'], key, entry['sha256'][:12], item['sha256'][:12]))
        elif item['uploadStatus'] == 'failed' and entry is not None and not (item.get('errorCode') or '').startswith('s3_transport'):
            rec.flag('failed_but_stored', 'item %r failed (%s) yet the server holds %r' % (item['name'], item['errorCode'], key))
    for key in stored:
        if key not in last:
            rec.note('stray key %r (%d bytes)' % (key, stored[key]['size']))
    return stored


@case('names-special', 'object names with spaces, unicode, +, %, ? # & and other characters')
def names_special(lab, rec):
    names = ['plain.txt', 'two  spaces.txt', ' leading space.txt', 'trailing space .txt', 'plus+sign.txt', 'plus and space + .txt', 'percent%20literal.txt', 'pct%.txt',
             '%2F.txt', '%.txt', 'café-nfc.txt', 'café-nfd.txt', '日本語-中文-한국어.txt', 'emoji-\U0001f600\U0001f389.txt',
             'rtl-שלום.txt', 'zero​width.txt', 'bidi‮exe.txt', 'q?mark.txt', 'hash#frag.txt', 'amp&eq=semi;.txt', 'star*.txt', 'colon:.txt', 'pipe|.txt',
             'lt<gt>.txt', 'quote\'dq".txt', 'back\\slash.txt', 'brack[1]{2}(3).txt', 'tilde~.txt', 'at@dollar$comma,.txt', '.dotfile', '..twodots', '-dash.txt', 'bang!.txt',
             'caret^.txt', 'backtick`.txt', 'tab\there.txt', 'ctl\x01char.txt', 'del\x7fchar.txt', 'new\nline.txt']
    directory = lab.work / ('c-names-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    files, by_name = [], {}
    for index, name in enumerate(names):
        path = directory / name
        try:
            path.write_bytes(('synthetic %d %s\n' % (index, name)).encode())
        except OSError as error:
            rec.note('local file %r not creatable: %s' % (name, error))
            continue
        files.append(path)
        by_name[name] = path
    lab.set_profile(objectPrefix=rec.prefix)
    # put.gd refuses a whole batch that holds a name with a control character, newline or backslash: those go one at a time.
    refused = [p for p in files if any(ord(c) < 32 or ord(c) == 127 or c == '\\' for c in p.name)]
    files = [p for p in files if p not in refused]
    summary = run_upload(lab, rec, files)
    for path in refused:
        run_upload(lab, rec, [path])
    stored = check_integrity(rec, by_name, lab)
    # What the item reports as its name must be the key put.gd uses, byte for byte.
    for item in summary['items']:
        if item['uploadStatus'] == 'uploaded' and relative(rec.prefix, item['key']) != item['name']:
            rec.note('key differs from the file name: %r -> %r' % (item['name'], relative(rec.prefix, item['key'])))


@case('names-long', 'long names and keys at and over the 1024-byte limit (no path segment over 200 bytes)')
def names_long(lab, rec):
    directory = lab.work / ('c-long-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    base = len(rec.prefix.encode())

    def deep_prefix(extra_bytes):
        segments, left = [], extra_bytes
        while left > 0:
            size = min(180, left - 1) if left > 181 else left - 1
            segments.append('p' * max(size, 1))
            left -= len(segments[-1]) + 1
        return '/'.join(segments) + '/' if segments else ''
    cases = [('name-255-ascii', 'a' * 251 + '.txt', 0), ('name-255-multibyte', '\u00e9' * 125 + '.txt', 0)]
    for label, total in (('key-1000', 1000), ('key-1024', 1024), ('key-1025', 1025), ('key-1100', 1100)):
        name = 'n' * 100 + '.txt'
        extra = total - base - len(name.encode())
        cases.append((label, name, extra))
    for label, name, extra in cases:
        path = directory / label / name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(b'synthetic long name\n')
        prefix = rec.prefix + deep_prefix(extra)
        lab.set_profile(objectPrefix=prefix)
        run_upload(lab, rec, [path])
        rec.note('%s: key bytes %d' % (label, len((prefix + name).encode())))
    check_integrity(rec, {}, lab)


@case('prefix-shapes', 'object prefixes: trailing and doubled slashes, leading slash, dots, spaces, unicode, percent')
def prefix_shapes(lab, rec):
    # A shape starting with '^' is the whole prefix (a leading slash cannot follow the run's own prefix), otherwise it is appended to it.
    shapes = ['dir', 'dir/', 'dir//', '/lead', 'a//b/', 'sp ace/', 'ü/ñ/', '%2F/', '.', '..', 'a/./b/', 'a/../b/', 'q?x#y/', '^/' + rec.prefix + 'ls/']
    directory = lab.work / ('c-prefix-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    for index, shape in enumerate(shapes):
        path = directory / ('p%d.txt' % index)
        path.write_bytes(('synthetic prefix %d\n' % index).encode())
        lab.set_profile(objectPrefix=shape[1:] if shape.startswith('^') else rec.prefix + shape)
        summary = run_upload(lab, rec, [path])
        # What the receipt names is what History deletes and the link opens: a signed HEAD of exactly that key, outside put.gd.
        for item in summary['items']:
            if item['uploadStatus'] == 'uploaded':
                heads = rec.data.setdefault('receipt_key_head', {})
                heads['^/<run>/ls/' if shape.startswith('^') else shape] = {'receiptKey': relative(rec.prefix, item['key']), 'head': lab.target.head(item['key']).status}
    check_integrity(rec, {}, lab)


@case('case-collision', 'names that differ only by case in one batch')
def case_collision(lab, rec):
    directory = lab.work / ('c-case-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    files = []
    for name in ('Case.txt', 'case.txt', 'CASE.TXT'):
        path = directory / name
        path.write_bytes(('synthetic %s\n' % name).encode())
        files.append(path)
    lab.set_profile(objectPrefix=rec.prefix)
    run_upload(lab, rec, files)
    check_integrity(rec, {}, lab)


@case('size-ladder', 'sizes: 0, 1, around and over the 64 MiB multipart threshold, many parts')
def size_ladder(lab, rec):
    directory = lab.work / ('c-size-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    sizes = [('s0', 0), ('s1', 1), ('s-under', MULTIPART_THRESHOLD - 1), ('s-at', MULTIPART_THRESHOLD), ('s-over', MULTIPART_THRESHOLD + 1), ('s-5parts', 65 * MIB),
             ('s-many', 330 * MIB + 1)]
    lab.set_profile(objectPrefix=rec.prefix)
    wanted = lab.options.sizes.split(',') if lab.options.sizes else None
    for label, size in sizes:
        if wanted is not None and label not in wanted:
            continue
        if lab.options.max_mib and size > lab.options.max_mib * MIB:
            rec.note('%s skipped (over --max-mib)' % label)
            continue
        path = make_file(directory / (label + '.bin'), size, seed=size)
        before = time.monotonic()
        summary = run_upload(lab, rec, [path])
        rec.note('%s: %d bytes in %.1fs' % (label, size, time.monotonic() - before))
        # Stored content must match this very file, not only the item's own hash.
        for item in summary['items']:
            if item['uploadStatus'] == 'uploaded':
                for entry in lab.target.stored(rec.prefix):
                    if relative(rec.prefix, entry['key']) == relative(rec.prefix, item['key']) and entry['sha256'] != sha256_file(path):
                        rec.flag('stored_bytes_differ_from_file', '%s: server holds other bytes than the file' % label)
        # Release space as we go: remove what this step stored, exactly.
        for entry in lab.target.stored(rec.prefix):
            if entry['size'] > 8 * MIB:
                rec.note('%s etag=%s parts=%s' % (label, entry['etag'], entry['etag'].split('-')[1] if '-' in entry['etag'] else '1'))
                lab.target.remove(entry['key'])
        os.unlink(path)


@case('collision-policies', 'rename, replace and skip against an existing object')
def collision_policies(lab, rec):
    directory = lab.work / ('c-coll-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    lab.set_profile(objectPrefix=rec.prefix)
    for policy in ('rename', 'overwrite', 'skip'):
        lab.set_policy(policy)
        name = 'dup-%s.txt' % policy
        status = lab.target.seed(rec.prefix + name, b'FOREIGN existing object\n')
        rec.note('seed %s -> %s' % (name, status))
        path = directory / name
        path.write_bytes(('put.gd content for %s\n' % policy).encode())
        run_upload(lab, rec, [path])
        # the same file again, as a second action
        path2 = directory / ('again-' + name)
        path2.write_bytes(b'second\n')
        os.rename(path2, directory / name)
        run_upload(lab, rec, [directory / name])
    lab.set_policy('rename')
    check_integrity(rec, {}, lab)


@case('dup-in-batch', 'two files with one name in one batch, and one name uploaded again later (rename policy)')
def dup_in_batch(lab, rec):
    directory = lab.work / ('c-dup-%d' % lab.case_index)
    (directory / 'a').mkdir(parents=True, exist_ok=True)
    (directory / 'b').mkdir(parents=True, exist_ok=True)
    (directory / 'a' / 'same.txt').write_bytes(b'from a\n')
    (directory / 'b' / 'same.txt').write_bytes(b'from b\n')
    lab.set_profile(objectPrefix=rec.prefix)
    lab.set_policy('rename')
    run_upload(lab, rec, [directory / 'a' / 'same.txt', directory / 'b' / 'same.txt'])
    (directory / 'a' / 'same.txt').write_bytes(b'from a again\n')
    run_upload(lab, rec, [directory / 'a' / 'same.txt'])
    check_integrity(rec, {}, lab)


@case('delete-paths', 'History deletion: normal, already gone, replaced by someone else, twice')
def delete_paths(lab, rec):
    directory = lab.work / ('c-del-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    lab.set_profile(objectPrefix=rec.prefix)
    names = ['normal.txt', 'gone.txt', 'replaced.txt', 'twice.txt']
    files = []
    for name in names:
        path = directory / name
        path.write_bytes(('synthetic delete case %s\n' % name).encode())
        files.append(path)
    summary = run_upload(lab, rec, files)
    by_name = {i['name']: i for i in summary['items']}
    ids = {i['name']: (raw.get('artifact') or {}).get('id') for i, raw in zip(summary['items'], summary['raw_items'])}
    keys = {n: by_name[n]['key'] for n in names if n in by_name and by_name[n]['key']}
    if 'gone.txt' in keys:
        lab.target.remove(keys['gone.txt'])
    if 'replaced.txt' in keys:
        lab.target.seed(keys['replaced.txt'], b'someone else replaced this\n')
    outcomes = {}
    for name in names:
        if name not in ids or not ids[name]:
            continue
        prepared, final = lab.delete([ids[name]])
        outcomes[name] = {'prepare': (prepared.get('error') or {}).get('code') if not prepared.get('ok') else 'ok', 'final': final}
        if name == 'twice.txt':
            prepared2, final2 = lab.delete([ids[name]])
            outcomes['twice.txt/second'] = {'prepare': (prepared2.get('error') or {}).get('code') if not prepared2.get('ok') else 'ok', 'final': final2}
    rec.data['deletions'] = json.loads(json.dumps(outcomes, default=str))
    stored = {relative(rec.prefix, e['key']): e for e in lab.target.stored(rec.prefix)}
    for name in ('normal.txt', 'twice.txt'):
        if name in keys and relative(rec.prefix, keys[name]) in stored:
            rec.flag('delete_left_object', '%s still stored after a confirmed deletion' % name)
    if 'replaced.txt' in keys:
        entry = stored.get(relative(rec.prefix, keys['replaced.txt']))
        if entry is None:
            rec.flag('delete_removed_foreign_object', 'the object someone else had replaced was deleted')


@case('lost-reply', 'the reply to PUT, to the multipart complete and to DELETE is lost after the server acted (existence check)', needs=('proxy',))
def lost_reply(lab, rec):
    directory = lab.work / ('c-lost-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    lab.set_profile(objectPrefix=rec.prefix)
    proxy = lab.target.proxy
    # 1. PUT of a small object: reply lost.
    small = directory / 'lost-put.txt'
    small.write_bytes(b'synthetic lost reply put\n')
    proxy.set_rules([Rule('PUT', match='lost-put.txt', action='lose_reply', count=1)])
    summary = run_upload(lab, rec, [small])
    rec.note('lost PUT reply: %s' % [(i['uploadStatus'], i['errorCode']) for i in summary['items']])
    # 2. multipart complete: reply lost.
    big = make_file(directory / 'lost-complete.bin', MULTIPART_THRESHOLD + MIB, seed=7)
    proxy.set_rules([Rule('POST', match='uploadId', action='lose_reply', count=1)])
    summary = run_upload(lab, rec, [big])
    rec.note('lost complete reply: %s' % [(i['uploadStatus'], i['errorCode']) for i in summary['items']])
    big_key = next((i['key'] for i in summary['items'] if i['key']), '')
    # 3. DELETE reply lost.
    proxy.set_rules([])
    item_id = next(((raw.get('artifact') or {}).get('id') for raw in summary['raw_items'] if raw.get('uploadStatus') == 'uploaded'), None)
    small2 = directory / 'lost-delete.txt'
    small2.write_bytes(b'synthetic lost reply delete\n')
    summary2 = run_upload(lab, rec, [small2])
    ident = next(((raw.get('artifact') or {}).get('id') for raw in summary2['raw_items'] if raw.get('uploadStatus') == 'uploaded'), None)
    if ident:
        proxy.set_rules([Rule('DELETE', match='lost-delete', action='lose_reply', count=1)])
        prepared, final = lab.delete([ident])
        rec.data['lost_delete'] = json.loads(json.dumps(final, default=str))
        proxy.set_rules([])
        delete_id = (final or {}).get('id') if isinstance(final, dict) else None
        if delete_id:
            retry = lab.resident.call('delete.retry', {'id': delete_id}, timeout=120, raw=True)
            end = time.monotonic() + 120
            again = None
            # The record still shows the earlier attempt until the retry has run: wait for an item status that is no longer 'unknown'.
            while time.monotonic() < end and retry.get('ok'):
                again = lab.resident.call('delete.inspect', {'id': delete_id}, raw=True).get('result') or {}
                record = again.get('deletion') or again
                statuses = [i.get('status') for i in record.get('items') or []]
                if record.get('state') == 'completed' and statuses and 'unknown' not in statuses:
                    break
                time.sleep(0.3)
            rec.data['lost_delete_retry'] = {'ok': retry.get('ok'), 'error': retry.get('error'), 'final': json.loads(json.dumps(again, default=str))}
    if item_id:
        lab.delete([item_id])


@case('orphans', 'a failing part, a failing complete, a cancel in the middle of a multipart upload: nothing may be left incomplete', needs=('proxy',))
def orphans(lab, rec):
    directory = lab.work / ('c-orph-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    lab.set_profile(objectPrefix=rec.prefix)
    proxy = lab.target.proxy
    big = make_file(directory / 'orphan.bin', 70 * MIB, seed=11)
    # a part keeps failing (all three attempts)
    proxy.set_rules([Rule('PUT', match='partNumber=3', action='status', count=3, status=500)])
    run_upload(lab, rec, [big])
    left = lab.target.uploads(rec.prefix)
    rec.note('after failing part: incomplete uploads %s' % (len(left) if left is not None else 'unlisted'))
    # complete fails
    big2 = make_file(directory / 'orphan2.bin', 70 * MIB, seed=12)
    proxy.set_rules([Rule('POST', match='uploadId', action='status', count=3, status=500)])
    summary = run_upload(lab, rec, [big2])
    left = lab.target.uploads(rec.prefix)
    rec.note('after failing complete: incomplete uploads %s' % (len(left) if left is not None else 'unlisted'))
    # The kept upload is what an explicit Retry resumes: it must finish the same upload and leave nothing incomplete.
    proxy.set_rules([])
    operation_id = next((raw.get('id') for raw in rec.raw[-1:] if raw), None)
    if operation_id:
        retry = lab.resident.call('operation.retry', {'id': operation_id}, timeout=130, raw=True)
        end = time.monotonic() + 300
        retried = None
        while retry.get('ok') and time.monotonic() < end:
            retried = lab.resident.call('operation.inspect', {'id': operation_id}, raw=True).get('result') or {}
            if retried.get('state') in TERMINAL_STATES:
                break
            time.sleep(0.5)
        rec.data['retry_after_failed_complete'] = {'ok': retry.get('ok'), 'error': (retry.get('error') or {}).get('code'),
                                                   'items': [summarize_item(i) for i in (retried or {}).get('items') or []]}
        again = lab.target.uploads(rec.prefix)
        rec.note('after Retry: incomplete uploads %s, item %s' % (len(again) if again is not None else 'unlisted', [i['uploadStatus'] for i in rec.data['retry_after_failed_complete']['items']]))
    # cancelled while parts are in flight
    big3 = make_file(directory / 'orphan3.bin', 80 * MIB, seed=13)
    proxy.set_rules([Rule('PUT', match='partNumber=2', action='delay', count=1, delay=4.0)])
    run_upload(lab, rec, [big3], cancel_after=1.5)
    time.sleep(1)
    left = lab.target.uploads(rec.prefix)
    rec.note('after cancel: incomplete uploads %s' % (len(left) if left is not None else 'unlisted'))
    proxy.set_rules([])
    if left:
        rec.flag('orphaned_multipart', '%d incomplete multipart upload(s) left after the run: %s' % (len(left), [k for k, _ in left][:3]))
    check_integrity(rec, {}, lab)


@case('tampered-part', 'a part altered in transit: Content-MD5 and the composite ETag must catch it', needs=('proxy',))
def tampered_part(lab, rec):
    directory = lab.work / ('c-tamper-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    lab.set_profile(objectPrefix=rec.prefix)
    proxy = lab.target.proxy
    big = make_file(directory / 'tamper.bin', 70 * MIB, seed=21)
    proxy.set_rules([Rule('PUT', match='partNumber=2', action='tamper', count=1)])
    summary = run_upload(lab, rec, [big])
    expected = sha256_file(big)
    for entry in lab.target.stored(rec.prefix):
        if entry['sha256'] != expected:
            rec.flag('corrupt_object_stored', 'a part was altered in transit; the server holds a different object (%s) than the file; put.gd status %s' % (entry['key'][-20:], [i['uploadStatus'] for i in summary['items']]))
    proxy.set_rules([])
    check_integrity(rec, {}, lab)


@case('race', 'another client creates the key between put.gd\'s check and its conditional PUT', needs=('proxy',))
def race(lab, rec):
    directory = lab.work / ('c-race-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    lab.set_profile(objectPrefix=rec.prefix)
    proxy = lab.target.proxy
    key = rec.prefix + 'race.txt'
    # The competitor appears when put.gd's PUT is about to go out: seed it from the proxy's request thread.
    competitor = b'COMPETITOR object, must survive\n'

    fired = []

    class Seed(Rule):
        def applies(self, method, target):
            if Rule.applies(self, method, target) and not fired:
                fired.append(1)
                lab.target.seed(key, competitor)
            return False
    proxy.set_rules([Seed('PUT', match='race.txt', action='none', count=1)])
    path = directory / 'race.txt'
    path.write_bytes(b'put.gd content\n')
    summary = run_upload(lab, rec, [path])
    proxy.set_rules([])
    held = {relative(rec.prefix, e['key']): e for e in lab.target.stored(rec.prefix)}
    foreign = held.get('race.txt')
    if foreign is None or foreign['sha256'] != hashlib.sha256(competitor).hexdigest():
        rec.flag('overwrote_foreign_object', 'the competitor\'s object at race.txt was replaced (server did not enforce If-None-Match); put.gd status %s' % [(i['uploadStatus'], i['errorCode']) for i in summary['items']])
    check_integrity(rec, {}, lab)


@case('content-and-metadata', 'content type, user metadata (publication identity) and ETag the server keeps')
def content_and_metadata(lab, rec):
    directory = lab.work / ('c-meta-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    lab.set_profile(objectPrefix=rec.prefix)
    files = []
    for name, body in (('text.txt', b'hello\n'), ('page.html', b'<p>x</p>\n'), ('data.json', b'{"a":1}\n'), ('noext', b'plain bytes\n'), ('doc.pdf', b'%PDF-1.4\n%synthetic\n'),
                       ('pic.png', af.png(8, 8, 3)), ('unknown.zzz', b'zzz\n')):
        path = directory / name
        path.write_bytes(body)
        files.append(path)
    summary = run_upload(lab, rec, files)
    for entry in lab.target.stored(rec.prefix):
        if 'putgd-publication' not in entry['metadata'] or 'putgd-sha256' not in entry['metadata']:
            rec.flag('identity_metadata_missing', '%s: server returns metadata %s (put.gd identifies its objects by putgd-publication and putgd-sha256)' % (entry['key'][-24:], sorted(entry['metadata'])))
    check_integrity(rec, {}, lab)


@case('public-url', 'the stored link for every special name fetched without credentials (bucket made public where the server allows)')
def public_url(lab, rec):
    directory = lab.work / ('c-url-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    target = lab.target
    # AWS: objects go out with the public-read ACL, which the test bucket allows; local servers: a public bucket policy where they take one.
    public_acl = target.name == 'aws'
    public = False if target.cloud else make_public(target)
    rec.note('links fetched anonymously: %s' % public)
    names = ['plain.txt', 'two  spaces.txt', 'plus+sign.txt', 'percent%20literal.txt', 'pct%.txt', 'café.txt', '日本語.txt', 'emoji-\U0001f600.txt', 'q?mark.txt', 'hash#frag.txt',
             'amp&eq=semi;.txt', 'brack[1]{2}.txt', "quote'.txt", 'tilde~.txt', 'at@dollar$.txt', '.dotfile']
    files = {}
    for index, name in enumerate(names):
        path = directory / name
        path.write_bytes(('synthetic public url %d\n' % index).encode())
        files[name] = path
    lab.set_profile(objectPrefix=rec.prefix + 'sub dir/ü/')
    summary = run_upload(lab, rec, list(files.values()))
    fetch = {}
    for item in summary['items']:
        if item['uploadStatus'] != 'uploaded' or not item['directUrl']:
            continue
        url = item['directUrl']
        # The link must name the stored key exactly (decode it as a server would).
        parsed = urllib.parse.urlsplit(url)
        decoded = urllib.parse.unquote(parsed.path)
        expected_tail = '/' + target.bucket + '/' + item['key']
        if not target.cloud or True:
            if decoded != expected_tail and not decoded.endswith('/' + item['key']):
                rec.flag('link_names_other_key', 'link for %r decodes to %r, stored key is %r' % (item['name'], decoded, item['key']))
        if public:
            try:
                reply = urllib.request.urlopen(urllib.request.Request(url), timeout=20)
                body = reply.read()
                fetch[item['name']] = reply.status if hashlib.sha256(body).hexdigest() == item['sha256'] else 'wrong-bytes'
            except urllib.error.HTTPError as error:
                fetch[item['name']] = error.code
            except Exception as error:  # noqa: BLE001
                fetch[item['name']] = repr(error)[:60]
            if fetch[item['name']] != 200:
                rec.flag('link_does_not_open', 'GET %s -> %s' % (url[-70:], fetch[item['name']]))
    rec.data['fetch'] = fetch
    check_integrity(rec, {}, lab)


def make_public(target):
    """Allow anonymous GET on the test bucket where the server takes a bucket policy; returns whether it worked."""
    if target.cloud:
        return False
    if isinstance(target, OracleTarget):
        return True
    policy = json.dumps({'Version': '2012-10-17', 'Statement': [{'Effect': 'Allow', 'Principal': {'AWS': ['*']}, 'Action': ['s3:GetObject'], 'Resource': ['arn:aws:s3:::%s/*' % target.bucket]}]}).encode()
    try:
        reply = target.raw.request('PUT', query={'policy': ''}, body=policy)
        if reply.status not in (200, 204):
            target.raw.request('PUT', query={'acl': ''}, headers={'x-amz-acl': 'public-read'})
        # Only a probe tells: some servers accept the call and still serve nothing without credentials.
        probe = '__s3diff_public_probe'
        target.seed(probe, b'probe')
        try:
            url = target.endpoint.rstrip('/') + '/' + target.bucket + '/' + probe
            return urllib.request.urlopen(url, timeout=10).status == 200
        except (urllib.error.URLError, OSError):
            return False
        finally:
            target.remove(probe)
    except (OSError, ConnectionError):
        return False


@case('acl-modes', 'aclMode public-read: whether the server takes the ACL, and what put.gd says when it does not')
def acl_modes(lab, rec):
    directory = lab.work / ('c-acl-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / 'acl.txt'
    path.write_bytes(b'synthetic acl\n')
    lab.set_profile(objectPrefix=rec.prefix, aclMode='public-read')
    run_upload(lab, rec, [path])
    check_integrity(rec, {}, lab)


@case('region-quirks', 'a wrong region and an unusual one against this server')
def region_quirks(lab, rec):
    directory = lab.work / ('c-region-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    for index, region in enumerate(('eu-west-3', 'us-east-1', 'us-east-2', 'auto', 'garage', 'ap-southeast-2')):
        if region == lab.target.region:
            continue
        path = directory / ('r%d.txt' % index)
        path.write_bytes(('synthetic region %s\n' % region).encode())
        lab.set_profile(objectPrefix=rec.prefix + region + '/', region=region)
        run_upload(lab, rec, [path])
    check_integrity(rec, {}, lab)


@case('addressing', 'path, virtual-host and auto addressing')
def addressing(lab, rec):
    directory = lab.work / ('c-addr-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    for mode in ('path', 'virtual', 'auto'):
        path = directory / ('addr-%s.txt' % mode)
        path.write_bytes(('synthetic addressing %s\n' % mode).encode())
        error = lab.set_profile(objectPrefix=rec.prefix + mode + '/', addressing=mode)
        if error:
            rec.note('%s: settings refused %s' % (mode, json.dumps(error)[:160]))
            rec.data.setdefault('settings_refused', {})[mode] = error.get('code') if isinstance(error, dict) else str(error)
            continue
        run_upload(lab, rec, [path])
    lab.set_profile(objectPrefix=rec.prefix)
    check_integrity(rec, {}, lab)


@case('listing-denied', 'no ListBucket: the name check (HEAD of a missing key) answers 403; put.gd must neither overwrite nor lose data')
def listing_denied(lab, rec):
    directory = lab.work / ('c-deny-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    lab.set_profile(objectPrefix=rec.prefix)
    path = directory / 'deny.txt'
    path.write_bytes(b'synthetic listing denied\n')
    if lab.target.proxy:
        # Emulated for every proxied server: a 403 on HEAD is what S3 answers for a missing key without s3:ListBucket.
        lab.target.proxy.set_rules([Rule('HEAD', action='status', count=100, status=403, code='AccessDenied')])
        summary = run_upload(lab, rec, [path])
        puts = [r for r in rec.lab.target.proxy.snapshot()[0] if r['method'] == 'PUT']
        if puts:
            rec.flag('put_after_inconclusive_head', 'put.gd sent PUT after a 403 on the name HEAD (%d PUT)' % len(puts))
        lab.target.proxy.set_rules([])
    key = lab.target.restricted() if hasattr(lab.target, 'restricted') else None
    if key is not None:
        original = lab.target.credentials
        lab.target.credentials = lambda: key
        lab.store_credentials()
        path2 = directory / 'deny-real.txt'
        path2.write_bytes(b'synthetic listing denied (real restricted key)\n')
        run_upload(lab, rec, [path2])
        lab.target.credentials = original
        lab.store_credentials()
    check_integrity(rec, {}, lab)


@case('connection-repeat', 'thirty small uploads, each its own operation (a new S3 worker and TLS connection each): how many fail to connect?')
def connection_repeat(lab, rec):
    directory = lab.work / ('c-rep-%d' % lab.case_index)
    directory.mkdir(parents=True, exist_ok=True)
    lab.set_profile(objectPrefix=rec.prefix)
    failures = {}
    for index in range(int(getattr(lab.options, 'repeat', 30))):
        path = directory / ('r%02d.txt' % index)
        path.write_bytes(('synthetic repeat %d\n' % index).encode())
        summary = run_upload(lab, rec, [path])
        for item in summary['items']:
            if item['uploadStatus'] != 'uploaded':
                key = '%s/%s/curl%s' % (item['errorCode'], item['requestStage'], item.get('curlCode'))
                failures[key] = failures.get(key, 0) + 1
    rec.data['repeat_failures'] = failures
    rec.note('failures over %d single-file operations: %s' % (index + 1, failures or 'none'))
    check_integrity(rec, {}, lab)


# ---------------------------------------------------------------------------------------------- running

def run_case(lab, name):
    function, doc, needs = CASES[name]
    prefix = case_prefix(lab, name)
    rec = Recorder(lab, name, prefix)
    lab.case_index += 1
    if 'proxy' in needs and not lab.target.proxy:
        rec.note('skipped: needs the fault proxy (not available for %s)' % lab.target.name)
        return rec.finish()
    started = time.monotonic()
    try:
        if lab.target.proxy:
            lab.target.proxy.clear()
        function(lab, rec)
    except Exception as error:  # noqa: BLE001
        rec.data['harness_error'] = ''.join(traceback.format_exception_only(type(error), error)).strip()[:400]
        rec.data['trace'] = traceback.format_exc()[-1500:]
    rec.data['seconds'] = round(time.monotonic() - started, 1)
    result = rec.finish()
    try:
        lab.target.cleanup(prefix)
    except Exception as error:  # noqa: BLE001
        result['cleanup_error'] = repr(error)[:400]
    return result


def run_server(options, name, run_id):
    """Run the selected cases against one server. Returns (results, clean): clean is False when a case's cleanup failed or anything of
    this run is still on the server at the end (including keys that start with '/'), so the command can exit non-zero."""
    run = Path(options.work) / name / 'oracle-run'
    run.mkdir(parents=True, exist_ok=True)
    (run / 'logs').mkdir(exist_ok=True)
    target = build_targets([name], run, run_id)[name]
    lab = Lab(options, target, run_id)
    selected = [c for c in (options.cases.split(',') if options.cases else CASES) if c in CASES]
    results = []
    clean = True
    preflight_ok = False
    try:
        # A cloud bucket that keeps versions would keep what cleanup deletes: refuse before the first byte is written.
        target.require_unversioned()
        preflight_ok = True
        lab.start()
        for case_name in selected:
            print('[%s] %s ...' % (name, case_name), flush=True)
            result = run_case(lab, case_name)
            path = lab.root / ('%s.json' % case_name)
            path.write_text(json.dumps(result, indent=1, default=str))
            flags = len(result.get('violations') or [])
            if result.get('cleanup_error'):
                clean = False
                print('[%s] %s: CLEANUP FAILED %s' % (name, case_name, result['cleanup_error']), flush=True)
            print('[%s] %s: %s%s' % (name, case_name, 'harness error ' + result['harness_error'] if result.get('harness_error') else 'ok', ', %d flag(s)' % flags if flags else ''), flush=True)
            results.append(result)
    finally:
        lab.stop()
        # A refused bucket was never written to by this run, so nothing of it is deleted (a reused --run-id could otherwise
        # delete older objects, and a versioned bucket would gain delete markers).
        clean = (final_leftover_check(target, lab, name, run_id) and clean) if preflight_ok else False
        target.close()
    return results, clean


def final_leftover_check(target, lab, name, run_id):
    """Whatever is still under this run's namespace (its prefix and the leading-slash twin): recorded, removed, and reported as a failure.
    Cloud runs must leave nothing; a failed check is a failure too."""
    root = '%s/%s/' % (target.root_prefix, run_id)
    try:
        leftovers = target.stored(root)
        uploads = target.uploads(root)
        (lab.root / 'leftovers.json').write_text(json.dumps({'keys': [e['key'] for e in leftovers], 'uploads': uploads}))
        print('[%s] leftovers: %d keys, %s incomplete uploads' % (name, len(leftovers), len(uploads) if uploads is not None else 'unlisted'), flush=True)
        found = bool(leftovers) or bool(uploads)
        if found:
            try:
                target.cleanup(root)
                print('[%s] LEFTOVERS removed (exact keys and upload IDs of this run)' % name, flush=True)
            except Exception as error:  # noqa: BLE001
                print('[%s] LEFTOVERS NOT REMOVED: %r' % (name, error), flush=True)
        # An upload listing the server would not give is unverified, never clean.
        return not found and uploads is not None
    except Exception as error:  # noqa: BLE001
        print('[%s] leftover check failed: %r' % (name, error), flush=True)
        return False


# ---------------------------------------------------------------------------------------------- report

GENERATED_SUFFIX = re.compile(r'-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}')


def normalise_key(key):
    """put.gd's collision rename appends a random UUID ('name-<uuid>.txt'): the same behaviour must give the same text on every server."""
    return GENERATED_SUFFIX.sub('-<uuid>', key or '')


def deletion_outcome(record):
    """What a deletion record says, without its identities (IDs, times, acknowledgement, profile revision, endpoint)."""
    if not isinstance(record, dict):
        return None
    record = record.get('deletion') or record
    if record.get('confirmError') is not None:
        return {'confirmError': (record['confirmError'] or {}).get('code')}
    items = []
    for item in record.get('items') or []:
        error = item.get('error') or {}
        items.append({'status': item.get('status'), 'phase': (item.get('progress') or {}).get('phase'), 'errorCode': error.get('code') or '',
                      'requestStage': (error.get('details') or {}).get('requestStage')})
    return {'state': record.get('state'), 'cancelled': record.get('cancelled'), 'items': items}


def signature(result):
    """What differs between servers for one case, normalised: per item status/error/key shape, stored content flags, and the recorded
    deletion outcomes, lost-delete retry and receipt-key HEAD results."""
    rows = []
    prefix = result.get('prefix', '')
    stored_keys = {e['key'] for e in result.get('stored') or []}
    for op in result.get('ops') or []:
        for item in op.get('items') or []:
            key = item.get('key') or ''
            # The receipt must name an object the server really holds: compared on the raw keys, shown on the normalised ones.
            named = bool(key) and key in stored_keys
            rows.append((item.get('name'), item.get('uploadStatus'), item.get('errorCode'), item.get('requestStage'), normalise_key(relative(prefix, key)),
                         bool(item.get('directUrl')), named if item.get('uploadStatus') == 'uploaded' else None))
        if op.get('error'):
            rows.append(('op-error', (op['error'] or {}).get('code')))
    # The oracle keeps no content type of a single-request upload, so it stays out of the comparison (content-and-metadata lists it per server).
    stored = sorted((normalise_key(relative(prefix, e['key'])), e['size'], e['sha256'][:12]) for e in (result.get('stored') or []))
    signed = {'items': rows, 'stored': stored, 'orphans': len(result.get('uploads') or []), 'flags': sorted(v['code'] for v in result.get('violations') or [])}
    deletions = {name: {'prepare': entry.get('prepare'), 'outcome': deletion_outcome(entry.get('final'))} for name, entry in sorted((result.get('deletions') or {}).items())}
    if deletions:
        signed['deletions'] = deletions
    if 'lost_delete' in result:
        retry = result.get('lost_delete_retry') or {}
        signed['lost_delete'] = {'first': deletion_outcome(result['lost_delete']),
                                 'retry': {'ok': retry.get('ok'), 'error': (retry.get('error') or {}).get('code') if isinstance(retry.get('error'), dict) else retry.get('error'),
                                           'outcome': deletion_outcome(retry.get('final'))} if retry else None}
    # The leading-slash shape names the whole prefix, run ID included; it is the same shape on every server and run.
    heads = {('^/<run>/ls/' if shape.startswith('^') else shape): {'receiptKey': normalise_key(entry.get('receiptKey')), 'head': entry.get('head')} for shape, entry in sorted((result.get('receipt_key_head') or {}).items())}
    if heads:
        signed['receipt_key_head'] = heads
    return signed


def report(options):
    base = Path(options.out)
    servers_dirs = sorted(p for p in base.iterdir() if p.is_dir() and any(p.glob('*.json')))
    cases = sorted({f.stem for d in servers_dirs for f in d.glob('*.json') if f.stem != 'leftovers'})
    lines = ['# S3 differential results', '']
    header = '| case | ' + ' | '.join(d.name for d in servers_dirs) + ' |'
    lines += [header, '|' + '---|' * (len(servers_dirs) + 1)]
    detail = []
    for case_name in cases:
        row = [case_name]
        signatures = {}
        for directory in servers_dirs:
            path = directory / (case_name + '.json')
            if not path.exists():
                row.append('-')
                continue
            result = json.loads(path.read_text())
            flags = [v['code'] for v in result.get('violations') or []]
            signatures[directory.name] = signature(result)
            mark = 'ERR' if result.get('harness_error') else ('FLAG:' + ','.join(sorted(set(flags)))) if flags else 'ok'
            uploaded = sum(1 for op in result.get('ops') or [] for i in op.get('items') or [] if i.get('uploadStatus') == 'uploaded')
            total = sum(len(op.get('items') or []) for op in result.get('ops') or [])
            row.append('%s (%d/%d up)' % (mark, uploaded, total))
        distinct = {json.dumps(s, sort_keys=True, default=str) for s in signatures.values()}
        if len(distinct) > 1:
            row[0] = '**%s** (differs)' % case_name
        lines.append('| ' + ' | '.join(row) + ' |')
        if len(distinct) > 1:
            detail.append((case_name, signatures))
    lines += ['', '## Differences per case', '']
    for case_name, signatures in detail:
        lines.append('### %s' % case_name)
        groups = {}
        for server_name, sig in signatures.items():
            groups.setdefault(json.dumps(sig, sort_keys=True, default=str), []).append(server_name)
        for blob, members in groups.items():
            sig = json.loads(blob)
            more = [name for name in ('deletions', 'lost_delete', 'receipt_key_head') if name in sig]
            lines.append('- **%s**: items=%d, stored=%d, orphans=%d, flags=%s%s' % (', '.join(members), len(sig['items']), len(sig['stored']), sig['orphans'], sig['flags'],
                                                                                 ', also compared: ' + ', '.join(more) if more else ''))
        lines.append('')
    text = '\n'.join(lines)
    (base / 'REPORT.md').write_text(text)
    print(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', default='build/release')
    parser.add_argument('--out', required=True, help='results directory (on disk)')
    parser.add_argument('--work', default=None, help='scratch directory for resident roots and payloads (default: <out>/work)')
    parser.add_argument('--max-mib', type=int, default=0, help='skip size-ladder steps above this many MiB (0 = all)')
    parser.add_argument('--repeat', type=int, default=30, help='operations in the connection-repeat case')
    parser.add_argument('--sizes', default='', help='size-ladder steps to run, e.g. s1,s-over (default: all)')
    parser.add_argument('command', choices=('run', 'report', 'cases'))
    parser.add_argument('--servers', default='oracle')
    parser.add_argument('--cases', default='')
    parser.add_argument('--run-id', default=None)
    options = parser.parse_args()
    if options.command == 'cases':
        for name, (_, doc, needs) in CASES.items():
            print('%-22s %s' % (name, doc))
        return 0
    Path(options.out).mkdir(parents=True, exist_ok=True)
    options.work = options.work or str(Path(options.out) / 'work')
    if options.command == 'report':
        report(options)
        return 0
    run_id = options.run_id or time.strftime('%m%d%H%M%S') + '-' + '%04x' % random.randrange(1 << 16)
    clean = True
    for name in options.servers.split(','):
        clean = run_server(options, name, run_id)[1] and clean
    if not clean:
        print('FAILED: this run left something on a server or could not remove it (see the CLEANUP FAILED / LEFTOVERS lines above)', file=sys.stderr)
    return 0 if clean else 1


if __name__ == '__main__':
    sys.exit(main())
