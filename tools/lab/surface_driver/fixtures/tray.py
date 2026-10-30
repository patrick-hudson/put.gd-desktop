"""Real synthetic tray states; no SNI/notification activation is used as input."""
from ..api import RecipeBlocked,RecipeFailed
from .base import Fixture
from .uploads import submit,track


def attention(scope):
    value=submit(scope,local_failure=True)
    return Fixture(value,attention=True,retry=lambda:scope.ctx.call('operation.retryLocalSave',{'id':value.operation_id}))


def two_uploads(scope):
    scope.ctx.fixture('app-profile')
    scope.patch({'limits':{'concurrentUploads':2,'pendingOperations':8}})
    values=[submit(scope,remote=True,active=True) for _ in range(2)]
    return Fixture(operations=values,operation_ids=[v.operation_id for v in values],
                   cancel=lambda identity:scope.ctx.call('operation.cancel',{'id':identity}),
                   inspect=lambda:[scope.ctx.call('operation.inspect',{'id':v.operation_id}) for v in values],
                   isolated=True,loopback=True)


def recording(scope):
    ctx=scope.ctx
    capture=ctx.fixture('recording-lifecycle')
    result=capture.prepare_recording()
    identity=result.get('recordingId') or result.get('id')
    ctx.wait(lambda:ctx.call('recording.inspect',{'id':identity})['state']=='ready','active tray recording ready',timeout=40)
    ctx.call('recording.start',{'id':identity})
    return Fixture(id=identity,recording_id=identity,synthetic=True,
                   inspect=lambda:ctx.call('recording.inspect',{'id':identity}),
                   pause=lambda:ctx.call('recording.pause',{'id':identity}),
                   resume=lambda:ctx.call('recording.resume',{'id':identity}),
                   stop=lambda:ctx.call('recording.stop',{'id':identity}))


def automatic_notice(scope):
    value=scope.ctx.fixture('events.expired-multipart')
    result=value.automatic_cleanup()
    return Fixture(value,automatic=True,cleanup_result=result)


def update(scope):
    ctx=scope.ctx
    value=ctx.fixture('settings-update-lifecycle')
    status=ctx.call('update.check')
    if not ctx.call('update.status').get('result',{}).get('updateAvailable'):
        raise RecipeFailed('authentic tagged fixture offers no newer update')
    return Fixture(value,update_available=True)


def other_menu(scope):
    from .desktop import guest
    guest(scope)
    ctx=scope.ctx
    role='fixture-popup-'+scope.token[:8]
    name='putgd other-client popup '+scope.token[:8]
    process=scope.launch(['python3','-c','''from PyQt6.QtCore import QTimer
from PyQt6.QtGui import QCursor
from PyQt6.QtWidgets import QApplication,QMenu
app=QApplication([]);menu=QMenu();menu.setWindowTitle(NAME);menu.setAccessibleName(NAME)
menu.addAction('Synthetic other-client menu');menu.addAction('No publication action')
QTimer.singleShot(0,lambda:menu.popup(QCursor.pos()))
app.exec()
'''.replace('NAME',repr(name))],name='other-menu')
    previous=ctx.profile.get('windows',{})
    ctx.profile['windows']={**previous,role:{'title':'.*','popup':True,'accessible':{'pid':process['pid'],'name':name,'role':'popup menu'}}}
    scope.defer('other-client popup role',lambda:ctx.profile.__setitem__('windows',previous))
    def nodes():return ctx.accessible({'owned':True,'pid':process['pid'],'visible':True,'showing':True})
    ctx.wait(nodes,'visible owned other-client popup',timeout=15)
    # Presence/focus are inspectable. A recipe requiring a native keyboard-grab
    # owner witness must still obtain that witness from its input backend.
    return Fixture(process=process,role=role,observe=nodes,focus=lambda:ctx.focus(role,raise_window=False),
                   keyboard_grab_verified=False)


def shortcut_recorder(scope):
    ctx=scope.ctx
    ctx.fixture('app-profile')
    observation=ctx.fixture('settings-observations')
    def status():return ctx.call('diagnostics').get('shortcuts',{})
    def start(action='region'):
        if action not in ('region','window','desktop','clipboard','edit','record'):
            raise ValueError('unknown shortcut action')
        ctx.view('ui.show',{'view':'settings'})
        ctx.use_catalog('settings','07-settings-general')
        ctx.click('settingsSection2',window='settings')
        ctx.wait(lambda:ctx.call('ui.viewState')['settings']['section']==2,'Shortcuts page')
        ctx.use_catalog('settings','06-settings-shortcuts')
        ctx.click('record'+action.title()+'Shortcut',window='settings')
        return ctx.wait(lambda:(r if (r:=observation.recording_status()) and 'Press shortcut' in (r.get('text') or '') else None),
                        'native shortcut recording ready',timeout=20)
    def cancel():
        # Do not send Esc if the person already finished/cancelled recording.
        if any(v.get('state')=='recording' for v in status().get('bindings',{}).values()):
            ctx.focus('settings',raise_window=True);ctx.keys('Esc')
    scope.defer('cancel owned shortcut recording',cancel,priority=700)
    return Fixture(start=start,cancel=cancel,status=status,recording_status=observation.recording_status,
                   no_operation_baseline=lambda:Fixture(active=ctx.call('diagnostics')['activeOperations'],
                                                         history_total=ctx.call('history.list',{'limit':1}).get('total',0)))


def keyboard_layout(scope,layout='de'):
    from .desktop import guest
    guest(scope)
    ctx=scope.ctx
    if ctx.profile.get('session')!='x11':
        raise RecipeBlocked('native Wayland layout changes need a profile-owned input-source controller; no XWayland substitute',issue=670)
    if layout not in ('us','de','fr'):
        raise ValueError('synthetic layout must be us, de or fr')
    import shlex
    result=ctx.shell('setxkbmap -query')
    if result['returncode']:raise RecipeBlocked('X11 native keyboard layout query unavailable',issue=670)
    baseline=dict(line.split(':',1) for line in result['stdout'].splitlines() if ':' in line)
    baseline={key:value.strip() for key,value in baseline.items()}
    def set_layout(values):
        args=['setxkbmap','-option','']
        for key in ('rules','model','layout','variant','options'):
            if key in values:
                args.extend(['-'+('option' if key=='options' else key),values[key]])
        reply=ctx.shell(shlex.join(args))
        if reply['returncode']:raise RecipeBlocked('X11 layout application failed',issue=670)
    def restore():
        set_layout(baseline)
        reply=ctx.shell('setxkbmap -query')
        actual=dict(line.split(':',1) for line in reply['stdout'].splitlines() if ':' in line)
        if reply['returncode'] or {k:v.strip() for k,v in actual.items()}!=baseline:
            raise RecipeFailed('native keyboard layout did not restore exactly')
    scope.defer('native X11 keyboard layout',restore,priority=800)
    set_layout({**baseline,'layout':layout,'variant':''})
    return Fixture(layout=layout,baseline=baseline,restore=restore,
                   shortcuts=lambda:ctx.call('diagnostics').get('shortcuts',{}))
