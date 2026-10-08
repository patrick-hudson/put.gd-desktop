from pathlib import Path
from collections import Counter
import hashlib,json,statistics

ROOT=Path('/tmp/putgd-live-outline-latency-trace')
OUT=Path('/home/patrick/work/put.gd/work/live-region-latency-probe-evidence')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def load_trace(name):
    records=[]
    for line in (ROOT/name).read_text().splitlines():
        if line.startswith('PUTGD_CAPTURE_TRACE '):
            value=json.loads(line.removeprefix('PUTGD_CAPTURE_TRACE '))
            assert set(value)=={'phase','tNs','operationId','ordinal'}
            records.append(value)
    return sorted(records,key=lambda item:item['tNs'])
resident=load_trace('resident.log');driver=load_trace('driver.log')
result=json.loads((ROOT/'results.json').read_text());operations=json.loads((ROOT/'operations.json').read_text())
begins=[record for record in driver if record['phase']=='driver.hotkey.begin']
opbegins=[record for record in resident if record['phase']=='invoke.target_freeze.begin']
assert len(begins)==len(opbegins)==len(operations)==35
def first(records,phase):return next(item['tNs'] for item in records if item['phase']==phase)
def pairs(records,begin,end):
    pending={};result=[]
    for record in records:
        key=record['ordinal']
        if record['phase']==begin:
            assert key not in pending,(begin,key)
            pending[key]=record['tNs']
        elif record['phase']==end:
            assert key in pending,(end,key)
            result.append((key,pending.pop(key),record['tNs']))
    assert not pending,begin
    return result
def duration(records,begin,end):return (first(records,end)-first(records,begin))/1e6
def total(records,begin,end):return sum(end-start for _,start,end in pairs(records,begin,end))/1e6
rows=[]
for index,start in enumerate(begins):
    ordinal=start['ordinal'];assert ordinal==index+1
    stop=begins[index+1]['tNs'] if index+1<len(begins) else 2**63-1
    opmatches=[record for record in opbegins if start['tNs']<=record['tNs']<stop]
    assert len(opmatches)==1
    opid=opmatches[0]['operationId'];records=[record for record in resident if record['operationId']==opid]
    injected=[record for record in driver if record['ordinal']==ordinal]
    keys=[record for record in resident if record['ordinal']==ordinal and record['phase'].startswith('shortcut.')]
    metrics={'driverHotkeyInjectionMs':duration(injected,'driver.hotkey.begin','driver.hotkey.issued'),
             'hotkeyDeliveryMs':(first(keys,'shortcut.key_received')-start['tNs'])/1e6,
             'passiveGrabReleaseMs':duration(keys,'shortcut.key_received','shortcut.grab_released'),
             'targetFreezeMs':duration(records,'invoke.target_freeze.begin','invoke.target_freeze.end')}
    for state in ['accepted','acquiring']:
        worker_begin=first(records,state+'.worker.begin');worker_end=first(records,state+'.worker.end')
        work=[record for record in records if worker_begin<=record['tNs']<=worker_end]
        metrics[state+'QueueMs']=duration(records,state+'.worker.queued',state+'.worker.begin')
        metrics[state+'OpenMs']=duration(work,'history.open.begin','history.open.end')
        metrics[state+'SQLiteOpenMs']=duration(work,'history.sqlite_open.begin','history.sqlite_open.end')
        metrics[state+'PostSQLiteOpenMs']=duration(work,'history.sqlite_open.end','history.open.end')
        metrics[state+'SaveMs']=duration(work,'history.save.begin','history.save.end')
        metrics[state+'CommitMs']=duration(work,'history.commit.begin','history.commit.end')
        metrics[state+'WorkerMs']=(worker_end-worker_begin)/1e6
        metrics[state+'CallbackDeliveryMs']=duration(records,state+'.worker.end',state+'.callback.begin')
    metrics['guiContinuationMs']=duration(records,'accepted.callback.begin','accepted.selector.continuation')
    metrics['nativeLayoutValidationMs']=duration(records,'native.layout_validation.begin','native.layout_validation.end')
    metrics['selectorConstructMs']=duration(records,'native.selector_construct.begin','native.selector_construct.end')
    metrics['selectorStartMs']=duration(records,'native.selector_start.begin','native.selector_start.end')
    metrics['surfaceShowTotalMs']=total(records,'surface.show.begin','surface.show.end')
    metrics['keyboardGrabMs']=duration(records,'selector.keyboard_grab.begin','selector.keyboard_grab.end')
    metrics['idlePainterTotalMs']=total(records,'surface.initial_paint.begin','surface.initial_paint.end')
    drag_pairs=pairs(records,'surface.drag_paint.begin','surface.drag_paint.end')
    updates=pairs(records,'surface.update_request.begin','surface.update_request.end')
    metrics['dragPainterTotalMs']=sum(end-begin for _,begin,end in drag_pairs)/1e6
    drag_updates=[]
    for surface,begin,end in drag_pairs:
        matches=[entry for entry in updates if entry[0]==surface and entry[1]<=begin<=end<=entry[2]]
        assert len(matches)==1
        drag_updates.append(matches[0])
    assert len(drag_pairs)==len(drag_updates)==3
    metrics['dragUpdateTotalMs']=sum(end-begin for _,begin,end in drag_updates)/1e6
    metrics['dragIssuedToLastPainterEndMs']=(max(end for _,_,end in drag_pairs)-first(injected,'driver.drag.issued'))/1e6
    metrics['dragIssuedToLastUpdateEndMs']=(max(end for _,_,end in drag_updates)-first(injected,'driver.drag.issued'))/1e6
    metrics['dragIssuedToPressReceivedMs']=(first(records,'surface.press_received')-first(injected,'driver.drag.issued'))/1e6
    moves=[record for record in records if record['phase']=='surface.move_received']
    assert len(moves)==2
    metrics['pressReceivedToDragMoveReceivedMs']=(moves[-1]['tNs']-first(records,'surface.press_received'))/1e6
    metrics['dragMoveReceivedToFirstUpdateBeginMs']=(min(begin for _,begin,_ in drag_updates)-moves[-1]['tNs'])/1e6
    metrics['hotkeyToSelectorStartEndMs']=(first(records,'native.selector_start.end')-start['tNs'])/1e6
    for phase in ['translate','geometry','get_image']:
        metrics['driver'+phase.title().replace('_','')+'QueryTotalMs']=total(injected,'driver.'+phase+'.begin','driver.'+phase+'.end')
    operation=next(item for item in operations if item['id']==opid)
    open_begin=first(records,'history.open.begin');open_end=first(records,'history.open.end')
    overlapped=[]
    for other_id in {record['operationId'] for record in resident if record['operationId'] and record['operationId']!=opid}:
        other=[record for record in resident if record['operationId']==other_id]
        for _,commit_begin,commit_end in pairs(other,'history.commit.begin','history.commit.end'):
            if commit_begin<open_end and commit_end>open_begin:
                overlapped.append({'operationId':other_id,'commitMs':(commit_end-commit_begin)/1e6,'commitStartRelativeToThisHotkeyMs':(commit_begin-start['tNs'])/1e6,'commitEndRelativeToThisHotkeyMs':(commit_end-start['tNs'])/1e6,'openReturnedAfterCommitMs':(open_end-commit_end)/1e6})
    rows.append({'iteration':ordinal,'warmup':ordinal<=5,'operationId':opid,'state':operation['state'],'copyStatus':operation['copyStatus'],'metrics':metrics,'otherCommitOverlapDuringAcceptedOpen':overlapped,'driverSample':None if ordinal<=5 else result['samples'][ordinal-6]})
sampled=[row for row in rows if not row['warmup']]
def summary(values):
    values=sorted(values)
    return {'min':values[0],'median':statistics.median(values),'p95':values[28],'max':values[-1]}
stats={metric:summary([row['metrics'][metric] for row in sampled]) for metric in sampled[0]['metrics']}
report={'schemaVersion':1,'runRoot':str(ROOT),'runExitCode':1,'scope':'Unscored synthetic cancellation attribution only; exact same 150 ms gate retained and failed. No instrumentation or driver costs subtracted.','p95PaintMs':result['p95PaintMs'],'budgetPassed':result['budgetPassed'],'error':result['error'],'operationCount':len(operations),'operationStates':dict(Counter(item['state'] for item in operations)),'traceCounts':{'resident':len(resident),'driver':len(driver),'matchedOperations':len(rows)},'statistics':stats,'rows':rows,'limitations':['One transient instrumented run does not establish a production timing pass. Large pre-map stalls were reproduced and attributed to HistoryStore work after SQLite open, overlapping a previous operation commit; the permission/PRAGMA boundary is not independently split.','Clock uses Linux CLOCK_MONOTONIC across processes; all fixed trace write/lock costs remain included.','surface.initial_paint labels idle drag state, not necessarily first actual paint. UpdateRequest return is a Qt event boundary, not compositor presentation.','Synchronous driver X11 query intervals overlap application/native waits. They cannot be subtracted or treated as independent additive app-versus-observer costs.'],'rawEvidence':{name:{'path':str(ROOT/name),'sha256':sha(ROOT/name)} for name in ['parameters.json','results.json','operations.json','driver.log','resident.log']}}
(OUT/'latency-attribution.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'p95PaintMs':report['p95PaintMs'],'budgetPassed':report['budgetPassed'],'statistics':stats,'slowSamples':[{'iteration':row['iteration'],'operationId':row['operationId'],'paintMs':row['driverSample']['paintMs'],'metrics':row['metrics']} for row in sorted(sampled,key=lambda row:row['driverSample']['paintMs'],reverse=True)[:3]]},indent=2))
