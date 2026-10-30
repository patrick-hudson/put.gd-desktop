"""Settings starting states and reversible faults in the private app profile."""
import copy
import stat

from ..api import RecipeBlocked
from .base import Fixture


def settings(scope, state='default'):
    profile=scope.ctx.fixture('app-profile')
    if state=='no-output':
        scope.patch({'archive':{'keepUploads':False},
                     'captureDefaults':{'destination':'upload','profileId':None,'publicationMode':'default'},
                     'actions':{a:{'destination':'default','profileId':None,'publicationMode':'default'} for a in ('region','window','desktop','clipboard','edit')}})
    elif state=='multi-profile':
        service=scope.ctx.fixture('destinations')
        profiles=[copy.deepcopy(service.profile) for _ in range(3)]
        for n,p in enumerate(profiles):p.update(id='synthetic-'+str(n),label='Synthetic '+str(n))
        scope.patch({'profiles':profiles,'captureDefaults':{'destination':'upload','profileId':profiles[0]['id'],'publicationMode':'default'},
                     'actions':{a:{'destination':'default','profileId':None,'publicationMode':'default'} for a in ('region','window','desktop','clipboard','edit')}})
        return Fixture(profile,state=state,profiles=profiles,profile_ids=[p['id'] for p in profiles],
                       loopback=True,share_profile_id=profiles[0]['id'],publication_modes=['default','share'],
                       private_supported=False)
    return Fixture(profile,state=state,recovery_actions=['chooseCaptureDestination','enableLocalCopies'] if state=='no-output' else [])


def save_failure(scope):
    """Fail QSaveFile's real directory write, preserving the accepted document.

    Call restore() before the person's real Retry input. No resident response,
    footer, conflict flag or UI error is manufactured by this fixture.
    """
    profile=scope.ctx.fixture('app-profile')
    scope.ctx.call('ui.show',{'view':'settings'})
    parent=profile.config_path.rsplit('/',1)[0]
    mode=scope.json('''import json,os,stat
from pathlib import Path
p=Path(PARENT);s=p.lstat()
assert p.resolve()==p and stat.S_ISDIR(s.st_mode) and s.st_uid==os.getuid()
assert p.is_relative_to(Path(ROOT)) and not s.st_mode&0o077
assert os.geteuid()!=0
print(json.dumps(stat.S_IMODE(s.st_mode)))
'''.replace('PARENT',repr(parent)).replace('ROOT',repr(scope.ctx.target.root)))
    def restore():
        scope.python('''import os,stat
from pathlib import Path
p=Path(PARENT);s=p.lstat()
assert p.resolve()==p and stat.S_ISDIR(s.st_mode) and s.st_uid==os.getuid()
os.chmod(p,MODE)
'''.replace('PARENT',repr(parent)).replace('MODE',repr(mode)))
    scope.defer('settings directory write permission',restore,priority=500)
    scope.python('import os\nos.chmod('+repr(parent)+','+repr(mode&~stat.S_IWUSR)+')')
    return Fixture(profile,restore=restore,retry=restore,failed_save=True,
                   observe=lambda:observe(scope),external_change=lambda patch:external_change(scope,patch))


def external_change(scope,patch):
    from .events import config_change
    return config_change(scope,patch)


def observe(scope):
    """Read actual, nonsecret footer/draft state and its live accessible labels."""
    ctx=scope.ctx
    state=ctx.call('ui.viewState')
    pid=ctx.geometry('settings')['pid']
    rows=ctx.accessible({'owned':True,'pid':pid,'visible':True,'showing':True})
    footer=ctx.call('ui.diagnostics',{'action':'inspect'}).get('autosave')
    return Fixture(settings=state.get('settings',{}),nodes=rows,footer=footer,
                   retry=[row for row in rows if row.get('name')=='Retry saving' and row.get('role') in ('push button','button')])


def observations(scope):
    scope.ctx.fixture('app-profile')
    def disclosure(name, *, window='settings'):
        # IDs are exact where exported; on Qt 6.4 the catalog supplies the
        # accessible label and role. Never read a password field or guess text.
        ctx=scope.ctx
        candidates=ctx.catalog.candidates(name,window,ctx._captures.get(window))
        from ..catalog import control_labels
        labels={label for candidate in candidates for label in control_labels(candidate)}
        title=ctx.geometry(window)
        rows=ctx.accessible({'owned':True,'pid':title['pid'],'visible':True,'showing':True})
        matches=[row for row in rows if row.get('objectName')==name or
                 (row.get('name') in labels and row.get('role') in ('toggle button','push button','button','check box'))]
        if len(matches)!=1:
            raise RecipeBlocked('disclosure must have one owned accessible control',issue=681)
        row=matches[0]
        states=row.get('states',{})
        # Ui::disclosure is a checkable QToolButton. AT-SPI exports both keys,
        # with expanded=False even while checked=True on Qt 6.4.
        expanded=states.get('checked',states.get('expanded'))
        if expanded is None:
            raise RecipeBlocked('disclosure exports neither expanded nor checked state',issue=681)
        return Fixture(control=row,expanded=expanded,text=[r.get('text') or r.get('name','') for r in rows
                      if not r.get('protected') and r.get('role') in ('label','text','paragraph','static')])
    def recording_status():
        rows=scope.ctx.accessible({'owned':True,'name':'Shortcut recording status','visible':True,'showing':True})
        if len(rows)>1:raise RecipeBlocked('shortcut recording status is ambiguous',issue=681)
        if not rows:return None
        row=rows[0]
        text=row.get('text') or row.get('description')
        if row.get('protected') or not text:
            raise RecipeBlocked('visible shortcut recording status exports no current accessible text or description',issue=862)
        return Fixture(row,text=text)
    return Fixture(observe=lambda:observe(scope),disclosure=disclosure,recording_status=recording_status)


def action_routes(scope):
    value=scope.ctx.fixture('settings-multi-profile')
    def route(action,profile_id=None,publication_mode='default'):
        if action not in ('region','window','desktop','clipboard','edit','record'):
            raise ValueError('unknown capture action')
        if profile_id is not None and profile_id not in value.profile_ids:
            raise ValueError('route must use an owned loopback profile')
        if publication_mode not in ('default','share'):
            raise ValueError('loopback S3 supports default/share publication')
        if profile_id is None:
            # Removing an exception restores inheritance, including the
            # optional recording route fields, by a full revision-guarded save.
            doc=scope.ctx.call('settings.get')['document']
            if action=='record':
                for key in ('destination','profileId','publicationMode'):doc['actions'][action].pop(key,None)
            else:doc['actions'][action].update(destination='default',profileId=None,publicationMode='default')
            return scope.save(doc)
        return scope.patch({'actions':{action:{'destination':'upload','profileId':profile_id,'publicationMode':publication_mode}}})
    return Fixture(value,add=route,remove=lambda action:route(action),
                   observe=lambda:scope.ctx.call('ui.viewState').get('settings',{}))


def diagnostics_upload(scope):
    value=scope.ctx.fixture('loopback-s3')
    scope.patch({'captureDefaults':{'destination':'upload','profileId':value.profile_id,'publicationMode':'default'},
                 'actions':{'desktop':{'destination':'upload','profileId':value.profile_id}}})
    return Fixture(isolated=True,loopback=True,profileId=value.profile_id)
