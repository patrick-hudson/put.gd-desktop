"""Settings and diagnostics starting states in the private app profile."""
from .base import Fixture


def settings(scope, state='default'):
    profile=scope.ctx.fixture('app-profile')
    if state=='no-output':
        scope.patch({'archive':{'keepUploads':False},'actions':{a:{'destination':'upload','profileId':None} for a in ('region','window','desktop','clipboard','edit')}})
    elif state=='multi-profile':
        service=scope.ctx.fixture('destinations')
        import copy
        profiles=[copy.deepcopy(service.profile) for _ in range(3)]
        for n,p in enumerate(profiles):p.update(id='synthetic-'+str(n),label='Synthetic '+str(n))
        scope.patch({'profiles':profiles})
    return Fixture(profile,state=state)


def diagnostics_upload(scope):
    value=scope.ctx.fixture('loopback-s3')
    scope.patch({'captureDefaults':{'destination':'upload','profileId':value.profile_id,'publicationMode':'default'},
                 'actions':{'desktop':{'destination':'upload','profileId':value.profile_id}}})
    return Fixture(isolated=True,loopback=True,profileId=value.profile_id)
