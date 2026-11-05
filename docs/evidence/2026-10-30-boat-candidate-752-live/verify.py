import hashlib,json,subprocess,time
from pathlib import Path
r=Path(__file__).resolve().parent
load=lambda p:json.loads(p.read_text())
meta=load(r/'concurrent.json');interruption=load(r/'interruption.json')
audit=[json.loads(s) for s in (r/'commands.jsonl').read_text().splitlines()]
new=[x for x in audit if 'new' in x['argv']]
assert len(new)==21,len(new)
assert all('--no-env' in x['argv'] and '--ttl' in x['argv'] for x in new)
assert not any('snapshots' in x['argv'] and 'delete' in x['argv'] for x in audit)
sets={};results={};deletion_timings=[]
for name in ['run-a','run-b','interrupted']:
 m=load(r/name/'manifest.json')
 assert m['commit']==meta['source']
 assert len(m['artifacts'])==7
 assert m['passed']==(name=='run-b'),(name,m)
 if name=='run-a':
  assert [(a['artifact'],a['state']) for a in m['artifacts'] if a['state']!='passed']==[('ubuntu26.04','failed')]
 pid=(interruption if name=='interrupted' else next(x for x in meta['runs'] if x['name']==name))['pid']
 ids=set()
 for a in m['artifacts']:
  assert a['cleanup']['verified'] is True
  assert a['cleanup']['finalState']=='absent'
  ids.update(a['cleanup']['deletedIds'])
  if 'sandboxId' in a: assert a['sandboxId'] in ids
  for req in a['cleanup']['requests']:
   op=req['operation'];started=next(x['time'] for x in audit if x['controller']==pid and 'delete' in x['argv'] and req['sandboxId'] in x['argv'])
   deletion_timings.append({'run':name,'artifact':a['artifact'],'id':req['sandboxId'],'initialStatus':op['status'],'deleteInvokedAt':started,'absenceVerifiedAt':a['cleanup']['verifiedAt'],'secondsToVerifiedAbsence':round(a['cleanup']['verifiedAt']-started,3),'noIdRecoveryWindowIncluded':'sandboxId' not in a})
 assert len(ids)==7,(name,ids)
 deletes={x['argv'][x['argv'].index('delete')+1] for x in audit if x['controller']==pid and 'delete' in x['argv']}
 assert deletes==ids,(name,deletes,ids)
 sets[name]=ids
 if name!='interrupted':
  with (r/f'{name}-sha256-verify.txt').open('w') as f:subprocess.run(['sha256sum','-c','SHA256SUMS'],cwd=r/name,stdout=f,stderr=subprocess.STDOUT,check=True)
  sums=(r/name/'SHA256SUMS').read_text().splitlines()
  results[name]={'passed':m['passed'],'nonce':m['runNonce'],'fileChecksumsVerified':len(sums),'sandboxIds':sorted(ids)}
  fixture=next((r/name/'lab-fixture/artifacts').glob('putgd-lab-fixture*.deb'))
  p=subprocess.run(['dpkg','-I',str(fixture)],capture_output=True,text=True,check=True)
  (r/f'{name}-fixture-dpkg-I.txt').write_text(p.stdout)
  assert 'Description: Disposable lab fixture build of put.gd; test use only' in p.stdout
  results[name]['fixtureSha256']=hashlib.file_digest(fixture.open('rb'),'sha256').hexdigest()
assert all(not a.intersection(b) for n,a in sets.items() for m,b in sets.items() if n!=m)
recovery=load(r/'ubuntu26-recovery/recovery.json')
assert recovery['state']=='passed'
assert recovery['source']==meta['source']
assert recovery['sandboxId'] in sets['run-a']
original_target=load(r/'run-a/ubuntu26.04/receipt.json')
assert recovery['containerImage']==original_target['containerImage']
assert (r/'run-a/ubuntu26.04/recipe.sh').read_text() in (r/'ubuntu26-recovery/recovery.sh').read_text()
for item in recovery['files']:
 assert hashlib.file_digest((r/'ubuntu26-recovery/artifacts'/item['path']).open('rb'),'sha256').hexdigest()==item['sha256']
results['ubuntu26Recovery']={'state':'passed','sandboxId':recovery['sandboxId'],'fileChecksumsVerified':len(recovery['files']),'originalControllerPreservedFailed':True}
listing=json.loads(subprocess.check_output(['boat','--no-update','--json','list','--all'],text=True))
assert not listing.get('pageInfo',{}).get('hasMore')
owned=set.union(*sets.values())
remaining=owned.intersection(x['id'] for x in listing['sandboxes'])
assert not remaining,remaining
nonces={load(r/n/'manifest.json')['runNonce'] for n in sets}
assert not any(any((x.get('name') or '').endswith(nonce) for nonce in nonces) for x in listing['sandboxes'])
initial=load(r/'initial-list.json')
unrelated={x['id'] for x in initial['sandboxes']}
all_deletes={x['argv'][x['argv'].index('delete')+1] for x in audit if 'delete' in x['argv']}
assert not unrelated.intersection(all_deletes)
(r/'deletion-timings.json').write_text(json.dumps(deletion_timings,indent=2)+'\n')
(r/'acceptance.json').write_text(json.dumps({'verifiedAt':time.time(),'source':meta['source'],'runs':results,'disjointOwnedSets':True,'all21StartsAccountedFor':True,'all21OwnedIdsAbsent':sorted(owned),'listPageInfo':listing.get('pageInfo'),'unrelatedSandboxesAtFinalList':len(listing['sandboxes']),'unrelatedInitialIdsNeverDeleted':True,'namedSnapshotsNeverDeleted':True},indent=2)+'\n')
print(json.dumps(results,indent=2))
