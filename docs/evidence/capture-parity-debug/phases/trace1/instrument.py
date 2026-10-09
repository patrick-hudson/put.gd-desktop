from pathlib import Path
import json,hashlib
root=Path('/home/patrick/.codex/worktrees/resident-native-boundary/put.gd')
back=Path('/home/patrick/work/put.gd/work/capture-parity/trace1/originals');back.mkdir(parents=True,exist_ok=True)
changes={
'src/capture/main.cpp':[
('int main(int argc,char** argv){','int main(int argc,char** argv){ parityTrace("child-main");'),
(' QApplication app(argc,argv);',' parityTrace("app-before"); QApplication app(argc,argv); parityTrace("app-after");'),
(' putgd::X11NativeSnapshot native;putgd::X11Capture resolver;putgd::X11SourceAdapter source;',' parityTrace("adapters-before"); putgd::X11NativeSnapshot native;putgd::X11Capture resolver;putgd::X11SourceAdapter source; parityTrace("adapters-after");'),
('bootstrapped=true;putgd::Error error;const auto target=native.resolve(frozen,resolver,error);','bootstrapped=true;putgd::Error error;parityTrace("resolve-before");const auto target=native.resolve(frozen,resolver,error);parityTrace("resolve-after");'),
('   source.acquireFrozen(target,limits,','   parityTrace("acquire-frozen"); source.acquireFrozen(target,limits,')],
'src/platform/x11/X11NativeSnapshot.cpp':[
('QJsonObject X11NativeSnapshot::layout(Error& error){','QJsonObject X11NativeSnapshot::layout(Error& error){ ParitySpan trace("native-layout");')],
'src/platform/x11/X11CoreSourceAdapter.cpp':[
('SourceAdapter::PreparedAcquisition X11CoreSourceAdapter::prepareAcquisition(const QString& action){','SourceAdapter::PreparedAcquisition X11CoreSourceAdapter::prepareAcquisition(const QString& action){ ParitySpan trace("prepare");')],
'src/app/OperationService.cpp':[
('void OperationService::invoke(const QString& action,JsonReply reply) {','void OperationService::invoke(const QString& action,JsonReply reply) { parityTrace("invoke");'),
('const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return op.publicJson();}', 'const auto path=databasePath();parityTrace("accepted-queue");background([path,op](Error& e) {ParitySpan trace("accepted-worker");HistoryStore store(path);e=store.saveOperation(op);return op.publicJson();}'),
('  if(e) {active_.remove(op.id);','  parityTrace("accepted-deliver");if(e) {active_.remove(op.id);'),
('background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};}', 'background([path,op](Error& e) {ParitySpan trace("acquiring-worker");HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};}'),
('    acquisition(op.settings.limits,','    parityTrace("acquiring-deliver"); acquisition(op.settings.limits,')],
'src/platform/x11/CaptureProcessController.cpp':[
('connect(child,&QProcess::started,this,[this,frozen]{','connect(child,&QProcess::started,this,[this,frozen]{parityTrace("child-started");'),
('child->start(helperPath_,{});','parityTrace("child-launch");child->start(helperPath_,{});')],
'src/ui/RegionOverlay.cpp':[
(' void paintEvent(QPaintEvent*) override {',' void paintEvent(QPaintEvent*) override { if(!traced_){parityTrace("surface-first-paint");traced_=true;} if(!outlineTraced_&&!owner_.selection_.isEmpty()){parityTrace("surface-outline-paint");outlineTraced_=true;}'),
(' RegionOverlay& owner_;',' bool traced_=false,outlineTraced_=false; RegionOverlay& owner_;'),
('void RegionOverlay::start(){','void RegionOverlay::start(){ ParitySpan trace("overlay-start");')]
}
manifest={}
for name,replacements in changes.items():
 p=root/name;s=p.read_text();b=back/name;b.parent.mkdir(parents=True,exist_ok=True);assert not b.exists();b.write_text(s);manifest[name]=hashlib.sha256(p.read_bytes()).hexdigest()
 for old,new in replacements:
  assert s.count(old)==1,(name,old,s.count(old));s=s.replace(old,new)
 p.write_text('#include "core/CaptureParityTrace.h" // CAPTURE_PARITY_TRACE\n'+s)
(back.parent/'originals.json').write_text(json.dumps(manifest,indent=2)+'\n')
(root/'src/core/CaptureParityTrace.h').write_text('''#pragma once
// CAPTURE_PARITY_TRACE: temporary, unscored fixed-label diagnostic only.
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
inline void parityTrace(const char* phase,const char* edge="point"){
 const char* path=std::getenv("PUTGD_CAPTURE_PARITY_TRACE");if(!path)return;
 const int fd=::open(path,O_WRONLY|O_APPEND|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return;
 struct stat s{};timespec t{};
 if(!::fstat(fd,&s)&&S_ISREG(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==0600&&s.st_nlink==1&&s.st_size<8*1024*1024&&!clock_gettime(CLOCK_MONOTONIC,&t)){
 char line[160];const int n=std::snprintf(line,sizeof(line),"%lld %ld %ld %s %s\\n",(long long)t.tv_sec*1000000000LL+t.tv_nsec,(long)getpid(),(long)getppid(),phase,edge);if(n>0&&n<(int)sizeof(line)) {const auto ignored=::write(fd,line,n);(void)ignored;}}
 ::close(fd);
}
struct ParitySpan{const char* phase;explicit ParitySpan(const char* p):phase(p){parityTrace(phase,"begin");}~ParitySpan(){parityTrace(phase,"end");}};
''')
print('Instrumented',len(changes),'owned files; originals hashed')
