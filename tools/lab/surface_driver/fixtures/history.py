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
    rows=[submit(scope,remote=True,active=True) for _ in range(2)]
    return Fixture(operation_ids=[r.operation_id for r in rows])


def saved_copy(scope):
    row=submit(scope)
    # History's Save a copy is driven by the recipe that tests it. A retained
    # synthetic independent copy is enough for preservation witnesses.
    path=scope.root+'/out/saved-copy.png'
    scope.python('import shutil\nshutil.copyfile('+repr(row.path)+','+repr(path)+')')
    return Fixture(row,path=path)
