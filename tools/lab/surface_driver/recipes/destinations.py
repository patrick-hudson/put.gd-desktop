"""Destinations real-input recipes; no live provider accounts.

Uses the core's owned guest loopback server and fake keyring fixture. No live
provider accounts or credential values enter this lane. Entry/reveal stay
blocked until the core supplies secret-safe input and capture suppression.
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


def _fixture(ctx):
    f = ctx.fixture('destinations')
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

    These recipes never enter or reveal credentials. A later credential recipe
    must suppress capture throughout any reveal instead of using this wrapper.
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
            if isinstance(error, RecipeBlocked) and error.issue is None and str(error).startswith("control:"):
                raise RecipeBlocked(str(error) + " (#712)", issue=712) from error
            raise
    return capture


def _open(ctx, count=1, *, saved_credentials=False):
    f, p = _fixture(ctx)
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
                 'preferences': {'unlockCredentialsAtStartup': False}})
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


def _edit(ctx, control, text):
    ctx.click(control, window='destinations')
    ctx.keys('Ctrl+A')
    ctx.type_text(text)
    ctx.keys('Tab')


def _choose(ctx, control, index):
    ctx.click(control, window='destinations')
    ctx.keys('Home', *(['Down'] * index), 'Return', 'Tab')


def _selected(ctx, index):
    ctx.click('destinationList', window='destinations')
    ctx.keys('Home', *(['Down'] * index))


def _accessible(ctx, control):
    # The merged API uses exported objectName and explicit ownership, not the
    # old lane-specific window/control pseudo query. Never read secret text.
    rows = ctx.accessible({'owned': True, 'objectName': control})
    if len(rows) != 1 or not isinstance(rows[0], dict):
        ctx.blocked('Need one current non-secret accessible control: ' + control, issue=CORE_ISSUE)
    return rows[0]


def _confirmation_capture(ctx, window):
    if not ctx.profile.get('catalog_windows', {}).get(window):
        ctx.blocked('Need an actual destination confirmation capture with a controls inventory (#722).', issue=722)


def _state(ctx, control, key):
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
    ctx.click('addDestinationCredentials', window='destinations')
    ctx.expect(_state(ctx, 'credentialAccessKey', 'focused') is True, 'Add credentials focuses access key entry')
    _saved(ctx, lambda doc: doc['profiles'][0]['credentials'] == {'mode': 'session', 'reference': None}, 'focus action preserves credential reference')
    _frame(ctx, 'after', '18-destination-credentials-compact')


@recipe('destinations.rename', 'destinations.actions-menu', needs=NEEDS, destinations=('local',))
@_failure_frame
def rename(ctx):
    _, profiles = _open(ctx)
    ctx.click('destinationActions', window='destinations')
    ctx.click('text:Rename destination', window='destinationActionsMenu')
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
        _edit(ctx, 's3Endpoint', endpoint)
        _saved(ctx, lambda doc: doc['profiles'][0]['settings']['endpoint'] == endpoint, 'service endpoint saved')
    _frame(ctx, 'after')


@recipe('destinations.advanced-connection', needs=NEEDS, destinations=('local',))
@_failure_frame
def advanced_connection(ctx):
    _, profiles = _open(ctx)
    ctx.click('advancedSettings', window='destinations')
    ctx.use_catalog('destinations', '62-destination-connection-expanded')
    _edit(ctx, 's3StorageClass', 'STANDARD_IA')
    _saved(ctx, lambda doc: doc['profiles'][0]['settings']['storageClass'] == 'STANDARD_IA', 'storage class saved')
    ctx.click('s3StorageClassReset', window='destinations')
    _edit(ctx, 's3ConnectTimeoutMs', '7')
    _saved(ctx, lambda doc: doc['profiles'][0]['settings']['connectTimeoutMs'] == 7000, 'seconds stored as milliseconds')
    ctx.click('s3ConnectTimeoutMsReset', window='destinations')
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
    for menu_label, provider, label in (('Amazon S3 / S3-compatible', 's3', 'Added S3'),
                                        ('Backblaze B2', 'backblaze-b2', 'Added B2')):
        ctx.click('addS3Destination', window='destinations')
        ctx.click('text:' + menu_label, window='destinationAddMenu')
        ctx.click('class:QLineEdit', window='confirmDestinationName')
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
    ctx.click('makeDefaultDestination', window='destinations')
    # Target the modal itself; foreground guards must never type through it.
    ctx.click('text:Cancel', window='confirmDestinationDefault')
    _saved(ctx, lambda doc: doc['captureDefaults']['destination'] == 'local', 'cancel untested default preserves routing')
    ctx.click('makeDefaultDestination', window='destinations')
    ctx.click('confirmDiscard', window='confirmDestinationDefault')
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
        ctx.click('destinationActions', window='destinations')
        ctx.click('text:Remove destination…', window='destinationActionsMenu')
    request()
    ctx.click('text:Cancel', window='confirmDestinationRemove')
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'cancel removal retains destination')
    request()
    ctx.click('confirmDiscard', window='confirmDestinationRemove')
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
    ctx.click('s3BucketHelp', window='destinations')
    ctx.expect(_state(ctx, 's3BucketHelp', 'checked') is True, 'bucket explanation expanded')
    _frame(ctx, 'expanded')
    ctx.click('s3BucketHelp', window='destinations')
    ctx.expect(_state(ctx, 's3BucketHelp', 'checked') is False, 'bucket explanation collapsed')
    _saved(ctx, lambda doc: len(doc['profiles']) == 1, 'help leaves configuration intact')
    _frame(ctx, 'after')


@recipe('destinations.credential-fields', 'destinations.store-credentials',
        'destinations.show-secret', needs=NEEDS, destinations=('local',))
@_failure_frame
def credential_lifecycle(ctx):
    _open(ctx)
    ctx.blocked('Fake keyring fixture is available; credential typing and reveal require '
                'secret-safe input and frame suppression (#666, #726).', issue=666)


@recipe('destinations.unlock-credentials', needs=NEEDS, destinations=('local',))
@_failure_frame
def unlock_credentials(ctx):
    _, profiles = _open(ctx, saved_credentials=True)
    identity = profiles[0]['id']
    def available():
        return ctx.call('setup.credentialHint', {'profileId': identity,
                        'expectedRevision': _get(ctx)['metadata']['observedRevision']})['available']
    ctx.expect(not available(), 'new profile identity starts with no cached credentials')
    ctx.click('addDestinationCredentials', window='destinations')
    ctx.use_catalog('destinations', '09-stored-credential-hint')
    ctx.click('text:Unlock saved credentials', window='destinations')
    ctx.wait(available, 'explicit unlock loads the owned fake keyring item')
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'unlock preserves credential references')
    _frame(ctx, 'after', '09-stored-credential-hint')


@recipe('destinations.test', needs=NEEDS, destinations=('local',))
@_failure_frame
def test_connection(ctx):
    # Unique profile identity exercises the GUI unlock before a real loopback
    # test; fixture setup is never counted as the tested interaction.
    _, profiles = _open(ctx, saved_credentials=True)
    ctx.click('addDestinationCredentials', window='destinations')
    ctx.use_catalog('destinations', '09-stored-credential-hint')
    ctx.click('text:Unlock saved credentials', window='destinations')
    ctx.wait(lambda: ctx.call('setup.credentialHint', {'profileId': profiles[0]['id'],
                     'expectedRevision': _get(ctx)['metadata']['observedRevision']})['available'],
             'fake credentials unlocked')
    ctx.click('testDestination', window='destinations')
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
    ctx.click('reloadDestinationConflict', window='destinations')
    ctx.click('text:Cancel', window='confirmDestinationReload')
    ctx.expect(_state(ctx, 'destinationConflictBanner', 'visible') is True, 'cancel reload keeps local draft')
    ctx.click('reloadDestinationConflict', window='destinations')
    ctx.click('confirmDiscard', window='confirmDestinationReload')
    ctx.wait(lambda: not _state(ctx, 'destinationConflictBanner', 'visible'), 'confirmed reload clears conflict')
    _saved(ctx, lambda doc: doc['profiles'] == profiles, 'reload discards invalid draft and preserves accepted settings')
    _frame(ctx, 'after')
