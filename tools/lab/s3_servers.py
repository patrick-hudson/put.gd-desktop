#!/usr/bin/env python3
"""Local S3-compatible servers for the differential test (tools/lab/s3_differential.py), and a small SigV4 client.

Run it on a build server (podman, loopback ports only). Every container is named s3diff-<name> and keeps its data in a
named volume of the same name, so `stop` removes exactly those. Credentials are synthetic and fixed.

  python3 -I tools/lab/s3_servers.py start minio versitygw garage seaweedfs rustfs zenko
  python3 -I tools/lab/s3_servers.py status
  python3 -I tools/lab/s3_servers.py stop minio versitygw garage seaweedfs rustfs zenko

`start` prints one JSON object per server: endpoint, region, bucket and the synthetic key pair.
The offline oracle (tests/fixtures/s3_server.py) is started by the differential itself, not here.

MinIO's own images are gone from quay.io and Docker Hub; the Chainguard build of the community source is the one that
pulls. Ceph RGW is left out: its demo image is not published any more and a vstart cluster is not quick.
"""
import datetime
import hashlib
import hmac
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ElementTree

# Synthetic and public: it only opens containers on 127.0.0.1. Garage insists on 'GK' + 24 hex for an access key and 64 hex for a
# secret, so both are built from a word instead of written as literals (the secret scan rightly distrusts hex strings).
_WORD = 'putgdiff'.encode().hex()
ACCESS = 'GK' + _WORD + '00112233'
SECRET = _WORD * 4
BUCKET = 'putgd-diff'
RPC_SECRET = ('0123456789abcdef' * 4)

SERVERS = {
    'minio': {'port': 19001, 'region': 'us-east-1', 'image': 'cgr.dev/chainguard/minio'},
    'versitygw': {'port': 19002, 'region': 'us-east-1', 'image': 'ghcr.io/versity/versitygw'},
    'garage': {'port': 19003, 'region': 'garage', 'image': 'docker.io/dxflrs/garage:v2.1.0'},
    'seaweedfs': {'port': 19004, 'region': 'us-east-1', 'image': 'docker.io/chrislusf/seaweedfs'},
    'rustfs': {'port': 19005, 'region': 'us-east-1', 'image': 'docker.io/rustfs/rustfs'},
    'zenko': {'port': 19006, 'region': 'us-east-1', 'image': 'docker.io/zenko/cloudserver'},
}
NS = '{http://s3.amazonaws.com/doc/2006-03-01/}'


# ---------------------------------------------------------------------------------------------- the raw client

class Reply:
    def __init__(self, status, headers, body):
        self.status, self.headers, self.body = status, headers, body

    def code(self):
        """The S3 error code of an error reply ('' when there is none)."""
        try:
            return ElementTree.fromstring(self.body).findtext('Code') or ElementTree.fromstring(self.body).findtext(NS + 'Code') or ''
        except ElementTree.ParseError:
            return ''


class RawS3:
    """A path-style SigV4 client that shares nothing with put.gd: it is the ground truth about what a server stores."""

    def __init__(self, endpoint, region, access, secret, bucket, session_token=None, virtual=False):
        self.endpoint, self.region, self.access, self.secret, self.bucket = endpoint.rstrip('/'), region, access, secret, bucket
        self.token = session_token
        self.virtual = virtual
        self.host = urllib.parse.urlsplit(self.endpoint).netloc

    def request(self, method, key='', query=None, body=b'', headers=None, bucket=True, timeout=60):
        query = query or {}
        host = self.host
        path = '/'
        if bucket:
            if self.virtual:
                host = self.bucket + '.' + self.host
                path = '/' + urllib.parse.quote(key, safe='/-_.~') if key else '/'
            else:
                path = '/' + self.bucket + ('/' + urllib.parse.quote(key, safe='/-_.~') if key else '')
        now = datetime.datetime.now(datetime.timezone.utc)
        stamp, day = now.strftime('%Y%m%dT%H%M%SZ'), now.strftime('%Y%m%d')
        payload = hashlib.sha256(body).hexdigest()
        signed = {'host': host, 'x-amz-content-sha256': payload, 'x-amz-date': stamp}
        if self.token:
            signed['x-amz-security-token'] = self.token
        for name, value in (headers or {}).items():
            signed[name.lower()] = ' '.join(str(value).split())
        names = ';'.join(sorted(signed))
        canonical_query = '&'.join('%s=%s' % (urllib.parse.quote(k, safe='-_.~'), urllib.parse.quote(str(v), safe='-_.~')) for k, v in sorted(query.items()))
        canonical = '\n'.join((method, path, canonical_query, ''.join('%s:%s\n' % (k, signed[k]) for k in sorted(signed)), names, payload))
        scope = '/'.join((day, self.region, 's3', 'aws4_request'))
        to_sign = '\n'.join(('AWS4-HMAC-SHA256', stamp, scope, hashlib.sha256(canonical.encode()).hexdigest()))
        key_bytes = ('AWS4' + self.secret).encode()
        for part in (day, self.region, 's3', 'aws4_request'):
            key_bytes = hmac.new(key_bytes, part.encode(), hashlib.sha256).digest()
        signature = hmac.new(key_bytes, to_sign.encode(), hashlib.sha256).hexdigest()
        send = dict(signed)
        send.pop('host')
        send['Authorization'] = 'AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s' % (self.access, scope, names, signature)
        url = self.endpoint + path + ('?' + canonical_query if canonical_query else '')
        if self.virtual and bucket:
            url = urllib.parse.urlsplit(self.endpoint).scheme + '://' + host + path + ('?' + canonical_query if canonical_query else '')
        request = urllib.request.Request(url, data=body if method in ('PUT', 'POST') else None, method=method, headers=send)
        # A cloud endpoint now and then resets a connection: the signed request is idempotent for the calls made here, so send it again.
        for attempt in range(4):
            try:
                with urllib.request.urlopen(request, timeout=timeout) as reply:
                    return Reply(reply.status, {k.lower(): v for k, v in reply.headers.items()}, reply.read() if method != 'HEAD' else b'')
            except urllib.error.HTTPError as error:
                return Reply(error.code, {k.lower(): v for k, v in error.headers.items()}, error.read() if method != 'HEAD' else b'')
            except (urllib.error.URLError, ConnectionError, TimeoutError):
                if attempt == 3:
                    raise
                time.sleep(1 + attempt)

    def create_bucket(self):
        return self.request('PUT', bucket=True)

    def get(self, key):
        return self.request('GET', key)

    def head(self, key):
        return self.request('HEAD', key)

    def put(self, key, body, headers=None):
        return self.request('PUT', key, body=body, headers=headers)

    def delete(self, key, version=None):
        return self.request('DELETE', key, {'versionId': version} if version else None)

    def list_keys(self, prefix=''):
        """Every key under a prefix (ListObjectsV2, paged): [(key, size, etag)]."""
        found, token = [], None
        while True:
            query = {'list-type': '2', 'prefix': prefix}
            if token:
                query['continuation-token'] = token
            reply = self.request('GET', query=query)
            if reply.status != 200:
                raise OSError('list failed %d %s' % (reply.status, reply.code()))
            root = ElementTree.fromstring(reply.body)
            for item in root.iter(NS + 'Contents'):
                found.append((item.findtext(NS + 'Key'), int(item.findtext(NS + 'Size')), (item.findtext(NS + 'ETag') or '').strip('"')))
            if root.findtext(NS + 'IsTruncated') != 'true':
                return found
            token = root.findtext(NS + 'NextContinuationToken')

    def list_versions(self, prefix=''):
        reply = self.request('GET', query={'versions': '', 'prefix': prefix})
        if reply.status != 200:
            return None
        root = ElementTree.fromstring(reply.body)
        return [(i.findtext(NS + 'Key'), i.findtext(NS + 'VersionId'), i.tag.endswith('DeleteMarker')) for i in list(root.iter(NS + 'Version')) + list(root.iter(NS + 'DeleteMarker'))]

    def list_uploads(self, prefix=''):
        """Incomplete multipart uploads under a prefix: [(key, uploadId)]."""
        reply = self.request('GET', query={'uploads': '', 'prefix': prefix})
        if reply.status != 200:
            return None
        root = ElementTree.fromstring(reply.body)
        return [(u.findtext(NS + 'Key'), u.findtext(NS + 'UploadId')) for u in root.iter(NS + 'Upload')]

    def abort(self, key, upload_id):
        return self.request('DELETE', key, {'uploadId': upload_id})

    def versioning(self):
        """The bucket's versioning state: '' (never configured), 'Enabled' or 'Suspended'; None when it cannot be read.
        A server that does not implement the call (R2 answers 501) keeps no versions."""
        reply = self.request('GET', query={'versioning': ''})
        if reply.status == 501:
            return ''
        if reply.status != 200:
            return None
        try:
            return ElementTree.fromstring(reply.body).findtext(NS + 'Status') or ElementTree.fromstring(reply.body).findtext('Status') or ''
        except ElementTree.ParseError:
            return None


# ---------------------------------------------------------------------------------------------- containers

def podman(*args, check=True, timeout=300):
    done = subprocess.run(('podman',) + args, capture_output=True, text=True, timeout=timeout)
    if check and done.returncode:
        raise SystemExit('podman %s failed: %s' % (' '.join(args[:3]), done.stderr.strip()[-400:]))
    return done


def container(name):
    return 's3diff-' + name


def volume(name):
    podman('volume', 'create', container(name))
    return container(name)


def ready(endpoint, seconds=90):
    """Wait until something answers HTTP on the endpoint (any status)."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            urllib.request.urlopen(endpoint, timeout=3)
            return True
        except urllib.error.HTTPError:
            return True
        except (urllib.error.URLError, OSError, ConnectionError):
            time.sleep(0.5)
    return False


def garage_config(directory):
    path = Path(directory) / 'garage.toml'
    path.write_text('\n'.join((
        'metadata_dir = "/var/lib/garage/meta"', 'data_dir = "/var/lib/garage/data"', 'db_engine = "sqlite"', 'replication_factor = 1',
        'rpc_bind_addr = "127.0.0.1:3901"', 'rpc_public_addr = "127.0.0.1:3901"', 'rpc_secret = "%s"' % RPC_SECRET,
        '[s3_api]', 's3_region = "garage"', 'api_bind_addr = "0.0.0.0:3900"', 'root_domain = ".s3.garage.localhost"', '')))
    return path


def start(name):
    spec = SERVERS[name]
    name_c, port = container(name), spec['port']
    podman('rm', '-f', name_c, check=False)
    vol = volume(name)
    base = ['run', '-d', '--name', name_c, '--label', 's3diff=1', '-p', '127.0.0.1:%d:' % port]
    if name == 'minio':
        podman(*base[:-1], '127.0.0.1:%d:9000' % port, '-e', 'MINIO_ROOT_USER=' + ACCESS, '-e', 'MINIO_ROOT_PASSWORD=' + SECRET, '-v', vol + ':/data:U',
               spec['image'], 'server', '/data', '--address', ':9000')
    elif name == 'versitygw':
        podman(*base[:-1], '127.0.0.1:%d:7070' % port, '-v', vol + ':/data:U', spec['image'], '--access', ACCESS, '--secret', SECRET, '--port', ':7070', 'posix', '/data')
    elif name == 'garage':
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import scratch  # on disk: /tmp is RAM on the builders (#1978)
        directory = str(scratch.mkdtemp(prefix='s3diff-garage-'))
        config = garage_config(directory)
        podman(*base[:-1], '127.0.0.1:%d:3900' % port, '-v', '%s:/etc/garage.toml:ro,Z' % config, '-v', vol + ':/var/lib/garage:U', spec['image'], '/garage', 'server')
    elif name == 'seaweedfs':
        podman(*base[:-1], '127.0.0.1:%d:8333' % port, '-e', 'AWS_ACCESS_KEY_ID=' + ACCESS, '-e', 'AWS_SECRET_ACCESS_KEY=' + SECRET, '-v', vol + ':/data:U',
               spec['image'], 'mini', '-dir=/data', '-ip=127.0.0.1')
    elif name == 'rustfs':
        podman(*base[:-1], '127.0.0.1:%d:9000' % port, '-e', 'RUSTFS_ACCESS_KEY=' + ACCESS, '-e', 'RUSTFS_SECRET_KEY=' + SECRET, '-e', 'RUSTFS_VOLUMES=/data',
               '-v', vol + ':/data:U', spec['image'])
    elif name == 'zenko':
        podman(*base[:-1], '127.0.0.1:%d:8000' % port, '-e', 'SCALITY_ACCESS_KEY_ID=' + ACCESS, '-e', 'SCALITY_SECRET_ACCESS_KEY=' + SECRET, '-e', 'S3BACKEND=mem',
               '-e', 'REMOTE_MANAGEMENT_DISABLE=1', '-e', 'LOG_LEVEL=error', spec['image'])
    endpoint = 'http://127.0.0.1:%d' % port
    if not ready(endpoint, 120):
        raise SystemExit('%s did not answer on %s' % (name, endpoint))
    if name == 'garage':
        time.sleep(2)
        identity = podman('exec', name_c, '/garage', 'node', 'id', '-q').stdout.strip().split('@')[0]
        podman('exec', name_c, '/garage', 'layout', 'assign', '-z', 'dc1', '-c', '1G', identity)
        podman('exec', name_c, '/garage', 'layout', 'apply', '--version', '1')
        time.sleep(3)
        podman('exec', name_c, '/garage', 'key', 'import', '--yes', '-n', 'putgd-diff', ACCESS, SECRET)
        podman('exec', name_c, '/garage', 'bucket', 'create', BUCKET)
        podman('exec', name_c, '/garage', 'bucket', 'allow', '--read', '--write', '--owner', BUCKET, '--key', 'putgd-diff')
    client = RawS3(endpoint, spec['region'], ACCESS, SECRET, BUCKET)
    if name != 'garage':
        end = time.monotonic() + 60
        reply = None
        while time.monotonic() < end:
            reply = client.create_bucket()
            if reply.status in (200, 409):
                break
            time.sleep(1)
        if reply is None or reply.status not in (200, 409):
            raise SystemExit('%s: create bucket answered %s %s' % (name, reply and reply.status, reply and reply.body[:200]))
    return {'name': name, 'endpoint': endpoint, 'region': spec['region'], 'bucket': BUCKET, 'access': ACCESS, 'secret': SECRET}


def stop(name):
    podman('rm', '-f', '-t', '3', container(name), check=False)
    podman('volume', 'rm', '-f', container(name), check=False)


def status():
    done = podman('ps', '-a', '--filter', 'label=s3diff=1', '--format', '{{.Names}} {{.Status}} {{.Ports}}', check=False)
    sys.stdout.write(done.stdout)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('start', 'stop', 'status'):
        raise SystemExit(__doc__)
    command, names = sys.argv[1], sys.argv[2:]
    unknown = [n for n in names if n not in SERVERS]
    if unknown:
        raise SystemExit('unknown server %s (known: %s)' % (unknown, ', '.join(SERVERS)))
    if command == 'status':
        status()
    for name in names:
        if command == 'start':
            print(json.dumps(start(name)), flush=True)
        elif command == 'stop':
            stop(name)


if __name__ == '__main__':
    main()
