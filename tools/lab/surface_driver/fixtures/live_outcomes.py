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
    """Select the second explicitly labelled pair, retaining shared bucket settings.

    No value, fragment, fingerprint or parser exception may enter evidence.
    The builder input has two Access key ID sections; duplicate/missing fields
    are rejected by the bridge parser rather than guessed.
    """
    from tools.lab import provision_live_credentials as bridge
    starts = list(re.finditer(r'(?im)^\s*Access key ID\s*:', text))
    if len(starts) != 2:
        raise RecipeBlocked('S3 input must contain exactly two labelled key sets')
    first = bridge.parse_s3_credentials(text[starts[0].start():starts[1].start()])
    second = bridge.parse_s3_credentials(text[starts[1].start():])
    return dict(credentials=second['credentials'], settings={**first['settings'], **second['settings']})


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
    profile_id, prefix = 'tier1-live', 'putgd-tier1-'+uuid.uuid4().hex+'/'
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
        if not settings.get('bucket'):
            raise RecipeBlocked('Key set 2 has no explicit shared bucket')
        options = dict(aws_access_key_id=keys['accessKey'], aws_secret_access_key=keys['secretKey'],
                       aws_session_token=keys.get('sessionToken') or None,
                       config=Config(connect_timeout=10, read_timeout=20, retries={'total_max_attempts': 2, 'mode': 'standard'}))
        # Only AWS S3 is authorized here. Do not infer a custom endpoint.
        if settings.get('endpoint'):
            raise RecipeBlocked('Tier 1 AWS S3 requires the standard service endpoint')
        session = boto3.Session(region_name=settings.get('region', 'us-east-1'))
        bootstrap = session.client('s3', **options)
        try:
            try:
                region = bootstrap.get_bucket_location(Bucket=settings['bucket'])['LocationConstraint'] or 'us-east-1'
            except bootstrap.exceptions.ClientError as error:
                # This is the provider preflight boundary. Never pass an SDK
                # message/response (which may contain a credential or URL) to
                # receipts, and never substitute another key set.
                code = error.response.get('Error', {}).get('Code')
                if code == 'InvalidAccessKeyId':
                    raise RecipeBlocked('S3 key set 2 rejected by AWS: InvalidAccessKeyId; no upload attempted') from None
                raise RecipeBlocked('S3 key set 2 read-only preflight failed; no upload attempted') from None
        finally:
            bootstrap.close()
        region = 'eu-west-1' if region == 'EU' else region
        client = session.client('s3', region_name=region, **options)
        scope.defer('independent S3 client', client.close)
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
                                 objectPrefix=prefix, publicUrlBase=None, aclMode='public-read', addressing='auto')
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
    if ctx.destination == 'proton':
        account = _setup(scope, profile_id, 'inspectAccount')
        profile['settings']['accountRootUid'] = account['accountRootUid']
        scope.patch({'profiles': [profile]})
        folder = _setup(scope, profile_id, 'createFolder', {'input': {'name': prefix.rstrip('/')}})
        profile['settings'].update(folderUid=folder['uid'], folderName=folder.get('name', prefix.rstrip('/')))
        fixture['folder_uid'] = folder['uid']
        scope.patch({'profiles': [profile]})
    fixture['credential_reference'] = reference
    checked = _setup(scope, profile_id, 'testDestination')
    ctx.expect(checked.get('testScope') == 'account-folder' if ctx.destination == 'proton' else bool(checked),
               'live destination read-only connection check completed')
    scope.patch({'actions': {name: dict(destination='upload', profileId=profile_id, publicationMode='share', shortcut='')
                             for name in ('region', 'window', 'desktop', 'edit', 'record')},
                 'archive': {'keepUploads': True}})
    return fixture


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
            return
        ctx.expect(False, 'authenticated S3 GET still resolves after app deletion')
    else:
        import hashlib
        link = urlsplit(receipt['directUrl'])
        endpoint = 's3.'+fixture.region+'.amazonaws.com'
        virtual = link.hostname == fixture.bucket+'.'+endpoint
        expected_path = '/'+key if virtual else '/'+fixture.bucket+'/'+key
        ctx.expect(link.scheme == 'https' and link.hostname in (endpoint, fixture.bucket+'.'+endpoint)
                   and not link.username and not link.password and link.port in (None, 443)
                   and not link.query and not link.fragment and unquote(link.path) == expected_path,
                   'copied public S3 link identifies the same independently verified key')
        head = fixture.client.head_object(Bucket=fixture.bucket, Key=key)
        ctx.expect(head['ContentLength'] == len(data), 'S3 HEAD length matches saved file')
        with fixture.client.get_object(Bucket=fixture.bucket, Key=key)['Body'] as body:
            remote = body.read(len(data)+1)
        ctx.expect(hashlib.sha256(remote).digest() == hashlib.sha256(data).digest(), 'S3 GET SHA-256 equals saved file')
