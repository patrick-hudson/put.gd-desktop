"""Seed real operation/history state through the production editor pipeline."""
from ..api import RecipeBlocked, RecipeFailed
from .base import Fixture
from .destinations import control

TERMINAL={'completed','failed','partial','cancelled','interrupted'}
# Exceeds the loopback client's 30 s request + 25 s low-speed timeouts,
# including a margin for cancellation delivery and journal persistence.
SETTLE_TIMEOUT=75


def cancel(scope, identity):
    if 'loopback-s3' in scope.values:
        control(scope)  # Release the server BEFORE asking the SDK to cancel.
    reply=scope.ctx.cli('operation.cancel', __import__('json').dumps({'id':identity}))
    if reply['returncode'] and not reply.get('stdout'):
        raise RecipeFailed('fixture operation cleanup transport unavailable')
    return scope.ctx.wait(lambda:(s if (s:=scope.ctx.call('operation.inspect',{'id':identity})).get('state') in TERMINAL else None),
                          'owned operation settles before fixture cleanup',timeout=SETTLE_TIMEOUT)


def track(scope, method, identity):
    def cleanup():
        if method == 'operation.cancel':
            cancel(scope,identity)
            return
        reply=scope.ctx.cli(method, __import__('json').dumps({'id':identity}))
        # Already completed/cancelled/cleared is harmless; transport failure isn't.
        if reply['returncode'] and not reply.get('stdout'):
            raise RecipeFailed('fixture operation cleanup transport unavailable')
    scope.defer('owned '+method,cleanup,priority=100)


def submit(scope, *, remote=False, local_failure=False, active=False, failure=False, cancelled=False):
    ctx=scope.ctx
    ctx.fixture('app-profile')
    if remote and ctx.destination not in ('local','s3'):
        raise RecipeBlocked('this slice publishes only to loopback S3; live provider adapters are deferred',issue=669)
    if remote:
        service=ctx.fixture('loopback-s3')
        scope.patch({'actions':{'edit':{'destination':'upload','profileId':service.profile_id}}})
        if active or cancelled:control(scope,stallSeconds=120)
        elif failure:control(scope,failWrites=True)
    else:
        scope.patch({'actions':{'edit':{'destination':'local','profileId':None}}})
    if local_failure:
        scope.write(scope.root+'/blocked',b'synthetic local-save blocker')
        scope.patch({'archive':{'root':scope.root+'/blocked','keepUploads':True}})
    source=ctx.fixture('image')
    source_path=scope.root+'/source-'+__import__('uuid').uuid4().hex[:8]+'.png'
    scope.python('import shutil\nshutil.copyfile('+repr(source.path)+','+repr(source_path)+')')
    operation=ctx.call('editor.open',{'source':'file','path':source_path})['operationId']
    track(scope,'editor.cancel',operation)
    ctx.wait(lambda:ctx.call('editor.inspect',{'id':operation}),'fixture editor ready',timeout=30)
    ctx.call('editor.submit',{'id':operation})
    track(scope,'operation.cancel',operation)
    if active or cancelled:
        def in_flight():
            state=ctx.call('operation.inspect',{'id':operation})
            return state if any(i.get('uploadStatus')=='uploading' for i in state.get('items',[])) else None
        state=ctx.wait(in_flight,'fixture upload in flight',timeout=30)
        if cancelled:
            state=cancel(scope,operation)
    else:
        state=ctx.wait(lambda:(s if (s:=ctx.call('operation.inspect',{'id':operation}))['state'] in TERMINAL else None),'fixture terminal operation',timeout=40)
        if not (local_failure or failure) and state['state']!='completed':
            errors=[state.get('error'),*(i.get('error') for i in state.get('items',[]))]
            codes=sorted({e['code'] for e in errors if isinstance(e,dict) and e.get('code')})
            raise RecipeFailed('synthetic fixture operation ended '+state['state']+''.join(': '+c for c in codes))
    rows=ctx.call('history.list',{'limit':100}).get('records',[])
    row=next((r for r in rows if r.get('operationId')==operation),None)
    if not row:
        raise RecipeBlocked('production fixture has no history row')
    receipt=row.get('receipt') or {}
    link=receipt.get('directUrl') or None
    artifact=row.get('artifact') or {}
    name=row.get('name') or artifact.get('name') or source.path.rsplit('/',1)[-1]
    # Some versions expose local data only in detail; do not invent a path.
    detail=ctx.call('history.detail',{'id':row['id']})
    local=detail.get('local') or row.get('local') or {}
    value=Fixture(id=row['id'],history_id=row['id'],operation_id=operation,operationId=operation,
                  name=name,path=local.get('path'),link=link,row=Fixture(row,name=name),state=state,
                  expect={'kind':'s3' if remote else 'local','name':name,'preview':True,'url_host':'127.0.0.1','text':None})
    if failure:control(scope) # Remove the fault BEFORE the recipe's real Retry.
    if local_failure:
        scope.python('from pathlib import Path\np=Path('+repr(scope.root+'/blocked')+');p.unlink();p.mkdir(mode=0o700)')
    return value


def upload(scope):
    # Links always need an actual loopback publication, including destination=local.
    return submit(scope,remote=True)


def local_history(scope):return submit(scope)


def clipboard_files(scope, paths):
    """Own a native URI-list clipboard source; never read a prior clipboard."""
    ctx=scope.ctx
    if ctx.placement not in ('guest','boat') or (ctx.host=='boat' and not ctx.profile.get('guest')):
        raise RecipeBlocked('file clipboard fixture requires a disposable nested guest')
    from .clipboard import owner
    scope.initialize()
    uri=scope.root+'/clipboard-uris'
    from urllib.parse import quote
    scope.write(uri,('\r\n'.join('file://'+quote(p) for p in paths)+'\r\n').encode())
    return owner(scope,uri,'text/uri-list',name='file-clipboard')


def incomplete_multipart(scope):
    ctx=scope.ctx
    service=ctx.fixture('loopback-s3')
    scope.patch({'actions':{'clipboard':{'destination':'upload','profileId':service.profile_id}},
                 'limits':{'maxFileBytes':128*1024**2,'maxBatchBytes':128*1024**2,'pendingArtifactBytes':256*1024**2}})
    path=scope.root+'/synthetic-multipart.bin'
    # Sparse zeros are synthetic and remain a single uncompressed file import.
    scope.python('with open('+repr(path)+',"xb") as f:f.truncate(65*1024**2)')
    clipboard_files(scope,[path])
    control(scope,failParts=True,denyAbort=True)
    # Remove the fault before cleanup, so production cache.clear can abort its
    # exact retained upload while the loopback server/credentials still exist.
    scope.defer('multipart fault',lambda:control(scope),priority=200)
    operation=ctx.call('action.invoke',{'action':'clipboard'})['operationId']
    track(scope,'operation.cancel',operation)
    state=ctx.wait(lambda:(s if (s:=ctx.call('operation.inspect',{'id':operation}))['state'] in TERMINAL else None),'multipart write fault',timeout=45)
    if state['state'] not in ('failed','partial') or not state.get('items'):
        raise RecipeFailed('multipart fault did not produce a failed retained item')
    return Fixture(address='127.0.0.1',operationId=operation,itemId=state['items'][0]['artifact']['id'])
