"""Guest-local feeds and reversible OS-event controllers."""
import json
from pathlib import Path
import shlex

from ..api import RecipeBlocked
from .base import Fixture
from .destinations import control
from .uploads import submit


def feed(scope, older=False):
    # Retain the Python helper entry point as well as the fixture registry name.
    from .updates import feed as update_feed
    return update_feed(scope,older=older)


def storage(scope):
    ctx=scope.ctx
    ctx.fixture('app-profile')
    if ctx.placement not in ('guest','boat') or (ctx.host=='boat' and not ctx.profile.get('guest')):
        raise RecipeBlocked('quota filesystem requires a disposable nested guest',issue=682)
    from .privileges import ensure
    ensure(scope)
    if ctx.shell('sudo -n /usr/bin/mount --version >/dev/null 2>&1')['returncode']:
        raise RecipeBlocked('guest needs a noninteractive privileged mount helper',issue=682)
    path=ctx.target.root+'/data/quota-'+scope.token[:8]
    scope.python('from pathlib import Path\nPath('+repr(path)+').mkdir(mode=0o700)')
    scope.defer('quota directory',lambda:scope.python('import os\nfrom pathlib import Path\np=Path('+repr(path)+')\nif p.exists():\n assert p.resolve()==p and p.stat().st_uid==os.getuid()\n p.rmdir()'),priority=-300)
    def unmount():
        # Stop the owned resident before unmounting its staging filesystem.
        scope.restart() # retire processes using the mounted data directory
        reply=ctx.shell('if mountpoint -q -- '+shlex.quote(path)+'; then sudo -n /usr/bin/umount -- '+shlex.quote(path)+'; fi')
        if reply['returncode']:raise RecipeBlocked('owned tmpfs remains mounted')
    scope.defer('quota tmpfs',unmount)
    result=ctx.shell('sudo -n /usr/bin/mount -t tmpfs -o size=1792m,mode=0700,uid=$(id -u),gid=$(id -g) tmpfs '+shlex.quote(path))
    if result['returncode']:raise RecipeBlocked('guest cannot create private bounded tmpfs',issue=682)
    # ArtifactStore uses data/putgd; mount only that owned subdirectory and route
    # its app data there while preserving the same session buses and runtime.
    scope.restart({'XDG_DATA_HOME':path,'PUTGD_RECORD_KEEP_WORKING':scope.root+'/working'})
    return Fixture(mountPath=path,quotaBytes=1792*1024**2)


def slow_upload(scope, *, destination=None):
    destination=destination or scope.ctx.destination
    if destination not in ('local','s3'):
        raise RecipeBlocked('slow upload fixture supports local/loopback S3 only',issue=669)
    row=submit(scope,remote=True,active=True)
    service=scope.ctx.fixture('loopback-s3')
    control(scope,stallSeconds=20)
    return Fixture(row,destination=destination,address='127.0.0.1',port=service.port,timeoutSeconds=60)


def incomplete(scope):
    from .uploads import incomplete_multipart
    return incomplete_multipart(scope)


def clipboard(scope):
    ctx=scope.ctx
    if ctx.placement not in ('guest','boat') or (ctx.host=='boat' and not ctx.profile.get('guest')):
        raise RecipeBlocked('clipboard fixture requires a disposable nested guest',issue=682)
    text='putgd synthetic clipboard '+scope.token[:8]
    scope.initialize()
    path=scope.root+'/clipboard-text'
    scope.write(path,text.encode())
    from .clipboard import owner
    process=owner(scope,path,'text/plain',name='text-clipboard')
    return Fixture(text=text,synthetic=True,process=process)


def controllers(scope):
    ctx=scope.ctx
    if ctx.placement not in ('boat','guest') or (ctx.host=='boat' and not ctx.profile.get('guest')):
        raise RecipeBlocked('disruptive controllers require a disposable guest',issue=682)
    def stall(address,port):
        if address!='127.0.0.1' or not isinstance(port,int) or not 0<port<65536:
            raise RecipeBlocked('network controller accepts only literal loopback fixture endpoints')
        from .privileges import ensure
        ensure(scope)
        if ctx.shell('sudo -n /usr/sbin/nft --version >/dev/null 2>&1')['returncode']:
            raise RecipeBlocked('guest needs a noninteractive privileged nft helper',issue=682)
        table='sd_'+scope.token[:12]
        # Register before mutation: termination after nft commits must still
        # remove exactly this table. An absent owned table is already clean.
        def remove_table():
            exists=ctx.shell('sudo -n /usr/sbin/nft list table inet '+table+' >/dev/null 2>&1')
            if exists['returncode']==0:
                checked(ctx.shell('sudo -n /usr/sbin/nft delete table inet '+table))
            else:
                checked(ctx.shell('sudo -n /usr/sbin/nft list tables >/dev/null'))
        scope.defer('owned nft table',remove_table,priority=250)
        result=ctx.shell("sudo -n /usr/sbin/nft -f - <<'SD_NFT'\ncreate table inet "+table+'\nadd chain inet '+table+' output { type filter hook output priority -10; policy accept; }\nadd rule inet '+table+' output ip daddr 127.0.0.1 tcp dport '+str(port)+' counter drop\nSD_NFT\n')
        if result['returncode']:raise RecipeBlocked('guest nft controller unavailable',issue=682)
        return table
    def display(spec):
        from tools.lab import lab
        target=ctx.target
        if target.physical:
            raise RecipeBlocked('QMP display controller requires a guest transport',issue=682)
        backend=lab.layout_backend(lab.load_profile(target.info['profile']))
        observed=lab.guest_outputs(target.run,backend)
        original=lab.format_layout([{k:o[k] for k in ('x','y','width','height')} for o in observed['outputs'] if o.get('enabled',True)])
        def restore():
            if not lab.layout(target.run,original,settle=10)['ok']:
                raise RecipeBlocked('guest display topology did not restore',issue=682)
        scope.defer('display topology',restore,priority=250)
        changed=lab.layout(target.run,spec,settle=10)
        if not changed['ok']:
            raise RecipeBlocked('guest QMP/native display mutation was not verified',issue=682)
        ctx._screen_dimensions.clear()
        return changed
    def relogin():
        raise RecipeBlocked('re-login requires core-owned visible console and protected login input; no ui.quit substitution',issue=682)
    return Fixture(network_stall=stall,display_change=display,relogin=relogin,config_change=lambda patch:config_change(scope,patch),config_path=ctx.target.root+'/config/putgd/config.json')


def checked(reply):
    if reply['returncode']:raise RecipeBlocked('OS-event cleanup failed')


def config_change(scope, patch):
    """Atomic external edit, with a revision/QLockFile guard, for conflict tests."""
    ctx=scope.ctx
    ctx.fixture('app-profile')
    current=ctx.call('settings.get')
    path=current['metadata']['path']
    code=r"""import hashlib,json,os,socket,tempfile
from pathlib import Path
p=Path(PATH)
assert p.resolve()==p and p.is_relative_to(Path(os.environ['XDG_CONFIG_HOME']).resolve())
lock=Path(str(p)+'.lock')
fd=os.open(lock,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
try:
 os.write(fd,('%d\npython3\n%s\n'%(os.getpid(),socket.gethostname())).encode())
 raw=p.read_bytes();assert hashlib.sha256(raw).hexdigest()==REVISION
 doc=json.loads(raw)
 def merge(a,b):
  for k,v in b.items():
   if isinstance(v,dict) and isinstance(a.get(k),dict):merge(a[k],v)
   else:a[k]=v
 merge(doc,PATCH)
 tmp,name=tempfile.mkstemp(dir=p.parent,prefix='.fixture-config-')
 try:
  with os.fdopen(tmp,'w') as out:json.dump(doc,out);out.flush();os.fsync(out.fileno())
  os.replace(name,p)
 finally:
  if os.path.exists(name):os.unlink(name)
finally:
 os.close(fd);lock.unlink()
"""
    scope.python(code.replace('PATH',repr(path)).replace('REVISION',repr(current['metadata']['observedRevision'])).replace('PATCH',repr(patch)))
    return Fixture(config_path=path,previous_revision=current['metadata']['observedRevision'])


def conflict(scope):
    profile=scope.ctx.fixture('app-profile')
    return Fixture(profile,external_change=lambda patch:config_change(scope,patch))
