"""Production seed rows and guarded, explicitly labelled local paging clones."""
from .base import Fixture
from .uploads import submit


def pair(scope):
    rows=[submit(scope) for _ in range(2)]
    return Fixture(ids=[r.id for r in rows],bytes=sum(i['artifact'].get('bytes',0) for r in rows for i in r.state.get('items',[])))


def grow_history(scope, total):
    """Reuse the guarded local-only terminal-fixture cloner, with app stopped."""
    from pathlib import Path
    import json
    helper=scope.root+'/native_acceptance.py'
    scope.write(helper,(Path(__file__).resolve().parents[2]/'native_acceptance.py').read_bytes())
    # The cloner checks a complete local-only config alongside the isolated
    # journal. It never edits the resident's accepted configuration.
    from .profile import seeded_config
    scope.write(scope.root+'/config/putgd/config.json',json.dumps(seeded_config(scope.root)).encode())
    argv=['python3',helper,'--output',scope.root+'/paging-growth.json','grow',
          '--fixture-root',scope.root,'--db',scope.root+'/data/putgd/history.sqlite',
          '--runtime',scope.ctx.target.root+'/runtime','--to',str(total)]
    result={}
    def grow():
        raw=scope.python('import subprocess\np=subprocess.run('+repr(argv)+',capture_output=True,text=True,timeout=30)\nassert p.returncode==0\nprint(p.stdout)')
        result.update(json.loads(raw)['result'])
    scope.restart({'PUTGD_RECORD_KEEP_WORKING':scope.root+'/working'},before=grow)
    return result


def paging(scope):
    submit(scope)
    growth=grow_history(scope,51)
    if growth.get('historyRows')!=51 or growth.get('independentOperations')!=51:
        from ..api import RecipeFailed
        raise RecipeFailed('guarded history growth did not create independent rows')
    return Fixture(total=51,synthetic_clones=True,growth=growth)


def rows(scope):
    image=submit(scope)
    # Seed only truthful categories. Empty categories exercise their empty UI;
    # media/text import support can add their own real operations later.
    failed=submit(scope,remote=True,failure=True)
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
