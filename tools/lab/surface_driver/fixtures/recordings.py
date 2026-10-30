"""Fresh production captures of the existing visible cover_player pattern."""
import json
from pathlib import Path
import shlex
import time

from ..api import RecipeBlocked, RecipeFailed
from .base import Fixture
from .uploads import track


def pattern(scope):
    scope.ctx.fixture('app-profile')
    # Reuse the established output-pinned cover; add private FIFO controls for
    # freeze/resume/raise. The real desktop, compositor and portal stay intact.
    source=(Path(__file__).resolve().parents[2]/'cover_player.py').read_text()
    for anchor in ('def on_stdin(channel, condition):', "if command == 'quit':"):
        if source.count(anchor) != 1:
            raise RecipeBlocked('cover_player.py no longer matches pattern-control anchors',issue=672)
    source=source.replace('def on_stdin(channel, condition):', 'def on_stdin(channel, condition):\n        from gi.repository import Gst')
    source=source.replace("if command == 'quit':", "if command == 'freeze' and state['pipeline'] is not None:\n            state['pipeline'].set_state(Gst.State.PAUSED)\n        if command == 'resume' and state['pipeline'] is not None:\n            state['pipeline'].set_state(Gst.State.PLAYING)\n        if command == 'raise':\n            window.present()\n        if command == 'quit':")
    scope.write(scope.root+'/cover.py',source.encode())
    fifo=scope.root+'/pattern.fifo'
    scope.python('import os\nos.mkfifo('+repr(fifo)+',0o600)')
    # A child holds the FIFO open; no leaked shell, implicit desktop input or
    # kill-by-name. Scope.launch owns this PID and its exact start identity.
    code='''import os,sys
fd=os.open(FIFO,os.O_RDWR)
os.dup2(fd,0)
os.execvp('python3',['python3',COVER,'play','--pattern','--media-now','--video-size','640x320','--title','putgd synthetic fixture'])
'''.replace('FIFO',repr(fifo)).replace('COVER',repr(scope.root+'/cover.py'))
    scope.launch(['python3','-c',code],name='pattern',environment={'GTK_USE_PORTAL':'0'})
    def ready():
        result=scope.ctx.shell('cat '+shlex.quote(scope.root+'/pattern.log'))
        records=[]
        for line in result['stdout'].splitlines():
            try:records.append(json.loads(line))
            except ValueError:pass
        covered=next((r for r in records if r.get('state')=='covered' and r.get('ok')),None)
        return covered if covered and any(r.get('state')=='playing' for r in records) else None
    info=scope.ctx.wait(ready,'visible synthetic scrolling pattern',timeout=20)
    x,y,w,h=info['rect']
    rect=[x+(w-640)//2,y+(h-320)//2,640,320]
    def command(value):
        scope.python('import os\nfd=os.open('+repr(fifo)+',os.O_WRONLY|os.O_NONBLOCK)\nos.write(fd,'+repr((value+'\n').encode())+');os.close(fd)')
    return Fixture(synthetic=True,rect=rect,screens=[info['rect']],output_dir=scope.root+'/out',
                   command=command,prepare={'target':{'kind':'region','rect':rect},'format':'mp4','audio':{'system':False}},
                   prepare_command="python3 -c "+shlex.quote('import os;f=os.open('+repr(fifo)+',os.O_WRONLY|os.O_NONBLOCK);os.write(f,b"raise\n");os.close(f)'))


def review_catalog(scope, name):
    ctx=scope.ctx
    if hasattr(ctx,'catalog') and (ctx.catalog.directory/(name+'.controls.json')).is_file():
        ctx.use_catalog('review',name)


def actual_working(scope, identity):
    from pathlib import PurePosixPath
    root=scope.ctx.call('diagnostics').get('locations',{}).get('data',{}).get('path')
    if not isinstance(root,str) or not PurePosixPath(root).is_relative_to(PurePosixPath(scope.ctx.target.root)/'data'):
        raise RecipeBlocked('recording fixture needs the exact owned data root',issue=672)
    return root+'/recording/'+identity+'/working.mkv'


def capture(scope, scene, *, seconds=4.5, automatic=False):
    ctx=scope.ctx
    scene.command('raise')
    prepared=ctx.call('recording.prepare',scene.prepare)
    identity=prepared.get('recordingId') or prepared.get('id')
    if not identity:raise RecipeBlocked('recording.prepare returned no identity',issue=672)
    track(scope,'recording.cancel',identity)
    ctx.wait(lambda:ctx.call('recording.inspect',{'id':identity})['state']=='ready','synthetic recording ready',timeout=40)
    ctx.call('recording.start',{'id':identity})
    if automatic:
        ctx.wait(lambda:ctx.call('recording.inspect',{'id':identity})['state']=='reviewing',
                 'authentic recording byte-budget stop',timeout=75)
    else:
        # Keep wall output live and honor the recipe deadline while recording.
        until=time.monotonic()+seconds
        ctx.wait(lambda:time.monotonic()>=until,'four-second synthetic capture',timeout=seconds+2,interval=.2)
        ctx.call('recording.stop',{'id':identity})
    state=ctx.wait(lambda:(r if (r:=ctx.call('recording.inspect',{'id':identity}))['state']=='reviewing' else None),'private recording review',timeout=40)
    if state.get('durationUs',0)<4_000_000:
        raise RecipeFailed('synthetic recording is shorter than four seconds')
    path=scope.root+'/working/'+identity+'.mkv'
    if automatic:
        if state.get('stopReason')!='budget':
            raise RecipeFailed('recording stopped without the required byte-budget reason')
        source=actual_working(scope,identity)
        scope.python('import shutil\nshutil.copyfile('+repr(source)+','+repr(path)+')')
    if ctx.shell('test -s '+shlex.quote(path))['returncode']:
        raise RecipeBlocked('recording did not retain its actual working clip',issue=672)
    return identity,path


def recording(scope, *, automatic=False):
    ctx=scope.ctx
    ctx.fixture('destination') # route/settings snapshot must precede prepare
    scene=ctx.fixture('events.recording-source')
    scene.command('freeze')
    frozen,frozen_path=capture(scope,scene)
    ctx.call('recording.cancel',{'id':frozen})
    scene.command('resume')
    if automatic:
        scope.patch({'limits':{'maxFileBytes':65*1024**2}})
    identity,working_path=capture(scope,scene,automatic=automatic)
    ctx._ids['review']=identity
    ctx.call('recording.review',{'id':identity,'action':'pause'})
    ctx.call('recording.review',{'id':identity,'action':'seek','time':0})
    review_catalog(scope,'30-review')
    return Fixture(id=identity,recording_id=identity,operation_id=identity,extension='mp4',
                   output_dir=scope.root+'/out',working_path=working_path,frozen_path=frozen_path,synthetic=True)


def controls(scope):
    path=scope.ctx.catalog.directory/'30-review.controls.json'
    if not path.is_file():raise RecipeBlocked('current-build review controls inventory missing',issue=672)
    return Fixture(json.loads(path.read_text()))


def preview_error(scope):
    """Latch a genuine decoder failure, then restore bytes before real Retry."""
    value=scope.ctx.fixture('recording')
    working=actual_working(scope,value.id)
    backup=scope.root+'/decoder-backup.mkv'
    scope.python('''import os
from pathlib import Path
p=Path(WORKING);backup=Path(BACKUP)
assert p.resolve()==p and p.is_file() and not backup.exists()
os.rename(p,backup)
try:
 with open(p,'xb') as f:f.write(b'synthetic invalid media')
except BaseException:
 os.rename(backup,p)
 raise
'''.replace('WORKING',repr(working)).replace('BACKUP',repr(backup)))
    def restore():
        scope.python('''import os
from pathlib import Path
p=Path(WORKING);backup=Path(BACKUP)
if backup.exists():
 assert backup.resolve()==backup and p.resolve()==p
 os.replace(backup,p)
'''.replace('WORKING',repr(working)).replace('BACKUP',repr(backup)))
    scope.defer('decoder working bytes',restore,priority=300)
    scope.ctx.call('recording.review',{'id':value.id,'action':'seek','time':1_000_000})
    scope.ctx.wait(lambda:scope.ctx.call('recording.review',{'id':value.id,'action':'inspect'})['preview'].get('error'),
                   'genuine decoder preview failure',timeout=20)
    restore()
    review_catalog(scope,'40-review-preview-error')
    return Fixture(value,decoder_fault_restored=True)


def export_failure(scope):
    value=scope.ctx.fixture('recording')
    blocker=scope.root+'/blocked-export'
    scope.write(blocker,b'synthetic export blocker')
    scope.ctx.call('recording.saveCopy',{'id':value.id,'path':blocker+'/retry.mp4'})
    scope.ctx.wait(lambda:(state if (state:=scope.ctx.call('recording.inspect',{'id':value.id})).get('lastExport',{}).get('status')=='failed' else None),
                   'authentic recoverable export failure',timeout=30)
    scope.python('from pathlib import Path\np=Path('+repr(blocker)+');p.unlink();p.mkdir(mode=0o700)')
    return Fixture(value,export_fault_removed=True,retry_path=blocker+'/retry.mp4')
