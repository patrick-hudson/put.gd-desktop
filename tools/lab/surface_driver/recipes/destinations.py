"""Destinations real-input recipes; no live provider accounts.

Uses the core's owned guest loopback server and fake keyring fixture. No live
provider accounts enter this lane. Synthetic credential values stay in memory
and use the core secret-safe typing and frame-masking contracts.
"""
from __future__ import annotations

from copy import deepcopy
from functools import wraps
import hashlib
import ipaddress
import json
from pathlib import PurePosixPath
from urllib.parse import urlsplit

from tools.lab.surface_driver.api import RecipeBlocked, recipe

REFERENCE = '04-destinations-s3'
CORE_ISSUE = 666
NEEDS = ()


def _result(value):
    return value.get('result', value)


def _get(ctx):
    return _result(ctx.call('settings.get'))


def _fixture(ctx, name='destinations'):
    f = ctx.fixture(name)
    if not isinstance(f, dict) or f.get('private') is not True:
        ctx.blocked('Need a clean private destinations resident fixture (#666).', issue=CORE_ISSUE)
    path = PurePosixPath(f.get('config_path', ''))
    if not path.is_absolute() or path.name != 'config.json':
        ctx.blocked('Private fixture must identify its exact config.json.', issue=CORE_ISSUE)
    p = deepcopy(f.get('profile', {}))
    endpoint = urlsplit(p.get('settings', {}).get('endpoint', ''))
    try:
        loopback = ipaddress.ip_address(endpoint.hostname or '').is_loopback
    except ValueError:
        loopback = False
    if (p.get('provider') != 's3' or not loopback or endpoint.scheme != 'http'
            or endpoint.username or endpoint.password or endpoint.query or endpoint.fragment):
        ctx.blocked('Only a loopback S3 fixture is allowed in this lane.', issue=CORE_ISSUE)
    ctx.expect(_get(ctx)['metadata']['path'] == str(path), 'resident uses the exact private config')
    script = """import json,os,stat
from pathlib import Path
p=Path(PATH)
root=Path(os.environ['XDG_CONFIG_HOME'])
s=root.lstat()
ok=(root.is_absolute() and root.resolve()==root and root!=Path.home()/'.config'
    and stat.S_ISDIR(s.st_mode) and s.st_uid==os.getuid() and not s.st_mode & 0o077
    and p==root/'putgd/config.json' and p.resolve()==p)
print(json.dumps({'privateConfig':ok}))
""".replace('PATH', repr(str(path)))
    checked = ctx.shell("python3 - <<'PY'\n" + script + '\nPY\n')
    if checked['returncode'] != 0 or json.loads(checked['stdout']) != {'privateConfig': True}:
        ctx.blocked('Target XDG config must be private, owned and isolated from the user config.', issue=CORE_ISSUE)
    return f, p


def _patch(ctx, patch):
    current = _get(ctx)
    reply = ctx.call('settings.patch', {
        'expectedRevision': current['metadata']['observedRevision'], 'patch': patch})
    ctx.expect(not reply.get('error'), 'non-secret fixture settings patch accepted')


def _frame(ctx, label, reference=REFERENCE):
    return ctx.frame(label, window='destinations', reference=reference)


def _failure_frame(function):
    """Retain a referenced after-frame on refusal/failure, preserving its cause.

    The core masks declared secret controls before writing any frame, including
    failures during synthetic credential entry or reveal.
    """
    @wraps(function)
    def capture(ctx):
        try:
            return function(ctx)
        except Exception as error:
            try:
                _frame(ctx, 'after-failure')
            except Exception:
                pass  # The core also retains a full-screen failure witness.
            raise
    return capture


def _open(ctx, count=1, *, saved_credentials=False, fixture='destinations'):
    f, p = _fixture(ctx, fixture)
    profiles = []
    for index in range(count):
        item = deepcopy(p)
        item['id'] = 'surface-destination-' + str(index + 1)
        item['label'] = 'Surface destination ' + str(index + 1)
        if not saved_credentials:
            item['credentials'] = {'mode': 'session', 'reference': None}
        profiles.append(item)
    # This is setup, never the interaction claimed by the recipe. Remove old
    # profile references too, including the optional recording route.
    actions = {name: {'destination': 'local', 'profileId': None,
                      'publicationMode': 'default', 'shortcut': ''}
               for name in ('region', 'window', 'desktop', 'clipboard', 'edit', 'record')}
    _patch(ctx, {'profiles': profiles, 'actions': actions,
                 'captureDefaults': {'destination': 'local', 'profileId': None,
                                     'publicationMode': 'default'},
                 'archive': {'keepUploads': True},
                 'preferences': {'unlockCredentialsAtStartup': False,
                                 'appearance': {'theme': 'dark'}}})
    ctx.view('ui.show', {'view': 'destinations'})
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'known destination fixture')
    ctx.use_catalog('destinations', REFERENCE)
    geometry = ctx.geometry('destinations')
    ctx.scroll(geometry['x'] + geometry['width'] - 32,
               geometry['y'] + geometry['height']//2, 30)
    # The clean fixture opens its first profile. Rail selection is exercised
    # by the list/reorder recipes themselves, not as unrelated setup input.
    _frame(ctx, 'before')
    return f, profiles


def _saved(ctx, predicate, what):
    accepted = ctx.wait(lambda: (value if predicate(value['document']) else None)
                        if (value := _get(ctx)) else None, what)
    # An independent disk assertion. Output contains booleans only, never the
    # document, keyring values, user paths or raw input.
    path = accepted['metadata']['path']
    expected = hashlib.sha256(json.dumps(accepted['document'], sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()).hexdigest()
    script = """import hashlib,json,os,stat
from pathlib import Path
p=Path(PATH)
s=p.lstat()
ok=stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid() and not s.st_mode & 0o077
raw=p.read_text()
doc=json.loads(raw)
forbidden={'accessKey','secretKey','sessionToken','password'}
def clean(x):
    if isinstance(x,dict): return not forbidden.intersection(x) and all(clean(v) for v in x.values())
    if isinstance(x,list): return all(clean(v) for v in x)
    return True
print(json.dumps({'matches':ok and hashlib.sha256(json.dumps(doc,sort_keys=True,separators=(',', ':'),ensure_ascii=True).encode()).hexdigest()==EXPECTED,'referencesOnly':clean(doc)}))
""".replace('PATH', repr(path)).replace('EXPECTED', repr(expected))
    disk = ctx.shell('python3 - <<\'PY\'\n' + script + '\nPY\n')
    ctx.expect(disk['returncode'] == 0, 'private config inspected without printing contents')
    flags = json.loads(disk['stdout'])
    ctx.expect(flags == {'matches': True, 'referencesOnly': True}, 'disk matches accepted config and contains references only')
    return accepted['document']


def _wheel(ctx, clicks):
    geometry = ctx.geometry('destinations')
    scale = geometry.get('scale', 1)
    ctx.scroll(round((geometry['x'] - geometry.get('screen_x', 0) + geometry['width'] - 24) * scale),
               round((geometry['y'] - geometry.get('screen_y', 0) + geometry['height']/2) * scale),
               clicks, head=geometry.get('head', 0))


def _click(ctx, control, *, window='destinations'):
    for attempt in range(12 if window == 'destinations' else 1):
        try:
            return ctx.click(control, window=window)
        except RecipeBlocked as error:
            if window != 'destinations' or not any(reason in str(error) for reason in
                    ('0 matches', 'falls outside compositor frame', 'clipped')):
                raise
            _wheel(ctx, 40 if attempt == 0 else -3)
    ctx.blocked('Destination control remains outside its live viewport: ' + control, issue=856)


def _edit(ctx, control, text):
    _click(ctx, control)
    ctx.keys('Ctrl+A')
    ctx.type_text(text)
    ctx.keys('Tab')


def _choose(ctx, control, index):
    _click(ctx, control)
    ctx.keys('Esc', 'Alt+Down', 'Home', *(['Down'] * index), 'Return', 'Tab')


def _selected(ctx, index):
    _click(ctx, 'destinationList')
    ctx.keys('Home', *(['Down'] * index))


def _accessible(ctx, control):
    query = {'owned': True, 'objectName': control}
    rows = ctx.accessible(query)
    if not rows:
        # Qt 6.4 omits exported IDs; use the profile's exact accessible label
        # and role. Geometry and foreground still come from core witnesses.
        alias = ctx.profile.get('live_controls', {}).get(control)
        if alias:
            rows = ctx.accessible({'owned': True, **alias})
    if len(rows) != 1 or not isinstance(rows[0], dict):
        ctx.blocked('Need one current non-secret accessible control: ' + control, issue=856)
    return rows[0]


def _confirmation_capture(ctx, window):
    if not ctx.profile.get('catalog_windows', {}).get(window):
        ctx.blocked('Need an actual destination confirmation capture with a controls inventory (#722).', issue=722)


def _confirmation_frame(ctx, window):
    reference = ctx.profile['catalog_windows'][window][0]
    ctx.frame('confirmation', window=window, reference=reference)


def _state(ctx, control, key):
    if control == 's3BucketError' and key == 'text':
        rows = ctx.accessible({'owned': True, 'objectName': control})
        if not rows:
            description = _accessible(ctx, 's3Bucket').get('description', '')
            return description.split('\n', 1)[1] if '\n' in description else ''
    row = _accessible(ctx, control)
    if key in ('focused', 'checked'):
        row = row.get('states', {})
    if key not in row:
        ctx.blocked('Accessible control lacks required non-secret state: ' + control + '/' + key, issue=CORE_ISSUE)
    return row[key]


@recipe('destinations.add-credentials', needs=NEEDS, destinations=('local',))
@_failure_frame
def add_credentials(ctx):
    _open(ctx)
    ctx.wait(lambda: _state(ctx, 'addDestinationCredentials', 'visible'), 'missing credential action appears')
    _click(ctx, 'addDestinationCredentials', window='destinations')
    ctx.expect(_state(ctx, 'credentialAccessKey', 'focused') is True, 'Add credentials focuses access key entry')
    _saved(ctx, lambda doc: doc['profiles'][0]['credentials'] == {'mode': 'session', 'reference': None}, 'focus action preserves credential reference')
    _frame(ctx, 'after', '18-destination-credentials-compact')


@recipe('destinations.rename', 'destinations.actions-menu', needs=NEEDS, destinations=('local',))
@_failure_frame
def rename(ctx):
    _, profiles = _open(ctx)
    _click(ctx, 'destinationActions', window='destinations')
    _click(ctx, 'text:Rename destination', window='destinationActionsMenu')
    _edit(ctx, 'destinationLabel', 'Renamed surface destination')
    _saved(ctx, lambda doc: doc['profiles'][0]['label'] == 'Renamed surface destination'
           and doc['profiles'][0]['id'] == profiles[0]['id'], 'rename autosaved without changing identity')
    _frame(ctx, 'after')


@recipe('destinations.s3-bucket', 'destinations.s3-region', 'destinations.s3-prefix',
        'destinations.s3-public-url', needs=NEEDS, destinations=('local',))
@_failure_frame
def s3_fields(ctx):
    _, profiles = _open(ctx)
    bucket = ('surface-edited-bucket' if profiles[0]['settings']['bucket'] != 'surface-edited-bucket'
              else 'surface-alternate-bucket')
    values = {'bucket': bucket, 'region': 'us-west-2',
              'objectPrefix': 'surface/typed/', 'publicUrlBase': 'https://surface.invalid/images'}
    for control, key in (('s3Bucket', 'bucket'), ('s3RegionChoices', 'region'),
                         ('s3ObjectPrefix', 'objectPrefix'), ('s3PublicUrlBase', 'publicUrlBase')):
        _edit(ctx, control, values[key])
        _saved(ctx, lambda doc, k=key: doc['profiles'][0]['settings'][k] == values[k], key + ' autosaved')
    _frame(ctx, 'after')


@recipe('destinations.s3-acl', needs=NEEDS, destinations=('local',))
@_failure_frame
def acl(ctx):
    _open(ctx)
    for index, value in ((1, 'public-read'), (0, 'omit')):
        _choose(ctx, 's3AclMode', index)
        _saved(ctx, lambda doc: doc['profiles'][0]['settings']['aclMode'] == value, 'access request autosaved')
    _frame(ctx, 'after')


@recipe('destinations.s3-service', needs=NEEDS, destinations=('local',))
@_failure_frame
def service(ctx):
    _, profiles = _open(ctx)
    # The selector opens endpoint editing; it does not supply an account's
    # endpoint. No test/upload is invoked for these reserved fixture addresses.
    for index, endpoint in ((1, 'https://surface.invalid.r2.cloudflarestorage.com'),
                             (2, profiles[0]['settings']['endpoint'])):
        _choose(ctx, 's3Service', index)
        _click(ctx, 'advancedSettings')
        ctx.use_catalog('destinations', '62-destination-connection-expanded')
        _click(ctx, 'customEndpoint')
        _edit(ctx, 's3Endpoint', endpoint)
        _click(ctx, 'advancedSettings')
        _saved(ctx, lambda doc: doc['profiles'][0]['settings']['endpoint'] == endpoint, 'service endpoint saved')
    _frame(ctx, 'after')


@recipe('destinations.advanced-connection', needs=NEEDS, destinations=('local',))
@_failure_frame
def advanced_connection(ctx):
    _, profiles = _open(ctx)
    _click(ctx, 'advancedSettings', window='destinations')
    ctx.use_catalog('destinations', '62-destination-connection-expanded')
    _edit(ctx, 's3StorageClass', 'STANDARD_IA')
    _saved(ctx, lambda doc: doc['profiles'][0]['settings']['storageClass'] == 'STANDARD_IA', 'storage class saved')
    _click(ctx, 's3StorageClassReset', window='destinations')
    _edit(ctx, 's3ConnectTimeoutMsControl', '7')
    _saved(ctx, lambda doc: doc['profiles'][0]['settings']['connectTimeoutMs'] == 7000, 'seconds stored as milliseconds')
    _click(ctx, 's3ConnectTimeoutMsReset', window='destinations')
    _saved(ctx, lambda doc: doc['profiles'][0]['settings']['storageClass'] == 'STANDARD'
           and doc['profiles'][0]['settings']['connectTimeoutMs'] == 5000
           and doc['profiles'][0]['settings']['endpoint'] == profiles[0]['settings']['endpoint'], 'connection defaults restored')
    _frame(ctx, 'after', '62-destination-connection-expanded')


@recipe('destinations.autosave', needs=NEEDS, destinations=('local',))
@_failure_frame
def autosave_validation(ctx):
    _open(ctx)
    _edit(ctx, 's3Bucket', 'invalid bucket with spaces')
    # An observed inline error makes this an eventual assertion, not a race
    # against the 400 ms autosave timer.
    ctx.wait(lambda: _state(ctx, 's3BucketError', 'text'), 'invalid bucket has an inline error')
    _saved(ctx, lambda doc: doc['profiles'][0]['settings']['bucket'] != 'invalid bucket with spaces', 'invalid edit stayed local')
    _frame(ctx, 'invalid')
    _edit(ctx, 's3Bucket', 'surface-valid-bucket')
    _saved(ctx, lambda doc: doc['profiles'][0]['settings']['bucket'] == 'surface-valid-bucket', 'corrected field autosaved')
    _frame(ctx, 'after')


@recipe('destinations.list', 'destinations.key-move', needs=NEEDS, destinations=('local',))
@_failure_frame
def select_and_key_reorder(ctx):
    _, profiles = _open(ctx, 3)
    _selected(ctx, 1)
    ctx.expect(_state(ctx, 'destinationLabel', 'text') == profiles[1]['label'], 'rail selection changes editor')
    ctx.keys('Alt+Up')
    _saved(ctx, lambda doc: [p['id'] for p in doc['profiles']] ==
           [profiles[i]['id'] for i in (1, 0, 2)], 'Alt+Up persists order')
    ctx.keys('Alt+Down')
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'Alt+Down restores order')
    _frame(ctx, 'after')


@recipe('destinations.drag-reorder', needs=NEEDS, destinations=('local',))
@_failure_frame
def drag_reorder(ctx):
    _, profiles = _open(ctx, 3)
    # Resolve actual rendered row bounds, never scale invented row heights.
    rail = _accessible(ctx, 'destinationList')
    rows = ctx.accessible({'owned': True, 'role': 'list item', 'within': rail['rect'],
                           'visible': True, 'showing': True})
    rows.sort(key=lambda row: row['bounds']['y'])
    if len(rows) != 3 or any('bounds' not in row for row in rows):
        ctx.blocked('Need rendered destination row bounds for QMP drag (#666).', issue=CORE_ISSUE)
    first, last = rows[0]['bounds'], rows[2]['bounds']
    ctx.drag((first['x'] + first['width']//2, first['y'] + first['height']//2),
             (last['x'] + last['width']//2, last['y'] + last['height'] - 2))
    _saved(ctx, lambda doc: [p['id'] for p in doc['profiles']] ==
           [profiles[i]['id'] for i in (1, 2, 0)], 'drag persists stable profile identities')
    _frame(ctx, 'after')


@recipe('destinations.add', 'confirm.add-destination-name', needs=NEEDS, destinations=('local',))
@_failure_frame
def add(ctx):
    _open(ctx)
    _confirmation_capture(ctx, 'confirmDestinationName')
    for menu_label, provider, label in (('Amazon S3 / S3-compatible', 's3', 'Added S3'),
                                        ('Backblaze B2', 'backblaze-b2', 'Added B2')):
        _click(ctx, 'addS3Destination', window='destinations')
        _click(ctx, 'text:' + menu_label, window='destinationAddMenu')
        _confirmation_frame(ctx, 'confirmDestinationName')
        _click(ctx, 'class:QLineEdit', window='confirmDestinationName')
        ctx.keys('Ctrl+A')
        ctx.type_text(label)
        ctx.keys('Return')
        # A new destination stays provisional until its required bucket is valid.
        _edit(ctx, 's3Bucket', 'surface-added-bucket')
        _saved(ctx, lambda doc: any(p['label'] == label and p['provider'] == provider
                                   for p in doc['profiles']), provider + ' added with typed name')
        _frame(ctx, 'added-' + provider)
    _frame(ctx, 'after')


@recipe('destinations.make-default', 'confirm.make-default-untested', needs=NEEDS, destinations=('local',))
@_failure_frame
def make_default(ctx):
    _, profiles = _open(ctx)
    _confirmation_capture(ctx, 'confirmDestinationDefault')
    _click(ctx, 'makeDefaultDestination', window='destinations')
    _confirmation_frame(ctx, 'confirmDestinationDefault')
    # Target the modal itself; foreground guards must never type through it.
    _click(ctx, 'text:Cancel', window='confirmDestinationDefault')
    _saved(ctx, lambda doc: doc['captureDefaults']['destination'] == 'local', 'cancel untested default preserves routing')
    _click(ctx, 'makeDefaultDestination', window='destinations')
    _click(ctx, 'confirmDiscard', window='confirmDestinationDefault')
    _saved(ctx, lambda doc: doc['captureDefaults']['profileId'] == profiles[0]['id']
           and doc['captureDefaults']['destination'] == 'upload', 'confirmed untested default saved')
    _frame(ctx, 'after')


@recipe('destinations.remove', 'confirm.remove-destination', 'destinations.empty', needs=NEEDS, destinations=('local',))
@_failure_frame
def remove(ctx):
    _, profiles = _open(ctx)
    _confirmation_capture(ctx, 'confirmDestinationRemove')
    # Resolve the actual live menu item before requesting its confirmation.
    def request():
        _click(ctx, 'destinationActions', window='destinations')
        _click(ctx, 'text:Remove destination…', window='destinationActionsMenu')
    request()
    _confirmation_frame(ctx, 'confirmDestinationRemove')
    _click(ctx, 'text:Cancel', window='confirmDestinationRemove')
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'cancel removal retains destination')
    request()
    _click(ctx, 'confirmDiscard', window='confirmDestinationRemove')
    _saved(ctx, lambda doc: doc['profiles'] == [], 'confirm removal leaves no profiles')
    ctx.expect(_state(ctx, 'destinationsEmpty', 'visible') is True, 'empty destination state visible')
    _frame(ctx, 'after')


@recipe('destinations.credential-mode', needs=NEEDS, destinations=('local',))
@_failure_frame
def credential_mode(ctx):
    _open(ctx)
    for index, mode in ((0, 'keychain'), (1, 'session')):
        _choose(ctx, 'credentialMode', index)
        _saved(ctx, lambda doc: doc['profiles'][0]['credentials']['mode'] == mode
               and (bool(doc['profiles'][0]['credentials']['reference']) if mode == 'keychain'
                    else doc['profiles'][0]['credentials']['reference'] is None), 'credential reference mode saved')
    _frame(ctx, 'after', '60-credential-storage-replacement')


@recipe('cli.credentials-refused', needs=NEEDS, destinations=('local',))
@_failure_frame
def cli_credentials_refused(ctx):
    _open(ctx)
    for method in ('setup.storeCredentials', 'setup.storeSharePassword', 'setup.revealCredentialSecret'):
        reply = ctx.cli(method, '{}')  # No secret, even in a rejected command.
        ctx.expect(reply['returncode'] != 0 and 'credentials_require_settings' in
                   reply['stdout'] + reply['stderr'], 'CLI refuses credential method before transport')
    _saved(ctx, lambda doc: len(doc['profiles']) == 1, 'CLI refusal preserves config')
    _frame(ctx, 'after')


@recipe('destinations.help-toggles', needs=NEEDS, destinations=('local',))
@_failure_frame
def help_toggles(ctx):
    _open(ctx)
    _click(ctx, 's3AclModeHelp', window='destinations')
    ctx.expect(_state(ctx, 's3AclModeHelp', 'checked') is True, 'bucket explanation expanded')
    _frame(ctx, 'expanded')
    _click(ctx, 's3AclModeHelp', window='destinations')
    ctx.expect(_state(ctx, 's3AclModeHelp', 'checked') is False, 'bucket explanation collapsed')
    _saved(ctx, lambda doc: len(doc['profiles']) == 1, 'help leaves configuration intact')
    _frame(ctx, 'after')


def _synthetic_credentials(ctx, fixture):
    path = PurePosixPath(fixture.get('credentials_file', ''))
    root = PurePosixPath(fixture['config_path']).parents[2]
    if not (fixture.get('loopback') is True and path.is_absolute()
            and path.is_relative_to(root) and path.name == 'fake-input.json'):
        ctx.blocked('Credential entry requires the owned synthetic loopback input file.', issue=666)
    # This output travels only in the core's memory channel. Evidence.command
    # retains lengths/presence; the credential-shaped JSON is registered before
    # returning. No value is interpolated into shell text, argv or notes.
    script = """import json,os,stat,secrets
from pathlib import Path
p=Path(PATH)
s=p.lstat()
assert p.resolve()==p and stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid() and not s.st_mode & 0o077
with os.fdopen(os.open(p,os.O_RDONLY|os.O_NOFOLLOW)) as f:value=json.load(f)
assert set(value)=={'accessKey','secretKey','sessionToken'}
assert value['accessKey'] and value['secretKey']
value['sessionToken']=value['sessionToken'] or secrets.token_hex(16)
print(json.dumps(value))
""".replace('PATH', repr(str(path)))
    reply = ctx.shell("python3 - <<'SD_SYNTHETIC_INPUT'\n" + script + '\nSD_SYNTHETIC_INPUT\n')
    ctx.expect(reply['returncode'] == 0, 'owned synthetic input is available')
    values = json.loads(reply['stdout'])
    return {key: ctx.sensitive(value) for key, value in values.items()}


def _enter_credentials(ctx):
    fixture, profiles = _open(ctx)
    values = _synthetic_credentials(ctx, fixture)
    for control in ('credentialAccessKey', 'credentialSecretKey', 'credentialSessionToken'):
        ctx.sensitive_control(control, 'destinations')
    ctx.use_catalog('destinations', '09-stored-credential-hint')
    for control, key in (('credentialAccessKey', 'accessKey'), ('credentialSecretKey', 'secretKey'),
                         ('credentialSessionToken', 'sessionToken')):
        _click(ctx, control)
        ctx.expect(_state(ctx, control, 'focused') is True, 'synthetic credential field has focus')
        ctx.keys('Ctrl+A')
        ctx.type_text(values[key])
        ctx.keys('Tab')
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'typing secrets preserves reference-only config')
    _frame(ctx, 'entered-masked', '09-stored-credential-hint')
    return profiles


def _hint(ctx, identity):
    return ctx.call('setup.credentialHint', {'profileId': identity,
                    'expectedRevision': _get(ctx)['metadata']['observedRevision']})


@recipe('destinations.credential-fields', needs=NEEDS, destinations=('local',), timeout=180)
@_failure_frame
def credential_fields(ctx):
    profiles = _enter_credentials(ctx)
    ctx.expect(not _hint(ctx, profiles[0]['id'])['available'], 'typing alone does not apply credentials')
    _frame(ctx, 'after', '09-stored-credential-hint')


@recipe('destinations.store-credentials', needs=NEEDS, destinations=('local',), timeout=180)
@_failure_frame
def store_credentials(ctx):
    profiles = _enter_credentials(ctx)
    ctx.expect(not _hint(ctx, profiles[0]['id'])['available'], 'session cache starts empty')
    _click(ctx, 'storeCredentials')
    ctx.wait(lambda: _hint(ctx, profiles[0]['id']).get('available'), 'explicit credential store fills session cache')
    ctx.expect(_hint(ctx, profiles[0]['id']).get('tokenStored') is True, 'optional session token was applied')
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'stored credentials leave only references on disk')
    _frame(ctx, 'after', '09-stored-credential-hint')


@recipe('destinations.show-secret', needs=NEEDS, destinations=('local',))
@_failure_frame
def show_secret(ctx):
    _open(ctx, saved_credentials=True)
    ctx.sensitive_control('credentialSecretKey', 'destinations')
    ctx.use_catalog('destinations', '09-stored-credential-hint')
    _click(ctx, 'text:Show secret access key')
    ctx.wait(lambda: ctx.accessible({'owned': True, 'name': 'Hide secret access key',
                                    'visible': True, 'showing': True}), 'secret reveal toggled on')
    _frame(ctx, 'revealed-masked', '09-stored-credential-hint')
    _click(ctx, 'text:Hide secret access key')
    ctx.wait(lambda: ctx.accessible({'owned': True, 'name': 'Show secret access key',
                                    'visible': True, 'showing': True}), 'secret reveal toggled off')
    _frame(ctx, 'after', '09-stored-credential-hint')


@recipe('destinations.unlock-credentials', needs=NEEDS, destinations=('local',))
@_failure_frame
def unlock_credentials(ctx):
    _, profiles = _open(ctx, saved_credentials=True, fixture='destinations-uncached')
    identity = profiles[0]['id']
    def available():
        return ctx.call('setup.credentialHint', {'profileId': identity,
                        'expectedRevision': _get(ctx)['metadata']['observedRevision']})['available']
    ctx.expect(not available(), 'uncached fixture has no resident credential cache')
    # Keychain profiles can be ready-to-test while still locked, so their
    # header Add credentials action is hidden. Scroll to the real unlock row.
    geometry = ctx.geometry('destinations')
    ctx.scroll(geometry['x'] + geometry['width'] - 32,
               geometry['y'] + geometry['height']//2, -30)
    ctx.use_catalog('destinations', '09-stored-credential-hint')
    _click(ctx, 'text:Unlock saved credentials', window='destinations')
    ctx.wait(available, 'explicit unlock loads the owned fake keyring item')
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'unlock preserves credential references')
    _frame(ctx, 'after', '09-stored-credential-hint')


@recipe('destinations.test', needs=NEEDS, destinations=('local',))
@_failure_frame
def test_connection(ctx):
    # The fixture pre-unlocks the owned reference. That setup is never
    # counted as a GUI unlock or connection-test interaction.
    _, profiles = _open(ctx, saved_credentials=True)
    # Keychain profiles can be ready-to-test while still locked, so their
    # header Add credentials action is hidden. Scroll to the real unlock row.
    geometry = ctx.geometry('destinations')
    ctx.scroll(geometry['x'] + geometry['width'] - 32,
               geometry['y'] + geometry['height']//2, -30)
    ctx.use_catalog('destinations', '09-stored-credential-hint')
    ctx.expect(ctx.call('setup.credentialHint', {'profileId': profiles[0]['id'],
               'expectedRevision': _get(ctx)['metadata']['observedRevision']})['available'],
               'loopback fixture supplied cached fake credentials')
    _click(ctx, 'testDestination', window='destinations')
    ctx.wait(lambda: 'Connection successful.' in _state(ctx, 'destinationStatus', 'text'),
             'loopback test upload, link and cleanup succeeded', timeout=45)
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'test preserves destination settings')
    _frame(ctx, 'after')


@recipe('destinations.conflict', needs=NEEDS, destinations=('local',))
@_failure_frame
def conflict(ctx):
    _, profiles = _open(ctx)
    _confirmation_capture(ctx, 'confirmDestinationReload')
    _edit(ctx, 's3Bucket', 'invalid unsaved draft')
    ctx.wait(lambda: _state(ctx, 's3BucketError', 'text'), 'invalid draft retained')
    _patch(ctx, {'preferences': {'notifications': not _get(ctx)['document']['preferences'].get('notifications', True)}})
    ctx.wait(lambda: _state(ctx, 'destinationConflictBanner', 'visible'), 'external edit raises conflict banner')
    _frame(ctx, 'conflict', '52-destinations-conflict')
    _click(ctx, 'reloadDestinationConflict', window='destinations')
    _confirmation_frame(ctx, 'confirmDestinationReload')
    _click(ctx, 'text:Cancel', window='confirmDestinationReload')
    ctx.expect(_state(ctx, 'destinationConflictBanner', 'visible') is True, 'cancel reload keeps local draft')
    _click(ctx, 'reloadDestinationConflict', window='destinations')
    _click(ctx, 'confirmDiscard', window='confirmDestinationReload')
    ctx.wait(lambda: not _state(ctx, 'destinationConflictBanner', 'visible'), 'confirmed reload clears conflict')
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'reload discards invalid draft and preserves accepted settings')
    _frame(ctx, 'after')
