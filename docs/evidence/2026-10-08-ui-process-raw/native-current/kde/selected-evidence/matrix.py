#!/usr/bin/env python3
import argparse,json,time,xml.etree.ElementTree as ET
from common import ROOT,OUT,RUN,now,save,sha,source,load_lab,assert_resident,ui
lab=load_lab();expected=assert_resident(lab);recipe=json.loads((ROOT/'work/memory-surfaces/kde/baseline-parameters.json').read_text());output=OUT/'matrix.json';assert not output.exists()
receipt={'schemaVersion':1,'startedUtc':now(),'sourceCommit':source()['commit'],'runningApp':expected,'runnerSha256':sha(ROOT/'tools/lab/lab.py'),'vncPort':5934,'attempts':[],'ok':False}
try:
 pairs=[(x,'qt') for x in ['region','window','desktop','clipboard-text','clipboard-image','clipboard-files','region-cancel-escape','region-cancel-right-click','region-cancel-zero-area']]+[(x,'gtk') for x in ['clipboard-image','clipboard-files','clipboard-text']]
 before=lab.cli(RUN,'operation.recent')['operations'];receipt['beforeOperationIds']=[op['id'] for op in before]
 for scenario,client in pairs:
  print('RUN',scenario,client,flush=True)
  helper_before=ui(lab);assert helper_before['pid']==0,'Native matrix requires absent UI helper'
  result=lab.fixture_run(argparse.Namespace(run=str(RUN),scenario=scenario,settings=None,history=False,clipboard_client=client,timeout=90,keep=True))
  result['uiBefore']=helper_before;result['uiAfter']=ui(lab)
  params_path=RUN/result['evidenceDirectory']/'parameters.json';checks={}
  if params_path.exists():
   parameters=json.loads(params_path.read_text())
   for key in ['profile','destination','hotkeys','displayLayout','qtVersion','acceleration','goldenSha256','timingSeconds','timeoutSeconds']:checks[key]=parameters.get(key)==recipe[key]
   checks['selectionShape']=all(parameters.get('selection',{}).get(k)==v for k,v in recipe['selectionShape'].items())
  result['baselineParameterChecks']=checks;receipt['attempts'].append(result);save(output,receipt)
  print('RESULT',scenario,client,result.get('ok'),flush=True)
  if not result.get('ok') or not checks or not all(checks.values()):raise RuntimeError('Native case failed or parameters drifted; evidence preserved without retry')
  assert result['uiAfter']['pid']==0 and assert_resident(lab)==expected
 after=lab.cli(RUN,'operation.recent')['operations'];receipt['afterOperationIds']=[op['id'] for op in after]
 assert len(set(receipt['afterOperationIds'])-set(receipt['beforeOperationIds']))==12
 receipt['ok']=True
except Exception as error:receipt['error']=str(error);raise
finally:
 receipt['finishedUtc']=now();save(output,receipt)
 suite=ET.Element('testsuite',name='kde-u11-native-matrix',tests=str(len(receipt['attempts'])),failures=str(sum(not x.get('ok') for x in receipt['attempts'])))
 for attempt in receipt['attempts']:
  case=ET.SubElement(suite,'testcase',name=attempt['scenario']+'-'+attempt.get('clipboardClient','qt'),classname='kde-wayland')
  if not attempt.get('ok'):ET.SubElement(case,'failure',message=attempt.get('error','incomplete'))
 ET.ElementTree(suite).write(OUT/'matrix.xml',encoding='utf-8',xml_declaration=True)
