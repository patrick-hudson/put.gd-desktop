from pathlib import Path
import ast,json,hashlib
root=Path('/home/patrick/.codex/worktrees/resident-native-boundary/put.gd');out=Path('/home/patrick/work/put.gd/work/capture-parity/journal-phases')
prior=ast.parse(Path('/home/patrick/work/put.gd/work/capture-parity/trace1/instrument.py').read_text())
header=next(n.args[0].value for n in ast.walk(prior) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute) and n.func.attr=='write_text' and n.args and isinstance(n.args[0],ast.Constant) and isinstance(n.args[0].value,str) and n.args[0].value.startswith('#pragma once'))
old=next(ast.literal_eval(n.value) for n in prior.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='changes' for t in n.targets))
changes={'src/app/OperationService.cpp':old['src/app/OperationService.cpp'],'src/core/HistoryStore.cpp':[
 ('HistoryStore::HistoryStore(QString path) {','HistoryStore::HistoryStore(QString path) {ParitySpan trace("history-open-initialize");'),
 ('Error HistoryStore::initialize() {','Error HistoryStore::initialize() {ParitySpan trace("history-schema");'),
 (' if(!db_.open()) {',' parityTrace("database-open-before");if(!db_.open()) {'),
 ('QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner);error_=initialize();','parityTrace("database-open-after");QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner);error_=initialize();'),
 ('Error HistoryStore::saveOperation(const Operation& op,const ProviderRegistry* providers) {','Error HistoryStore::saveOperation(const Operation& op,const ProviderRegistry* providers) {ParitySpan trace("history-save");')]
}
manifest={}
for name,replacements in changes.items():
 p=root/name;s=p.read_text();backup=out/'originals'/name;backup.parent.mkdir(parents=True,exist_ok=True);assert not backup.exists();backup.write_text(s);manifest[name]=hashlib.sha256(p.read_bytes()).hexdigest()
 for a,b in replacements:
  assert s.count(a)==1,(name,a,s.count(a));s=s.replace(a,b)
 if name.endswith('HistoryStore.cpp'):
  a=s.index('Error HistoryStore::saveOperation');b=s.index('Error HistoryStore::indexPublications');body=s[a:b]
  oldcommit='if(!ok||!db_.commit()) {db_.rollback();return sqlError();}return {};';assert body.count(oldcommit)==1
  body=body.replace(oldcommit,'parityTrace("history-commit-before");if(!ok||!db_.commit()) {db_.rollback();return sqlError();}parityTrace("history-commit-after");return {};');s=s[:a]+body+s[b:]
 p.write_text('#include "core/CaptureParityTrace.h" // CAPTURE_PARITY_TRACE\n'+s)
header=header.replace('#include <unistd.h>','#include <unistd.h>\n#include <sys/syscall.h>').replace('"%lld %ld %ld %s %s\\n",','"%lld %ld %ld %ld %s %s\\n",').replace('(long)getpid(),(long)getppid(),phase,edge','(long)getpid(),(long)getppid(),(long)syscall(SYS_gettid),phase,edge')
(out/'originals.json').write_text(json.dumps(manifest,indent=2)+'\n');(root/'src/core/CaptureParityTrace.h').write_text(header)
