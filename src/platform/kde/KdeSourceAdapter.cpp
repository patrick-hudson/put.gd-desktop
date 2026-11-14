#include "platform/ProcessStat.h"
#include "KdeSourceAdapter.h"
#include "platform/wayland/CaptureCompose.h"
#include "platform/wayland/CaptureSequence.h"
#include "platform/SessionLock.h"
#include <QGuiApplication>
#include <QCoreApplication>
#include <QScreen>
#include <QJsonArray>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUuid>
#include <QFile>
#include <cmath>
namespace putgd {
namespace {
QList<CaptureFrame> graphicalLayout(){QList<CaptureFrame> out;for(auto* s:QGuiApplication::screens()){CaptureFrame frame;frame.identity=s->name();frame.logicalRect=s->geometry();frame.density=s->devicePixelRatio();frame.rotation=s->angleBetween(s->nativeOrientation(),s->orientation());frame.nativeRect=QRect(QPoint(qRound(s->geometry().x()*frame.density),qRound(s->geometry().y()*frame.density)),QSize(qRound(s->geometry().width()*frame.density),qRound(s->geometry().height()*frame.density)));out.append(frame);}return out;}

const QString cleanupMessage="Region helper exited before confirming surface cleanup; restart put.gd before capturing again";
struct WindowSnapshot {QImage image;Error error;bool done=false;std::function<void(PayloadResult)> accepted;ResourceLimits acceptedLimits;};
PayloadResult imageResult(const QImage& image,const ResourceLimits& limits,Error error={}){if(error)return {{},error};if(image.isNull())return {{},{"capture_failed","KWin provided no image",{}}};if(qint64(image.width())*image.height()>limits.imagePixels||image.sizeInBytes()>limits.imageBytes)return {{},{"input_excessive","Capture exceeds accepted operation resource limits",{}}};ClipboardPayload payload;payload.kind="image";payload.mime="image/png";payload.image=image;return {payload,{}};}
}
KdeSourceAdapter::KdeSourceAdapter(QObject* parent):QObject(parent),core_(!qobject_cast<QGuiApplication*>(QCoreApplication::instance())){connect(&outputs_,&WaylandOutputs::changed,this,[this]{if(region_.busy())region_.cancel({"display_changed","Display layout changed during region selection",{}});emit capabilitiesChanged();});connect(&capture_,&KWinCapture::changed,this,&KdeSourceAdapter::capabilitiesChanged);connect(&clipboard_,&DataControlClipboard::changed,this,&KdeSourceAdapter::capabilitiesChanged);connect(&shortcuts_,&KdeShortcuts::changed,this,&KdeSourceAdapter::capabilitiesChanged);connect(&shortcuts_,&KdeShortcuts::activated,this,&KdeSourceAdapter::actionRequested);connect(&clipboard_,&DataControlClipboard::keyboardChanged,this,[this]{shortcuts_.setKeyboardMap(clipboard_.keyboardMap());});shortcuts_.setKeyboardMap(clipboard_.keyboardMap());}
KdeSourceAdapter::~KdeSourceAdapter(){region_.shutdown();cancelAcquisition();clipboard_.cancel();for(auto* worker:workers_){worker->wait();delete worker;}const auto selectors=findChildren<LayerShellRegionOverlay*>(QString{},Qt::FindDirectChildrenOnly);for(auto* selector:selectors)delete selector;}
quint64 KdeSourceAdapter::clipboardRevision()const{return clipboard_.revision();}
QList<CaptureFrame> KdeSourceAdapter::layout()const{return core_?outputs_.layout():graphicalLayout();}
bool KdeSourceAdapter::unchanged(const QList<CaptureFrame>& before,quint64 revision)const{
 const auto now=layout();if(core_&&outputs_.revision()!=revision)return false;
 return sameOutputLayout(before,now);
}
QJsonObject KdeSourceAdapter::capabilities()const{
 auto region=capture_.status();auto desktop=capture_.status();auto window=capture_.status();
 const QJsonObject outputs=core_?outputs_.status():QJsonObject{{"available",!layout().isEmpty()},{"protocol","qt-screens"}};
 if(core_&&!outputs["available"].toBool()){const auto error=outputs["error"];region["available"]=false;region["error"]=error;desktop["available"]=false;desktop["error"]=error;}
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 region["available"]=false;region["error"]="Native region selection requires current distro per-window LayerShellQt and Qt 6.5 or later";
#endif
 if(core_&&region_.cleanupRequired()){for(auto* status:{&region,&window,&desktop}){(*status)["available"]=false;(*status)["error"]=cleanupMessage;(*status)["code"]="capture_cleanup_pending";}}
 const QJsonObject helper=region_.diagnostics();
 // One monitor and a repeat read outputs as a desktop capture does; neither needs the selector.
 auto monitor=desktop;auto repeat=desktop;repeat["remembered"]=lastRegion_.has_value();
 // The countdown pill is a layer surface of the region helper.
 QJsonObject delay{{"available",region["available"].toBool()},{"seconds",QJsonArray{3,5,10}},{"indicator","layer-surface"}};if(!region["available"].toBool())delay["error"]=region["error"];
 if(core_)region["helper"]=helper;
 return {{"adapter","kde-wayland"},{"resident",core_?"qt-core":"qt-widgets"},{"captureHelper",helper},{"outputs",outputs},{"capture",QJsonObject{{"region",region},{"text",region},{"window",window},{"desktop",desktop},{"monitor",monitor},{"repeat",repeat}}},{"delay",delay},{"clipboard",clipboard_.status()},{"shortcuts",QJsonObject{{"available",shortcuts_.available()},{"bindings",shortcuts_.status()}}}};
}
SourceAdapter::PreparedAcquisition KdeSourceAdapter::prepareAcquisition(const QString& requested){
 // Copy text (#1428) selects exactly as a region does, with the same selector,
 // cleanup and fence; only Repeat's memory is left alone.
 const bool text=requested==QLatin1String("text");const QString action=text?QStringLiteral("region"):requested;
 QPointer<KdeSourceAdapter> guard(this);
 if(action=="clipboard"){const auto revision=clipboard_.revision();return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(guard)guard->clipboard_.acquire(limits,std::move(cb),revision);else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};}
 if(action=="window"){
  if(core_&&region_.cleanupRequired())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"capture_cleanup_pending",cleanupMessage,{}}});};
  if(region_.busy())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"busy_selecting","Region selection is already active",{}}});};
  if(!core_&&QGuiApplication::applicationState()==Qt::ApplicationActive&&QGuiApplication::focusWindow())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"target_unavailable","Focus an external window and invoke the window hotkey or CLI",{}}});};
  // ScreenShot2 freezes active focus at method invocation. Starting this bounded
  // request now prevents persistence or GUI focus changes from retargeting it.
  auto snapshot=std::make_shared<WindowSnapshot>();const auto generation=generation_;
  // Keep the invocation's helper state even if that process exits or restarts
  // while ScreenShot2 transfers the frozen window's pixels.
  const bool checkUi=uiProcess_!=0;
  auto complete=[guard,snapshot,generation](QImage image,Error error){
   snapshot->done=true;
   if(!guard||guard->generation_!=generation)snapshot->error={"cancelled","Window acquisition cancelled",{}};
   else{snapshot->error=std::move(error);if(!snapshot->error)snapshot->image=std::move(image);}
   if(snapshot->accepted){auto cb=std::move(snapshot->accepted);cb(imageResult(snapshot->image,snapshot->acceptedLimits,snapshot->error));}
  };
  capture_.request("CaptureActiveWindow",{},limits_,[guard,checkUi,complete](QImage image,QVariantMap metadata,Error error){
   if(error||!checkUi||!guard){complete(std::move(image),std::move(error));return;}
   // Inspect the returned UUID, never current focus. No image leaves this
   // adapter for persistence/publication until the native identity is checked.
   guard->verifyExternalWindow(metadata.value("windowId").toString(),[complete,image=std::move(image)](Error error)mutable{complete(std::move(image),std::move(error));});
  });
  return [guard,snapshot,generation](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Window acquisition cancelled",{}}});return;}if(snapshot->done)cb(imageResult(snapshot->image,limits,snapshot->error));else{snapshot->acceptedLimits=limits;snapshot->accepted=std::move(cb);}};
 }
 const auto frozen=layout();const auto revision=core_?outputs_.revision():0;
 if(action=="monitor"){
  // The pointer's output is frozen now, at invocation, as a window's focus is.
  struct PointerSnapshot {QRectF selection;Error error;bool done=false;std::function<void()> ready;};
  auto snapshot=std::make_shared<PointerSnapshot>();const auto generation=generation_;
  windows_.pointer([snapshot,frozen](Error error,QPointF position){
   snapshot->done=true;
   if(error)snapshot->error={"capture_target","KWin did not report the pointer position",{{"reason",error.message}}};
   else if(const auto* frame=outputAt(frozen,position))snapshot->selection=frame->logicalRect;
   else snapshot->error={"capture_target","The pointer is not over a monitor",{}};
   if(auto ready=std::exchange(snapshot->ready,{}))ready();
  });
  return [guard,snapshot,frozen,revision,generation](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
   auto proceed=[guard,snapshot,frozen,revision,generation,limits,cb=std::move(cb)]()mutable{
    if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Monitor capture cancelled",{}}});return;}
    if(snapshot->error){cb({{},snapshot->error});return;}
    guard->captureArea(frozen,revision,snapshot->selection,limits,std::move(cb));
   };
   if(snapshot->done)proceed();else snapshot->ready=std::move(proceed);
  };
 }
 if(action=="repeat"){
  Error error;QRectF selection;
  if(!lastRegion_)error=noLastRegion();
  else if(!sameOutputLayout(lastRegion_->layout,frozen))error=lastRegionLayoutChanged();
  else selection=lastRegion_->rectangle;
  return [guard,frozen,revision,error,selection](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
   if(error){cb({{},error});return;}
   if(!guard){cb({{},{"source_unavailable","Desktop adapter closed",{}}});return;}
   guard->captureArea(frozen,revision,selection,limits,std::move(cb));
  };
 }
 return [guard,action,frozen,revision,text,outcome=captureOutcome_,screenshot=action=="region"&&regionScreenshotFirst_,delay=action=="region"&&!regionScreenshotFirst_?captureDelay_:0](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(!guard){cb({{},{"source_unavailable","Desktop adapter closed",{}}});return;}guard->capture(action,frozen,revision,limits,std::move(cb),outcome,screenshot,delay,!text);};
}
void KdeSourceAdapter::captureArea(QList<CaptureFrame> frozen,quint64 revision,QRectF selection,ResourceLimits limits,std::function<void(PayloadResult)> cb){
 if(core_&&region_.cleanupRequired()){cb({{},{"capture_cleanup_pending",cleanupMessage,{}}});return;}
 if(overlay_||region_.busy()){cb({{},{"busy_selecting","Region selection is already active",{}}});return;}
 if(frozen.isEmpty()||!unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed after invocation",{}}});return;}
 captureScreens(std::move(frozen),revision,selection,limits,std::move(cb));
}
std::function<void(PayloadResult)> KdeSourceAdapter::remembering(QRectF selection,QList<CaptureFrame> layout,std::function<void(PayloadResult)> cb,bool remember){
 if(!remember)return cb;
 QPointer<KdeSourceAdapter> guard(this);for(auto& frame:layout)frame.pixels={};
 return [guard,selection,layout=std::move(layout),cb=std::move(cb)](PayloadResult result)mutable{
  if(guard&&!result.error)guard->lastRegion_=RegionMemory{selection,std::move(layout)};
  cb(std::move(result));
 };
}
void KdeSourceAdapter::lockGate(int delay,std::function<void(Error)> next){
 if(delay<=0){next({});return;}
 sessionLocked([next=std::move(next)](bool locked){next(locked?lockedError():Error{});});
}
void KdeSourceAdapter::countdown(int seconds,std::function<void(Error)> reply){
 if(overlay_||region_.busy()){reply({"busy_selecting","Region selection is already active",{}});return;}
 countdown_.start(seconds,captureAppearance_,std::move(reply));
}
void KdeSourceAdapter::sessionLocked(std::function<void(bool)> reply){querySessionLocked(this,std::move(reply));}
SourceAdapter::PreparedAcquisition KdeSourceAdapter::prepareAcquisition(const QString& requested,const ResourceLimits& acceptedLimits,std::function<void(Error)> terminal,std::function<void()> released){
 const bool text=requested==QLatin1String("text");const QString action=text?QStringLiteral("region"):requested;
#if defined(PUTGD_HAVE_LAYER_SHELL) && QT_VERSION >= QT_VERSION_CHECK(6,5,0)
 // The selector returns geometry only, so it need not wait for the two journal
 // saves; the pixels still do. Every other case keeps its path through capture().
 const auto frozen=core_&&action=="region"&&!regionScreenshotFirst_?layout():QList<CaptureFrame>{};
 const auto revision=core_?outputs_.revision():0;
 if(!frozen.isEmpty()&&!overlay_&&!region_.busy()&&!region_.cleanupRequired()&&unchanged(frozen,revision)){
  QPointer<KdeSourceAdapter> guard(this);const auto generation=generation_;const int delay=captureDelay_;
  region_.setDelaySeconds(delay);
  auto ticket=region_.prepare(frozen,captureOutcome_,captureAppearance_,std::move(terminal),std::move(released));
  return [guard,ticket,frozen,revision,acceptedLimits,generation,delay,text](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
   if(!guard){cb({{},{"source_unavailable","Desktop adapter closed",{}}});return;}
   if(limits.imagePixels!=acceptedLimits.imagePixels||limits.imageBytes!=acceptedLimits.imageBytes||limits.maxFileBytes!=acceptedLimits.maxFileBytes){guard->region_.discard(ticket);cb({{},{"input_excessive","Prepared capture limits changed",{}}});return;}
   guard->region_.begin(ticket,CaptureSequence::afterSelection(guard,frozen,revision,limits,generation,delay,!text,std::move(cb)));
  };
 }
#else
 Q_UNUSED(acceptedLimits);Q_UNUSED(terminal);
#endif
 if(released)released();
 return prepareAcquisition(requested);
}
void KdeSourceAdapter::verifyExternalWindow(const QString& windowId,std::function<void(Error)> reply){
 const QUuid frozenId(windowId);
 if(frozenId.isNull()){reply({"target_unavailable","KWin did not identify the captured window",{}});return;}
 auto message=QDBusMessage::createMethodCall("org.kde.KWin","/KWin","org.kde.KWin","getWindowInfo");message.setArguments({windowId});message.setAutoStartService(false);
 auto* call=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,1000),this);
 connect(call,&QDBusPendingCallWatcher::finished,this,[call,frozenId,reply=std::move(reply)]()mutable{
  const QDBusPendingReply<QVariantMap> result=*call;call->deleteLater();
  const auto info=result.isError()?QVariantMap{}:result.value();
  const auto text=[](const QVariant& value){return value.metaType().id()==QMetaType::QString||value.metaType().id()==QMetaType::QByteArray;};
  if(result.isError()||!text(info.value("uuid"))||QUuid(info.value("uuid").toString())!=frozenId||!text(info.value("resourceClass"))||!text(info.value("desktopFile"))){reply({"target_unavailable","KWin could not verify the captured window identity",{}});return;}
  // Plasma 6.3/6.6 exposes the native xdg app ID here, not a verified PID.
  // The supervised helper sets this dedicated ID before creating any surface.
  if(info.value("resourceClass").toString()=="putgd-ui"||info.value("desktopFile").toString()=="putgd-ui"){reply({"target_unavailable","Focus an external window and invoke the window hotkey or CLI",{}});return;}
  reply({});
 });
}
void KdeSourceAdapter::listWindows(RecordingWindows::Reply reply){
 const qint64 resident=QCoreApplication::applicationPid(),ui=uiProcess_;
 // KWin lists the native helper PID: UI/editor/recorder are direct QProcess
 // children (#1278), so preserve the direct-child rule shared with X11.
 constexpr int residentAncestorDepth=1;
 const auto parent=[residentAncestorDepth](qint64 pid)->qint64{return ancestorProcess(pid,residentAncestorDepth);};
 windows_.list([resident,ui,parent](const QJsonObject& w){
  const auto pid=qint64(w["pid"].toDouble());
  if(w["resourceClass"].toString()=="putgd-ui"||w["desktopFile"].toString()=="putgd-ui")return true;
  return pid>0&&(pid==resident||(ui>0&&pid==ui)||parent(pid)==resident);
 },std::move(reply));
}
void KdeSourceAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){prepareAcquisition(action)(limits,std::move(cb));}
void KdeSourceAdapter::capture(QString action,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(PayloadResult)> cb,const QString& outcome,bool screenshotFirst,int delay,bool remember){
 if(action!="region"&&action!="desktop"){cb({{},{"source_unsupported","Unknown capture action",{}}});return;}
 if(core_&&region_.cleanupRequired()){cb({{},{"capture_cleanup_pending",cleanupMessage,{}}});return;}
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 if(action=="region"){cb({{},{"region_unavailable","Native region selection requires current distro per-window LayerShellQt",{}}});return;}
#endif
 if(overlay_||region_.busy()){cb({{},{"busy_selecting","Region selection is already active",{}}});return;}
 if(frozen.isEmpty()||!unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed after invocation",{}}});return;}
 if(action=="region"&&screenshotFirst){selectFrozen(std::move(frozen),revision,limits,std::move(cb),outcome,remember);return;}
 if(action=="region"){
  if(core_){
   const auto generation=generation_;QPointer<KdeSourceAdapter> guard(this);
   region_.setDelaySeconds(delay);
   region_.start(frozen,CaptureSequence::afterSelection(guard,frozen,revision,limits,generation,delay,remember,std::move(cb)),outcome,{},captureAppearance_);return;
  }
  auto* selector=new LayerShellRegionOverlay(frozen,this,false,outcome);overlay_=selector;auto reply=std::make_shared<std::function<void(PayloadResult)>>(std::move(cb));const auto generation=generation_;
  connect(selector,&LayerShellRegionOverlay::selected,this,[this,selector,frozen,revision,limits,reply,generation,delay,remember](QRectF selection){
   overlay_=nullptr;
   connect(selector,&QObject::destroyed,this,[this,frozen,revision,selection,limits,reply,generation,delay,remember]{
    if(generation!=generation_){(*reply)({{},{"cancelled","Desktop capture cancelled",{}}});return;}
    const auto read=[this,frozen,revision,selection,limits,reply,generation,remember](Error error){
     if(generation!=generation_){(*reply)({{},{"cancelled","Desktop capture cancelled",{}}});return;}
     if(error){(*reply)({{},std::move(error)});return;}
     captureScreens(frozen,revision,selection,limits,remembering(selection,frozen,std::move(*reply),remember));
    };
    // Area first, then the countdown, then the pixels (#1363).
    if(delay>0)delayed(delay,read);else read({});
   });
   selector->deleteLater();
  });
  connect(selector,&LayerShellRegionOverlay::cancelled,this,[this,selector,reply](Error error){overlay_=nullptr;selector->deleteLater();(*reply)({{},error});});selector->start();return;
 }
 QRectF bounds;for(const auto& frame:frozen)bounds=bounds.united(frame.logicalRect);captureScreens(std::move(frozen),revision,bounds,limits,std::move(cb));
}
void KdeSourceAdapter::captureScreens(QList<CaptureFrame> frozen,quint64 revision,QRectF selection,ResourceLimits limits,std::function<void(PayloadResult)> cb){
 QPointer<KdeSourceAdapter> guard(this);auto layout=frozen;
 // Only the outputs the selection touches are read; a desktop capture's selection is all of them.
 QList<CaptureFrame> needed;for(const auto& frame:frozen)if(frame.logicalRect.intersects(selection))needed.append(frame);
 if(needed.isEmpty()){cb({{},{"capture_geometry","The selection lies outside every output",{}}});return;}
 captureFrames(std::move(needed),layout,revision,limits,[guard,layout,revision,selection,limits,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  if(error){cb({{},error});return;}
  if(!guard){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
  guard->compose(std::move(frames),std::move(layout),revision,selection,limits,std::move(cb));
 });
}
void KdeSourceAdapter::selectFrozen(QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(PayloadResult)> cb,const QString& outcome,bool remember){
 QPointer<KdeSourceAdapter> guard(this);const auto generation=generation_;auto layout=frozen;
 // Read every output before any selector surface maps; open menus stay in frame.
 captureFrames(std::move(frozen),layout,revision,limits,[guard,layout,revision,limits,generation,outcome,remember,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  if(error){if(!error.details.contains("stage"))error.details["stage"]="freeze";cb({{},error});return;}
  if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
  auto pixels=std::make_shared<QList<CaptureFrame>>(std::move(frames));auto reply=std::make_shared<std::function<void(PayloadResult)>>(std::move(cb));
  // The result is cut from the same frames; nothing is read again.
  auto selected=[guard,layout,revision,limits,generation,pixels,reply,remember](QRectF selection,Error error){
   if(!guard||guard->generation_!=generation){(*reply)({{},{"cancelled","Desktop capture cancelled",{}}});return;}
   if(error){(*reply)({{},std::move(error)});return;}
   if(!guard->unchanged(layout,revision)){(*reply)({{},{"display_changed","Display layout changed during region selection",{}}});return;}
   guard->compose(std::move(*pixels),layout,revision,selection,limits,guard->remembering(selection,layout,std::move(*reply),remember));
  };
  if(guard->core_){guard->region_.start(layout,std::move(selected),outcome,*pixels,guard->captureAppearance_);return;}
  auto* selector=new LayerShellRegionOverlay(*pixels,guard,false,outcome,true);guard->overlay_=selector;
  connect(selector,&LayerShellRegionOverlay::selected,guard,[guard,selector,selected](QRectF selection){
   guard->overlay_=nullptr;connect(selector,&QObject::destroyed,guard,[selected,selection]{selected(selection,{});});selector->deleteLater();
  });
  connect(selector,&LayerShellRegionOverlay::cancelled,guard,[guard,selector,selected](Error error){guard->overlay_=nullptr;selector->deleteLater();selected({},std::move(error));});
  selector->start();
 });
}
void KdeSourceAdapter::captureFrames(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(QList<CaptureFrame>,Error)> cb){
 // frames: the outputs to read; frozen: the whole layout, which must not change.
 // Qt screen DPR may be rounded on fractional Wayland outputs. Enforce image
 // budgets against ScreenShot2 native metadata, never guessed Qt pixel counts.
 QPointer<KdeSourceAdapter> guard(this);
 CaptureSequence::readFrames(guard,std::move(frames),std::move(frozen),revision,limits,
  [guard](const CaptureFrame& frame,const ResourceLimits& remaining,CaptureSequence::FrameReply reply){
   guard->capture_.request("CaptureScreen",{frame.identity},remaining,[reply=std::move(reply)](QImage image,QVariantMap metadata,Error error){reply(std::move(image),std::move(metadata),std::move(error));});
  },
  [](CaptureFrame& frame,QImage image,const QVariantMap& metadata,const ResourceLimits& remaining)->Error{
   if(metadata.value("screen").toString()!=frame.identity)return {"capture_metadata","KWin returned a different capture output",{}};
   const auto scale=metadata.value("scale").toDouble();
   if(std::abs(image.width()/scale-frame.logicalRect.width())>1.0||std::abs(image.height()/scale-frame.logicalRect.height())>1.0)return {"display_changed","KWin native image does not match frozen output geometry",{}};
   const auto pixels=qint64(image.width())*image.height();
   if(pixels>remaining.imagePixels)return {"input_excessive","Desktop frame snapshots exceed image pixel limits",{}};
   if(image.sizeInBytes()>remaining.imageBytes)return {"input_excessive","Desktop frame snapshots exceed resource limits",{}};
   frame.pixels=std::move(image);frame.density=scale;
   frame.nativeRect=QRect(QPoint(qRound(frame.logicalRect.x()*scale),qRound(frame.logicalRect.y()*scale)),frame.pixels.size());
   return {};
  },std::move(cb));
}
void KdeSourceAdapter::compose(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,QRectF selection,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
 const auto generation=generation_;auto result=std::make_shared<PayloadResult>();auto* worker=QThread::create([frames=std::move(frames),selection,limits,result]{result->payload.image=composeOutputFrames(frames,selection,std::min(limits.imagePixels,limits.imageBytes/4),result->error);result->payload.kind="image";result->payload.mime="image/png";});workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,generation,frozen=std::move(frozen),revision,result,cb=std::move(cb)]()mutable{workers_.removeOne(worker);worker->deleteLater();if(generation!=generation_)cb({{},{"cancelled","Capture acquisition cancelled",{}}});else if(!unchanged(frozen,revision))cb({{},{"display_changed","Display layout changed during capture composition",{}}});else cb(std::move(*result));});worker->start();
}
void KdeSourceAdapter::cancelAcquisition(){countdown_.cancel();++generation_;capture_.cancel();clipboard_.cancel();region_.cancel();if(overlay_)overlay_->cancel();}
void KdeSourceAdapter::copy(const CopyPayload& payload,std::function<void(Error)> cb){clipboard_.copy(payload,std::move(cb));}
void KdeSourceAdapter::invalidateCopyEligibility(){clipboard_.invalidateCopyEligibility();}
void KdeSourceAdapter::bindShortcuts(const QHash<QString,QString>& desired){shortcuts_.bind(desired);}
}
