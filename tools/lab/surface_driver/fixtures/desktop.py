"""Owned external windows and actual native shortcut conflicts, guest-only."""
import json
import shlex

from ..api import RecipeBlocked
from .base import Fixture


def guest(scope):
    ctx=scope.ctx
    if ctx.placement not in ('guest','boat') or not ctx.profile.get('guest'):
        raise RecipeBlocked('desktop state fixtures require a disposable nested guest',issue=681)
    ctx.fixture('app-profile')


def focus_loss(scope):
    guest(scope)
    ctx=scope.ctx
    title='putgd external focus '+scope.token[:8]
    role='fixture-external-'+scope.token[:8]
    # It inherits private XDG identity, but is a distinct real process/window.
    process=scope.launch(['python3','-c','''from PyQt6.QtWidgets import QApplication,QPushButton
app=QApplication([])
button=QPushButton('Synthetic external focus target');button.setWindowTitle(TITLE);button.resize(360,120);button.show()
app.exec()
'''.replace('TITLE',repr(title))],name='external-focus')
    previous=ctx.profile.get('windows',{})
    ctx.profile['windows']={**previous,role:{'title':title}}
    scope.defer('external window role',lambda:ctx.profile.__setitem__('windows',previous))
    ctx.wait(lambda:ctx.windows({'pid':process['pid']}),'external focus window',timeout=15)
    def hide():
        if ctx.profile.get('session')!='x11':
            raise RecipeBlocked('native hide needs a compositor minimize operation; focus/deactivate remains available',issue=681)
        geometry=ctx.geometry('settings')
        identity=geometry.get('window_id')
        if identity is None:
            raise RecipeBlocked('native window identity unavailable for minimize',issue=681)
        result=ctx.shell('xdotool windowminimize -- '+shlex.quote(str(identity)))
        if result['returncode']:
            raise RecipeBlocked('owned Settings window did not minimize',issue=681)
    return Fixture(process=process,role=role,focus=lambda:ctx.focus(role,raise_window=True),
                   deactivate=lambda:ctx.focus(role,raise_window=True),hide=hide,
                   restore=lambda:ctx.focus('settings',raise_window=True),observe=lambda:ctx.call('diagnostics').get('shortcuts',{}))


def input_events(scope):
    scope.ctx.fixture('app-profile')
    ctx=scope.ctx
    def overlap(first,second,modifiers=('Ctrl','Shift')):
        # All foreground, deadline and release handling stays with the input
        # guard owner. Never dispatch QMP or mask signals in a fixture.
        with ctx.held_keys(*modifiers,first):
            ctx.keys(second)
    def altgr(key='E'):
        with ctx.held_keys('AltGr'):
            ctx.keys(key)
    return Fixture(overlap=overlap,altgr=altgr,
                   shifted_digit=lambda digit:overlap(str(digit),'A',('Shift',)))


def binding(scope, state='conflict'):
    guest(scope)
    ctx=scope.ctx
    if state!='conflict' or ctx.profile.get('session')!='x11':
        current=ctx.call('diagnostics').get('shortcuts',{})
        bindings=current.get('bindings',{})
        if any(value.get('state')==state for value in bindings.values()) or (state=='unavailable' and current.get('available') is False):
            return Fixture(state=state,observe=lambda:ctx.call('diagnostics').get('shortcuts',{}),controlled=False)
        raise RecipeBlocked('requested native shortcut state '+state+' is absent; no production state injection or substituted desktop identity is used',issue=681)
    shortcut='Ctrl+Shift+F12'
    scope.patch({'actions':{'region':{'shortcut':shortcut}}})
    ready=scope.root+'/grab-ready'
    # Start the contender while the resident is stopped, so the server, rather
    # than a manufactured diagnostics reply, reports BadAccess on re-registration.
    code='''import ctypes,ctypes.util,time
from pathlib import Path
x=ctypes.CDLL(ctypes.util.find_library('X11'))
x.XOpenDisplay.restype=ctypes.c_void_p;x.XOpenDisplay.argtypes=[ctypes.c_char_p]
x.XDefaultRootWindow.restype=ctypes.c_ulong;x.XDefaultRootWindow.argtypes=[ctypes.c_void_p]
x.XStringToKeysym.restype=ctypes.c_ulong;x.XStringToKeysym.argtypes=[ctypes.c_char_p]
x.XKeysymToKeycode.restype=ctypes.c_ubyte;x.XKeysymToKeycode.argtypes=[ctypes.c_void_p,ctypes.c_ulong]
x.XGrabKey.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_uint,ctypes.c_ulong,ctypes.c_int,ctypes.c_int,ctypes.c_int]
x.XSync.argtypes=[ctypes.c_void_p,ctypes.c_int]
errors=[]
handler=ctypes.CFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_void_p)(lambda *_:errors.append(True) or 0)
x.XSetErrorHandler(handler)
d=x.XOpenDisplay(None);assert d
root=x.XDefaultRootWindow(d);key=x.XKeysymToKeycode(d,x.XStringToKeysym(b'F12'));assert key
for extra in (0,2,16,18):x.XGrabKey(d,key,5|extra,root,0,1,1)
x.XSync(d,0);assert not errors
Path(READY).write_text('ready')
while True:time.sleep(1)
'''.replace('READY',repr(ready))
    identities=[]
    def start():
        identities.append(scope.launch(['python3','-c',code],name='shortcut-contender'))
        ctx.wait(lambda:ctx.shell('test -f '+shlex.quote(ready))['returncode']==0,'native shortcut contender',timeout=10)
    scope.restart({},before=start)
    released=False
    def release():
        nonlocal released
        if not released:
            scope.stop(identities[0]);released=True
    return Fixture(state=state,shortcut=shortcut,controlled=True,release=release,
                   observe=lambda:ctx.call('diagnostics').get('shortcuts',{}))
