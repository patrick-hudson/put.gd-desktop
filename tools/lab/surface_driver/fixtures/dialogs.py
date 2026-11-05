"""Chooser launch environments are applied before the helper family starts."""
from ..api import RecipeBlocked
from .base import Fixture


def backend(scope, name):
    profile=scope.ctx.fixture('app-profile')
    env=scope.ctx.profile.get('dialogs',{}).get('backends',{}).get(name,{}).get('environment',{})
    env=dict(env)
    if any(k in env for k in ('DBUS_SESSION_BUS_ADDRESS','DISPLAY','WAYLAND_DISPLAY','XDG_RUNTIME_DIR')):
        raise RecipeBlocked('chooser fixture must preserve the native session buses and display',issue=671)
    # Exercise Escape on the chooser itself. A fresh diagnostics preview names
    # out/diagnostics before it exists; Nautilus shows a missing-folder alert
    # whose first Escape only dismisses the alert, without a portal Response.
    # Prepare this synthetic start folder inside the fixture's cleanup boundary.
    scope.python('from pathlib import Path\nPath('+repr(profile.output_dir+'/diagnostics')+
                 ').mkdir(mode=0o700,exist_ok=True)')
    env['PUTGD_FILE_DIALOG']='fallback' if name=='fallback' else ''
    scope.restart(env)
    def observe(dialog='outputFolder',field=None):
        if dialog not in ('editImage','historySaveAs','outputFolder','diagnosticsFolder'):
            raise ValueError('unknown production chooser')
        params={'dialog':dialog,'action':'inspect'}
        if field is not None:
            if dialog!='outputFolder' or field not in ('root','images','videos','text','other'):
                raise ValueError('unknown output folder field')
            params['field']=field
        return scope.ctx.call('ui.fileDialog',params)
    from .resident import identity
    value=Fixture(backend=name,environment=env,resident=lambda:identity(scope),observe=observe)
    # Availability of the real session portal is observational, never a fake.
    if name=='portal':
        result=scope.ctx.shell("gdbus call --session --dest org.freedesktop.portal.Desktop --object-path /org/freedesktop/portal/desktop --method org.freedesktop.DBus.Properties.Get org.freedesktop.portal.FileChooser version")
        if result['returncode']:
            return Fixture(value,available=False,reason='native session FileChooser portal unavailable')
    return Fixture(value,available=True,reason='')


def failure(scope):
    # Product has no production mechanism that deterministically returns Failed:
    # portal failures deliberately fall back. Refuse to kill the shared portal.
    raise RecipeBlocked('isolated chooser failure needs a production failure injection boundary; portal Response 2 falls back',issue=671)
