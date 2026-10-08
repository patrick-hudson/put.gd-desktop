from pathlib import Path
import datetime, difflib, hashlib, json, signal, subprocess

ROOT=Path('/home/patrick/work/put.gd')
PROBE=Path('/tmp/putgd-live-outline-latency-probe')
PATHS=['src/app/OperationService.cpp','src/core/HistoryStore.cpp','src/platform/x11/X11SourceAdapter.cpp','src/platform/x11/X11Shortcuts.cpp','src/ui/RegionOverlay.cpp','tests/performance/bench_capture.cpp']
def sha(data): return hashlib.sha256(data).hexdigest()
before={name:(ROOT/name).read_bytes() for name in PATHS}
sources={name:data.decode() for name,data in before.items()}
def replace(name,old,new):
    assert sources[name].count(old)==1,(name,old[:100],sources[name].count(old))
    sources[name]=sources[name].replace(old,new,1)
for name in PATHS:
    sources[name]='#include "/tmp/putgd-live-outline-latency-probe/trace.h"\n'+sources[name]

name='src/app/OperationService.cpp'
replace(name,' auto acquisition=source_.prepareAcquisition(action);',' putgd_capture_probe::setOperation(qPrintable(op.id));putgd_capture_probe::trace("invoke.target_freeze.begin");\n auto acquisition=source_.prepareAcquisition(action);putgd_capture_probe::trace("invoke.target_freeze.end");putgd_capture_probe::trace("accepted.worker.queued");')
replace(name,'const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return op.publicJson();},[this,op,flag,reply,acquisition](Error e,QJsonObject result) mutable {','const auto path=databasePath();background([path,op](Error& e) {putgd_capture_probe::setOperation(qPrintable(op.id));putgd_capture_probe::Span span("accepted.worker.begin","accepted.worker.end");HistoryStore store(path);putgd_capture_probe::trace("accepted.store.opened");e=store.saveOperation(op);return op.publicJson();},[this,op,flag,reply,acquisition](Error e,QJsonObject result) mutable {\n  putgd_capture_probe::setOperation(qPrintable(op.id));putgd_capture_probe::trace("accepted.callback.begin");')
replace(name,'QTimer::singleShot(0,this,[this,op,flag,acquisition]() mutable {','QTimer::singleShot(0,this,[this,op,flag,acquisition]() mutable {\n   putgd_capture_probe::setOperation(qPrintable(op.id));putgd_capture_probe::trace("accepted.selector.continuation");')
replace(name,'op.state="acquiring";const auto path=databasePath();background([path,op](Error& e) {HistoryStore store(path);e=store.saveOperation(op);return QJsonObject{};},[this,op,flag,acquisition](Error e,QJsonObject) mutable {','op.state="acquiring";const auto path=databasePath();putgd_capture_probe::trace("acquiring.worker.queued");background([path,op](Error& e) {putgd_capture_probe::setOperation(qPrintable(op.id));putgd_capture_probe::Span span("acquiring.worker.begin","acquiring.worker.end");HistoryStore store(path);putgd_capture_probe::trace("acquiring.store.opened");e=store.saveOperation(op);return QJsonObject{};},[this,op,flag,acquisition](Error e,QJsonObject) mutable {\n    putgd_capture_probe::setOperation(qPrintable(op.id));putgd_capture_probe::trace("acquiring.callback.begin");')
replace(name,'    acquisition(op.settings.limits,','    putgd_capture_probe::trace("native.acquisition.call");acquisition(op.settings.limits,')

name='src/core/HistoryStore.cpp'
replace(name,'HistoryStore::HistoryStore(QString path) {','HistoryStore::HistoryStore(QString path) {\n putgd_capture_probe::Span span("history.open.begin","history.open.end");')
replace(name,' if(!db_.open()) {error_=sqlError();return;}',' putgd_capture_probe::trace("history.sqlite_open.begin");const bool opened=db_.open();putgd_capture_probe::trace("history.sqlite_open.end");if(!opened) {error_=sqlError();return;}')
replace(name,'Error HistoryStore::saveOperation(const Operation& op,const ProviderRegistry* providers) {','Error HistoryStore::saveOperation(const Operation& op,const ProviderRegistry* providers) {\n putgd_capture_probe::Span span("history.save.begin","history.save.end",qPrintable(op.id));')
# Scope the transaction/commit markers to saveOperation alone.
start=sources[name].index('Error HistoryStore::saveOperation(');end=sources[name].index('Error HistoryStore::indexPublications(',start)
section=sources[name][start:end]
assert section.count(' if(!db_.transaction()) return sqlError();')==1
section=section.replace(' if(!db_.transaction()) return sqlError();',' putgd_capture_probe::trace("history.transaction.begin",qPrintable(op.id));if(!db_.transaction()) return sqlError();putgd_capture_probe::trace("history.transaction.started",qPrintable(op.id));',1)
assert section.count(' if(!ok||!db_.commit())')==1
section=section.replace(' if(!ok||!db_.commit())',' putgd_capture_probe::trace("history.commit.begin",qPrintable(op.id));const bool committed=ok&&db_.commit();putgd_capture_probe::trace("history.commit.end",qPrintable(op.id));if(!committed)',1)
sources[name]=sources[name][:start]+section+sources[name][end:]

name='src/platform/x11/X11SourceAdapter.cpp'
replace(name,'void X11SourceAdapter::capture(const X11Target& target,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){','void X11SourceAdapter::capture(const X11Target& target,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){\n putgd_capture_probe::trace("native.capture.begin");')
replace(name,' Error error;if(!capture_.unchanged(target,error)){cb({{},error});return;}\n if(target.action!=',' putgd_capture_probe::trace("native.layout_validation.begin");Error error;if(!capture_.unchanged(target,error)){cb({{},error});return;}putgd_capture_probe::trace("native.layout_validation.end");\n if(target.action!=')
replace(name,' auto* overlay=new RegionOverlay(target.layout,this);',' putgd_capture_probe::trace("native.selector_construct.begin");auto* overlay=new RegionOverlay(target.layout,this);putgd_capture_probe::trace("native.selector_construct.end");')
replace(name,'(*callback)({{},error});});overlay->start();','(*callback)({{},error});});putgd_capture_probe::trace("native.selector_start.begin");overlay->start();putgd_capture_probe::trace("native.selector_start.end");')

name='src/platform/x11/X11Shortcuts.cpp'
replace(name,' xcb_ungrab_keyboard(connection_,XCB_CURRENT_TIME);',' const auto ordinal=++putgd_capture_probe::nextHotkey;putgd_capture_probe::trace("shortcut.key_received","",ordinal);xcb_ungrab_keyboard(connection_,XCB_CURRENT_TIME);')
replace(name,' emit activated(grab.action);break;',' putgd_capture_probe::trace("shortcut.grab_released","",ordinal);emit activated(grab.action);putgd_capture_probe::trace("shortcut.activation_returned","",ordinal);break;')

name='src/ui/RegionOverlay.cpp'
replace(name,'protected:\n void paintEvent(QPaintEvent*) override {QPainter painter(this);','protected:\n bool event(QEvent* event)override{if(event->type()!=QEvent::UpdateRequest)return QWidget::event(event);putgd_capture_probe::Span span("surface.update_request.begin","surface.update_request.end",operation_.c_str(),ordinal_);return QWidget::event(event);}\n void paintEvent(QPaintEvent*) override {putgd_capture_probe::Span span(owner_.dragging_?"surface.drag_paint.begin":"surface.initial_paint.begin",owner_.dragging_?"surface.drag_paint.end":"surface.initial_paint.end",operation_.c_str(),ordinal_);QPainter painter(this);')
replace(name,' void mousePressEvent(QMouseEvent* e) override {',' void mousePressEvent(QMouseEvent* e) override {putgd_capture_probe::trace("surface.press_received",operation_.c_str(),ordinal_);')
replace(name,' void mouseMoveEvent(QMouseEvent* e) override {',' void mouseMoveEvent(QMouseEvent* e) override {putgd_capture_probe::trace("surface.move_received",operation_.c_str(),ordinal_);')
replace(name,' RegionOverlay& owner_;',' const std::string operation_=putgd_capture_probe::operation;const unsigned long long ordinal_=++putgd_capture_probe::nextSurface;\n RegionOverlay& owner_;')
replace(name,'surface->show();surface->raise();','putgd_capture_probe::trace("surface.show.begin");surface->show();surface->raise();putgd_capture_probe::trace("surface.show.end");')
replace(name,' if(focus)focus->grabKeyboard();',' putgd_capture_probe::trace("selector.keyboard_grab.begin");if(focus)focus->grabKeyboard();putgd_capture_probe::trace("selector.keyboard_grab.end");')

name='tests/performance/bench_capture.cpp'
replace(name,'xcb_window_t window,const QRectF& selection){','xcb_window_t window,const QRectF& selection,unsigned long long ordinal){')
replace(name,' auto* position=xcb_translate_coordinates_reply',' putgd_capture_probe::trace("driver.translate.begin","",ordinal);auto* position=xcb_translate_coordinates_reply')
replace(name,' auto* geometry=xcb_get_geometry_reply',' putgd_capture_probe::trace("driver.translate.end","",ordinal);putgd_capture_probe::trace("driver.geometry.begin","",ordinal);auto* geometry=xcb_get_geometry_reply')
replace(name,' if(!position||!geometry){',' putgd_capture_probe::trace("driver.geometry.end","",ordinal);if(!position||!geometry){')
replace(name,' auto* pixels=xcb_get_image_reply(c,xcb_get_image(c,XCB_IMAGE_FORMAT_Z_PIXMAP,window,',' putgd_capture_probe::trace("driver.get_image.begin","",ordinal);auto* pixels=xcb_get_image_reply(c,xcb_get_image(c,XCB_IMAGE_FORMAT_Z_PIXMAP,window,')
replace(name,' if(!pixels)return false;\n const auto* bytes=',' putgd_capture_probe::trace("driver.get_image.end","",ordinal);if(!pixels)return false;\n const auto* bytes=')
replace(name,'   QElapsedTimer timer;timer.start();auto elapsedMs=','   QElapsedTimer timer;timer.start();putgd_capture_probe::trace("driver.hotkey.begin","",iteration+1);auto elapsedMs=')
replace(name,'   const double hotkeyIssuedMs=elapsedMs();','   const double hotkeyIssuedMs=elapsedMs();putgd_capture_probe::trace("driver.hotkey.issued","",iteration+1);')
replace(name,'outlinePainted(connection,root,w,selection):','outlinePainted(connection,root,w,selection,iteration+1):')
replace(name,'motion(end);xcb_flush(connection);inputIssuedMs=elapsedMs();','motion(end);xcb_flush(connection);inputIssuedMs=elapsedMs();putgd_capture_probe::trace("driver.drag.issued","",iteration+1);')

def interrupted(signum,frame): raise KeyboardInterrupt
for sig in [signal.SIGTERM,signal.SIGHUP]:signal.signal(sig,interrupted)
identity={'createdAt':datetime.datetime.now(datetime.timezone.utc).isoformat(),'sourceRoot':str(ROOT),'buildRoot':str(PROBE/'build'),'files':[], 'restored':False,'measurement':'Unscored locating trace only; no physical or VM run performed by build script.'}
for name,data in before.items():
    p=PROBE/'before'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
    p=PROBE/'instrumented'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(sources[name])
    identity['files'].append({'path':name,'beforeSha256':sha(data),'instrumentedSha256':sha(sources[name].encode())})
patch=''.join(part for name in PATHS for part in difflib.unified_diff(before[name].decode().splitlines(True),sources[name].splitlines(True),fromfile='a/'+name,tofile='b/'+name,n=5))
(PROBE/'instrumentation.patch').write_text(patch)
identity['traceHeaderSha256']=sha((PROBE/'trace.h').read_bytes());identity['instrumentationPatchSha256']=sha(patch.encode())
commands=[['cmake','--preset','release','-B',str(PROBE/'build'),'-DBUILD_TESTING=OFF','-DPUTGD_PHYSICAL_BENCHMARKS=ON'],['cmake','--build',str(PROBE/'build'),'--target','putgd','putgd-cli','putgd-bench-capture','--parallel','1']]
identity['commands']=commands
try:
    for name in PATHS:(ROOT/name).write_text(sources[name])
    for i,command in enumerate(commands):
        print(json.dumps({'stage':i,'command':command}),flush=True)
        with (PROBE/('configure.log' if i==0 else 'build.log')).open('w') as log:
            result=subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        identity.setdefault('exitCodes',[]).append(result.returncode)
        if result.returncode:raise RuntimeError('Probe configure/build failed; inspect saved stage log')
    identity['binarySha256']={name:sha((PROBE/'build'/name).read_bytes()) for name in ['putgd','putgd-cli','putgd-bench-capture']}
    identity['buildReady']=True
finally:
    for name,data in before.items():(ROOT/name).write_bytes(data)
    identity['restored']=all((ROOT/name).read_bytes()==data for name,data in before.items())
    (PROBE/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    print(json.dumps({'buildReady':identity.get('buildReady',False),'restored':identity['restored'],'identityPath':str(PROBE/'identity.json')}),flush=True)
