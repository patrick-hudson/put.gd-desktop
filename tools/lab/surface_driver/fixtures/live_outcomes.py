"""Builder-only Tier 1 provisioning. Secrets remain in this process and bridge pipes."""
import json
import os
from pathlib import Path
import re
import shlex
import uuid
from urllib.parse import unquote, urlsplit
import urllib.request

from ..api import RecipeBlocked
from .base import Fixture


def capture_name(prefix):
    """A capture file name nobody else can produce, and the exact object key it will have under ``prefix``.

    The profile's capture template is set to this literal name, so the key is known before publishing
    and the collision policy has nothing to rename (the prefix is new and the name is unique).
    """
    template = 'Tier1-'+uuid.uuid4().hex+'.{ext}'
    return template, prefix+template.replace('{ext}', 'png')


class RemotePending(dict):
    """``pending['remote']``, with a host-side marker file in the run directory while it is true.

    The marker names the destination, the object prefix, the exact object key and the run id, never a
    credential. It is written before publishing and removed only when the remote cleanup is confirmed, so a
    runner killed after publishing still leaves it for recovery (tools/lab/lab.py refuses to stop or retire
    such a guest) with the key to verify or delete.
    """

    def __init__(self, marker, details):
        super().__init__(remote=False)
        self.marker, self.details = Path(marker), details
        if self.marker.exists():
            raise RecipeBlocked('an earlier live remote cleanup in this run is unresolved; preserve the guest')

    def write(self):
        temporary = self.marker.with_suffix('.tmp')
        with open(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), 'w') as stream:
            json.dump(dict(schemaVersion=1, **self.details), stream)
        temporary.replace(self.marker)

    def record_key(self, key):
        """The receipt names the key the destination actually stored; keep the marker in step with it."""
        if self['remote'] and key != self.details.get('objectKey'):
            self.details['objectKey'] = key
            self.write()

    def __setitem__(self, key, value):
        if key == 'remote':
            if value:
                self.write()
            else:
                self.marker.unlink(missing_ok=True)
        super().__setitem__(key, value)


def key_set_two(text):
    """Select the sole bare AWS pair before other providers; settings are set 1's.

    Labels after the provider boundary are unrelated credentials. Never report
    input values, fragments, fingerprints or parser exceptions in evidence.
    """
    from tools.lab import provision_live_credentials as bridge
    if not isinstance(text, str) or '\0' in text or len(text.encode()) > bridge.MAX_CREDENTIAL_FILE:
        raise RecipeBlocked('S3 credential input has an invalid shape')
    boundary = re.search(r'(?im)^.*\b(?:Backblaze|Cloudflare)\b.*$', text)
    aws = text[:boundary.start()] if boundary else text
    pairs = list(re.finditer(r'(?m)^[ \t]*(AKIA[A-Z0-9]{16})[ \t]+([^\s]+)[ \t]*$', aws))
    if len(pairs) != 1 or len(pairs[0][2]) != 40:
        raise RecipeBlocked('S3 input must contain exactly one bare AWS key set 2 before other providers')
    first_block = re.sub(r'(?im)^(\s*)AWS Region ID\s*:', r'\1Region:', aws[:pairs[0].start()])
    if len(re.findall(r'(?im)^\s*Access key ID\s*:', first_block)) != 1:
        raise RecipeBlocked('S3 shared settings require exactly one labelled set 1 block')
    try:
        first = bridge.parse_s3_credentials(first_block)
    except Exception:
        raise RecipeBlocked('S3 labelled set 1 settings could not be parsed') from None
    if not first['settings'].get('bucket') or not first['settings'].get('region'):
        raise RecipeBlocked('S3 labelled set 1 must specify bucket and region')
    return dict(credentials=dict(accessKey=pairs[0][1], secretKey=pairs[0][2], sessionToken=''),
                settings=first['settings'])


def r2_scoped(text):
    """The bucket-scoped R2 key set and its bucket, account and public r2.dev base.

    The scoped block is the file's last (after the AWS and B2 sets): an ``R2 object-scoped`` heading,
    its labelled key pair, then ``R2 public URL`` and ``R2 bucket``. The account comes from the one R2
    S3 API address in the file. Never report input values, fragments or parser exceptions.
    """
    from tools.lab import provision_live_credentials as bridge
    if not isinstance(text, str) or '\0' in text or len(text.encode()) > bridge.MAX_CREDENTIAL_FILE:
        raise RecipeBlocked('R2 credential input has an invalid shape')
    headings = list(re.finditer(r'(?im)^.*\bR2 object-scoped\b.*$', text))
    if len(headings) != 1:
        raise RecipeBlocked('R2 input must contain exactly one object-scoped key block')
    block = text[headings[0].end():]
    if (len(re.findall(r'(?im)^\s*Access key ID\s*:', block)) != 1
            or len(re.findall(r'(?im)^\s*Secret access key\s*:', block)) != 1):
        raise RecipeBlocked('R2 object-scoped block must hold one labelled key pair')
    try:
        keys = bridge.parse_s3_credentials(block)['credentials']
    except Exception:
        raise RecipeBlocked('R2 object-scoped key pair could not be parsed') from None
    # R2 keys have their own shape: a 32-hex key ID and a 64-hex secret. Anything else (an AWS pair
    # pasted under the heading) is refused here, before any request could carry it to Cloudflare.
    if (not re.fullmatch(r'[0-9a-f]{32}', keys['accessKey']) or not re.fullmatch(r'[0-9a-f]{64}', keys['secretKey'])
            or keys.get('sessionToken')):
        raise RecipeBlocked('R2 object-scoped block does not hold an R2 key pair')
    public = re.findall(r'(?im)^\s*R2 public URL\s*:\s*(https://pub-[0-9a-f]+\.r2\.dev)/?\s*$', text)
    bucket = re.findall(r'(?im)^\s*R2 bucket\s*:\s*([a-z0-9][a-z0-9-]{1,61}[a-z0-9])\s*$', text)
    accounts = set(re.findall(r'https://([0-9a-f]{32})\.r2\.cloudflarestorage\.com', text))
    if len(public) != 1 or len(bucket) != 1 or len(accounts) != 1:
        raise RecipeBlocked('R2 input needs one public r2.dev URL, one bucket and one account endpoint')
    return dict(credentials=dict(accessKey=keys['accessKey'], secretKey=keys['secretKey'], sessionToken=''),
                settings=dict(accountId=accounts.pop(), bucket=bucket[0], publicUrlBase=public[0]))


def admit(ctx):
    """Require an approved disposable guest, held leases and private storage before live setup."""
    if (ctx.destination not in ('s3', 'proton') or ctx.placement != 'guest'
            or ctx.target.physical or not ctx.profile.get('guest') or not ctx._live_admitted):
        raise RecipeBlocked('Tier 1 live requires the admitted builder guest; boat and native are forbidden')
    allowed = {'mint-cinnamon-x11': ('d69', 'f44', 'm74'), 'ubuntu-gnome-wayland': ('d69', 'f44'),
               'kubuntu-kde-wayland': ('f44', 'k72'), 'aurora-kde-wayland': ('f44', 'd69', 'aurora-amd'),
               'fedora-gnome-wayland': ('f44',), 'fedora-budgie-wayland': ('d69', 'f44'),
               'omarchy-hyprland-wayland': ('m74',)}
    if ctx.host not in allowed.get(ctx.desktop, ()):
        raise RecipeBlocked('Tier 1 live desktop is not on an authorized builder')
    # Additional AppImage coverage is confined to the two declared builder guests.
    # Bind the requested profile to the actual owned run before any secret read.
    appimage_profile = {('m74', 'mint-cinnamon-x11'): 'cinnamon-x11-appimage',
                        ('d69', 'ubuntu-gnome-wayland'): 'gnome-wayland-appimage'}.get((ctx.host, ctx.desktop))
    appimage_guest = False
    if ctx.package == 'appimage' and appimage_profile and ctx.profile.get('guest') == appimage_profile:
        from tools.lab import lab
        appimage_guest = lab.state(ctx.target.run)['profile'] == appimage_profile
    expected = {'aurora-kde-wayland': 'appimage', 'fedora-gnome-wayland': 'rpm', 'fedora-budgie-wayland': 'rpm',
                'omarchy-hyprland-wayland': 'appimage'}.get(ctx.desktop, 'deb')
    if not appimage_guest and ctx.package != expected:
        raise RecipeBlocked('Tier 1 live requires an approved guest package/profile combination')
    # The VM-only desktops are bound to their one lab golden profile.
    golden = {'fedora-budgie-wayland': 'fedora45-budgie', 'omarchy-hyprland-wayland': 'omarchy-hyprland'}.get(ctx.desktop)
    if golden:
        from tools.lab import lab
        if lab.state(ctx.target.run)['profile'] != golden:
            raise RecipeBlocked('Tier 1 live requires the desktop\'s own lab golden profile')
    from tools.lab import desktop_lock
    session = os.environ.get('PUTGD_TIER1_SESSION', '')
    if not session:
        raise RecipeBlocked('Tier 1 live requires an explicit lock session')
    lock = desktop_lock.Desktop(Path.home()/'putgd-lab/locks', ctx.host)
    lock.heartbeat(session)  # Refuses another holder, expiry and missing locks.
    guest_lock = Path.home()/'putgd-lab/locks/tier1-guest.lock.d/session'
    if guest_lock.is_symlink() or guest_lock.read_text().strip() != session:
        raise RecipeBlocked('Tier 1 live requires the builder guest slot lock')
    if ctx.destination == 'proton':
        desktop_lock.Desktop(Path.home()/'putgd-lab/locks', ctx.host, resource='proton').heartbeat(session)
        if Path(os.environ.get('PUTGD_LAB_RUN', '/missing')).resolve() != ctx.target.run:
            raise RecipeBlocked('Proton requires this same-builder run under proton_lab lend')
        from tools.lab import provision_live_credentials as bridge
        _, lent = bridge._load_receipt(Path(os.environ.get('PUTGD_LAB_PROTON_RECEIPTS', '/missing'))/'provision.json')
        if (lent['provider'] != 'proton' or lent['kind'] != 'chain_guest' or lent['state'] != 'import_confirmed'
                or lent['run'] != str(ctx.target.run)
                or lent['credentialReference']['reference'] != os.environ.get('PUTGD_LAB_PROTON_REFERENCE')):
            raise RecipeBlocked('Proton bridge lend receipt does not match this exact guest')
    # Both application metadata (including History) and Firefox state must be
    # on guest tmpfs. No working URL is serialized onto the guest disk.
    result = ctx.shell("stat -f -c %T " + shlex.quote(str(Path(ctx.target.root).parent)))
    if result['returncode'] or result['stdout'].strip() != 'tmpfs':
        raise RecipeBlocked('Tier 1 live private app root must be on guest tmpfs')
    browser_root = ctx.profile.get('browser', {}).get('profile_root', '')
    if not browser_root.startswith(('/run/user/', '/dev/shm/')):
        raise RecipeBlocked('Tier 1 live guarded Firefox profile must be on guest tmpfs')
    result = ctx.shell('test ! -L '+shlex.quote(browser_root)+' && stat -f -c %T '+shlex.quote(browser_root))
    if result['returncode'] or result['stdout'].strip() != 'tmpfs':
        raise RecipeBlocked('Tier 1 live Firefox profile is not an existing guest tmpfs directory')


def _setup(scope, profile_id, kind, extra=None):
    ctx = scope.ctx
    current = ctx.call('settings.get')
    job = ctx.call('setup.'+kind, dict(profileId=profile_id,
                   expectedRevision=current['metadata']['observedRevision'], **(extra or {})))
    result = ctx.wait(lambda: (v if (v := ctx.call('setup.inspect', {'id': job['setupId']}))['state'] != 'running' else None),
                      'live destination setup settled', timeout=180)
    if result['state'] != 'completed':
        code = result.get('error', {}).get('code', '')
        if re.fullmatch(r'[a-z][a-z0-9_]{0,79}', code):
            ctx.note('setup_failure_code='+code)
    ctx.expect(result['state'] == 'completed', 'live setup.'+kind+' completed')
    return result.get('result', {})


def provider(scope, *, r2=False):
    """The admitted live destination; with R2, a Cloudflare R2 destination on the S3 lane."""
    ctx = scope.ctx
    admit(ctx)
    if r2 and ctx.destination != 's3':
        raise RecipeBlocked('Cloudflare R2 runs on the S3 live lane only')
    ctx.fixture('app-profile')
    profile_id = 'tier1-live'
    prefix = ('acceptance/tier1-live/' if ctx.destination == 's3' else 'putgd-tier1-')+uuid.uuid4().hex+'/'
    from tools.lab import lab
    capture_template, object_key = capture_name(prefix)
    # The marker names R2 as its own destination: the S3 recovery check reads AWS only.
    pending = RemotePending(Path(ctx.target.run)/lab.LIVE_CLEANUP_MARKER,
                            dict(destination='cloudflare-r2' if r2 else ctx.destination, prefix=prefix,
                                 objectKey=object_key, runId=ctx.target.info['id']))
    # The run's receipt reads this at the end, so an unresolved remote object is also a field of the receipt.
    ctx.target.live_cleanup_pending = pending
    def guarded(function):
        def finish():
            if pending['remote']:
                raise RecipeBlocked('live remote cleanup unresolved; preserve the guest, history and credential reference')
            return function()
        return finish
    # A failed delete/revoke must never be concealed by History/cache or XDG
    # cleanup. Target.close also refuses a root with a retained fixture scope.
    scope.finalizers[:] = [(priority, label, guarded(function) if label in
                           ('owned history and cache', 'accepted settings', 'owned directory') else function)
                          for priority, label, function in scope.finalizers]
    fixture = Fixture(profile_id=profile_id, prefix=prefix, pending=pending, object_key=object_key, r2=r2)
    if r2:
        provider_name, provider_settings, reference = _r2_setup(scope, fixture, prefix, guarded)
    elif ctx.destination == 's3':
        from tools.lab import provision_live_credentials as bridge
        try:
            import boto3
            from botocore.config import Config
        except ImportError:
            raise RecipeBlocked('live S3 verifier requires the builder distro python3-boto3 package') from None
        # Input is opened with the bridge's owned/private/no-symlink checks.
        selected = key_set_two(bridge.read_private_credentials(Path.home()/'Documents/creds.txt'))
        keys, settings = selected['credentials'], selected['settings']
        for value in keys.values():
            if value:
                ctx.evidence.redactor.remember(value, opaque=True)
        if settings.get('bucket') != 'put.gd':
            raise RecipeBlocked('Tier 1 key set 2 requires the explicitly authorized shared bucket')
        options = dict(aws_access_key_id=keys['accessKey'], aws_secret_access_key=keys['secretKey'],
                       aws_session_token=keys.get('sessionToken') or None,
                       config=Config(connect_timeout=10, read_timeout=20, retries={'total_max_attempts': 2, 'mode': 'standard'}))
        # Only AWS S3 is authorized here. Do not infer a custom endpoint.
        if settings.get('endpoint'):
            raise RecipeBlocked('Tier 1 AWS S3 requires the standard service endpoint')
        # Set 1 supplies the authorized bucket's region. GetBucketLocation is
        # unnecessary and can require a permission the acceptance key lacks.
        region = settings['region']
        if not re.fullmatch(r'[a-z]{2}(?:-gov)?-[a-z]+-[1-9][0-9]?', region):
            raise RecipeBlocked('S3 shared region is not a standard AWS region identifier')
        session = boto3.Session(region_name=region)
        client = session.client('s3', region_name=region, endpoint_url='https://s3.'+region+'.amazonaws.com', **options)
        scope.defer('independent S3 client', client.close)
        preflight_s3(ctx, client, settings['bucket'], prefix)
        reference = _import_keys(scope, keys, guarded)
        provider_settings = json.loads((Path(__file__).resolve().parents[4]/'config/development.example.json').read_text())['profiles'][0]['settings']
        provider_settings.update(bucket=settings['bucket'], region=region, endpoint='https://s3.'+region+'.amazonaws.com',
                                 objectPrefix=prefix, publicUrlBase='https://put.gd', aclMode='public-read', addressing='auto')
        fixture.update(client=client, bucket=settings['bucket'], region=region)
        provider_name = 's3'
    else:
        reference = os.environ.get('PUTGD_LAB_PROTON_REFERENCE', '')
        from tools.lab import provision_live_credentials as bridge
        bridge.validate_reference(reference, lab_only=True)
        provider_name = 'proton-drive'
        provider_settings = dict(accountRootUid='', folderUid='', shareByDefault=True,
                                 linkExpirationDays=0, sharePasswordReference=None, requestTimeoutMs=120000)
    profile = dict(id=profile_id, label='Tier 1 isolated live destination', provider=provider_name,
                   settings=provider_settings, credentials=dict(mode='keychain', reference=reference))
    scope.patch({'profiles': [profile]})
    if ctx.destination == 's3':
        _setup(scope, profile_id, 'unlockCredentials')
    if ctx.destination == 'proton':
        account = _setup(scope, profile_id, 'inspectAccount')
        profile['settings']['accountRootUid'] = account['accountRootUid']
        scope.patch({'profiles': [profile]})
        folder = _setup(scope, profile_id, 'createFolder', {'input': {'name': prefix.rstrip('/')}})
        profile['settings'].update(folderUid=folder['uid'], folderName=folder.get('name', prefix.rstrip('/')))
        fixture['folder_uid'] = folder['uid']
        scope.patch({'profiles': [profile]})
    fixture['credential_reference'] = reference
    if ctx.destination == 'proton':
        checked = _setup(scope, profile_id, 'testDestination')
        ctx.expect(checked.get('testScope') == 'account-folder', 'Proton account-folder connection check completed')
    # S3 testDestination publishes its own synthetic/multipart probes and
    # deletes them outside History. This lane instead tests its real capture
    # and verifies only that exact object with HEAD/GET and History deletion.
    scope.patch({'actions': {name: dict(destination='upload', profileId=profile_id, publicationMode='share', shortcut='')
                             for name in ('region', 'window', 'desktop', 'edit', 'record')},
                 'archive': {'keepUploads': True},
                 # A fixed capture name makes the object key known before anything is published.
                 'naming': {'captureTemplate': capture_template}})
    return fixture


def _import_keys(scope, keys, guarded):
    """Lend KEYS to the guest keychain through the S3 bridge; return the credential reference."""
    ctx = scope.ctx
    from tools.lab import provision_live_credentials as bridge
    from tools.lab import proton_lab
    guest = proton_lab.Guest(ctx.target.run)
    broker = guest.place_brokers(Path.home()/'putgd-lab/proton/guest-bridge')['qt']
    scope.defer('guest bridge executables', guest.remove_brokers, priority=-10)
    guest.prepare_keyring()
    reference = 'putgd-lab-'+str(uuid.uuid4())
    receipt = ctx.out/'s3-bridge.json'
    state = bridge._receipt_value('s3', 'guest', str(ctx.target.run), reference, 'one_time', 'import_pending', True)
    bridge._create_receipt(receipt, state)
    scope.defer('S3 bridge credential', guarded(lambda: bridge.cleanup(dict(op='cleanup', provider='s3',
                run=str(ctx.target.run), guestBroker=broker, reference=reference, receipt=str(receipt)))), priority=10)
    bridge.guest_call(str(ctx.target.run), broker, 's3', dict(op='import', reference=reference, credentials=keys))
    bridge._update_receipt(receipt, state, state='import_confirmed')
    return reference


def _r2_setup(scope, fixture, prefix, guarded):
    """The bucket-scoped R2 key, an independent client for it, and the cloudflare-r2 settings."""
    ctx = scope.ctx
    from tools.lab import provision_live_credentials as bridge
    try:
        import boto3
        from botocore.config import Config
    except ImportError:
        raise RecipeBlocked('live R2 verifier requires the builder distro python3-boto3 package') from None
    selected = r2_scoped(bridge.read_private_credentials(Path.home()/'Documents/creds.txt'))
    keys, settings = selected['credentials'], selected['settings']
    for value in (keys['accessKey'], keys['secretKey'], settings['accountId']):
        ctx.evidence.redactor.remember(value, opaque=True)
    endpoint = 'https://'+settings['accountId']+'.r2.cloudflarestorage.com'
    client = boto3.Session(region_name='auto').client(
        's3', region_name='auto', endpoint_url=endpoint, aws_access_key_id=keys['accessKey'],
        aws_secret_access_key=keys['secretKey'],
        config=Config(s3={'addressing_style': 'path'}, connect_timeout=10, read_timeout=20,
                      retries={'total_max_attempts': 2, 'mode': 'standard'}))
    scope.defer('independent R2 client', client.close)
    preflight_s3(ctx, client, settings['bucket'], prefix, r2=True)
    reference = _import_keys(scope, keys, guarded)
    provider_settings = dict(accountId=settings['accountId'], jurisdiction='default', bucket=settings['bucket'],
                             objectPrefix=prefix, linkMode='public', linkValidityDays=7,
                             publicUrlBase=settings['publicUrlBase'], connectTimeoutMs=5000, requestTimeoutMs=60000,
                             lowSpeedBytesPerSecond=1, lowSpeedTimeSeconds=30)
    fixture.update(client=client, bucket=settings['bucket'], region='auto', public_base=settings['publicUrlBase'],
                   account=settings['accountId'])
    return 'cloudflare-r2', provider_settings, reference


def preflight_s3(ctx, client, bucket, prefix, *, r2=False):
    """One read-only HEAD, confined to the acceptance identity's new prefix."""
    if (bucket != 'put.gd' and not r2) or not re.fullmatch(r'acceptance/tier1-live/[0-9a-f]{32}/', prefix):
        raise RecipeBlocked('S3 preflight cannot leave the authorized new acceptance prefix')
    try:
        client.head_object(Bucket=bucket, Key=prefix+'missing-preflight.png')
    except client.exceptions.ClientError as error:
        if error.response.get('ResponseMetadata', {}).get('HTTPStatusCode') == 404:
            ctx.note('s3_missing_head_404_count=1')
            return
        raise RecipeBlocked('S3 missing-key HEAD did not return 404; no upload attempted') from None
    raise RecipeBlocked('S3 preflight key unexpectedly exists; no upload attempted')


def verify_s3(ctx, fixture, detail, data=None, *, absent=False):
    """Read only the exact newly owned key; never list or mutate a bucket."""
    receipt = detail['receipt']
    key = receipt['objectReference']['key']
    ctx.expect(key.startswith(fixture.prefix) and receipt['destination']['settings']['bucket'] == fixture.bucket,
               'independent S3 check confined to the new campaign key')
    if absent:
        try:
            fixture.client.get_object(Bucket=fixture.bucket, Key=key)['Body'].close()
        except fixture.client.exceptions.NoSuchKey:
            ctx.note('s3_origin_absence_count=1')
            return
        ctx.expect(False, 'authenticated S3 GET still resolves after app deletion')
    else:
        import hashlib
        link = urlsplit(receipt['directUrl'])
        ctx.expect(link.scheme == 'https' and fixture.bucket == 'put.gd' and link.hostname == 'put.gd'
                   and not link.username and not link.password and link.port in (None, 443)
                   and not link.query and not link.fragment and unquote(link.path) == '/'+key,
                   'copied public S3 link identifies the same independently verified key')
        head = fixture.client.head_object(Bucket=fixture.bucket, Key=key)
        ctx.expect(head['ContentLength'] == len(data), 'S3 HEAD length matches saved file')
        with fixture.client.get_object(Bucket=fixture.bucket, Key=key)['Body'] as body:
            remote = body.read(len(data)+1)
        ctx.expect(hashlib.sha256(remote).digest() == hashlib.sha256(data).digest(), 'S3 GET SHA-256 equals saved file')


def reconcile_refused_s3(ctx, fixture, op, detail):
    """Clear pending only for one definitive Put refusal plus exact-key GET 404."""
    items = op.get('items', [])
    if detail.get('uploadStatus') != 'failed' or len(items) != 1:
        return False
    item = items[0]
    error = item.get('error', {})
    wire = error.get('details', {})
    if (error.get('code') != 's3_permission_denied' or wire.get('requestStage') != 'upload_put'
            or wire.get('httpStatus') != 403):
        return False
    key = item.get('objectReference', {}).get('key', '')
    ctx.expect(bool(key) and key.startswith(fixture.prefix) and key == fixture.prefix+item.get('candidate', ''),
               'refused Put key belongs to this exact new prefix and candidate')
    try:
        fixture.client.get_object(Bucket=fixture.bucket, Key=key)['Body'].close()
    except fixture.client.exceptions.NoSuchKey:
        fixture.pending['remote'] = False
        ctx.note('failed_upload_absence_count=1')
        return True
    ctx.expect(False, 'refused Put still has a remote object; preserve its row and credential')


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """A checked link must answer itself: a redirect is its status, never followed."""
    def redirect_request(self, *args, **kwargs):
        return None


def _public_get(url, *, timeout=20):
    """An independent unauthenticated GET from the builder: status and SHA-256, nothing else."""
    import hashlib
    import urllib.error
    import urllib.request
    request = urllib.request.Request(url, headers={'User-Agent': 'putgd-tier1-check'})
    try:
        with urllib.request.build_opener(_NoRedirect).open(request, timeout=timeout) as response:
            data = response.read(32*1024*1024+1)
            return dict(status=response.status, sha256=hashlib.sha256(data).hexdigest(), bytes=len(data))
    except urllib.error.HTTPError as error:
        return dict(status=error.code)
    except (urllib.error.URLError, OSError):
        return dict(status=0)


# The R2 test bucket's custom domain (Patrick, 2026-11-20); its r2.dev address comes from the credentials file.
R2_CUSTOM_DOMAIN = 'https://cf.ul.gy'


def verify_r2(ctx, fixture, detail, data=None, *, absent=False, base=None):
    """The exact new key through the scoped R2 key; with BASE, its public link on that host without credentials.

    After a deletion the scoped GET is decisive. A public host can serve a cached copy for a while (as
    CloudFront does), so a public GET then is recorded, not judged.
    """
    import hashlib
    from urllib.parse import quote
    receipt = detail['receipt']
    key = receipt['objectReference']['key']
    ctx.expect(key.startswith(fixture.prefix) and receipt['destination']['settings']['bucket'] == fixture.bucket,
               'independent R2 check confined to the new campaign key')
    if absent:
        try:
            fixture.client.get_object(Bucket=fixture.bucket, Key=key)['Body'].close()
        except fixture.client.exceptions.NoSuchKey:
            ctx.note('r2_origin_absence_count=1')
            if base:
                after = _public_get(base+'/'+quote(key, safe='/'))
                ctx.note('r2_public_after_delete_informational='+json.dumps(dict(host=base.split('//', 1)[1],
                                                                                 status=after['status'])))
            return
        ctx.expect(False, 'scoped R2 GET still resolves after app deletion')
    if base:
        ctx.expect(receipt['directUrl'] == base+'/'+quote(key, safe='/'),
                   'copied R2 link is the public '+base.split('//', 1)[1]+' address of the same key')
    head = fixture.client.head_object(Bucket=fixture.bucket, Key=key)
    ctx.expect(head['ContentLength'] == len(data), 'R2 HEAD length matches saved file')
    with fixture.client.get_object(Bucket=fixture.bucket, Key=key)['Body'] as body:
        remote = body.read(len(data)+1)
    ctx.expect(hashlib.sha256(remote).digest() == hashlib.sha256(data).digest(), 'R2 GET SHA-256 equals saved file')
    if base:
        public = _public_get(receipt['directUrl'])
        ctx.note('r2_public_status='+json.dumps(dict(host=base.split('//', 1)[1], status=public['status'])))
        ctx.expect(public['status'] == 200 and public.get('sha256') == hashlib.sha256(data).hexdigest(),
                   'public '+base.split('//', 1)[1]+' GET 200 SHA-256 equals saved file')


def verify_presigned(ctx, fixture, detail, data, *, host, refused=(403,)):
    """An expiring link (#1366): signed GET returns the file, the same URL unsigned is refused (REFUSED).

    The link must be on exactly HOST, the destination's S3 API endpoint, and name this run's bucket and
    key (path style, or the bucket as HOST's first label), before any request is sent.
    """
    import hashlib
    from urllib.parse import parse_qs, unquote, urlsplit, urlunsplit
    receipt = detail['receipt']
    key = receipt['objectReference']['key']
    link = urlsplit(receipt['directUrl'])
    query = parse_qs(link.query)
    expires = query.get('X-Amz-Expires', ['0'])[0]
    place = {(host, '/'+fixture.bucket+'/'+key), (fixture.bucket+'.'+host, '/'+key)}
    ctx.expect(key.startswith(fixture.prefix) and receipt['destination']['settings']['bucket'] == fixture.bucket
               and link.scheme == 'https' and link.port in (None, 443) and not link.username and not link.password
               and (link.hostname, unquote(link.path)) in place and not link.fragment
               and query.get('X-Amz-Algorithm') == ['AWS4-HMAC-SHA256'] and 'X-Amz-Signature' in query
               and expires.isdigit() and 0 < int(expires) <= 86400,
               'copied link is a SigV4 presigned GET of this run\'s key on the destination\'s S3 API host, '
               'valid for at most the chosen day')
    ctx.expect(bool(receipt.get('linkExpiresAt') or detail.get('linkExpiresAt')), 'the receipt records when the link expires')
    digest = hashlib.sha256(data).hexdigest()
    signed = _public_get(receipt['directUrl'])
    ctx.expect(signed['status'] == 200 and signed.get('sha256') == digest, 'presigned GET 200 SHA-256 equals saved file')
    unsigned = _public_get(urlunsplit((link.scheme, link.netloc, link.path, '', '')))
    ctx.note('presigned_status=200; unsigned_status='+str(unsigned['status']))
    ctx.expect(unsigned['status'] in refused, 'the same object without the signature is refused ('
               + '/'.join(map(str, refused)) + ')')
