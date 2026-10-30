"""Guest-only Secret Service for synthetic KDE loopback credentials.

No default KWallet, alias, real item or desktop identity is changed. An existing
ksecretd keeps its name and serves one unique disposable blank wallet. Otherwise
the distro GNOME daemon uses private storage and an exact process cleanup lease.
"""
from ..api import RecipeBlocked

OWNER = '''import json
from pathlib import Path
import gi
gi.require_version('Gio','2.0')
from gi.repository import Gio,GLib
bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
def call(method,signature,args,reply):
 return bus.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus',method,
  GLib.Variant(signature,args),GLib.VariantType(reply),Gio.DBusCallFlags.NONE,3000,None).unpack()[0]
name='org.freedesktop.secrets'
owner=call('GetNameOwner','(s)',(name,),'(s)') if call('NameHasOwner','(s)',(name,),'(b)') else None
pid=call('GetConnectionUnixProcessID','(s)',(owner,),'(u)') if owner else None
provider=Path('/proc/%d/comm'%pid).read_text().strip() if pid else None
print(json.dumps({'owner':owner,'pid':pid,'provider':provider}))
'''


def isolated_service(scope):
    ctx=scope.ctx
    if ctx.placement not in ('guest','boat') or not ctx.profile.get('guest'):
        raise RecipeBlocked('isolated Secret Service requires a disposable nested guest',issue=919)
    owner = scope.json(OWNER)
    if owner['owner']:
        # Aurora already runs KWallet's Secret Service. Never replace it: a
        # unique blank-password wallet can hold this guest's synthetic item.
        if owner.get('provider') != 'ksecretd':
            raise RecipeBlocked('isolated Secret Service refuses an already owned bus name; existing wallets are preserved',issue=919)
        scope.restart({'QTKEYCHAIN_BACKEND':'libsecret','KDE_SESSION_VERSION':''})
        return dict(owner, provider='ksecretd')
    if ctx.shell('command -v gnome-keyring-daemon >/dev/null')['returncode']:
        raise RecipeBlocked('KDE fixture needs profiles/provision-probes.sh --keyring-overlay in the disposable overlay',issue=919)
    scope.initialize()
    root=scope.root+'/secret-service'
    from ..target import checked_socket
    checked_socket(root+'/control/control')
    scope.python('from pathlib import Path\n'+
                 '\n'.join('Path('+repr(root+'/'+part)+').mkdir(mode=0o700,parents=True,exist_ok=False)'
                           for part in ('config','data','cache','control')))
    identity=scope.launch(['gnome-keyring-daemon','--foreground','--components=secrets','--control-directory='+root+'/control'],
                          environment={'XDG_CONFIG_HOME':root+'/config','XDG_DATA_HOME':root+'/data',
                                       'XDG_CACHE_HOME':root+'/cache'},name='secret-service')
    def ready():
        owner=scope.json(OWNER)
        if owner['owner'] and owner['pid']!=identity['pid']:
            raise RecipeBlocked('Secret Service name was acquired by another process; no collection was created',issue=919)
        return owner['pid']==identity['pid']
    ctx.wait(ready,'owned isolated Secret Service',timeout=10)
    # QtKeychain 0.15 has no QTKEYCHAIN_BACKEND override. Its unknown-version
    # branch chooses libsecret. This hint belongs only to the private resident:
    # XDG_CURRENT_DESKTOP=KDE and the real Wayland/KWin adapter remain intact.
    scope.restart({'QTKEYCHAIN_BACKEND':'libsecret','KDE_SESSION_VERSION':''})
    return identity


KWALLET_COLLECTION = '''
import hashlib,pwd,time
wallet=LABEL
assert wallet not in call('/ksecretd','org.kde.KWallet','wallets',None,'(as)')[0]
folder=Path(pwd.getpwuid(os.getuid()).pw_dir)/'.local/share/kwalletd'
assert folder.resolve()==folder and folder.is_dir()
salt_path=folder/(wallet+'.salt')
with open(os.open(salt_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as f:f.write(os.urandom(56))
digest=hashlib.pbkdf2_hmac('sha512',b'',salt_path.read_bytes(),50000,dklen=56)
call('/ksecretd','org.kde.KWallet','pamOpen',GLib.Variant('(sayi)',(wallet,digest,0)),'()')
collection='/org/freedesktop/secrets/collection/'+wallet
deadline=time.monotonic()+15
while collection not in call(service,'org.freedesktop.DBus.Properties','Get',GLib.Variant('(ss)',('org.freedesktop.Secret.Service','Collections')),'(v)')[0]:
 assert time.monotonic()<deadline,'synthetic KWallet did not appear'
 time.sleep(.2)
assert not call(collection,'org.freedesktop.DBus.Properties','Get',GLib.Variant('(ss)',('org.freedesktop.Secret.Collection','Locked')),'(v)')[0]
'''
