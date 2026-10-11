#!/usr/bin/env python3
"""Cold physical Cinnamon run with full process-family and mapped-window checks."""
from pathlib import Path
import argparse,json,os,subprocess,sys,time
ROOT=Path('/home/patrick/work/put.gd');sys.path.insert(0,str(ROOT/'tests/performance'))
import bench_ui_process as bench
from bench_views import fixture_config
from bench_idle import footprint
from observe_idle import check_physical_session,operation_count,owned_windows,stop_owned_process_group
p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--seconds',type=float,default=300);p.add_argument('--acceptance-receipt',action='append',default=[]);args=p.parse_args();assert 300<=args.seconds<=3600;assert len(args.acceptance_receipt)==3, 'Require the three inspected current native receipts'
check_physical_session();source=json.loads((ROOT/'work/u11/native/source.json').read_text());binaries=ROOT/'work/u11/native/host/source/build/release'
argv=['--mode','split','--app',str(binaries/'putgd'),'--cli',str(binaries/'putgd-cli'),'--helper',str(binaries/'putgd-ui'),'--source-revision',source['commit'],'--source-archive',source['archive'],'--output',args.output,'--fixture','/tmp/putgd-startup-reclaim-exp001','--interval','5']
for receipt in args.acceptance_receipt:argv+=['--acceptance-receipt',receipt]
parameters=bench.parser().parse_args(argv);parameters.seconds=args.seconds;parameters.label='cold-idle';record=bench.Recorder(parameters);record.data.update(command=[sys.executable,str(Path(__file__).resolve()),*sys.argv[1:]],scope='cold physical Cinnamon; full process family; no views, captures, clipboard or networking',workloadSha256=bench.sha256(__file__),durationSeconds=args.seconds)
family=resident=log=None;passed=False
try:
 record.initialize(parameters)
 if record.data['sourceArchive']['sha256']!=source['archiveSha256']:raise bench.EvidenceError('source_archive_changed')
 relative,document,fixture=fixture_config(Path(parameters.fixture));record.data['fixture']=fixture;record.data['dependencies']={name:bench.sha256(ROOT/'tests/performance'/name) for name in ('bench_views.py','observe_idle.py','bench_idle.py')}
 environment=os.environ.copy()
 for variable,name in [('XDG_CONFIG_HOME','config'),('XDG_DATA_HOME','data'),('XDG_CACHE_HOME','cache'),('XDG_RUNTIME_DIR','runtime')]:
  path=record.root/name;path.mkdir(mode=0o700);environment[variable]=str(path)
 environment['QT_QPA_PLATFORM']='xcb';config=record.root/'config'/relative;config.parent.mkdir(parents=True,mode=0o700);document['archive']['root']=str(record.root/'archive');bench.write_json(config,document)
 log=(record.root/'resident.log').open('wb');os.chmod(record.root/'resident.log',0o600)
 resident=subprocess.Popen([parameters.app,'--background'],env=environment,stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True)
 family=bench.ProcessFamily(resident.pid,parameters.app,parameters.helper);record.data['residentIdentity']=family.root
 call=lambda method,params=None:bench.request(parameters.cli,method,params,environment=environment)
 def ready():
  if resident.poll() is not None:raise bench.EvidenceError('resident_exited_before_readiness')
  try:return call('diagnostics').get('journalReady') is True
  except (bench.EvidenceError,subprocess.SubprocessError,json.JSONDecodeError):return False
 bench.wait_until(ready,20,'resident_readiness_timeout')
 diagnostics=bench.checked_diagnostics(parameters,call,record);bench.checked_helper(parameters,family,diagnostics,closed=True)
 time.sleep(5);begin=time.monotonic();initial=None;lastRecorded=-5
 while True:
  sample=family.sample()
  if sample['uiProcessCount'] or sample['escapedProcessCount'] or sample['processCount']!=1:raise bench.EvidenceError('cold_run_has_other_processes')
  if any(owned_windows(row['pid']) for row in sample['processes']):raise bench.EvidenceError('cold_run_has_owned_window')
  cpu=sum(row['cpuTicks'] for row in sample['processes'])
  if initial is None:initial=cpu
  elapsed=time.monotonic()-begin
  if elapsed>=lastRecorded+5 or elapsed>=args.seconds:
   record.sample(family,'cold-idle');lastRecorded=elapsed
  if elapsed>=args.seconds:break
  time.sleep(1)
 record.data['coldEndFootprint']={'scope':'After timed interval, before final CLI diagnostics; mappings grouped by basename','mappings':footprint(resident.pid)}
 if not bench.same_identity(bench.identity(resident.pid),family.root):raise bench.EvidenceError('resident_identity_changed')
 diagnostics=bench.checked_diagnostics(parameters,call,record);bench.checked_helper(parameters,family,diagnostics,closed=True)
 if operation_count(record.root):raise bench.EvidenceError('unexpected_operation')
 peak=max(x['totals']['rssBytes'] for x in record.data['samples']);cpuPercent=(cpu-initial)/os.sysconf('SC_CLK_TCK')/elapsed*100
 record.data['coldAcceptance']={'elapsedSeconds':elapsed,'peakSampledTotalRssBytes':peak,'totalCpuPercentOneCore':cpuPercent,'operationCount':0,'mappedOwnedWindowObserved':False,'helperObserved':False,'legacyIdleBudgetPassed':cpuPercent<.5 and peak<=100*1024**2,'target40MiBPassed':peak<=40*1024**2,'measurementControls':'All observed family members and mapped-window ownership checked once per second; full records every five seconds. More observation than historical resident-only idle harness; CPU comparisons require this same workload.'}
 passed=True
except BaseException as error:
 record.data['errorCode']=str(error) if isinstance(error,bench.EvidenceError) else type(error).__name__;raise
finally:
 record.finish(family,passed)
 if passed:
  record.data['summary']['coldIdleBudgetEstablished']=record.data['coldAcceptance']['legacyIdleBudgetPassed']
  record.data['summary']['cold40MiBTargetEstablished']=record.data['coldAcceptance']['target40MiBPassed']
  record.data['limitations']=[line for line in record.data['limitations'] if 'not a leak proof' not in line]+['This cold workload establishes only its recorded empty-history/no-action physical idle conditions; post-capture, populated history and S3 are separate gates.']
  record.persist(final=True)
 if resident is not None:stop_owned_process_group(resident)
 if log is not None:log.close()
print(json.dumps(record.data['coldAcceptance'],indent=2))
