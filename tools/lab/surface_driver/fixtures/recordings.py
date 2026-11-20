"""Fresh production captures of the existing visible cover_player pattern."""
import json
from pathlib import Path
import re
import shlex
import time

from ..api import RecipeBlocked, RecipeFailed
from . import consent, media
from .base import Fixture
from .uploads import track

PATTERN_PLUGINS = '''import gi
gi.require_version('Gst','1.0')
from gi.repository import Gst
Gst.init(None)
missing = [name for name in ('gtksink', 'timeoverlay') if not Gst.ElementFactory.find(name)]
if missing:
    print(', '.join(missing))
    raise SystemExit(3)
'''


def media_preflight(scope):
    """The timestamped pattern plays through GStreamer's GTK3 sink (#784)."""
    result=scope.ctx.shell("python3 - <<'SD_GST_PATTERN'\n"+PATTERN_PLUGINS+'SD_GST_PATTERN\n')
    if result['returncode']:
        missing=result['stdout'].strip() or 'GStreamer with Python GI bindings'
        raise RecipeBlocked('recording source is missing '+missing,issue=784)


def desktop_contract(scope):
    """Read native logical heads and reject a recording helper from another build."""
    build=scope.ctx.call('ui.snapshot')['build']
    value=scope.json('''import json,shutil,subprocess
from pathlib import Path
app=shutil.which(APP)
assert app
helper=Path(app).resolve().with_name('putgd-record')
manifest=json.loads(subprocess.check_output([str(helper),'--version-json'],text=True))
for key in ('version','gitCommit','gitTree','dirty'):
 assert key in BUILD and manifest[key]==BUILD[key]
outputs=json.loads(subprocess.check_output(['python3',COVER,'outputs'],text=True))
assert outputs['logical']
for group in outputs['logical']:
 assert len(group['rect'])==4 and all(isinstance(n,int) for n in group['rect'])
 assert group['rect'][2]>=640 and group['rect'][3]>=320 and group['names']
print(json.dumps({'record_helper':str(helper),'logical':outputs['logical']}))
'''.replace('APP',repr(scope.ctx.target.app_executable)).replace('BUILD',repr(build)).replace('COVER',repr(scope.root+'/cover.py')))
    return value


def text_overlays(lines):
    """GStreamer textoverlay elements drawing LINES in black, centred one above the other."""
    for line in lines:
        if not re.fullmatch(r'[A-Z0-9 ]{1,24}', line):
            raise ValueError('text pattern lines are short upper-case words and digits')
    spacing=80
    first=-(len(lines)-1)*spacing//2
    return ''.join('textoverlay text="%s" valignment=center halignment=center deltay=%d font-desc="Sans Bold 30" '
                   'color=4278190080 draw-outline=false draw-shadow=false shaded-background=false ! '
                   % (line, first+i*spacing) for i, line in enumerate(lines))


def pattern(scope, text=None):
    """The moving pattern; with TEXT (a few short lines) a still white video with that text instead."""
    scope.ctx.fixture('app-profile')
    # Add a clock to distinguish repeated bar positions across a cut.
    # Reuse the output-pinned cover and add private FIFO controls.
    source=(Path(__file__).resolve().parents[2]/'cover_player.py').read_text()
    bars='framerate=30/1 ! videoconvert ! gtksink name=video'
    moving='pattern=%s horizontal-speed=6 '
    for anchor in (bars, moving, 'def on_stdin(channel, condition):', "if command == 'quit':"):
        if source.count(anchor) != 1:
            raise RecipeBlocked('cover_player.py no longer matches pattern-control anchors',issue=672)
    if text:
        # A plain white source keeps the text the only thing in the video for recognition.
        source=source.replace(moving, 'pattern=white %.0s')
        source=source.replace(bars, 'framerate=30/1 ! '+text_overlays(text)+'videoconvert ! gtksink name=video')
    else:
        source=source.replace(bars, 'framerate=30/1 ! timeoverlay font-desc="Sans Bold 40" '
                                   'shaded-background=true ! videoconvert ! gtksink name=video')
    source=source.replace('def on_stdin(channel, condition):', 'def on_stdin(channel, condition):\n        from gi.repository import Gst')
    source=source.replace("if command == 'quit':", "if command == 'freeze' and state['pipeline'] is not None:\n            state['pipeline'].set_state(Gst.State.PAUSED)\n        if command == 'resume' and state['pipeline'] is not None:\n            state['pipeline'].set_state(Gst.State.PLAYING)\n        if command == 'raise':\n            window.present()\n        if command == 'quit':")
    scope.write(scope.root+'/cover.py',source.encode())
    media_preflight(scope)
    desktop=desktop_contract(scope)
    fifo=scope.root+'/pattern.fifo'
    scope.python('import os\nos.mkfifo('+repr(fifo)+',0o600)')
    # A child holds the FIFO open; no leaked shell, implicit desktop input or
    # kill-by-name. Scope.launch owns this PID and its exact start identity.
    code='''import os,sys
fd=os.open(FIFO,os.O_RDWR)
os.dup2(fd,0)
os.execvp('python3',['python3',COVER,'play','--output',OUTPUT,'--pattern','--media-now','--video-size','640x320','--title','putgd synthetic fixture'])
'''.replace('FIFO',repr(fifo)).replace('COVER',repr(scope.root+'/cover.py')).replace('OUTPUT',repr(desktop['logical'][0]['names'][0]))
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
    if info['rect']!=desktop['logical'][0]['rect']:
        raise RecipeBlocked('synthetic cover moved away from the observed first logical head',issue=664)
    return Fixture(synthetic=True,rect=rect,screens=[group['rect'] for group in desktop['logical']],output_dir=scope.root+'/out',
                   record_helper=desktop['record_helper'],
                   command=command,prepare={'target':{'kind':'region','rect':rect},'format':'mp4','audio':{'system':False}},
                   prepare_command="python3 -c "+shlex.quote('import os;f=os.open('+repr(fifo)+',os.O_WRONLY|os.O_NONBLOCK);os.write(f,b"raise\n");os.close(f)'))


def review_catalog(scope, name):
    ctx=scope.ctx
    if hasattr(ctx,'catalog') and (ctx.catalog.directory/(name+'.controls.json')).is_file():
        ctx.use_catalog('review',name)


def actual_working(scope, identity):
    from pathlib import PurePosixPath
    root=scope.ctx.call('diagnostics').get('locations',{}).get('data',{}).get('path')
    data=scope._environment.get('XDG_DATA_HOME',scope.ctx.target.root+'/data')
    if (not isinstance(root,str) or not PurePosixPath(data).is_relative_to(PurePosixPath(scope.ctx.target.root))
            or not PurePosixPath(root).is_relative_to(PurePosixPath(data))):
        raise RecipeBlocked('recording fixture needs the exact owned data root',issue=672)
    return root+'/recording/'+identity+'/working.mkv'


def capture(scope, scene, *, seconds=4.5, automatic=False):
    ctx=scope.ctx
    scene.command('raise')
    # Observe the portal before prepare: consent is driven only for this request.
    watch=consent.start(scope, target=scene.prepare.get('target'))
    prepared=ctx.call('recording.prepare',scene.prepare)
    identity=prepared.get('recordingId') or prepared.get('id')
    if not identity:raise RecipeBlocked('recording.prepare returned no identity',issue=672)
    track(scope,'recording.cancel',identity)
    consent.await_ready(scope,identity,watch,timeout=40)
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
    media.register(scope, outside_receipts=True) # preserve raw clips outside the shareable audit root
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


def copy_recording(scope):
    ctx=scope.ctx
    ctx.fixture('file-dialog-fallback')
    return ctx.fixture('recording')


def _replace_working(scope, identity):
    """Temporarily replace the owned working file and register its restoration."""
    working=actual_working(scope,identity)
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
    scope.defer('recording working bytes',restore,priority=300)
    return restore


def preview_error(scope):
    """Latch a genuine decoder failure, then restore bytes before real Retry."""
    value=scope.ctx.fixture('recording')
    restore=_replace_working(scope,value.id)

    scope.ctx.call('recording.review',{'id':value.id,'action':'seek','time':1_000_000})
    scope.ctx.wait(lambda:scope.ctx.call('recording.review',{'id':value.id,'action':'inspect'})['preview'].get('error'),
                   'genuine decoder preview failure',timeout=20)
    restore()
    review_catalog(scope,'40-review-preview-error')
    return Fixture(value,decoder_fault_restored=True)


def export_failure(scope):
    from ..recipes.dialogs import choose_review_copy

    value=scope.ctx.fixture('recording-copy')
    path=value['output_dir']+'/retry.mp4'
    restore=_replace_working(scope,value.id)
    try:
        # Only a chooser save tells the review to show a failed export banner.
        # Force the worker to fail inside that event, with a valid save path.
        choose_review_copy(scope.ctx,value.id,path,output_dir=value['output_dir'])
        def failed():
            result=scope.ctx.call('recording.inspect',{'id':value.id}).get('lastExport') or {}
            return result.get('status')=='failed' and result.get('path')==path
        scope.ctx.wait(failed,'recoverable export failure at the chosen path',timeout=30)
        scope.ctx.wait(lambda: scope.ctx.call('recording.review',{'id':value.id,'action':'inspect'})['errorDetails']['visible'],
                       'the chooser export failure reaches the review')
    finally:
        restore()
    return Fixture(value,retry_path=path)
