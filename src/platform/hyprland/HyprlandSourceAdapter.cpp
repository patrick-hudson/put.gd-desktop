#include "HyprlandSourceAdapter.h"
#include "platform/wayland/CaptureCompose.h"
#include "platform/wayland/ToplevelMatch.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <memory>
namespace putgd {
namespace {
const QString cleanupMessage="Region helper exited before confirming surface cleanup; restart put.gd before capturing again";
const QString focusMessage="Focus an external window and invoke the window shortcut or CLI";
struct WindowSnapshot {QImage image;Error error;bool done=false;std::function<void(PayloadResult)> accepted;ResourceLimits acceptedLimits;};
PayloadResult imageResult(const QImage& image,const ResourceLimits& limits,Error error={}){
 if(error)return {{},error};
 if(image.isNull())return {{},{"capture_failed","The compositor provided no image",{}}};
 if(qint64(image.width())*image.height()>limits.imagePixels||image.sizeInBytes()>limits.imageBytes)return {{},{"input_excessive","Capture exceeds accepted operation resource limits",{}}};
 ClipboardPayload payload;payload.kind="image";payload.mime="image/png";payload.image=image;return {payload,{}};
}
// The parent of a process, from /proc/<pid>/stat (the field after the command's closing parenthesis).
qint64 parentOf(qint64 pid){
 QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
 if(!stat.open(QIODevice::ReadOnly))return 0;
 const QByteArray text=stat.read(4096);const auto close=text.lastIndexOf(')');
 if(close<0)return 0;
 const auto fields=text.mid(close+2).split(' ');
 return fields.size()>1?fields[1].toLongLong():0;
}
}
HyprlandSourceAdapter::HyprlandSourceAdapter(QObject* parent):QObject(parent),shortcuts_(&ipc_){
 connect(&outputs_,&WaylandOutputs::changed,this,[this]{if(region_.busy())region_.cancel({"display_changed","Display layout changed during region selection",{}});emit capabilitiesChanged();});
 connect(&capture_,&ImageCopyCapture::changed,this,&HyprlandSourceAdapter::capabilitiesChanged);
 connect(&clipboard_,&DataControlClipboard::changed,this,&HyprlandSourceAdapter::capabilitiesChanged);
 connect(&shortcuts_,&ShortcutBackend::changed,this,&HyprlandSourceAdapter::capabilitiesChanged);
 connect(&shortcuts_,&ShortcutBackend::activated,this,&HyprlandSourceAdapter::actionRequested);
}
HyprlandSourceAdapter::~HyprlandSourceAdapter(){region_.shutdown();cancelAcquisition();clipboard_.cancel();for(auto* worker:workers_){worker->wait();delete worker;}}
quint64 HyprlandSourceAdapter::clipboardRevision()const{return clipboard_.revision();}
bool HyprlandSourceAdapter::unchanged(const QList<CaptureFrame>& before,quint64 revision)const{
 if(outputs_.revision()!=revision)return false;
 const auto now=layout();if(now.size()!=before.size())return false;
 for(const auto& frame:before){bool found=false;for(const auto& current:now)if(current.identity==frame.identity&&current.logicalRect==frame.logicalRect&&current.nativeRect==frame.nativeRect&&current.density==frame.density&&current.rotation==frame.rotation)found=true;if(!found)return false;}
 return true;
}
bool HyprlandSourceAdapter::ownWindow(qint64 pid,const QString& appId)const{
 if(putgdAppId(appId))return true;
 if(pid<=0)return false;
 const qint64 self=QCoreApplication::applicationPid();
 if(pid==self||(uiProcess_&&pid==uiProcess_))return true;
 // putgd-ui, putgd-record and putgd-region run below the resident.
 qint64 current=pid;
 for(int depth=0;depth<8&&current>1;++depth){current=parentOf(current);if(current==self)return true;}
 return false;
}
bool HyprlandSourceAdapter::refusedFrame(const QImage& image){
 if(image.isNull()||image.depth()!=32)return false;
 // Hyprland clears a denied frame to transparent black but may still draw the
 // cursor into it (measured on Omarchy: ~2,000 of 2,073,600 pixels). Real
 // content is opaque, so even a black screen is never 0x00000000.
 const qint64 total=qint64(image.width())*image.height();
 const qint64 allowed=std::max<qint64>(total/100,1);qint64 nonZero=0;
 for(int y=0;y<image.height();++y){
  const auto* row=reinterpret_cast<const quint32*>(image.constScanLine(y));
  for(int x=0;x<image.width();++x)if(row[x]&&++nonZero>allowed)return false;
 }
 return true;
}
QString HyprlandSourceAdapter::permissionRule(const QString& executable,const QString& type){
 // Hyprland matches binary as a regular expression: escape its metacharacters.
 const auto escape=[](const QString& text){QString out;for(const auto c:text){if(QStringLiteral(".[]{}()*+?^$|\\").contains(c))out+='\\';out+=c;}return out;};
 // An AppImage runs from a new /tmp/.mount_<random> directory each start.
 static const QRegularExpression mount(QStringLiteral("^(.*/)\\.mount_[^/]+/(.*)$"));
 const auto match=mount.match(executable);
 const QString pattern=match.hasMatch()?escape(match.captured(1))+"\\.mount_[^/]+/"+escape(match.captured(2)):escape(executable);
 // A Lua long string keeps the backslashes as written (a "\." escape is a Lua error).
 const QString binary=pattern.contains(QStringLiteral("]]"))?HyprlandIpc::luaString(pattern):"[["+pattern+"]]";
 return QStringLiteral("hl.permission({ binary = %1, type = \"%2\", mode = \"allow\" })").arg(binary,type);
}
Error HyprlandSourceAdapter::permissionError(){
 const auto rule=permissionRule(QCoreApplication::applicationFilePath());
 return {"capture_permission","Hyprland denied screen capture to put.gd. Allow it in your Hyprland configuration: "+rule,{{"rule",rule},{"setting","ecosystem:enforce_permissions"}}};
}
void HyprlandSourceAdapter::freezeActiveWindow(std::function<void(QJsonObject,Error)> reply){
 QPointer<HyprlandSourceAdapter> guard(this);
 ipc_.json("j/activewindow",[guard,reply=std::move(reply)](QJsonDocument document,Error error){
  if(!guard){reply({},{"cancelled","Window acquisition cancelled",{}});return;}
  if(error){reply({},error);return;}
  const auto window=document.object();
  const auto identity=window["stableId"].toString();
  if(identity.isEmpty()||window["address"].toString().isEmpty()){reply({},{"target_unavailable",focusMessage,{}});return;}
  const auto pid=qint64(window["pid"].toDouble());
  if(guard->ownWindow(pid,window["class"].toString())){reply({},{"target_unavailable",focusMessage,{{"reason","put.gd's own window is active"}}});return;}
  QJsonObject frozen{{"identity",identity},{"address",window["address"]},{"pid",double(pid)},{"appId",window["class"]},{"title",window["title"]},
   {"at",window["at"]},{"size",window["size"]},{"workspace",window["workspace"].toObject()["name"]},{"xwayland",window["xwayland"]}};
  // The protocol, not the IPC, must say that this identifier is that window.
  guard->mapping_.addressFor(identity,[guard,frozen,reply](QString address,Error error){
   if(!guard){reply({},{"cancelled","Window acquisition cancelled",{}});return;}
   if(error){reply({},error);return;}
   if(!ToplevelMapping::sameAddress(address,frozen["address"].toString())){
    reply({},{"capture_target","The active window changed while it was being confirmed",{{"identity",frozen["identity"]},{"ipcAddress",frozen["address"]},{"protocolAddress",address}}});return;
   }
   reply(frozen,{});
  });
 });
}
void HyprlandSourceAdapter::listWindows(RecordingWindows::Reply reply){
 QPointer<HyprlandSourceAdapter> guard(this);
 ipc_.json("j/activewindow",[guard,reply=std::move(reply)](QJsonDocument active,Error error)mutable{
  if(!guard){reply({"cancelled","Desktop adapter closed",{}},{});return;}
  if(error){reply(error,{});return;}
  const auto activeId=active.object()["stableId"].toString();
  guard->ipc_.json("j/clients",[guard,activeId,reply=std::move(reply)](QJsonDocument clients,Error error){
   if(!guard){reply({"cancelled","Desktop adapter closed",{}},{});return;}
   if(error){reply(error,{});return;}
   QJsonArray out;bool activeExcluded=false;
   for(const auto& value:clients.array()){
    const auto w=value.toObject();const auto identity=w["stableId"].toString();
    if(identity.isEmpty()||!w["mapped"].toBool(true))continue;
    if(guard->ownWindow(qint64(w["pid"].toDouble()),w["class"].toString())){if(identity==activeId)activeExcluded=true;continue;}
    const auto at=w["at"].toArray(),size=w["size"].toArray();
    out.append(QJsonObject{{"identity",identity},{"title",w["title"]},{"appId",w["class"]},
     {"rect",QJsonArray{at.at(0),at.at(1),size.at(0),size.at(1)}},{"active",identity==activeId},
     {"minimized",w["hidden"].toBool()||w["workspace"].toObject()["name"].toString().startsWith("special:")}});
   }
   reply({},{{"windows",out},{"activeExcluded",activeExcluded}});
  });
 });
}
QJsonObject HyprlandSourceAdapter::capabilities()const{
 const auto pixels=capture_.status();const auto outputs=outputs_.status();
 QString outputReason,windowReason;const bool outputsReady=capture_.outputsAvailable(&outputReason);const bool windowsReady=capture_.windowsAvailable(&windowReason);
 QJsonObject desktop{{"available",outputsReady},{"protocol","ext-image-copy-capture-v1"}};if(!outputsReady)desktop["error"]=outputReason;
 QJsonObject region=desktop;
 QJsonObject window{{"available",windowsReady},{"protocol","ext-foreign-toplevel-image-capture-source-v1"},{"content","client-only"},{"occlusion","ignored"},{"target","hyprland-activewindow"}};
 if(!windowsReady){window["error"]=windowReason;window["code"]="window_unavailable";}
 if(!outputs["available"].toBool()){const auto error=outputs["error"];for(auto* status:{&region,&desktop}){(*status)["available"]=false;(*status)["error"]=error;}}
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 region["available"]=false;region["error"]="Native region selection requires current distro per-window LayerShellQt and Qt 6.5 or later";
#endif
 if(region_.cleanupRequired()){for(auto* status:{&region,&window,&desktop}){(*status)["available"]=false;(*status)["error"]=cleanupMessage;(*status)["code"]="capture_cleanup_pending";}}
 const QJsonObject helper=region_.diagnostics();region["helper"]=helper;
 QJsonObject shortcuts{{"available",shortcuts_.available()},{"backend",shortcuts_.name()},{"bindings",shortcuts_.status()},{"hyprland",shortcuts_.diagnostics()}};
 if(!shortcuts_.error().isEmpty())shortcuts["error"]=shortcuts_.error();
 return {{"adapter","hyprland-wayland"},{"resident","qt-core"},{"captureHelper",helper},{"outputs",outputs},{"imageCopy",pixels},
  {"capture",QJsonObject{{"region",region},{"window",window},{"desktop",desktop}}},{"clipboard",clipboard_.status()},
  {"shortcuts",shortcuts}};
}
SourceAdapter::PreparedAcquisition HyprlandSourceAdapter::prepareAcquisition(const QString& action){
 QPointer<HyprlandSourceAdapter> guard(this);
 if(action=="clipboard"){const auto revision=clipboard_.revision();return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(guard)guard->clipboard_.acquire(limits,std::move(cb),revision);else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};}
 if(action=="window"){
  if(region_.cleanupRequired())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"capture_cleanup_pending",cleanupMessage,{}}});};
  if(region_.busy())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"busy_selecting","Region selection is already active",{}}});};
  // Freeze the active window at invocation through IPC, then read its pixels
  // now: a focus change or put.gd's own window opening later cannot retarget it.
  auto snapshot=std::make_shared<WindowSnapshot>();const auto generation=generation_;const auto limits=limits_;
  auto complete=[guard,snapshot,generation](QImage image,Error error){
   snapshot->done=true;
   if(!guard||guard->generation_!=generation)snapshot->error={"cancelled","Window acquisition cancelled",{}};
   else{snapshot->error=std::move(error);if(!snapshot->error)snapshot->image=std::move(image);}
   if(snapshot->accepted){auto cb=std::move(snapshot->accepted);cb(imageResult(snapshot->image,snapshot->acceptedLimits,snapshot->error));}
  };
  freezeActiveWindow([guard,complete,limits](QJsonObject target,Error error){
   if(error){complete({},std::move(error));return;}
   if(!guard){complete({},{"cancelled","Window acquisition cancelled",{}});return;}
   guard->capture_.captureToplevel(target["identity"].toString(),limits,[complete](QImage image,Error error){
    if(!error&&refusedFrame(image)){complete({},permissionError());return;}
    complete(std::move(image),std::move(error));
   });
  });
  return [guard,snapshot,generation](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Window acquisition cancelled",{}}});return;}if(snapshot->done)cb(imageResult(snapshot->image,limits,snapshot->error));else{snapshot->acceptedLimits=limits;snapshot->accepted=std::move(cb);}};
 }
 const auto frozen=layout();const auto revision=outputs_.revision();
 return [guard,action,frozen,revision,outcome=captureOutcome_,screenshot=action=="region"&&regionScreenshotFirst_](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(!guard){cb({{},{"source_unavailable","Desktop adapter closed",{}}});return;}guard->capture(action,frozen,revision,limits,std::move(cb),outcome,screenshot);};
}
SourceAdapter::PreparedAcquisition HyprlandSourceAdapter::prepareAcquisition(const QString& action,const ResourceLimits& acceptedLimits,std::function<void(Error)> terminal,std::function<void()> released){
#if defined(PUTGD_HAVE_LAYER_SHELL) && QT_VERSION >= QT_VERSION_CHECK(6,5,0)
 // A live selection shows before the journal saves; its pixels still wait.
 const auto frozen=action=="region"&&!regionScreenshotFirst_?layout():QList<CaptureFrame>{};
 const auto revision=outputs_.revision();
 if(!frozen.isEmpty()&&!region_.busy()&&!region_.cleanupRequired()&&unchanged(frozen,revision)){
  QPointer<HyprlandSourceAdapter> guard(this);const auto generation=generation_;
  auto ticket=region_.prepare(frozen,captureOutcome_,captureAppearance_,std::move(terminal),std::move(released));
  return [guard,ticket,frozen,revision,acceptedLimits,generation](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
   if(!guard){cb({{},{"source_unavailable","Desktop adapter closed",{}}});return;}
   if(limits.imagePixels!=acceptedLimits.imagePixels||limits.imageBytes!=acceptedLimits.imageBytes||limits.maxFileBytes!=acceptedLimits.maxFileBytes){guard->region_.discard(ticket);cb({{},{"input_excessive","Prepared capture limits changed",{}}});return;}
   guard->region_.begin(ticket,[guard,frozen,revision,limits,generation,cb=std::move(cb)](QRectF selection,Error error)mutable{
    if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
    if(error){cb({{},std::move(error)});return;}
    if(!guard->unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed during region selection",{}}});return;}
    guard->captureScreens(frozen,revision,selection,limits,std::move(cb));
   });
  };
 }
#else
 Q_UNUSED(acceptedLimits);Q_UNUSED(terminal);
#endif
 if(released)released();
 return prepareAcquisition(action);
}
void HyprlandSourceAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){prepareAcquisition(action)(limits,std::move(cb));}
void HyprlandSourceAdapter::capture(QString action,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(PayloadResult)> cb,const QString& outcome,bool screenshotFirst){
 if(action!="region"&&action!="desktop"){cb({{},{"source_unsupported","Unknown capture action",{}}});return;}
 if(region_.cleanupRequired()){cb({{},{"capture_cleanup_pending",cleanupMessage,{}}});return;}
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 if(action=="region"){cb({{},{"region_unavailable","Native region selection requires current distro per-window LayerShellQt",{}}});return;}
#endif
 if(region_.busy()){cb({{},{"busy_selecting","Region selection is already active",{}}});return;}
 if(frozen.isEmpty()||!unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed after invocation",{}}});return;}
 if(action=="region"&&screenshotFirst){selectFrozen(std::move(frozen),revision,limits,std::move(cb),outcome);return;}
 if(action=="region"){
  const auto generation=generation_;QPointer<HyprlandSourceAdapter> guard(this);
  region_.start(frozen,[guard,frozen,revision,limits,generation,cb=std::move(cb)](QRectF selection,Error error)mutable{
   if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
   if(error){cb({{},std::move(error)});return;}
   if(!guard->unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed during region selection",{}}});return;}
   guard->captureScreens(frozen,revision,selection,limits,std::move(cb));
  },outcome,{},captureAppearance_);
  return;
 }
 QRectF bounds;for(const auto& frame:frozen)bounds=bounds.united(frame.logicalRect);captureScreens(std::move(frozen),revision,bounds,limits,std::move(cb));
}
void HyprlandSourceAdapter::captureScreens(QList<CaptureFrame> frozen,quint64 revision,QRectF selection,ResourceLimits limits,std::function<void(PayloadResult)> cb){
 QPointer<HyprlandSourceAdapter> guard(this);auto layout=frozen;
 QList<CaptureFrame> needed;for(const auto& frame:frozen)if(frame.logicalRect.intersects(selection))needed.append(frame);
 if(needed.isEmpty()){cb({{},{"capture_geometry","The selection lies outside every output",{}}});return;}
 captureFrames(std::move(needed),revision,limits,[guard,layout,revision,selection,limits,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  if(error){cb({{},error});return;}
  if(!guard){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
  guard->compose(std::move(frames),std::move(layout),revision,selection,limits,std::move(cb));
 });
}
void HyprlandSourceAdapter::selectFrozen(QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(PayloadResult)> cb,const QString& outcome){
 QPointer<HyprlandSourceAdapter> guard(this);const auto generation=generation_;auto layout=frozen;
 // Read every output before any selector surface maps; open menus stay in frame.
 captureFrames(std::move(frozen),revision,limits,[guard,layout,revision,limits,generation,outcome,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  if(error){if(!error.details.contains("stage"))error.details["stage"]="freeze";cb({{},error});return;}
  if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
  auto pixels=std::make_shared<QList<CaptureFrame>>(std::move(frames));auto reply=std::make_shared<std::function<void(PayloadResult)>>(std::move(cb));
  guard->region_.start(layout,[guard,layout,revision,limits,generation,pixels,reply](QRectF selection,Error error){
   if(!guard||guard->generation_!=generation){(*reply)({{},{"cancelled","Desktop capture cancelled",{}}});return;}
   if(error){(*reply)({{},std::move(error)});return;}
   if(!guard->unchanged(layout,revision)){(*reply)({{},{"display_changed","Display layout changed during region selection",{}}});return;}
   guard->compose(std::move(*pixels),layout,revision,selection,limits,std::move(*reply));
  },outcome,*pixels,guard->captureAppearance_);
 });
}
void HyprlandSourceAdapter::captureFrames(QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(QList<CaptureFrame>,Error)> cb){
 struct Batch {QList<CaptureFrame> frames,frozenLayout;qint64 bytes=0,pixels=0;int index=0;std::function<void(QList<CaptureFrame>,Error)> reply;std::function<void()> next;};
 auto batch=std::make_shared<Batch>();batch->frames=frozen;batch->frozenLayout=frozen;batch->reply=std::move(cb);QPointer<HyprlandSourceAdapter> guard(this);const auto generation=generation_;std::weak_ptr<Batch> weak=batch;
 batch->next=[guard,batchWeak=weak,revision,limits,generation]{auto batch=batchWeak.lock();if(!batch)return;
  auto fail=[batch](Error error){auto cb=std::move(batch->reply);batch->next={};if(cb)cb({},error);};
  if(!guard||generation!=guard->generation_){fail({"cancelled","Desktop capture cancelled",{}});return;}
  if(!guard->unchanged(batch->frozenLayout,revision)){fail({"display_changed","Display layout changed during acquisition",{}});return;}
  if(batch->index==batch->frames.size()){auto frames=std::move(batch->frames);auto reply=std::move(batch->reply);batch->next={};reply(std::move(frames),{});return;}
  const auto index=batch->index;
  ResourceLimits remaining=limits;remaining.imagePixels=limits.imagePixels-batch->pixels;remaining.imageBytes=limits.imageBytes-batch->bytes;
  guard->capture_.captureOutput(batch->frames[index].identity,remaining,[guard,batch,index,revision,limits,generation](QImage image,Error error){
   const auto reject=[batch](Error failure){auto cb=std::move(batch->reply);batch->next={};if(cb)cb({},failure);};
   if(!guard||generation!=guard->generation_){reject({"cancelled","Desktop capture cancelled",{}});return;}
   if(error){reject(error);return;}
   if(!guard->unchanged(batch->frozenLayout,revision)){reject({"display_changed","Display layout changed during acquisition",{}});return;}
   // A denied client gets a transparent frame of the right size, never an error.
   if(refusedFrame(image)){reject(permissionError());return;}
   auto& frame=batch->frames[index];
   if(image.size()!=frame.nativeRect.size()){reject({"display_changed","Captured output does not match frozen output geometry",{{"captured",QStringLiteral("%1x%2").arg(image.width()).arg(image.height())},{"expected",QStringLiteral("%1x%2").arg(frame.nativeRect.width()).arg(frame.nativeRect.height())}}});return;}
   const auto pixels=qint64(image.width())*image.height();
   if(pixels>limits.imagePixels-batch->pixels||image.sizeInBytes()>limits.imageBytes-batch->bytes){reject({"input_excessive","Desktop frame snapshots exceed resource limits",{}});return;}
   batch->pixels+=pixels;batch->bytes+=image.sizeInBytes();image.setDevicePixelRatio(frame.density);frame.pixels=std::move(image);
   ++batch->index;auto next=batch->next;next();
  });
 };
 auto next=batch->next;next();
}
void HyprlandSourceAdapter::compose(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,QRectF selection,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
 const auto generation=generation_;auto result=std::make_shared<PayloadResult>();
 auto* worker=QThread::create([frames=std::move(frames),selection,limits,result]{result->payload.image=composeOutputFrames(frames,selection,std::min(limits.imagePixels,limits.imageBytes/4),result->error);result->payload.kind="image";result->payload.mime="image/png";});workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,generation,frozen=std::move(frozen),revision,result,cb=std::move(cb)]()mutable{workers_.removeOne(worker);worker->deleteLater();if(generation!=generation_)cb({{},{"cancelled","Capture acquisition cancelled",{}}});else if(!unchanged(frozen,revision))cb({{},{"display_changed","Display layout changed during capture composition",{}}});else cb(std::move(*result));});
 worker->start();
}
void HyprlandSourceAdapter::cancelAcquisition(){++generation_;capture_.cancel();clipboard_.cancel();region_.cancel();}
void HyprlandSourceAdapter::copy(const CopyPayload& payload,std::function<void(Error)> cb){clipboard_.copy(payload,std::move(cb));}
void HyprlandSourceAdapter::invalidateCopyEligibility(){clipboard_.invalidateCopyEligibility();}
}
