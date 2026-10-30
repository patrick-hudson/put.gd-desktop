"""Private accepted configuration and explicit first-launch setup."""
import copy
import json
from pathlib import Path

from ..api import RecipeBlocked
from .base import Fixture


def seeded_config(root):
    # Reuse the canonical complete config shape; replace all provider references.
    path = Path(__file__).resolve().parents[4]/'config/development.example.json'
    doc = json.loads(path.read_text())
    doc['profiles'] = []
    doc['actions'] = {name: {'destination': 'local', 'profileId': None, 'shortcut': value['shortcut']}
                      for name, value in doc['actions'].items()}
    doc['actions']['edit'] = {'destination':'local', 'profileId':None, 'shortcut':''}
    doc['archive'].update(root=root+'/files', keepUploads=True)
    doc['preferences'].update(launchAtLogin=False, unlockCredentialsAtStartup=False)
    return doc


def seed_profile(target, *, first_launch=False):
    """2a startup hook: call after private mkdir, BEFORE spawning the resident.

    Does not create a config for first_launch=True. Never overwrites an existing
    file or a user profile; Target owns the new root and the socket preflight.
    """
    import shlex
    code='''import json,os,stat
from pathlib import Path
root=Path(ROOT)
s=root.lstat()
assert root.resolve()==root and stat.S_ISDIR(s.st_mode) and s.st_uid==os.getuid() and not s.st_mode & 0o077
p=root/'config/putgd/config.json'
assert not p.exists()
if not FIRST:
 p.parent.mkdir(mode=0o700)
 with open(os.open(p,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'w') as f:json.dump(DOC,f)
'''.replace('ROOT',repr(target.root)).replace('FIRST',repr(first_launch)).replace('DOC',repr(seeded_config(target.root)))
    result=target.execute('python3 -c '+shlex.quote(code))
    if result['returncode']:
        raise RecipeBlocked('cannot seed new private app profile')


def app_profile(scope, *, first_launch=False):
    scope.initialize()
    ctx=scope.ctx
    current=ctx.call('settings.get')
    expected=ctx.target.root+'/config/putgd/config.json'
    if current['metadata']['path']!=expected:
        raise RecipeBlocked('fixture refuses a resident outside its private XDG config')
    baseline=copy.deepcopy(current['document'])
    scope.defer('accepted settings',lambda:scope.save(baseline),priority=40)
    if ctx.call('history.list',{'limit':1}).get('total',0):
        raise RecipeBlocked('fixture requires an empty owned history; existing rows are preserved')
    def clean_history():
        # All rows admitted after this empty baseline belong to this recipe's
        # private resident. Never clear a user/borrowed history.
        ctx.call('history.clear')
        ctx.call('cache.clear')
    scope.defer('owned history and cache',clean_history,priority=50)
    scope.patch({'archive':{'root':scope.root+'/out','keepUploads':True},
                 'actions':{name:{'destination':'local','profileId':None} for name in ('region','window','desktop','clipboard','edit')},
                 'preferences':{'launchAtLogin':False,'unlockCredentialsAtStartup':False}})
    # The accepted file now exists, so this restart closes first-launch Settings.
    scope.restart({'PUTGD_RECORD_KEEP_WORKING':scope.root+'/working'},
                  before=(lambda:scope.python('from pathlib import Path\nPath('+repr(expected)+').unlink()')) if first_launch else None)
    scope.python('from pathlib import Path\n'+
                 '\n'.join('Path('+repr(scope.root+'/'+name)+').mkdir(mode=0o700,exist_ok=True)' for name in ('out','working')))
    return Fixture(private=True,isolated=True,config_path=expected,root=ctx.target.root,output_dir=scope.root+'/out',first_launch=first_launch)
