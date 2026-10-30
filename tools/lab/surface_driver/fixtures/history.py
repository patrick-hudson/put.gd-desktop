"""Independent production rows; no cloned IDs or fabricated SQLite journals."""
from .base import Fixture
from .uploads import submit


def pair(scope):
    rows=[submit(scope) for _ in range(2)]
    return Fixture(ids=[r.id for r in rows],bytes=sum(i['artifact'].get('bytes',0) for r in rows for i in r.state.get('items',[])))


def paging(scope):
    rows=[submit(scope) for _ in range(51)]
    return Fixture(total=len(rows),ids=[r.id for r in rows])


def rows(scope):
    image=submit(scope)
    # Seed only truthful categories. Empty categories exercise their empty UI;
    # media/text import support can add their own real operations later.
    failed=submit(scope,local_failure=True)
    return Fixture(image={'id':image.id,'name':image.name},category_counts={'':2,'images':2,'videos':0,'text+other':0,'failed':1},
                   ids=[image.id,failed.id])


def active(scope):
    scope.ctx.fixture('loopback-s3')
    scope.patch({'limits':{'concurrentUploads':2}})
    rows=[submit(scope,remote=True,active=True) for _ in range(2)]
    return Fixture(operation_ids=[r.operation_id for r in rows])


def saved_copy(scope):
    ctx=scope.ctx
    ctx.fixture('file-dialog-fallback')
    row=submit(scope)
    path=scope.root+'/out/saved-copy.png'
    ctx.view('ui.show',{'view':'history'})
    ctx.call('ui.history',{'action':'select','ids':[row.id]})
    ctx.call('ui.fileDialog',{'dialog':'historySaveAs','action':'open','id':row.id})
    ctx.wait(lambda:ctx.call('ui.fileDialog',{'dialog':'historySaveAs','action':'inspect'}).get('open'),
             'fixture Save a copy chooser',timeout=15)
    ctx.keys('Ctrl+L');ctx.type_text(path);ctx.keys('Return')
    state=ctx.wait(lambda:(s if (s:=ctx.call('ui.history',{'action':'inspect'})).get('copyJob',{}).get('status')=='saved' else None),
                   'production Save a copy completes',timeout=30)
    if state.get('savedPath')!=path:
        from ..api import RecipeFailed
        raise RecipeFailed('Save a copy completed at an unexpected fixture path')
    scope.python('import hashlib\nfrom pathlib import Path\nassert hashlib.sha256(Path('+repr(row.path)+').read_bytes()).digest()==hashlib.sha256(Path('+repr(path)+').read_bytes()).digest()')
    return Fixture(row,path=path)
