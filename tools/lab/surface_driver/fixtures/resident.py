"""Private resident quit/launch ownership, including a real tray Quit action."""
import shlex

from ..api import RecipeBlocked
from .base import Fixture


def identity(scope):
    return scope.json(r'''import json,os,socket,struct
from pathlib import Path
with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as peer:
 peer.connect(__SOCKET__)
 pid,uid,gid=struct.unpack('3i',peer.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
assert uid==os.getuid() and pid>1
p=Path('/proc')/str(pid)
env=dict(v.split(b'=',1) for v in (p/'environ').read_bytes().split(b'\0') if b'=' in v)
assert env[b'XDG_RUNTIME_DIR'].decode()==__RUNTIME__
print(json.dumps({'pid':pid,'start':(p/'stat').read_text().split(') ',1)[1].split()[19]}))
'''.replace('__SOCKET__',repr(scope.ctx.target.socket)).replace('__RUNTIME__',repr(scope.ctx.target.root+'/runtime')))


def lifecycle(scope):
    ctx=scope.ctx
    ctx.fixture('app-profile')
    initial=identity(scope)
    def stopped():
        return ctx.shell('test ! -e '+shlex.quote(ctx.target.socket))['returncode']==0
    def launch():
        if not stopped():
            raise RecipeBlocked('private resident has not quit; refusing a duplicate launch')
        ctx.target.started=False
        scope._relaunch()
        return identity(scope)
    def restore():
        if stopped():launch()
    scope.defer('private resident after real Quit',restore,priority=600)
    return Fixture(initial=initial,identity=lambda:identity(scope),stopped=stopped,launch=launch,
                   restart=lambda:scope.restart({}),quit=lambda:ctx.call('ui.quit'))


def graphical(scope):
    """Use Qt's real native -platform override, preserving desktop identity."""
    ctx=scope.ctx
    ctx.fixture('app-profile')
    platform={'x11':'xcb','wayland':'wayland'}.get(ctx.profile.get('session'))
    if platform is None:raise RecipeBlocked('Qt tray fallback requires a native X11/Wayland session',issue=670)
    original=ctx.target.app_executable
    wrapper=scope.root+'/graphical-resident'
    script='#!/bin/sh\nexec '+shlex.quote(original)+' -platform '+platform+' "$@"\n'
    scope.write(wrapper,script.encode())
    scope.python('import os\nos.chmod('+repr(wrapper)+',0o700)')
    def restore():
        ctx.target.app_executable=original
        scope.restart({})
    scope.defer('original native resident route',restore,priority=500)
    ctx.target.app_executable=wrapper
    scope.restart({})
    return Fixture(platform=platform,graphical=True,identity=lambda:identity(scope),
                   native_session=ctx.profile['session'],executable=original)
