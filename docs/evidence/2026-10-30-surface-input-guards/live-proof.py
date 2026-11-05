"""Private synthetic live proof; run on the identified Boat QEMU host."""
import json
from pathlib import Path
import time
from tools.lab.surface_driver.api import recipe,RecipeFailed
from tools.lab.surface_driver.run import main


@recipe('shell.window-maximize','shell.window-minimize','shell.window-close',timeout=240)
def notification_cover(ctx):
    ctx.fixture('history_retry_upload')
    ctx.view('ui.show',{'view':'history'})
    box=ctx.wait(lambda:ctx.geometry('history'),'history mapped')
    notices=ctx.wait(lambda:[n for n in ctx.notifications() if 'fail' in (n['summary']+' '+n.get('body','')).lower()],
                     'real production failed-upload notification',timeout=20)
    notice=notices[-1]
    ctx.note('owned failed-upload notification '+json.dumps({k:notice.get(k) for k in ('id','summary','timeout')}))
    ctx.frame('notification-before')
    for control in ('text:Maximize','text:Minimize','text:Close window'):
        try:
            ctx.click(control,window='history')
        except RecipeFailed as error:
            if 'cinnamon' not in str(error) or not any(word in str(error).lower() for word in ('fail','problem')):
                raise
            ctx.note(control+' refused before dispatch: '+str(error))
        else:
            raise RecipeFailed(control+' was not refused under the real failure banner')
    ctx.frame('notification-refused')
    result=ctx.shell('gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications '
                     '--method org.freedesktop.Notifications.CloseNotification '+str(notice['id']))
    ctx.expect(result['returncode']==0,'dismiss owned failure notification')
    ctx.wait(lambda:any(n.get('closed') for n in ctx.notifications() if n['id']==notice['id']), 'notification close signal',timeout=10)
    ctx.click('text:Maximize',window='history')
    ctx.wait(lambda:ctx.geometry('history')['width']>box['width'],'uncovered Maximize succeeds')
    ctx.frame('uncovered-maximized')
    ctx.click('text:Restore',window='history')
    ctx.wait(lambda:ctx.geometry('history')['width']==box['width'],'restore size')
    ctx.click('text:Minimize',window='history')
    ctx.wait(lambda:any(w['states']['minimized'] for w in ctx.windows({'title':'^put\\.gd$','include_hidden':True})), 'uncovered Minimize succeeds')
    ctx.frame('uncovered-minimized')
    ctx.view('ui.show',{'view':'history'})
    ctx.wait(lambda:ctx.windows({'title':'^put\\.gd$'}),'restored window')
    ctx.click('text:Close window',window='history')
    ctx.wait(lambda:not ctx.windows({'title':'^put\\.gd$'}),'uncovered Close succeeds')
    ctx.frame('uncovered-closed')


@recipe('shell.header-drag',timeout=1800)
def header_twenty(ctx):
    point=ctx.fixture('history_window').header_drag_point
    for number in range(1,21):
        before=ctx.geometry('history')
        old_deadline=ctx._deadline
        ctx._deadline=min(old_deadline,time.monotonic()+120)
        started=time.monotonic()
        ctx.frame('drag-%02d-before'%number)
        ctx.drag(point,(point[0]+64,point[1]+48))
        moved=ctx.wait(lambda:(g if ((g:=ctx.geometry('history'))['x'],g['y'])!=(before['x'],before['y']) else None),'real header motion')
        ctx.expect((moved['width'],moved['height'])==(before['width'],before['height']),'header preserves size')
        ctx.frame('drag-%02d-after'%number)
        shifted=(point[0]+moved['x']-before['x'],point[1]+moved['y']-before['y'])
        ctx.drag(shifted,point)
        restored=ctx.geometry('history')
        point=(point[0]+restored['x']-before['x'],point[1]+restored['y']-before['y'])
        ctx.note('header repetition '+str(number)+' passed in %.3fs; persistent SSH pid %s'%(time.monotonic()-started,ctx.target._probe_process.pid))
        ctx._deadline=old_deadline


@recipe('shell.header-drag',timeout=120)
def deadline_channel(ctx):
    from tools.lab.surface_driver.api import RecipeBlocked
    ctx.view('ui.show',{'view':'history'})
    ctx.wait(lambda:ctx.windows({'title':'^put\\.gd$'}),'History mapped')
    ctx.focus('history')
    ctx.frame('deadline-before')
    original=ctx._deadline
    ctx._deadline=time.monotonic()+.1
    try:
        ctx._probe('accessible',{'application':'cinnamon*','showing':True})
    except RecipeBlocked as error:
        ctx.expect('recipe deadline exhausted during accessible probe' in str(error),'deadline names outstanding probe')
        ctx.expect(ctx.target._probe_process.poll() is None,'SSH channel remains alive at timeout')
        ctx.note('live near-deadline cause: '+str(error))
    else:
        raise RecipeFailed('100ms fresh shell probe unexpectedly completed; no timeout reproduction')
    finally:
        ctx._deadline=original
    try:
        ctx.keys('Escape')
    except RecipeBlocked as error:
        ctx.expect('start a new run' in str(error),'later input withheld without retrying late witness')
        ctx.note('later input withheld: '+str(error))
    else:
        raise RecipeFailed('input was not withheld after outstanding probe timeout')
    ctx.frame('deadline-after')


@recipe('shell.header-drag',timeout=240)
def witness_move_control(ctx):
    ctx.fixture('app-profile')
    ctx.view('ui.show',{'view':'history'})
    ctx.wait(lambda:ctx.windows({'name':'put.gd','showing':True}),'native History appears')
    box=ctx.geometry('history')
    nodes=ctx.accessible({'owned':True,'pid':box['pid'],'visible':True,'showing':True})
    menus=[n for n in nodes if n['name']=='Application menu' and n.get('rect')]
    ctx.expect(len(menus)==1,'one live header menu anchor')
    _,y,_,h=menus[0]['rect'];y+=h/2
    intervals=[(box['x']+2,box['x']+box['width']-2)]
    for node in nodes:
        rect=node.get('rect')
        if not rect or node.get('role') in ('frame','panel','filler','root pane','application'):
            continue
        x,top,w,h=rect
        if top<=y<top+h:
            intervals=[part for left,right in intervals for part in ((left,min(right,x-2)),(max(left,x+w+2),right)) if part[1]>part[0]]
    ctx.expect(bool(intervals) and max(r-l for l,r in intervals)>16,'live blank header interval')
    left,right=max(intervals,key=lambda v:v[1]-v[0]);start=(round((left+right)/2),round(y))
    before=ctx._probe('resize',{})
    ctx.frame('move-witness-before')
    ctx.drag(start,(start[0]+32,start[1]+24),steps=4)
    after=ctx._probe('resize',{})
    ctx.frame('move-witness-after')
    ctx.note('native grab positive control '+json.dumps({'before':before,'after':after}))
    ctx.expect(any(e['serial']>before['serial'] and e['window_id']==box['window_id'] and e['kind']=='move' and e['phase']=='begin' for e in after['events']),
               'Mutter move-begin positive control proves signal delivery')


@recipe('shell.edge-resize',timeout=180)
def original_corner(ctx):
    ctx.fixture('app-profile')
    ctx.view('ui.show',{'view':'history'})
    ctx.wait(lambda:ctx.windows({'name':'put.gd','showing':True}),'native History appears')
    box=ctx.geometry('history')
    edge=(box['x']+box['width']-1,box['y']+box['height']-1)
    ctx.frame('original-corner-before')
    try:
        ctx.drag(edge,(edge[0]-40,edge[1]-30),require_resize=True)
    except Exception as error:
        from tools.lab.surface_driver.api import RecipeBlocked
        if not isinstance(error,RecipeBlocked) or 'resize-start' not in str(error):
            raise
        ctx.note('original corner precisely blocked: '+str(error))
    ctx.frame('original-corner-after')


@recipe('shell.edge-resize','shell.minimum-size',timeout=600)
def resize_five(ctx):
    ctx.fixture('app-profile')
    ctx.view('ui.show',{'view':'history'})
    ctx.wait(lambda:ctx.windows({'name':'put.gd','showing':True}),'native History appears')
    for number in range(1,6):
        box=ctx.geometry('history')
        edge=(box['x']+box['width']-1,box['y']+box['height']//2)
        ctx.frame('resize-%d-before'%number)
        try:
            ctx.drag(edge,(edge[0]-30,edge[1]),require_resize=True)
        finally:
            ctx.frame('resize-%d-after'%number)
        resized=ctx.wait(lambda:(g if ((g:=ctx.geometry('history'))['width'],g['height'])!=(box['width'],box['height']) else None),'witnessed resize changes geometry')
        ctx.note('resize repetition '+str(number)+' proof retained; geometry '+json.dumps(resized))


if __name__=='__main__':
    from tools.lab.surface_driver.api import REGISTRY
    from tools.lab.surface_driver.recipes import core_demo,tray
    # The deliberate channel-poison test must follow all functional input.
    REGISTRY.sort(key=lambda item:item.function is deadline_channel)
    raise SystemExit(main())
