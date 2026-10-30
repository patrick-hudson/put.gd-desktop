"""Loopback-only destination profiles and an owned blank-password collection.

Uses the GNOME Secret Service calls already used by lab_keyring.py, without
moving the desktop's default alias or reading any existing item.
"""
import json
from pathlib import Path

from ..api import RecipeBlocked
from .base import Fixture

# Query/return metadata only. Fake material is generated/read solely on target.
KEYRING = '''import base64,json,os,secrets
from pathlib import Path
import gi
gi.require_version('Gio','2.0')
from gi.repository import Gio,GLib
bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
def call(path,iface,method,params,reply):
 return bus.call_sync('org.freedesktop.secrets',path,iface,method,params,GLib.VariantType(reply),Gio.DBusCallFlags.NONE,20000,None).unpack()
service='/org/freedesktop/secrets'
'''


def fake_keyring(scope):
    ctx=scope.ctx
    if ctx.placement not in ('guest','boat') or (ctx.host=='boat' and not ctx.profile.get('guest')):
        raise RecipeBlocked('fake credential collection is restricted to a disposable nested guest',issue=666)
    scope.initialize()
    reference='putgd-lab-'+str(__import__('uuid').uuid4())
    path=scope.root+'/fake-input.json'
    scope.defer('fake credential input',lambda:scope.python('from pathlib import Path\nPath('+repr(path)+').unlink(missing_ok=True)'),priority=1000)
    # No default-alias mutation: QtKeychain's libsecret lookup searches the
    # unique reference in unlocked collections. Each item is explicitly owned.
    created=scope.json(KEYRING+'''
session=call(service,'org.freedesktop.Secret.Service','OpenSession',GLib.Variant('(sv)',('plain',GLib.Variant('s',''))),'(vo)')[1]
collection=call(service,'org.gnome.keyring.InternalUnsupportedGuiltRiddenInterface','CreateWithMasterPassword',
 GLib.Variant('(a{sv}(oayays))',({'org.freedesktop.Secret.Collection.Label':GLib.Variant('s',LABEL)},(session,b'',b'','text/plain'))),'(o)')[0]
try:
 value={'accessKey':'SYNTHETIC'+secrets.token_hex(12),'secretKey':'synthetic-'+secrets.token_hex(24),'sessionToken':''}
 with open(os.open(PATH,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'w') as f:json.dump(value,f)
 attrs={'xdg:schema':'org.qt.keychain','user':REFERENCE,'server':'putgd','type':'base64'}
 secret=base64.b64encode(json.dumps(value,separators=(',',':')).encode())
 item,prompt=call(collection,'org.freedesktop.Secret.Collection','CreateItem',
  GLib.Variant('(a{sv}(oayays)b)',({'org.freedesktop.Secret.Item.Label':GLib.Variant('s',LABEL),
  'org.freedesktop.Secret.Item.Attributes':GLib.Variant('a{ss}',attrs)},(session,b'',secret,'text/plain'),False)),'(oo)')
 assert prompt=='/'
except BaseException:
 call(collection,'org.freedesktop.Secret.Collection','Delete',None,'(o)')
 raise
print(json.dumps({'collection':collection,'reference':REFERENCE}))
'''.replace('LABEL',repr('putgd fixture '+scope.token)).replace('PATH',repr(path)).replace('REFERENCE',repr(reference)))
    scope.defer('fake keyring collection',lambda:scope.python(KEYRING+'''
p=COLLECTION
label=call(p,'org.freedesktop.DBus.Properties','Get',GLib.Variant('(ss)',('org.freedesktop.Secret.Collection','Label')),'(v)')[0]
assert label==LABEL
prompt=call(p,'org.freedesktop.Secret.Collection','Delete',None,'(o)')[0]
assert prompt=='/'
'''.replace('COLLECTION',repr(created['collection'])).replace('LABEL',repr('putgd fixture '+scope.token))))
    return Fixture(reference=reference,collection=created['collection'],credentials_file=path,blank_password=True,private=True)


def loopback(scope):
    scope.ctx.fixture('app-profile')
    scope.initialize()
    scope.write(scope.root+'/control.json',b'{}')
    scope.write(scope.root+'/s3_server.py',Path(__file__).with_name('s3_server.py').read_bytes())
    process=scope.launch(['python3',scope.root+'/s3_server.py',scope.root],name='s3')
    def port():
        result=scope.ctx.shell('cat '+__import__('shlex').quote(scope.root+'/port')+' 2>/dev/null')
        return int(result['stdout']) if result['returncode']==0 and result['stdout'].strip().isdigit() else None
    port=scope.ctx.wait(port,'guest loopback S3 listener',timeout=10)
    endpoint='http://127.0.0.1:'+str(port)
    keyring=scope.ctx.fixture('fake-keyring')
    profile_id='surface-'+scope.token[:12]
    profile={'id':profile_id,'label':'Synthetic loopback S3','provider':'s3',
             'credentials':{'mode':'keychain','reference':keyring.reference},
             'settings':{'bucket':'surface-fixture','region':'us-east-1','endpoint':endpoint,
                         'objectPrefix':'campaign-'+scope.token[:12]+'/', 'storageClass':'STANDARD','aclMode':'public-read',
                         'publicUrlBase':endpoint+'/surface-fixture','addressing':'path','connectTimeoutMs':1000,
                         'requestTimeoutMs':30000,'lowSpeedBytesPerSecond':1,'lowSpeedTimeSeconds':25}}
    scope.patch({'profiles':[profile],'archive':{'keepUploads':True}})
    current=scope.ctx.call('settings.get')
    job=scope.ctx.call('setup.unlockCredentials',{'profileId':profile_id,'expectedRevision':current['metadata']['observedRevision']})['setupId']
    result=scope.ctx.wait(lambda:(s if (s:=scope.ctx.call('setup.inspect',{'id':job}))['state']!='running' else None),'fake credential unlock',timeout=30)
    if result['state']!='completed':
        raise RecipeBlocked('owned fake credential collection was not accepted by QtKeychain',issue=666)
    return Fixture(private=True,isolated=True,loopback=True,address='127.0.0.1',port=port,endpoint=endpoint,
                   profile=profile,profile_id=profile_id,profileId=profile_id,config_path=scope.ctx.target.root+'/config/putgd/config.json',
                   credentials_file=keyring.credentials_file,keyring=keyring,process=process)


def control(scope, **values):
    scope.python('''import json,os,tempfile
from pathlib import Path
p=Path(ROOT)/'control.json'
fd,name=tempfile.mkstemp(dir=p.parent,prefix='.ctl-')
with os.fdopen(fd,'w') as f:json.dump(VALUES,f)
os.replace(name,p)
'''.replace('ROOT',repr(scope.root)).replace('VALUES',repr(values)))


def destinations(scope, kind='s3'):
    if kind=='empty':
        profile=scope.ctx.fixture('app-profile')
        scope.patch({'profiles':[],'captureDefaults':{'destination':'local','profileId':None,'publicationMode':'default'}})
        return Fixture(private=True,config_path=profile.config_path,profiles=[])
    fixture=scope.ctx.fixture('loopback-s3')
    profile=json.loads(json.dumps(fixture.profile))
    # B2/generic are configuration fixtures only; never contact their APIs.
    if kind=='b2':
        profile['provider']='backblaze-b2'
        # B2 accepts HTTPS settings only. This template has no B2/TLS service;
        # uploads remain guarded to the S3 adapter above.
        endpoint='https://127.0.0.1:'+str(fixture.port)
        profile['settings']={'apiUrl':endpoint,'bucket':'surface-fixture','objectPrefix':'campaign-'+scope.token[:12],
                             'linkMode':'public','linkValidityDays':7,'publicUrlBase':endpoint+'/surface-fixture',
                             'connectTimeoutMs':1000,'requestTimeoutMs':15000}
    return Fixture(fixture,profile=profile,kind=kind,configuration_only=kind=='b2')


def destination(scope):
    if scope.ctx.destination=='local':
        scope.ctx.fixture('app-profile')
        return Fixture(id='local',label='Save locally')
    if scope.ctx.destination!='s3':
        raise RecipeBlocked('recording destination adapter is local/loopback S3 only in this slice',issue=669)
    value=scope.ctx.fixture('loopback-s3')
    scope.patch({'actions':{'record':{'destination':'upload','profileId':value.profile_id}}})
    return Fixture(id=value.profile_id,label=value.profile['label'])


def locked_keyring(scope):
    """Lock only the unique synthetic collection; never change a default alias."""
    value=scope.ctx.fixture('fake-keyring')
    def lock():
        scope.python(KEYRING+'''
locked,prompt=call(service,'org.freedesktop.Secret.Service','Lock',GLib.Variant('(ao)',([COLLECTION],)),'(aoo)')
assert COLLECTION in locked and prompt=='/'
'''.replace('COLLECTION',repr(value.collection)))
    def unlock():
        scope.python(KEYRING+'''
unlocked,prompt=call(service,'org.freedesktop.Secret.Service','Unlock',GLib.Variant('(ao)',([COLLECTION],)),'(aoo)')
assert prompt=='/' and COLLECTION in unlocked
'''.replace('COLLECTION',repr(value.collection)))
    scope.defer('unlock owned blank-password collection',unlock,priority=60)
    lock()
    return Fixture(value,locked=True,lock=lock,unlock=unlock)
