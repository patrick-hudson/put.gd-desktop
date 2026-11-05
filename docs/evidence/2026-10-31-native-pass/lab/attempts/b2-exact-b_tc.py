"""Restore-safe 100 KiB/s shaping of one disposable loopback fixture port."""
from contextlib import contextmanager
import json,os,subprocess,time
from urllib.parse import urlsplit
from physical_campaign import require


def command(*args,input=None):
 r=subprocess.run(['sudo','-n',*args],input=input,capture_output=True,text=True,timeout=30)
 require(r.returncode==0,'scoped_network_command_failed:'+args[0]);return r.stdout


def root(qdiscs):return [{k:v for k,v in x.items() if k not in ('refcnt','stats','stats2')} for x in qdiscs if x.get('root')]


@contextmanager
def scoped(run):
 p=urlsplit(run.args.endpoint);require(p.scheme=='http' and p.hostname=='127.0.0.1' and p.port,'non_loopback_shaping_refused')
 before=json.loads(command('tc','-j','qdisc','show','dev','lo'))
 require(len(root(before))==1 and root(before)[0]['kind']=='noqueue','existing_loopback_qdisc_preserved')
 table='np379_'+str(os.getpid());handle='379:';mark='0x37900000'
 tables=command('nft','list','tables');require(('table inet '+table) not in tables,'existing_table_preserved')
 receipt={'fixturePort':p.port,'destination':'127.0.0.1','rateSpecification':'100kbyte','rateBytesPerSecond':None,'qdiscHandle':handle,'nftTable':table,'beforeQdisc':before,'scope':'only IPv4 TCP packets to this literal loopback fixture port receive the matching mark; unmarked traffic has a 100 Gbit/s default class','startedMonotonic':time.monotonic()}
 nft_started=tc_started=False
 try:
  nft_started=True
  command('nft','-f','-',input='create table inet '+table+'\nadd chain inet '+table+' output { type filter hook output priority -1; policy accept; }\nadd rule inet '+table+' output ip daddr 127.0.0.1 tcp dport '+str(p.port)+' counter meta mark set '+mark+'\n')
  tc_started=True;command('tc','qdisc','add','dev','lo','root','handle',handle,'htb','default','2')
  command('tc','class','add','dev','lo','parent','379:','classid','379:1','htb','rate','100gbit','ceil','100gbit')
  command('tc','class','add','dev','lo','parent','379:1','classid','379:2','htb','rate','100gbit','ceil','100gbit')
  command('tc','class','add','dev','lo','parent','379:1','classid','379:10','htb','rate','100kbyte','ceil','100kbyte','burst','64k','cburst','64k')
  command('tc','filter','add','dev','lo','parent','379:','protocol','ip','prio','379','handle',mark,'fw','flowid','379:10')
  receipt['installedClasses']=json.loads(command('tc','-j','class','show','dev','lo'))
  shaped=next(x for x in receipt['installedClasses'] if x.get('handle')=='379:10')
  receipt['rateBytesPerSecond']=shaped.get('options',{}).get('rate')
  run.result['tcFixtureScope']=receipt;run.persist()
  yield
 finally:
  if tc_started:
   current=json.loads(command('tc','-j','qdisc','show','dev','lo'));ours=[q for q in current if q.get('root') and q.get('kind')=='htb' and q.get('handle')==handle]
   require(len(ours)==1 or root(current)==root(before),'owned_shaper_identity_changed_preserve_for_recovery')
   if ours:
    receipt['finalClassCounters']=json.loads(command('tc','-s','-j','class','show','dev','lo'))
    command('tc','qdisc','del','dev','lo','root','handle',handle)
  if nft_started and ('table inet '+table) in command('nft','list','tables'):
   receipt['nftCounters']=json.loads(command('nft','-j','list','table','inet',table))
   command('nft','delete','table','inet',table)
  after=json.loads(command('tc','-j','qdisc','show','dev','lo'));receipt['afterQdisc']=after
  receipt['qdiscRestored']=root(before)==root(after);receipt['ownedTableAbsent']=('table inet '+table) not in command('nft','list','tables')
  run.result['tcFixtureScope']=receipt;run.persist()
  require(receipt['qdiscRestored'] and receipt['ownedTableAbsent'],'scoped_shaping_restore_failed')

