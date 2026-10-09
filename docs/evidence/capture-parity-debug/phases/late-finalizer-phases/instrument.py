from pathlib import Path
import ast,json,hashlib
root=Path('/home/patrick/.codex/worktrees/resident-native-boundary/put.gd');out=Path(__file__).parent
prior=ast.parse(Path('/home/patrick/work/put.gd/work/capture-parity/trace1/instrument.py').read_text())
header=next(n.args[0].value for n in ast.walk(prior) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute) and n.func.attr=='write_text' and n.args and isinstance(n.args[0],ast.Constant) and isinstance(n.args[0].value,str) and n.args[0].value.startswith('#pragma once'))
old=next(ast.literal_eval(n.value) for n in prior.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='changes' for t in n.targets))
changes={'src/app/OperationService.cpp':old['src/app/OperationService.cpp']+[
 ('auto acquisition=source_.prepareAcquisition(action,snapshot.limits);','parityTrace("prepare-before");auto acquisition=source_.prepareAcquisition(action,snapshot.limits);parityTrace("prepare-after");')],
 'src/platform/x11/CaptureProcessController.cpp':[
 ('CaptureProcessController::Ticket CaptureProcessController::prepare(const QJsonObject& frozen,const ResourceLimits& limits){','CaptureProcessController::Ticket CaptureProcessController::prepare(const QJsonObject& frozen,const ResourceLimits& limits){ParitySpan trace("prepare-controller");'),
 ('connect(child,&QProcess::started,this,[this,child,epoch,frozen]{','connect(child,&QProcess::started,this,[this,child,epoch,frozen]{parityTrace("child-started");'),
 ('ready_=true;if(beginRequested_)sendBegin();','parityTrace("ready-parent");ready_=true;if(beginRequested_)sendBegin();'),
 (' const bool sent=control_&&control_->send("begin",',' parityTrace("begin-parent");const bool sent=control_&&control_->send("begin",')],
 'src/capture/main.cpp':[
 (' QApplication app(argc,argv);',' parityTrace("app-before");QApplication app(argc,argv);parityTrace("app-after");'),
 (' const auto build=QJsonDocument',' parityTrace("adapters-after");const auto build=QJsonDocument'),
 ('target=native.resolve(frozen,resolver,error);','parityTrace("resolve-before");target=native.resolve(frozen,resolver,error);parityTrace("resolve-after");'),
 ('    cursor.create();cursor.setCursor(Qt::CrossCursor);','    parityTrace("prewarm-create-before");cursor.create();parityTrace("prewarm-cursor-before");cursor.setCursor(Qt::CrossCursor);parityTrace("prewarm-cursor-after");'),
 ('   ready=true;auto reply=envelope();','   parityTrace("ready-child");ready=true;auto reply=envelope();'),
 ('   begun=true;putgd::Error changed;','   parityTrace("begin-child");begun=true;putgd::Error changed;'),
 ('QJsonObject nativeCursorSeed(){','QJsonObject nativeCursorSeed(){ParitySpan trace("cursor-seed");'),
 ('   preparedCursor=quint32(cursorId);cacheCursor=cursor["cache"].toBool();','   preparedCursor=quint32(cursorId);cacheCursor=cursor["cache"].toBool();parityTrace(preparedCursor?"cursor-cache-hit":"cursor-cache-miss");')],
 'src/platform/x11/NativeRegionOverlay.cpp':[
 ('void NativeRegionOverlay::start(){','void NativeRegionOverlay::start(){parityTrace("native-start");'),
 (' cursorWindow_->show();',' parityTrace("show-before");cursorWindow_->show();parityTrace("show-after");'),
 ('void NativeRegionOverlay::initializeSurfaces(){','void NativeRegionOverlay::initializeSurfaces(){parityTrace("native-init");'),
 (' for(const auto& surface:surfaces_)xcb_map_window(connection_,surface.window);',' parityTrace("native-map-before");for(const auto& surface:surfaces_)xcb_map_window(connection_,surface.window);'),
 (' notifier_=new QSocketNotifier',' parityTrace("native-map-flushed");notifier_=new QSocketNotifier')]
}
changes['src/capture/main.cpp'] += [
 ('cursor.create();', 'cursor.create();parityTrace(cursor.screen()==QGuiApplication::screenAt(QPoint(0,0))?"prewarm-screen-origin":"prewarm-screen-other");')]
changes['src/platform/x11/NativeRegionOverlay.cpp'] += [
 (' cursorWindow_->setParent(foreignParent_);',' parityTrace(cursorWindow_->screen()==foreignParent_->screen()?"parent-screen-same":"parent-screen-different");cursorWindow_->setParent(foreignParent_);'),
 ('if(event.detail==1){anchor_', 'if(event.detail==1){parityTrace("input-left-press");anchor_'),
 ('move(logicalPoint({event.root_x,event.root_y}));','parityTrace("input-motion");move(logicalPoint({event.root_x,event.root_y}));'),
 ('if(event.detail==1){release', 'if(event.detail==1){parityTrace("input-left-release");release'),
 ('==XK_Escape){cancel();', '==XK_Escape){parityTrace("input-escape");cancel();'),
 ('const auto result=selection_;', 'parityTrace("selection-complete");const auto result=selection_;'),
 ('void NativeRegionOverlay::cancel(Error error){','void NativeRegionOverlay::cancel(Error error){parityTrace("selection-cancel");'),
 ('xcb_poly_rectangle(connection_,surface.window,gc_,1,&rectangle);','xcb_poly_rectangle(connection_,surface.window,gc_,1,&rectangle);parityTrace("outline-draw");')]
changes['src/core/HistoryStore.cpp']=[
 ('HistoryStore::HistoryStore(QString path) {','HistoryStore::HistoryStore(QString path) {ParitySpan trace("history-open");'),
 ('Error HistoryStore::initialize() {','Error HistoryStore::initialize() {ParitySpan trace("history-initialize");'),
 ('Error HistoryStore::saveOperation(const Operation& op,const ProviderRegistry* providers) {','Error HistoryStore::saveOperation(const Operation& op,const ProviderRegistry* providers) {ParitySpan trace("history-save");'),
 (' if(!ok||!db_.commit()) {db_.rollback();return sqlError();}return {};\n}', ' parityTrace("history-commit-before");const bool committed=ok&&db_.commit();parityTrace("history-commit-after");if(!committed) {db_.rollback();return sqlError();}return {};\n}')]
# The matching commit tail appears twice: instrument only saveOperation explicitly.
a,b=changes['src/core/HistoryStore.cpp'].pop();original=(root/'src/core/HistoryStore.cpp').read_text();start=original.index('Error HistoryStore::saveOperation(');end=original.index('Error HistoryStore::indexPublications(',start);section=original[start:end];assert section.count(a)==1;changes['src/core/HistoryStore.cpp'].insert(0,(section,section.replace(a,b)))
changes['src/app/OperationService.cpp'] += [
 ('void OperationService::finish(Operation op,JsonReply reply) {','void OperationService::finish(Operation op,JsonReply reply) {parityTrace("finish-queue");'),
 ('background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return e?QJsonObject{}:store.operation(op.id,&e).toJson();}', 'background([path,op](Error& e) {ParitySpan trace("finish-worker");HistoryStore store(path);e=store.saveOperation(op);return e?QJsonObject{}:store.operation(op.id,&e).toJson();}'),
 ('  emit changed(id);','  parityTrace("finish-deliver");emit changed(id);')]
changes['src/core/HistoryStore.cpp'] += [
 ('QSqlQuery q(db_);if(!q.exec("PRAGMA foreign_keys=ON")||!q.exec("PRAGMA user_version")||!q.next()) return sqlError(); const int version=q.value(0).toInt();',
  'QSqlQuery q(db_);parityTrace("foreign-keys-before");const bool fk=q.exec("PRAGMA foreign_keys=ON");parityTrace("foreign-keys-after");parityTrace("user-version-exec-before");const bool uv=fk&&q.exec("PRAGMA user_version");parityTrace("user-version-exec-after");parityTrace("user-version-next-before");const bool fetched=uv&&q.next();parityTrace("user-version-next-after");if(!fetched)return sqlError();const int version=q.value(0).toInt();')]

# Validate every insertion before mutating any file.
prepared={};manifest={}
for name,replacements in changes.items():
 p=root/name;s=p.read_text();original=s
 for a,b in replacements:
  assert s.count(a)==1,(name,a,s.count(a));s=s.replace(a,b)
 prepared[name]=(original,'#include "core/CaptureParityTrace.h" // CAPTURE_PARITY_TRACE\n'+s)
 manifest[name]=hashlib.sha256(p.read_bytes()).hexdigest()
for name,(original,updated) in prepared.items():
 backup=out/'originals'/name;backup.parent.mkdir(parents=True,exist_ok=True);assert not backup.exists();backup.write_text(original);(root/name).write_text(updated)
(out/'originals.json').write_text(json.dumps(manifest,indent=2)+'\n')
header=header.replace('#include <unistd.h>','#include <unistd.h>\n#include <sys/syscall.h>').replace('"%lld %ld %ld %s %s\\n",','"%lld %ld %ld %ld %s %s\\n",').replace('(long)getpid(),(long)getppid(),phase,edge','(long)getpid(),(long)getppid(),(long)syscall(SYS_gettid),phase,edge')
(root/'src/core/CaptureParityTrace.h').write_text(header)

p=root/'src/core/CaptureParityTrace.h';h=p.read_text();h=h.replace('timespec t{};', 'timespec t{},wall{};').replace('!clock_gettime(CLOCK_MONOTONIC,&t)', '!clock_gettime(CLOCK_MONOTONIC,&t)&&!clock_gettime(CLOCK_REALTIME,&wall)').replace('%s %s\\n",','%s %s %lld\\n",').replace('phase,edge);','phase,edge,(long long)wall.tv_sec*1000000000LL+wall.tv_nsec);');p.write_text(h)
