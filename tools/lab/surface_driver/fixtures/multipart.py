"""Expire a genuine owned multipart failure, then await startup auto-cleanup."""
import json

from ..api import RecipeBlocked, RecipeFailed
from .base import Fixture
from .destinations import control
from .uploads import incomplete_multipart


def expired(scope):
    ctx=scope.ctx
    value=incomplete_multipart(scope)
    service=ctx.fixture('loopback-s3')
    # Age only the real operation's creation time while its resident is stopped.
    # Upload identity/settings/journal remain exactly what the SDK persisted.
    database=scope._environment.get('XDG_DATA_HOME',scope.root+'/data')+'/putgd/history.sqlite'
    scope.patch({'retention':{'failedArtifactDays':1}})
    def age():
        scope.python('''import datetime,json,os,sqlite3,stat
from pathlib import Path
p=Path(DATABASE);s=p.lstat()
assert p.resolve()==p and p.is_relative_to(Path(ROOT)) and stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid()
with sqlite3.connect(p) as db:
 row=db.execute('SELECT data FROM operations WHERE id=?',(IDENTITY,)).fetchone();assert row
 op=json.loads(row[0]);assert op['id']==IDENTITY and op['state'] in ('failed','partial')
 assert any(item.get('uploadJournal') for item in op['items'])
 assert all(item.get('uploadStatus') not in ('uploading','unknown') for item in op['items'])
 op['created']=(datetime.datetime.now(datetime.timezone.utc)-datetime.timedelta(days=2)).isoformat(timespec='milliseconds').replace('+00:00','Z')
 for item in op['items']:
  item.get('error',{}).get('details',{}).pop('incompleteUploadReported',None)
 db.execute('UPDATE operations SET data=? WHERE id=?',(json.dumps(op,separators=(',',':')),IDENTITY))
'''.replace('DATABASE',repr(database)).replace('ROOT',repr(scope.root)).replace('IDENTITY',repr(value.operationId)))
    def listing():
        return scope.json('''import json,urllib.request,xml.etree.ElementTree as ET
root=ET.fromstring(urllib.request.urlopen(URL,timeout=5).read())
print(json.dumps([{'uploadId':node.findtext('{*}UploadId'),'key':node.findtext('{*}Key')} for node in root.findall('{*}Upload')]))
'''.replace('URL',repr(service.endpoint+'/surface-fixture?uploads')))
    before=listing()
    if not before:
        raise RecipeFailed('multipart fixture server has no retained upload')
    triggered=False
    def trigger():
        nonlocal triggered
        if triggered:
            raise ValueError('automatic-cleanup startup trigger is single-use')
        triggered=True
        # Startup schedules the actual production cleanup after 60 seconds.
        # There is no cache.clear, test clock, signal or synthetic notification.
        scope.restart({},before=age)
        current=ctx.call('settings.get')
        identity=ctx.call('setup.unlockCredentials',{'profileId':service.profile_id,
                          'expectedRevision':current['metadata']['observedRevision']})['setupId']
        ctx.wait(lambda:(s if (s:=ctx.call('setup.inspect',{'id':identity}))['state']!='running' else None),
                 'synthetic credentials after cleanup restart',timeout=30)
        def automatic():
            cleanup=ctx.call('diagnostics').get('artifactCleanup',{}).get('last') or {}
            if cleanup.get('trigger')=='automatic' and any(row.get('operationId')==value.operationId for row in cleanup.get('incompleteUploads',[])):
                return cleanup
        return ctx.wait(automatic,'authentic startup automatic multipart cleanup',timeout=100)
    def requests():
        return scope.json('import urllib.request\nprint(urllib.request.urlopen('+repr(service.endpoint+'/__events')+',timeout=5).read().decode())')
    return Fixture(value,automatic_cleanup=trigger,listing=listing,requests=requests,
                   initial_uploads=before,cleanup_eligible=True,automatic_delay_seconds=60)
