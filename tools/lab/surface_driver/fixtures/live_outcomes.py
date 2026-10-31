"""Builder-only Tier 1 provisioning. Secrets remain in this process and bridge pipes."""
import json
import os
from pathlib import Path
import re
import shlex
import uuid
from urllib.parse import unquote, urlsplit

from ..api import RecipeBlocked
from .base import Fixture


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


def admit(ctx):
    if (ctx.destination not in ('s3', 'proton') or ctx.placement != 'guest'
            or ctx.target.physical or not ctx.profile.get('guest') or not ctx._live_admitted):
        raise RecipeBlocked('Tier 1 live requires the admitted builder guest; boat and native are forbidden')
    allowed = {'mint-cinnamon-x11': ('d69', 'f44'), 'ubuntu-gnome-wayland': ('d69', 'f44'),
               'kubuntu-kde-wayland': ('f44', 'k72'), 'aurora-kde-wayland': ('f44', 'd69', 'aurora-amd')}
    if ctx.host not in allowed.get(ctx.desktop, ()):
        raise RecipeBlocked('Tier 1 live desktop is not on an authorized builder')
    if ctx.package != ('appimage' if ctx.desktop == 'aurora-kde-wayland' else 'deb'):
        raise RecipeBlocked('Tier 1 live requires the published guest package, with AppImage on Aurora')
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


def provider(scope):
    ctx = scope.ctx
    admit(ctx)
    ctx.fixture('app-profile')
    pending = {'remote': False}
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
    profile_id = 'tier1-live'
    prefix = ('acceptance/tier1-live/' if ctx.destination == 's3' else 'putgd-tier1-')+uuid.uuid4().hex+'/'
    fixture = Fixture(profile_id=profile_id, prefix=prefix, pending=pending)
    if ctx.destination == 's3':
        from tools.lab import provision_live_credentials as bridge
        from tools.lab import proton_lab
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
        provider_settings = json.loads((Path(__file__).resolve().parents[4]/'config/development.example.json').read_text())['profiles'][0]['settings']
        provider_settings.update(bucket=settings['bucket'], region=region, endpoint='https://s3.'+region+'.amazonaws.com',
                                 objectPrefix=prefix, publicUrlBase='https://put.gd', aclMode='omit', addressing='auto')
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
                 'archive': {'keepUploads': True}})
    return fixture


def preflight_s3(ctx, client, bucket, prefix):
    """One read-only HEAD, confined to the acceptance identity's new prefix."""
    if bucket != 'put.gd' or not re.fullmatch(r'acceptance/tier1-live/[0-9a-f]{32}/', prefix):
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
        endpoint = 's3.'+fixture.region+'.amazonaws.com'
        public = fixture.bucket == 'put.gd' and link.hostname == 'put.gd'
        virtual = link.hostname == fixture.bucket+'.'+endpoint
        expected_path = '/'+key if public or virtual else '/'+fixture.bucket+'/'+key
        ctx.expect(link.scheme == 'https' and link.hostname in (endpoint, fixture.bucket+'.'+endpoint, 'put.gd' if public else endpoint)
                   and not link.username and not link.password and link.port in (None, 443)
                   and not link.query and not link.fragment and unquote(link.path) == expected_path,
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
