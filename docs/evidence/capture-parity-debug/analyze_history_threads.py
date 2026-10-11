#!/usr/bin/env python3
"""Same-PID/TID sequential history phase pairs. Numerical locator, not acceptance."""
from pathlib import Path
from collections import defaultdict
import argparse,bisect,hashlib,json,math,statistics

def statistics_for(values):
    if not values:return {'count':0}
    ordered=sorted(values)
    return {'count':len(values),'medianMs':statistics.median(values),'p95Ms':ordered[math.ceil(.95*len(values))-1],'minMs':min(values),'maxMs':max(values)}

def analyze(path):
    raw=path.read_bytes();rows=[]
    for index,line in enumerate(raw.decode('ascii').splitlines(),1):
        values=line.split()
        if len(values)!=6:raise ValueError(f'Expected six numerical/fixed-label fields at line {index}')
        stamp,pid,parent,tid,phase,edge=values
        rows.append({'ns':int(stamp),'pid':int(pid),'parent':int(parent),'tid':int(tid),'phase':phase,'edge':edge,'line':index})
    rows.sort(key=lambda row:(row['ns'],row['line']))
    invocations=[row for row in rows if row['phase']=='invoke' and row['edge']=='point']
    starts=[row['ns'] for row in invocations]
    markers={'historyOpen':(('history-open','begin'),('history-open','end')),
             'historyInitialize':(('history-initialize','begin'),('history-initialize','end')),
             'historySave':(('history-save','begin'),('history-save','end')),
             'historyCommit':(('history-commit-before','point'),('history-commit-after','point')),
             'acceptedWorker':(('accepted-worker','begin'),('accepted-worker','end')),
             'acquiringWorker':(('acquiring-worker','begin'),('acquiring-worker','end'))}
    begin_marker={v[0]:k for k,v in markers.items()};end_marker={v[1]:k for k,v in markers.items()}
    stacks=defaultdict(list);pairs=defaultdict(list);unmatched=[];nested=[]
    for row in rows:
        marker=(row['phase'],row['edge'])
        if marker in begin_marker:
            kind=begin_marker[marker];key=(row['pid'],row['tid'],kind)
            if stacks[key]:nested.append({'pid':row['pid'],'tid':row['tid'],'kind':kind,'line':row['line']})
            stacks[key].append(row)
        elif marker in end_marker:
            kind=end_marker[marker];key=(row['pid'],row['tid'],kind)
            if not stacks[key]:unmatched.append({'edge':'end','pid':row['pid'],'tid':row['tid'],'kind':kind,'line':row['line']});continue
            begin=stacks[key].pop();assert row['ns']>=begin['ns']
            pairs[kind].append({'pid':row['pid'],'tid':row['tid'],'beginNs':begin['ns'],'endNs':row['ns'],'durationMs':(row['ns']-begin['ns'])/1e6,'beginLine':begin['line'],'endLine':row['line']})
    for (pid,tid,kind),pending in stacks.items():
        unmatched.extend({'edge':'begin','pid':pid,'tid':tid,'kind':kind,'line':row['line']} for row in pending)
    for values in pairs.values():values.sort(key=lambda pair:(pair['beginNs'],pair['beginLine']))
    history_kinds=['historyOpen','historyInitialize','historySave','historyCommit'];groups=[]
    for index,invoke in enumerate(invocations):
        next_ns=starts[index+1] if index+1<len(starts) else 10**30
        group={'invocationIndex':index,'warmup':index<5,'invokeNs':invoke['ns'],'invokePid':invoke['pid'],'invokeTid':invoke['tid'],'workers':{},'priorWorkerOverlap':[]}
        for role in ['acceptedWorker','acquiringWorker']:
            candidates=[pair for pair in pairs[role] if pair['pid']==invoke['pid'] and invoke['ns']<=pair['beginNs']<next_ns]
            if len(candidates)!=1:raise ValueError(f'{role} invocation {index}: expected one exact worker, got {len(candidates)}')
            worker=candidates[0];history={};extra={}
            for kind in history_kinds:
                matches=[pair for pair in pairs[kind] if pair['pid']==worker['pid'] and pair['tid']==worker['tid'] and worker['beginNs']<=pair['beginNs'] and pair['endNs']<=worker['endNs']]
                history[kind]=matches[0] if matches else None
                if len(matches)>1:extra[kind]=len(matches)
            group['workers'][role]=dict(worker,history=history,additionalSequentialPairs=extra)
        accepted=group['workers']['acceptedWorker']
        for kind in history_kinds:
            for pair in pairs[kind]:
                if pair['pid']!=accepted['pid'] or pair['tid']==accepted['tid']:continue
                if not(pair['beginNs']<accepted['beginNs']<pair['endNs']):continue
                origin=bisect.bisect_right(starts,pair['beginNs'])-1
                role='not_accepted_or_acquiring_worker'
                for worker_role in ['acceptedWorker','acquiringWorker']:
                    if any(w['pid']==pair['pid'] and w['tid']==pair['tid'] and w['beginNs']<=pair['beginNs'] and pair['endNs']<=w['endNs'] for w in pairs[worker_role]):role=worker_role
                group['priorWorkerOverlap'].append(dict(pair,kind=kind,role=role,originInvocationIndex=origin,relativeAcceptedBeginMs=(pair['beginNs']-accepted['beginNs'])/1e6,relativeAcceptedEndMs=(pair['endNs']-accepted['beginNs'])/1e6,overlapMs=(min(pair['endNs'],accepted['endNs'])-accepted['beginNs'])/1e6))
        group['priorWorkerOverlap'].sort(key=lambda pair:(pair['beginNs'],pair['beginLine']))
        groups.append(group)
    legacy_cross_thread=[]
    for index,invoke in enumerate(invocations):
        next_ns=starts[index+1] if index+1<len(starts) else 10**30
        first={}
        for row in sorted(rows,key=lambda item:item['line']):
            if invoke['ns']<=row['ns']<next_ns:first.setdefault((row['phase'],row['edge']),row)
        for kind in history_kinds:
            begin_marker,end_marker=markers[kind]
            begin=first.get(begin_marker);end=first.get(end_marker)
            if begin and end and ((begin['pid'],begin['tid'])!=(end['pid'],end['tid']) or end['ns']<begin['ns']):
                legacy_cross_thread.append({'invocationIndex':index,'warmup':index<5,'kind':kind,'beginPid':begin['pid'],'beginTid':begin['tid'],'endPid':end['pid'],'endTid':end['tid'],'beginLine':begin['line'],'endLine':end['line'],'invalidDurationMs':(end['ns']-begin['ns'])/1e6,'status':'invalid legacy first-point merge; not a real span'})
    stats={}
    for label,selected in [('allInvocations',groups),('measuredInvocations',groups[5:])]:
        role_stats={}
        for role in ['acceptedWorker','acquiringWorker']:
            role_stats[role]={'worker':statistics_for([g['workers'][role]['durationMs'] for g in selected])}
            for kind in history_kinds:
                role_stats[role][kind]=statistics_for([g['workers'][role]['history'][kind]['durationMs'] for g in selected if g['workers'][role]['history'][kind]])
        stats[label]={'invocationCount':len(selected),'roles':role_stats}
    worst=max(groups,key=lambda g:g['workers']['acceptedWorker']['history']['historyInitialize']['durationMs'])
    accepted=worst['workers']['acceptedWorker'];initialize=accepted['history']['historyInitialize']
    worst_observation={'invocationIndex':worst['invocationIndex'],'warmup':worst['warmup'],'acceptedPid':accepted['pid'],'acceptedTid':accepted['tid'],'acceptedBeginNs':accepted['beginNs'],'initialize':initialize,'initializeBeginRelativeAcceptedMs':(initialize['beginNs']-accepted['beginNs'])/1e6,'initializeEndRelativeAcceptedMs':(initialize['endNs']-accepted['beginNs'])/1e6,'priorCommits':[]}
    for prior in worst['priorWorkerOverlap']:
        if prior['kind']=='historyCommit':worst_observation['priorCommits'].append(dict(prior,initializeEndAfterPriorCommitMs=(initialize['endNs']-prior['endNs'])/1e6))
    return {'schemaVersion':1,'unscored':True,'acceptance':False,'traceSha256':hashlib.sha256(raw).hexdigest(),'chronology':'Stable timestamp order, then original line; first sequential pair on the exact worker PID/TID; five warmups remain separate from thirty measured invocations.','pairing':'Same PID/TID and phase begin/end; repeated nested begins flagged, unmatched events retained. Worker history spans must fit inside their exact worker interval.','groups':len(groups),'warmupGroups':len(groups[:5]),'measuredGroups':len(groups[5:]),'pairCounts':{k:len(v) for k,v in pairs.items()},'unmatchedEvents':unmatched,'nestedBegins':nested,'allHistoryThreads':{kind:statistics_for([p['durationMs'] for p in pairs[kind]]) for kind in history_kinds},'statistics':stats,'legacyCrossThreadPairs':legacy_cross_thread,'worstAcceptedInitialize':worst_observation,'invocations':groups,'limits':['Numerical overlap is not proof of a specific blocked SQLite call, kernel lock or retry schedule.','Unassigned other worker roles are not explicit finalizer markers; source/workload context may identify cancellation finalization.','Corrected statistical p95 is nearest rank. Phase statistics are locators, never the frozen capture budget.']}

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--trace',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();result=analyze(args.trace);args.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'groups':result['groups'],'warmupGroups':result['warmupGroups'],'measuredGroups':result['measuredGroups'],'pairCounts':result['pairCounts'],'unmatchedEvents':len(result['unmatchedEvents']),'nestedBegins':len(result['nestedBegins']),'statistics':result['statistics'],'worstAcceptedInitialize':result['worstAcceptedInitialize']},indent=2))
