"""Chooser launch environments are applied before the helper family starts."""
from ..api import RecipeBlocked
from .base import Fixture


def backend(scope, name):
    scope.ctx.fixture('app-profile')
    env=scope.ctx.profile.get('dialogs',{}).get('backends',{}).get(name,{}).get('environment',{})
    env=dict(env)
    if any(k in env for k in ('DBUS_SESSION_BUS_ADDRESS','DISPLAY','WAYLAND_DISPLAY','XDG_RUNTIME_DIR')):
        raise RecipeBlocked('chooser fixture must preserve the native session buses and display',issue=671)
    env['PUTGD_FILE_DIALOG']='fallback' if name=='fallback' else ''
    scope.restart(env)
    # Availability of the real session portal is observational, never a fake.
    if name=='portal':
        result=scope.ctx.shell("gdbus call --session --dest org.freedesktop.portal.Desktop --object-path /org/freedesktop/portal/desktop --method org.freedesktop.DBus.Properties.Get org.freedesktop.portal.FileChooser version")
        if result['returncode']:
            return Fixture(available=False,reason='native session FileChooser portal unavailable')
    return Fixture(available=True,reason='',backend=name,environment=env)


def failure(scope):
    # Product has no production mechanism that deterministically returns Failed:
    # portal failures deliberately fall back. Refuse to kill the shared portal.
    raise RecipeBlocked('isolated chooser failure needs a production failure injection boundary; portal Response 2 falls back',issue=671)
