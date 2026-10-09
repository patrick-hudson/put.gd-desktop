#!/usr/bin/env python3
"""Export only fixed safe trace tokens and bounded attribution from a parent run."""
import argparse
import hashlib
import json
from pathlib import Path
import re

STAGES = {'main-entry','qapplication-pre','qapplication-post','visual-style-post',
          'bootstrap-accepted','snapshot-request','snapshot-reply','snapshot-accepted',
          'ready-pre','ready-post','settings-constructor-enter','settings-constructor-exit',
          'settings-activate-enter','settings-show-pre','settings-show-post','settings-shown-sent',
          'resident-launch-pre','resident-process-started'}
PATTERN = re.compile(rb'^startup-stage-v1 ([0-9]{1,20}) ([0-9]{1,20}) ([a-z-]{1,64})$')
PAIRS = {'nativeQApplication':('qapplication-pre','qapplication-post'),
         'styleAndBranding':('qapplication-post','visual-style-post'),
         'controlSetupAndBootstrap':('visual-style-post','bootstrap-accepted'),
         'snapshotRequestReply':('snapshot-request','snapshot-reply'),
         'snapshotValidation':('snapshot-reply','snapshot-accepted'),
         'controllerAndReadySend':('snapshot-accepted','ready-post'),
         'readyToActivation':('ready-post','settings-activate-enter'),
         'settingsConstructor':('settings-constructor-enter','settings-constructor-exit'),
         'restoreAndShowPreparation':('settings-constructor-exit','settings-show-pre'),
         'showCall':('settings-show-pre','settings-show-post'),
         'raiseActivateAndShownSend':('settings-show-post','settings-shown-sent'),
         'mainToShownSent':('main-entry','settings-shown-sent')}
def parse(log, result):
    rows=[]; rejected=0
    with Path(log).open('rb') as stream:
        for line in stream:
            if not line.startswith(b'startup-stage-v1 '):continue
            matched=PATTERN.fullmatch(line.rstrip(b'\n'))
            if not matched or matched[3].decode() not in STAGES: rejected+=1;continue
            rows.append({'pid':int(matched[1]),'monotonicNs':int(matched[2]),'stage':matched[3].decode()})
    data=json.loads(Path(result).read_text())
    launches=[r for r in rows if r['stage']=='resident-launch-pre']
    helpers={}
    for row in rows:
        if row['stage'].startswith('resident-'):continue
        stages=helpers.setdefault(row['pid'],{})
        stages.setdefault(row['stage'],row['monotonicNs'])
    attribution=[]
    for event in data.get('events',[]):
        if event.get('kind')!='view-shown' or event.get('view')!='settings':continue
        pid=event.get('ui',{}).get('pid'); stages=helpers.get(pid,{})
        intervals={name:(stages[end]-stages[start])/1e6 for name,(start,end) in PAIRS.items() if start in stages and end in stages}
        main=stages.get('main-entry')
        prior=[r for r in launches if main is not None and r['monotonicNs']<=main]
        if prior:
            launch=max(prior,key=lambda r:r['monotonicNs'])
            intervals['launchToMain']=(main-launch['monotonicNs'])/1e6
            # QProcess's started callback may occur after the child reaches main.
            upper=stages.get('settings-shown-sent',main)
            starts=[r for r in rows if r['pid']==launch['pid'] and r['stage']=='resident-process-started' and launch['monotonicNs']<=r['monotonicNs']<=upper]
            if starts:intervals['launchToProcessStarted']=(starts[0]['monotonicNs']-launch['monotonicNs'])/1e6
        cli=event.get('acknowledgementMilliseconds'); mapped=event.get('mappedReadyMilliseconds')
        attribution.append({'cycle':event.get('cycle'),'workloadKind':'cold first open' if event.get('cycle')==1 else 'closed-view reopen','helperPid':pid,'stageMilliseconds':intervals,
                            'cliShownAcknowledgementMilliseconds':cli,'independentlyObservedMappedMilliseconds':mapped,
                            'mappedAfterCliOverheadMilliseconds':mapped-cli if cli is not None and mapped is not None else None,
                            'missingStages':sorted(STAGES-set(stages)-{'resident-launch-pre','resident-process-started'})})
    return {'classification':'BASE41 + diagnostic overlay attribution only; no product saving or native certification',
            'rawLogSha256':hashlib.sha256(Path(log).read_bytes()).hexdigest(),
            'rawResultsSha256':hashlib.sha256(Path(result).read_bytes()).hexdigest(),
            'trace':rows,'rejectedTraceLineCount':rejected,'settingsReopens':attribution,
            'limitations':['stderr instrumentation adds overhead','control.send return records queued frames rather than resident receipt','QWidget show return and CLI shown acknowledgement do not prove mapped or painted pixels','mapped observer includes process/window/diagnostic inspection overhead','not p95 or populated-history/post-upload memory evidence']}
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log',required=True);parser.add_argument('--results',required=True)
    parser.add_argument('--output',required=True);args=parser.parse_args()
    Path(args.output).write_text(json.dumps(parse(args.log,args.results),indent=2)+'\n')
