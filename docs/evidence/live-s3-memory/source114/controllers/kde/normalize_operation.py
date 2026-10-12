#!/usr/bin/env python3
"""Whitelist saved operation metadata. No guest/network/credential access."""
import argparse,datetime,json,os,re
from pathlib import Path
STAGES={'name_head','object_head','current_head','upload_put','delete_versioning','delete_current_head','delete_marker','delete_version','delete_object'}
BOUNDS={'httpStatus':(-1,999),'sdkErrorType':(-1,65535),'curlCode':(-1,999),'elapsedMs':(0,86400000)}
CODES={'AccessControlListNotSupported','AccessDenied','InvalidAccessKeyId','SignatureDoesNotMatch','ExpiredToken','PreconditionFailed','ConditionalRequestConflict','InvalidRequest','ObjectLocked','SlowDown','RequestTimeout','InternalError','ServiceUnavailable','Unclassified'}
def normalized_error(error):
 code=error.get('code','');assert type(code)is str and (not code or re.fullmatch('[a-z][a-z0-9_]{0,95}',code));detail=error.get('details',{});assert type(detail)is dict;out={'code':code,'diagnostics':{}}
 for key,(lo,hi) in BOUNDS.items():
  if key in detail:
   value=detail[key];assert type(value)is int and lo<=value<=hi;out['diagnostics'][key]=value
 if 'sdkRetryable' in detail:assert type(detail['sdkRetryable'])is bool;out['diagnostics']['sdkRetryable']=detail['sdkRetryable']
 if 'requestStage' in detail:assert detail['requestStage'] in STAGES;out['diagnostics']['requestStage']=detail['requestStage']
 if 'serviceCode' in detail:assert detail['serviceCode'] in CODES;out['diagnostics']['serviceCode']=detail['serviceCode']
 return out
def normalize(op,commit):
 assert re.fullmatch('[0-9a-f]{40}',commit);assert re.fullmatch('[0-9a-f-]{36}',op['id']);assert op['action'] in {'region','window','desktop','clipboard'};assert op['state'] in {'queued','acquiring','running','completed','partial','failed','cancelled','interrupted'}
 out={'sourceCommit':commit,'operationId':op['id'],'action':op['action'],'state':op['state'],'error':normalized_error(op.get('error',{})),'items':[],'rawMessagesHeadersPayloadsIncluded':False}
 for index,item in enumerate(op.get('items',[])):
  assert type(item['uploadStatus'])is str and re.fullmatch('[a-z_]{1,32}',item['uploadStatus']);out['items'].append({'index':index,'uploadStatus':item['uploadStatus'],'error':normalized_error(item.get('error',{}))})
 return out
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('--operation',type=Path,required=True);a.add_argument('--source-commit',required=True);a.add_argument('--output',type=Path,required=True);args=a.parse_args();os.umask(0o077);out=normalize(json.loads(args.operation.read_text()),args.source_commit);out['normalizedUTC']=datetime.datetime.now(datetime.timezone.utc).isoformat()
 with args.output.open('x') as f:json.dump(out,f,indent=2);f.write('\n')
 print(json.dumps({'output':str(args.output),'rawValuesExcluded':True}))
