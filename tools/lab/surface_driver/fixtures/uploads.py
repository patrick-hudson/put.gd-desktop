"""Seed real operation/history state through the production editor pipeline."""
from ..api import RecipeBlocked, RecipeFailed
from .base import Fixture
from .destinations import control

TERMINAL={'completed','failed','partial','cancelled','interrupted'}


def track(scope, method, identity):
    def cleanup():
        reply=scope.ctx.cli(method, __import__('json').dumps({'id':identity}))
        # Already completed/cancelled/cleared is harmless; transport failure isn't.
        if reply['returncode'] and not reply.get('stdout'):
            raise RecipeFailed('fixture operation cleanup transport unavailable')
        if method == 'operation.cancel':
            scope.ctx.wait(lambda:scope.ctx.call('operation.inspect',{'id':identity}).get('state') in TERMINAL,
                           'owned operation settles before fixture cleanup',timeout=20)
    scope.defer('owned '+method,cleanup,priority=100)


def submit(scope, *, remote=False, local_failure=False, active=False, failure=False, cancelled=False):
    ctx=scope.ctx
    ctx.fixture('app-profile')
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
    operation=ctx.call('editor.open',{'source':'file','path':source.path})['operationId']
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
            ctx.call('operation.cancel',{'id':operation})
            state=ctx.wait(lambda:(s if (s:=ctx.call('operation.inspect',{'id':operation}))['state'] in TERMINAL else None),'fixture cancellation',timeout=30)
            control(scope)
    else:
        state=ctx.wait(lambda:(s if (s:=ctx.call('operation.inspect',{'id':operation}))['state'] in TERMINAL else None),'fixture terminal operation',timeout=40)
        if not (local_failure or failure) and state['state']!='completed':
            raise RecipeFailed('synthetic fixture operation ended '+state['state'])
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
    uri=scope.root+'/clipboard-uris'
    from urllib.parse import quote
    scope.write(uri,('\r\n'.join('file://'+quote(p) for p in paths)+'\r\n').encode())
    if ctx.profile['session']=='wayland':
        args=['wl-copy','--foreground','--type','text/uri-list']
    else:
        args=['xclip','-quiet','-selection','clipboard','-target','text/uri-list']
    code='import os\nf=os.open('+repr(uri)+',os.O_RDONLY);os.dup2(f,0)\nos.execvp('+repr(args[0])+','+repr(args)+')'
    return scope.launch(['python3','-c',code],name='file-clipboard')


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
