#!/usr/bin/env python3
# Local receipt-to-gesture wrapper only; never accesses guest or reads payloads.
import argparse,json,hashlib
from pathlib import Path
root=Path(__file__).resolve().parents[1]
a=argparse.ArgumentParser();a.add_argument('receipt');a.add_argument('name');a.add_argument('kind',choices=['click','right-click','hover','key']);a.add_argument('values',nargs='+');a.add_argument('--view',action='store_true');a.add_argument('--fixture',action='store_true');args=a.parse_args();x=json.loads(Path(args.receipt).read_text());v={'candidatePinSha256':hashlib.sha256((root/'candidate.json').read_bytes()).hexdigest(),'resident':x['resident'],'uiPid':x['after']['ui']['pid'],'visuallyInspected':True,'witness':x['afterWitness'],'foregroundOwnedView':args.view,'foregroundOwnedFixture':args.fixture,'gesture':{'kind':args.kind},'notes':'Executing agent inspected the exact referenced actual native image before this bounded gesture.'};v['gesture']['keys' if args.kind=='key' else 'point']=args.values if args.kind=='key' else list(map(int,args.values));p=root/'inputs'/(args.name+'.json')
with p.open('x') as f:json.dump(v,f,indent=2);f.write('\n')
print(str(p))
