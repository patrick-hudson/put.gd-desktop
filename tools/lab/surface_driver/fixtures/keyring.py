"""Guest-only Secret Service for synthetic KDE loopback credentials.

No default KWallet, alias, real item or desktop identity is changed. An existing
ksecretd keeps its name and all its collections. The distro GNOME daemon uses
private storage, a pinned unique bus connection and an exact cleanup lease.
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
    prior = scope.json(OWNER)
    if prior['owner'] and prior.get('provider') != 'ksecretd':
        raise RecipeBlocked('isolated Secret Service refuses an already owned bus name; existing wallets are preserved',issue=919)
    if ctx.shell('command -v gnome-keyring-daemon >/dev/null')['returncode']:
        raise RecipeBlocked('KDE fixture needs profiles/provision-probes.sh --keyring-overlay in the disposable overlay',issue=919)
    scope.initialize()
    if prior['owner']:
        def preserved():
            if scope.json(OWNER) != prior:
                raise RecipeBlocked('existing Secret Service owner changed; no existing wallet is modified',issue=955)
        # After our daemon exits, before the private scope is removed.
        scope.defer('preserved desktop Secret Service', preserved, priority=-100)
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
        if prior['owner']:
            if owner != prior:
                raise RecipeBlocked('desktop Secret Service owner changed; fixture refuses to proceed',issue=955)
            # Modern ksecretd intentionally exports no pamOpen wallet API.
            # GNOME's isolated daemon still exports its service on its unique
            # connection when the well-known name is held. libsecret supports
            # SECRET_SERVICE_BUS_NAME; bind only this resident to that exact,
            # authenticated owned PID. The desktop keeps its real service/bus.
            names=scope.json(UNIQUE_SERVICE.replace('IDENTITY',repr(identity)))
            if len(names)>1:
                raise RecipeBlocked('isolated Secret Service connection is ambiguous',issue=955)
            return names[0] if names else None
        if owner['owner'] and owner['pid']!=identity['pid']:
            raise RecipeBlocked('Secret Service name was acquired by another process; no collection was created',issue=919)
        return 'org.freedesktop.secrets' if owner['pid']==identity['pid'] else None
    bus_name=ctx.wait(ready,'owned isolated Secret Service',timeout=10)
    environment={'QTKEYCHAIN_BACKEND':'libsecret','KDE_SESSION_VERSION':''}
    if prior['owner']:
        environment['SECRET_SERVICE_BUS_NAME']=bus_name
    scope.restart(environment)
    return dict(identity,bus_name=bus_name)


UNIQUE_SERVICE = '''import json,os
from pathlib import Path
from gi.repository import Gio,GLib
identity=IDENTITY
p=Path('/proc')/str(identity['pid'])
assert p.stat().st_uid==os.getuid()
assert (p/'stat').read_text().rsplit(')',1)[1].split()[19]==identity['start']
bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
def call(dest,path,iface,method,params,reply):
 return bus.call_sync(dest,path,iface,method,params,GLib.VariantType(reply),Gio.DBusCallFlags.NO_AUTO_START,1000,None).unpack()[0]
names=call('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','ListNames',None,'(as)')
found=[]
for name in names:
 if not name.startswith(':'):continue
 try:
  pid=call('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','GetConnectionUnixProcessID',GLib.Variant('(s)',(name,)),'(u)')
  if pid!=identity['pid']:continue
  call(name,'/org/freedesktop/secrets','org.freedesktop.DBus.Properties','Get',GLib.Variant('(ss)',('org.freedesktop.Secret.Service','Collections')),'(v)')
  found.append(name)
 except GLib.Error:pass
print(json.dumps(found))
'''
