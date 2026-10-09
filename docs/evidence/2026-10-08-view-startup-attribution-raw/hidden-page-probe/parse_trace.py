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
PAGE_TOKENS = {'shell','capture','destinations','shortcuts','preferences'}
POLISH_PHASES = {'ctor-enter','ctor-exit','show-pre','show-post','shown-sent'}
STAGES.update('settings-page-'+page+'-'+phase for page in PAGE_TOKENS-{'shell'} for phase in ('pre','post'))
STAGES.update('settings-'+section+'-'+phase for section in ('wiring','refresh') for phase in ('pre','post'))
POLISH_PATTERN = re.compile(rb'^startup-polish-v1 ([0-9]{1,20}) ([0-9]{1,20}) ([a-z-]{1,64}) ([a-z-]{1,64}) ([0-9]{1,20}) ([0-9]{1,20})$')
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
PAIRS.update({'construct'+page.title():('settings-page-'+page+'-pre','settings-page-'+page+'-post') for page in PAGE_TOKENS-{'shell'}})
PAIRS.update({'settings'+section.title():('settings-'+section+'-pre','settings-'+section+'-post') for section in ('wiring','refresh')})
POLISH_PAIRS = {'constructor':('ctor-enter','ctor-exit'), 'restoreAndShowPreparation':('ctor-exit','show-pre'), 'showCall':('show-pre','show-post'), 'raiseActivateAndShownSend':('show-post','shown-sent'), 'constructorThroughShownSend':('ctor-enter','shown-sent')}
def polish_intervals(rows, pid):
    snapshots={}
    for row in rows:
        if row['pid']==pid:snapshots.setdefault((row['phase'],row['page']),row)
    output={};missing=[];invalid=[]
    for label,(start,end) in POLISH_PAIRS.items():
        by_page={}
        for page in sorted(PAGE_TOKENS):
            a=snapshots.get((start,page));b=snapshots.get((end,page))
            if a is None or b is None:
                missing.append(label+':'+page);continue
            if b['exclusiveNs']<a['exclusiveNs'] or b['count']<a['count'] or b['monotonicNs']<a['monotonicNs']:
                invalid.append(label+':'+page);continue
            by_page[page]={'exclusiveMilliseconds':(b['exclusiveNs']-a['exclusiveNs'])/1e6,'eventCount':b['count']-a['count']}
        output[label]={'pages':by_page,'exclusiveTotalMilliseconds':sum(value['exclusiveMilliseconds'] for value in by_page.values()), 'eventCount':sum(value['eventCount'] for value in by_page.values())}
    return {'intervals':output,'missingSnapshots':missing,'invalidCounterIntervals':invalid}
def parse(log, result):
    rows=[]; polish_rows=[]; rejected=0
    with Path(log).open('rb') as stream:
        for line in stream:
            if line.startswith(b'startup-polish-v1 '):
                matched=POLISH_PATTERN.fullmatch(line.rstrip(b'\n'))
                if not matched or matched[3].decode() not in POLISH_PHASES or matched[4].decode() not in PAGE_TOKENS:
                    rejected+=1;continue
                polish_rows.append({'pid':int(matched[1]),'monotonicNs':int(matched[2]),'phase':matched[3].decode(),'page':matched[4].decode(),'exclusiveNs':int(matched[5]),'count':int(matched[6])})
                continue
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
        page_keys=['constructCapture','constructDestinations','constructShortcuts','constructPreferences','settingsWiring','settingsRefresh']
        sections_total=sum(intervals[key] for key in page_keys if key in intervals)
        accounting={'measuredSectionMilliseconds':sections_total,'unassignedShellAndTraceMilliseconds':intervals['settingsConstructor']-sections_total if 'settingsConstructor' in intervals and all(key in intervals for key in page_keys) else None,'completeSections':all(key in intervals for key in page_keys)}
        cli=event.get('acknowledgementMilliseconds'); mapped=event.get('mappedReadyMilliseconds')
        attribution.append({'cycle':event.get('cycle'),'workloadKind':'cold first open' if event.get('cycle')==1 else 'closed-view reopen','helperPid':pid,'stageMilliseconds':intervals,'constructorAccounting':accounting,'polishAttribution':polish_intervals(polish_rows,pid),
                            'cliShownAcknowledgementMilliseconds':cli,'independentlyObservedMappedMilliseconds':mapped,
                            'mappedAfterCliOverheadMilliseconds':mapped-cli if cli is not None and mapped is not None else None,
                            'missingStages':sorted(STAGES-set(stages)-{'resident-launch-pre','resident-process-started'})})
    return {'classification':'BASE41 + diagnostic overlay attribution only; no product saving or native certification',
            'rawLogSha256':hashlib.sha256(Path(log).read_bytes()).hexdigest(),
            'rawResultsSha256':hashlib.sha256(Path(result).read_bytes()).hexdigest(),
            'trace':rows,'polishSnapshots':polish_rows,'rejectedTraceLineCount':rejected,'settingsReopens':attribution,
            'limitations':['Polish timings measure exclusive QApplication::notify dispatch only, excluding nested Polish dispatch; other Qt style/layout/font work is outside this attribution','Page ancestry is sampled at dispatch time; pre-reparenting work can be shell or untracked','Polish totals are subsets of constructor/show wall time and must not be added to them','Constructor shell remainder includes fixed trace overhead and setup; wiring interval includes visibility/footer setup','stderr instrumentation adds overhead','control.send return records queued frames rather than resident receipt','QWidget show return and CLI shown acknowledgement do not prove mapped or painted pixels','mapped observer includes process/window/diagnostic inspection overhead','not p95 or populated-history/post-upload memory evidence']}
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log',required=True);parser.add_argument('--results',required=True)
    parser.add_argument('--output',required=True);args=parser.parse_args()
    Path(args.output).write_text(json.dumps(parse(args.log,args.results),indent=2)+'\n')
