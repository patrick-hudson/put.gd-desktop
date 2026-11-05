"""Restore-safe 100,000 B/s shaping of one disposable loopback fixture port."""
from contextlib import contextmanager
import json,os,subprocess,time
from urllib.parse import urlsplit
from physical_campaign import require

def root(q):return [{k:v for k,v in x.items() if k not in ('refcnt','stats','stats2')} for x in q if x.get('root')]
@contextmanager
def scoped(run):
 p=urlsplit(run.args.endpoint);require(p.scheme=='http' and p.hostname=='127.0.0.1' and p.port,'non_loopback_shaping_refused')
 receipt={'fixturePort':p.port,'destination':'127.0.0.1','rateSpecification':'800kbit','rateBytesPerSecond':100000,'commands':[],'scope':'IPv4 TCP destination 127.0.0.1 exact fixture port only; unmarked traffic default 100 Gbit/s'}
 def command(*args,input=None):
  r=subprocess.run(['sudo','-n',*args],input=input,capture_output=True,text=True,timeout=30)
  receipt['commands'].append({'argv':list(args),'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr});run.result['tcFixtureScope']=receipt;run.persist()
  require(r.returncode==0,'scoped_network_command_failed:'+repr(args)+':'+r.stderr);return r.stdout
 before=json.loads(command('tc','-j','qdisc','show','dev','lo'));receipt['beforeQdisc']=before
 require(len(root(before))==1 and root(before)[0]['kind']=='noqueue','existing_loopback_qdisc_preserved')
 table='np379_'+str(os.getpid());handle='379:';mark='0x37900000';receipt.update(nftTable=table,qdiscHandle=handle)
 require(('table inet '+table) not in command('nft','list','tables'),'existing_table_preserved')
 nft_started=tc_started=False
 try:
  nft_started=True
  command('nft','-f','-',input='create table inet '+table+'\nadd chain inet '+table+' output { type filter hook output priority -1; policy accept; }\nadd rule inet '+table+' output ip daddr 127.0.0.1 tcp dport '+str(p.port)+' counter meta mark set '+mark+'\n')
  tc_started=True;command('tc','qdisc','add','dev','lo','root','handle',handle,'htb','default','2')
  command('tc','class','add','dev','lo','parent','379:','classid','379:1','htb','rate','100gbit','ceil','100gbit')
  command('tc','class','add','dev','lo','parent','379:1','classid','379:2','htb','rate','99gbit','ceil','100gbit')
  command('tc','class','add','dev','lo','parent','379:1','classid','379:10','htb','rate','800kbit','ceil','800kbit','burst','65536','cburst','65536')
  command('tc','filter','add','dev','lo','parent','379:','protocol','ip','prio','379','handle',mark,'fw','flowid','379:10')
  receipt['installedClassesText']=command('tc','class','show','dev','lo');require('rate 800Kbit' in receipt['installedClassesText'],'shaper_rate_not_verified')
  yield
 finally:
  try:
   if tc_started:
    current=json.loads(command('tc','-j','qdisc','show','dev','lo'));ours=[q for q in current if q.get('root') and q.get('kind')=='htb' and q.get('handle')==handle]
    require(len(ours)==1 or root(current)==root(before),'owned_shaper_identity_changed_preserve_for_recovery')
    if ours:
     try:receipt['finalClassCountersText']=command('tc','-s','class','show','dev','lo')
     finally:command('tc','qdisc','del','dev','lo','root','handle',handle)
  finally:
   if nft_started and ('table inet '+table) in command('nft','list','tables'):
    try:receipt['nftCounters']=json.loads(command('nft','-j','list','table','inet',table))
    finally:command('nft','delete','table','inet',table)
   after=json.loads(command('tc','-j','qdisc','show','dev','lo'));receipt['afterQdisc']=after;receipt['qdiscRestored']=root(before)==root(after);receipt['ownedTableAbsent']=('table inet '+table) not in command('nft','list','tables');run.persist()
   require(receipt['qdiscRestored'] and receipt['ownedTableAbsent'],'scoped_shaping_restore_failed')
