#!/usr/bin/env python3
"""Replay numeric live-S3 evidence. Never manufactures a compositor test.

Run the recorded native workload first. This evaluator reads immutable numeric
samples, actual upload/public-paste receipts and exact cleanup results. It does
not contact S3 or read credentials/payloads. Suitable for saved-evidence CI.
"""
import argparse,collections,hashlib,json,xml.etree.ElementTree as ET
from pathlib import Path

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--bundle',type=Path,required=True);a=ap.parse_args();root=a.bundle
    c=json.loads((root/'baseline102.json').read_text());params=json.loads((root/'parameters.json').read_text());rows=[json.loads(l) for l in (root/'samples.jsonl').read_text().splitlines()]
    assert params['pid']==c['resident']['pid'] and params['startTicks']==c['resident']['startTicks'] and params['executableSha256']==c['resident']['sha256']
    assert params['build']==c['build'] and not c['build']['dirty'];complete=json.loads((root/'completed.json').read_text());assert complete['residentIdentityUnchanged']
    quiet=[r for r in rows if r['stage']==c['quietStage']];assert quiet
    warm=quiet[0]['elapsedSeconds']+5;quiet=[r for r in quiet if r['elapsedSeconds']>=warm];assert quiet
    chosen=[]
    for r in quiet:
        chosen.append(r)
        if r['elapsedSeconds']-quiet[0]['elapsedSeconds']>=300:break
    duration=chosen[-1]['elapsedSeconds']-chosen[0]['elapsedSeconds'];assert duration>=300,'quiet_window_incomplete'
    assert max(b['elapsedSeconds']-a['elapsedSeconds'] for a,b in zip(chosen,chosen[1:]))<=2.5,'sample_gap'
    def resident(r):
        found=[p for p in r['family'] if p['pid']==c['resident']['pid'] and p['startTicks']==c['resident']['startTicks']];assert len(found)==1;return found[0]
    rss=lambda r:sum(p['memoryBytes']['Rss'] for p in r['family'])
    swap=lambda r:sum(p['memoryBytes'].get('Swap',0) for p in r['family'])
    diagnostics=[r['diagnostics'] for r in chosen if 'diagnostics' in r];assert len(diagnostics)>=29
    expected=set(c['operationIds']);verified=set();region_ids=set();counts=collections.Counter();uploaded=0
    for receipt in sorted(root.glob('batch-*/receipt.json')):
        b=json.loads(receipt.read_text())
        if not b['passed'] or not set(b.get('operations',[]))<=expected:continue
        for index,ident in enumerate(b['operations']):
            assert ident not in verified;initial=json.loads((receipt.parent/('operation-'+str(index)+'.json')).read_text());op=json.loads((root/'final-operations'/(ident+'.json')).read_text());v=json.loads((receipt.parent/('verification-'+str(index)+'.json')).read_text());assert initial['id']==op['id']==ident and initial['state']==op['state']=='completed' and op['copyStatus']=='copied';assert initial['items']==op['items']
            assert v['independentNativeUrlPaste'] and len(v['public'])==len(op['items'])
            for item,public in zip(op['items'],v['public']):
                assert item['uploadStatus']=='uploaded';identity=item['receipt']['objectReference'];assert identity['key'].startswith(c['prefix']) and '/' not in identity['key'];assert public['publicExactBytesMatched'] and identity['bytes']==public['bytes'] and identity['sha256']==public['sha256'];uploaded+=1
            counts[b['kind']]+=1;verified.add(ident)
            if b['kind']=='region':region_ids.add(ident)
    assert expected==verified and dict(counts)=={'clipboard-text':11,'clipboard-image':1,'clipboard-files':1,'region':15,'window':1,'desktop':1} and uploaded==31
    pixel_proof=json.loads((root/'public-region-pixel-oracle.json').read_text());cases=pixel_proof['cases']
    assert pixel_proof['passed'] is True and len(cases)==15 and {p['operationId'] for p in cases}==region_ids
    assert all(p['passed'] is True and p['width']==80 and p['height']==60 and p['everyPixelRgb']==[220,30,40] for p in cases),'region_pixel_evidence_missing_or_mismatched'
    for receipt in root.glob('batch-*/receipt.json'):
        batch=json.loads(receipt.read_text())
        if batch['passed'] and batch.get('kind') in ('window','desktop'):assert len(batch['publicIndependentFrameOracles'])==batch['count'] and all(v['passed'] for v in batch['publicIndependentFrameOracles'])
    deletion=json.loads((root/'cleanup-completed.json').read_text());assert deletion['state']=='completed';items=deletion['items'];assert {i['historyId'] for i in items}==set(c['historyIds']);assert len(items)==uploaded
    assert all(i['status']=='removed' and i['progress']['phase']=='done' for i in items)
    tests=ET.parse(root/'baseline-dev.xml').getroot();assert all(x.attrib.get('failures','0')=='0' and x.attrib.get('errors','0')=='0' for x in tests.iter('testsuite'))
    stages=collections.defaultdict(list)
    for r in rows:stages[r['stage']].append(r)
    phases=[{'stage':k,'samples':len(v),'lastResidentRssMiB':resident(v[-1])['memoryBytes']['Rss']/1048576,'lastResidentSwapMiB':resident(v[-1])['memoryBytes'].get('Swap',0)/1048576} for k,v in stages.items()]
    ticks=resident(chosen[-1])['cpuTicks']-resident(chosen[0])['cpuTicks'];cpu=ticks/params['clockTicksPerSecond']/duration*100
    result={'schemaVersion':1,'scope':'Measured gnome-wayland VM live-S3 workload through production CLI/native acquisition; saved-evidence replay; not a fresh desktop test or full native-hotkey matrix','sourceCommit':c['build']['gitCommit'],'version':c['build']['version'],'post_s3_idle_rss_mib':max(rss(r) for r in chosen)/1048576,'post_s3_idle_rss_plus_swap_mib':max(rss(r)+swap(r) for r in chosen)/1048576,'idle_cpu_percent_one_core':cpu,'max_swap_mib':max(swap(r) for r in chosen)/1048576,'quiet_seconds':duration,'quiet_family_max':max(len(r['family']) for r in chosen),'quiet_active_operations_max':max(d['activeOperations'] for d in diagnostics),'quiet_ui_pid_max':max(d['ui']['pid'] for d in diagnostics),'resident_identity_unchanged':True,'build_clean':True,'all_six_live_source_types_verified':True,'public_bytes_and_native_url_paste_verified':True,'exact_scoped_cleanup_verified':True,'confirmed_remote_objects':uploaded,'workload_operation_counts':dict(counts),'quiet_samples':len(chosen),'diagnostic_samples':len(diagnostics),'phases':phases,'cold_last_rss_mib':resident(stages['cold'][-1])['memoryBytes']['Rss']/1048576,'final_pss_mib':resident(chosen[-1])['memoryBytes']['Pss']/1048576,'final_anonymous_mib':resident(chosen[-1])['memoryBytes']['Anonymous']/1048576,'reproduction_gate':max(rss(r)+swap(r) for r in chosen)>40*1048576}
    assert result['quiet_family_max']==1 and result['quiet_active_operations_max']==0 and result['quiet_ui_pid_max']==0 and cpu<.5
    (root/'evaluated-result.json').write_text(json.dumps(result,indent=2)+'\n');assert json.loads((root/'evaluated-result.json').read_text())==result
    print(json.dumps(result))

if __name__=='__main__':main()
