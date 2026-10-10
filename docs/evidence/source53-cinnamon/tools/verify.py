#!/usr/bin/env python3
"""Portable source53 numerical projection verifier. No original paths required."""
import argparse,hashlib,json,math,re
from pathlib import Path
MIB=1024**2
COMMIT='1ee42f32c61c3d93d88cc1d3dc624d9ad086bb70'
GATE_HASH='834071fcf867ac6be7a032a1e2a67228703046776820c854fef820387926fece'


def need(v,code):
    if not v:raise ValueError(code)


def approx(a,b,tol=1e-8):need(abs(a-b)<=tol,'numeric_summary_mismatch')


def number(v):need(type(v)in(int,float) and math.isfinite(v),'numeric_required');return v


def peak(samples,field):return max(number(x['totals'][field])for x in samples)


def percent95(values):
    need(bool(values),'samples_empty')
    return sorted(number(v)for v in values)[math.ceil(.95*len(values))-1]


def coverage(samples,min_span,max_gap,min_count):
    need(len(samples)>=min_count,'coverage_count_short')
    t=[number(x['elapsedSeconds'])for x in samples]
    gaps=[b-a for a,b in zip(t,t[1:])]
    need(gaps and min(gaps)>0 and max(gaps)<=max_gap and t[-1]-t[0]>=min_span,'coverage_inadequate')
    return {'sampleCount':len(t),'spanSeconds':t[-1]-t[0],'maxGapSeconds':max(gaps)}


def timing(d,params,ops):
    need(d['budgetPassed']is True and d['errorPresent']is False and len(d['samples'])==30,'timing_invalid')
    need(d['warmupInvocations']==5 and params['warmupInvocations']==5 and params['sampleCount']==30,'timing_recipe_changed')
    p=percent95([x['paintMs']for x in d['samples']]);approx(p,d['p95PaintMs']);need(p<=150,'paint_gate_failed')
    o=ops['operations'];need(len(o)==35 and len({x['id']for x in o})==35 and all(x['state']=='cancelled'and x['copyStatus']=='no_result'for x in o),'operation_gate_failed')
    return p


def cold(primary,family):
    need(primary['budgetPassed']is True and family['passed']is True and family['cold40MiBTargetEstablished']is True
         and family['identitiesUnchanged']is True and family['observerExitCode']==0,'cold_flags_failed')
    need(family['sourceRevision']==COMMIT and family['exitedProcesses']==[] and family['observedResidentDescendants']==[],'cold_identity_or_exit_failed')
    s=family['samples'];c=coverage(s,295,3,101)
    need(all(x['processCount']==1 and x['uiProcessCount']==0 and x['escapedProcessCount']==0
         and len(x['processes'])==1 for x in s),'cold_family_not_single')
    for x in s:
        need(all(x['totals'][k]==x['processes'][0][k]for k in ('rssBytes','pssBytes','privateBytes')),'cold_totals_not_charged')
    summary=family['summary'];approx(c['spanSeconds'],summary['coverage']['sampleSpanSeconds'])
    approx(c['maxGapSeconds'],summary['coverage']['maximumSampleGapSeconds']);need(summary['sampleCount']==len(s),'cold_count_mismatch')
    for k in ('rssBytes','pssBytes','privateBytes'):
        need(peak(s,k)==summary['peakSampledTotals'][k] and s[-1]['totals'][k]==summary['finalTotals'][k],'family_peak_or_final_mismatch')
    primary_peak=max(x['rssBytes']for x in primary['samples'])
    need(primary_peak==primary['peakSampledRssBytes'],'primary_peak_mismatch')
    expected=max(primary_peak,peak(s,'rssBytes'));need(expected==summary['peakSampledRssBytesIncludingPrimary']and expected<=40*MIB,'cold_peak_gate_failed')
    e=family['primaryEvidence'];need(e['resultsSha256']==primary['_originalSha256']and e['operationCount']==0 and e['coldIdleUninterrupted']is True,'cold_primary_bind_failed')
    approx(primary['elapsedSeconds'],e['elapsedSeconds']);need(primary['elapsedSeconds']>=300,'primary_duration_short')
    # CLK_TCK was not exported by the primary. Preserve its reported CPU value;
    # no fabricated machine-specific tick conversion is introduced here.
    need(0<=number(primary['idleCpuPercentOneCore'])<.5,'reported_cpu_budget_failed')
    approx(primary['idleCpuPercentOneCore'],e['idleCpuPercentOneCore'])
    return {'peakRssMiB':expected/MIB,'reportedIdleCpuPercentOneCore':primary['idleCpuPercentOneCore'],**c}


def views(d,family):
    need(d['passed']is True and d['sourceRevision']==COMMIT and d['residentBuild']['gitCommit']==COMMIT
         and d['residentBuild']['dirty']is False and d['operationCount']==0 and d['completeViewCycles']==3,'views_flags_failed')
    p=d['parameters'];need(p['cycles']==3 and p['views']==['settings','history','activity']and p['warmup_seconds']==5
         and p['visible_seconds']==2 and p['quiet_seconds']==30 and p['interval']==5,'view_recipe_changed')
    s=d['samples'];need(len(s)==42 and d['sampleCount']==42 and len(family['samples'])==42,'view_samples_missing')
    for x,y in zip(s,family['samples']):
        need(all(x[k]==y[k]for k in ('label','elapsedSeconds','totals','processCount','uiProcessCount','escapedProcessCount')),'detail_samples_mismatch')
        need(y['processCount']==len(y['processes']) and all(sum(z[k]for z in y['processes'])==y['totals'][k] for k in ('rssBytes','pssBytes','privateBytes')),'view_totals_not_charged')
    need(all(x['escapedProcessCount']==0 for x in s),'escaped_process')
    opened=[x for x in s if '-open-'in x['label']];need(len(opened)==18,'open_samples_incomplete')
    need(all(x['processCount']==2 and x['uiProcessCount']==1 for x in opened),'open_family_wrong')
    open_pss=peak(opened,'pssBytes');need(open_pss<=60*MIB and open_pss==d['summary']['peakOpenTotals']['pssBytes'],'open_pss_gate_failed')
    settled=[]
    for cycle in (1,2,3):
        closed=[x for x in s if x['label'].startswith('cycle-'+str(cycle)+'-closed-')]
        coverage(closed,29.5,7.5,7)
        need(len(closed)==7 and all(x['processCount']==1 and x['uiProcessCount']==0 for x in closed),'closed_family_wrong')
        last=closed[-1];need(last['totals']['rssBytes']<=40*MIB and last['totals']['pssBytes']<=29*MIB,'settled_memory_failed')
        settled.append({'cycle':cycle,'rssMiB':last['totals']['rssBytes']/MIB,'pssMiB':last['totals']['pssBytes']/MIB})
    normal={e['ui']['lastExit']['generation']for e in d['events']if 'ui'in e and e['ui']['lastExit'].get('status')=='normal'and e['ui']['lastExit'].get('code')==0}
    need(normal=={1,2},'original_normalexit_qualifier_changed')
    need(all('ui'not in e for e in d['events']if e['kind']=='all-views-closed'),'unexpected_original_final_exit_proof')
    return {'peakOpenPssMiB':open_pss/MIB,'settledCycles':settled,'originalNormalZeroGenerations':sorted(normal),'missingNormalZeroGenerations':[3]}


def exits(d,raw_sha,measured_resident):
    need(d['passed']is True and d['knownOwnedCleanupComplete']is True and d['exitCode']==0 and d['queryCount']==3
         and d['failurePresent']is False and d['cleanupFailurePresent']is False and d['rawFrozenResult']['sha256']==raw_sha,'supplement_flags_or_bind_failed')
    need([x['cycle']for x in d['cycles']]==[1,2,3],'supplement_cycles_missing')
    for x in d['cycles']:
        s=x['snapshot'];e=s['ui']['lastExit'];r=x['resident'];peer=x['socketPeer']
        need(all(type(r.get(k))is type(measured_resident.get(k))and k in measured_resident
             and r.get(k)==measured_resident[k]for k in ('pid','startTicks','uid','device','inode','sha256')),
             'supplement_measured_resident_join_failed')
        need(s['build']['gitCommit']==COMMIT and s['build']['dirty']is False and s['operationsReady']is True
          and s['journalReady']is True and s['activeOperations']==0 and s['trayAvailable']is True
          and s['ui']['pid']==0 and s['ui']['state']=='stopped'and e['generation']==x['cycle']and e['code']==0
          and e['status']=='normal'and peer['pid']==r['pid']and peer['uid']==r['uid'],'supplement_normal_exit_unproven')
    return [1,2,3]


def verify(directory):
    directory=Path(directory);manifest=json.loads((directory/'manifest.json').read_text())
    need(manifest['sourceCommit']==COMMIT and manifest['sourceCount']==53 and manifest['transformed']is True
         and manifest['gateOriginal']['sha256']==GATE_HASH and len(manifest['originalGateFingerprints'])==96,'manifest_scope_invalid')
    tool_refs={x['file']:x['sha256'] for x in manifest['tools']}
    need(set(tool_refs)=={'tools/export.py','tools/verify.py','tools/test_verify.py'},'tool_manifest_invalid')
    for rel,expected in tool_refs.items():
        actual=directory/rel
        need(hashlib.sha256(actual.read_bytes()).hexdigest()==expected,'verification_tool_changed')
    gate_refs={r['path']:r['sha256'] for r in manifest['originalGateFingerprints']}
    need(len(gate_refs)==96,'duplicate_original_gate_reference')
    all_data={};all_sha={}
    for row in manifest['exports']:
        rel=Path(row['file']);need(len(rel.parts)==1 and rel.suffix=='.json','export_path_invalid')
        p=directory/rel;need(not p.is_symlink(),'export_symlink')
        raw=p.read_bytes();need(hashlib.sha256(raw).hexdigest()==row['sha256']and len(raw)==row['bytes'],'export_changed')
        doc=json.loads(raw);need(doc['transformed']is True and doc['original']==row['original'],'projection_provenance_changed')
        original=doc['original']
        if row['binding']=='final-gate-receipt':
            need(gate_refs.get(original['path'])==original['sha256'] or original==manifest['gateOriginal'],'raw_gate_bind_mismatch')
        else:
            need((row['binding']=='additional-numeric-detail' and re.fullmatch('views-(original|supplement)-family-[12]\\.json',row['file']))
                 or (row['binding']=='prior-failed-checkpoint' and re.fullmatch('failed-source5[12](-exit)?\\.json',row['file'])),'unapproved_additional_export')
        all_data[row['file']]=doc['data'];all_sha[row['file']]=original['sha256']
    need(len(all_data)==22,'export_set_incomplete')
    result={'sourceCommit':COMMIT,'p95PaintMilliseconds':timing(all_data['timing.json'],all_data['timing-parameters.json'],all_data['timing-operations.json']),
            'cold':[],'views':[],'normalExitSupplements':[]}
    for repeat in (1,2):
        c=all_data['cold-'+str(repeat)+'.json'];c['_originalSha256']=all_sha['cold-'+str(repeat)+'.json']
        result['cold'].append({'repeat':repeat,**cold(c,all_data['family-'+str(repeat)+'.json'])})
        for label in ('original','supplement'):
            v=all_data['views-'+label+'-'+str(repeat)+'.json'];f=all_data['views-'+label+'-family-'+str(repeat)+'.json']
            result['views'].append({'repeat':repeat,'label':label,**views(v,f)})
        result['normalExitSupplements'].append({'repeat':repeat,'generations':exits(all_data['view-exits-'+str(repeat)+'.json'],all_sha['views-supplement-'+str(repeat)+'.json'],all_data['views-supplement-'+str(repeat)+'.json']['residentIdentity'])})
    for v in ('51','52'):
        q=all_data['failed-source'+v+'.json'];need(q.get('accepted',q.get('wholeFamilyAcceptance'))is False
            and all_data['failed-source'+v+'-exit.json']['exitCode']==1,'failed_attempt_promoted')
    gate=all_data['gate-summary.json'];need(gate['passed']is True and gate['sourceCommit']==COMMIT,'gate_summary_invalid')
    approx(result['p95PaintMilliseconds'],gate['p95PaintMilliseconds'])
    for actual,expected in zip(result['cold'],gate['cold']):approx(actual['peakRssMiB'],expected['peakRssMiB'])
    for actual,expected in zip(result['views'],gate['views']):
        approx(actual['peakOpenPssMiB'],expected['peakOpenPssMiB']);need(actual['settledCycles']==expected['settledCycles'],'gate_settled_summary_mismatch')
    result.update({'verified':True,'exportFiles':len(all_data),'runtimePerformed':False,
       'cpuQualification':'Reported primary CPU preserved; CLK_TCK not recorded here, no independent tick conversion claimed.',
       'normalExitQualification':'Original generation3 missing; separate read-only-query supplements retained as distinct runs.'})
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',nargs='?',type=Path,default=Path(__file__).resolve().parent.parent)
    try:print(json.dumps(verify(p.parse_args().directory),indent=2))
    except Exception as e:print(json.dumps({'verified':False,'errorType':type(e).__name__,'code':str(e)if isinstance(e,ValueError)else 'verification_failed'}));raise SystemExit(1)
