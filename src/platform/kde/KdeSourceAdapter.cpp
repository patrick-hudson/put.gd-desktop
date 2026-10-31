#include "KdeSourceAdapter.h"
#include "KdeCaptureCompose.h"
#include <QGuiApplication>
#include <QCoreApplication>
#include <QScreen>
#include <QJsonArray>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUuid>
#include <cmath>
namespace putgd {
namespace {
QList<CaptureFrame> graphicalLayout(){QList<CaptureFrame> out;for(auto* s:QGuiApplication::screens()){CaptureFrame frame;frame.identity=s->name();frame.logicalRect=s->geometry();frame.density=s->devicePixelRatio();frame.rotation=s->angleBetween(s->nativeOrientation(),s->orientation());frame.nativeRect=QRect(QPoint(qRound(s->geometry().x()*frame.density),qRound(s->geometry().y()*frame.density)),QSize(qRound(s->geometry().width()*frame.density),qRound(s->geometry().height()*frame.density)));out.append(frame);}return out;}

const QString cleanupMessage="Region helper exited before confirming surface cleanup; restart put.gd before capturing again";
struct WindowSnapshot {QImage image;Error error;bool done=false;std::function<void(PayloadResult)> accepted;ResourceLimits acceptedLimits;};
PayloadResult imageResult(const QImage& image,const ResourceLimits& limits,Error error={}){if(error)return {{},error};if(image.isNull())return {{},{"capture_failed","KWin provided no image",{}}};if(qint64(image.width())*image.height()>limits.imagePixels||image.sizeInBytes()>limits.imageBytes)return {{},{"input_excessive","Capture exceeds accepted operation resource limits",{}}};ClipboardPayload payload;payload.kind="image";payload.mime="image/png";payload.image=image;return {payload,{}};}
}
KdeSourceAdapter::KdeSourceAdapter(QObject* parent):QObject(parent),core_(!qobject_cast<QGuiApplication*>(QCoreApplication::instance())){connect(&outputs_,&KdeOutputs::changed,this,[this]{if(region_.busy())region_.cancel({"display_changed","Display layout changed during region selection",{}});emit capabilitiesChanged();});connect(&capture_,&KWinCapture::changed,this,&KdeSourceAdapter::capabilitiesChanged);connect(&clipboard_,&DataControlClipboard::changed,this,&KdeSourceAdapter::capabilitiesChanged);connect(&shortcuts_,&KdeShortcuts::changed,this,&KdeSourceAdapter::capabilitiesChanged);connect(&shortcuts_,&KdeShortcuts::activated,this,&KdeSourceAdapter::actionRequested);connect(&clipboard_,&DataControlClipboard::keyboardChanged,this,[this]{shortcuts_.setKeyboardMap(clipboard_.keyboardMap());});shortcuts_.setKeyboardMap(clipboard_.keyboardMap());}
KdeSourceAdapter::~KdeSourceAdapter(){region_.shutdown();cancelAcquisition();clipboard_.cancel();for(auto* worker:workers_){worker->wait();delete worker;}const auto selectors=findChildren<KdeRegionOverlay*>(QString{},Qt::FindDirectChildrenOnly);for(auto* selector:selectors)delete selector;}
quint64 KdeSourceAdapter::clipboardRevision()const{return clipboard_.revision();}
QList<CaptureFrame> KdeSourceAdapter::layout()const{return core_?outputs_.layout():graphicalLayout();}
bool KdeSourceAdapter::unchanged(const QList<CaptureFrame>& before,quint64 revision)const{
 const auto now=layout();if(core_&&outputs_.revision()!=revision)return false;
 if(now.size()!=before.size())return false;
 for(const auto& frame:before){bool found=false;for(const auto& current:now)if(current.identity==frame.identity&&current.logicalRect==frame.logicalRect&&current.nativeRect==frame.nativeRect&&current.density==frame.density&&current.rotation==frame.rotation)found=true;if(!found)return false;}
 return true;
}
QJsonObject KdeSourceAdapter::capabilities()const{
 auto region=capture_.status();auto desktop=capture_.status();auto window=capture_.status();
 const QJsonObject outputs=core_?outputs_.status():QJsonObject{{"available",!layout().isEmpty()},{"protocol","qt-screens"}};
 if(core_&&!outputs["available"].toBool()){const auto error=outputs["error"];region["available"]=false;region["error"]=error;desktop["available"]=false;desktop["error"]=error;}
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 region["available"]=false;region["error"]="Native region selection requires current distro per-window LayerShellQt and Qt 6.5 or later";
#endif
 if(core_&&region_.cleanupRequired()){for(auto* status:{&region,&window,&desktop}){(*status)["available"]=false;(*status)["error"]=cleanupMessage;(*status)["code"]="capture_cleanup_pending";}}
 const QJsonObject helper=region_.diagnostics();if(core_)region["helper"]=helper;
 return {{"adapter","kde-wayland"},{"resident",core_?"qt-core":"qt-widgets"},{"captureHelper",helper},{"outputs",outputs},{"capture",QJsonObject{{"region",region},{"window",window},{"desktop",desktop}}},{"clipboard",clipboard_.status()},{"shortcuts",QJsonObject{{"available",shortcuts_.available()},{"bindings",shortcuts_.status()}}}};
}
SourceAdapter::PreparedAcquisition KdeSourceAdapter::prepareAcquisition(const QString& action){
 QPointer<KdeSourceAdapter> guard(this);
 if(action=="clipboard"){const auto revision=clipboard_.revision();return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(guard)guard->clipboard_.acquire(limits,std::move(cb),revision);else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};}
 if(action=="window"){
  if(core_&&region_.cleanupRequired())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"capture_cleanup_pending",cleanupMessage,{}}});};
  if(region_.busy())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"busy_selecting","Region selection is already active",{}}});};
  if(!core_&&QGuiApplication::applicationState()==Qt::ApplicationActive&&QGuiApplication::focusWindow())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"target_unavailable","Focus an external window and invoke the window shortcut or CLI",{}}});};
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
 const auto frozen=layout();const auto revision=core_?outputs_.revision():0;return [guard,action,frozen,revision,outcome=captureOutcome_,screenshot=action=="region"&&regionScreenshotFirst_](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(!guard){cb({{},{"source_unavailable","Desktop adapter closed",{}}});return;}guard->capture(action,frozen,revision,limits,std::move(cb),outcome,screenshot);};
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
  if(info.value("resourceClass").toString()=="putgd-ui"||info.value("desktopFile").toString()=="putgd-ui"){reply({"target_unavailable","Focus an external window and invoke the window shortcut or CLI",{}});return;}
  reply({});
 });
}
void KdeSourceAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){prepareAcquisition(action)(limits,std::move(cb));}
void KdeSourceAdapter::capture(QString action,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(PayloadResult)> cb,const QString& outcome,bool screenshotFirst){
 if(action!="region"&&action!="desktop"){cb({{},{"source_unsupported","Unknown capture action",{}}});return;}
 if(core_&&region_.cleanupRequired()){cb({{},{"capture_cleanup_pending",cleanupMessage,{}}});return;}
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 if(action=="region"){cb({{},{"region_unavailable","Native region selection requires current distro per-window LayerShellQt",{}}});return;}
#endif
 if(overlay_||region_.busy()){cb({{},{"busy_selecting","Region selection is already active",{}}});return;}
 if(frozen.isEmpty()||!unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed after invocation",{}}});return;}
 if(action=="region"&&screenshotFirst){selectFrozen(std::move(frozen),revision,limits,std::move(cb),outcome);return;}
 if(action=="region"){
  if(core_){
   const auto generation=generation_;QPointer<KdeSourceAdapter> guard(this);
   region_.start(frozen,[guard,frozen,revision,limits,generation,cb=std::move(cb)](QRectF selection,Error error)mutable{
    if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
    if(error){cb({{},std::move(error)});return;}
    // The helper reports geometry only after compositor unmap confirmation
    // and normal exit. Pixel transfer remains in this resident connection.
    if(!guard->unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed during region selection",{}}});return;}
    guard->captureScreens(frozen,revision,selection,limits,std::move(cb));
   },outcome,{},captureAppearance_);return;
  }
  auto* selector=new KdeRegionOverlay(frozen,this,false,outcome);overlay_=selector;auto reply=std::make_shared<std::function<void(PayloadResult)>>(std::move(cb));const auto generation=generation_;
  connect(selector,&KdeRegionOverlay::selected,this,[this,selector,frozen,revision,limits,reply,generation](QRectF selection){
   overlay_=nullptr;
   connect(selector,&QObject::destroyed,this,[this,frozen,revision,selection,limits,reply,generation]{if(generation!=generation_){(*reply)({{},{"cancelled","Desktop capture cancelled",{}}});return;}captureScreens(frozen,revision,selection,limits,std::move(*reply));});
   selector->deleteLater();
  });
  connect(selector,&KdeRegionOverlay::cancelled,this,[this,selector,reply](Error error){overlay_=nullptr;selector->deleteLater();(*reply)({{},error});});selector->start();return;
 }
 QRectF bounds;for(const auto& frame:frozen)bounds=bounds.united(frame.logicalRect);captureScreens(std::move(frozen),revision,bounds,limits,std::move(cb));
}
void KdeSourceAdapter::captureScreens(QList<CaptureFrame> frozen,quint64 revision,QRectF selection,ResourceLimits limits,std::function<void(PayloadResult)> cb){
 QPointer<KdeSourceAdapter> guard(this);auto layout=frozen;
 captureFrames(std::move(frozen),revision,limits,[guard,layout,revision,selection,limits,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  if(error){cb({{},error});return;}
  if(!guard){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
  guard->compose(std::move(frames),std::move(layout),revision,selection,limits,std::move(cb));
 });
}
void KdeSourceAdapter::selectFrozen(QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(PayloadResult)> cb,const QString& outcome){
 QPointer<KdeSourceAdapter> guard(this);const auto generation=generation_;auto layout=frozen;
 // Read every output before any selector surface maps; open menus stay in frame.
 captureFrames(std::move(frozen),revision,limits,[guard,layout,revision,limits,generation,outcome,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  if(error){if(!error.details.contains("stage"))error.details["stage"]="freeze";cb({{},error});return;}
  if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
  auto pixels=std::make_shared<QList<CaptureFrame>>(std::move(frames));auto reply=std::make_shared<std::function<void(PayloadResult)>>(std::move(cb));
  // The result is cut from the same frames; nothing is read again.
  auto selected=[guard,layout,revision,limits,generation,pixels,reply](QRectF selection,Error error){
   if(!guard||guard->generation_!=generation){(*reply)({{},{"cancelled","Desktop capture cancelled",{}}});return;}
   if(error){(*reply)({{},std::move(error)});return;}
   if(!guard->unchanged(layout,revision)){(*reply)({{},{"display_changed","Display layout changed during region selection",{}}});return;}
   guard->compose(std::move(*pixels),layout,revision,selection,limits,std::move(*reply));
  };
  if(guard->core_){guard->region_.start(layout,std::move(selected),outcome,*pixels,guard->captureAppearance_);return;}
  auto* selector=new KdeRegionOverlay(*pixels,guard,false,outcome,true);guard->overlay_=selector;
  connect(selector,&KdeRegionOverlay::selected,guard,[guard,selector,selected](QRectF selection){
   guard->overlay_=nullptr;connect(selector,&QObject::destroyed,guard,[selected,selection]{selected(selection,{});});selector->deleteLater();
  });
  connect(selector,&KdeRegionOverlay::cancelled,guard,[guard,selector,selected](Error error){guard->overlay_=nullptr;selector->deleteLater();selected({},std::move(error));});
  selector->start();
 });
}
void KdeSourceAdapter::captureFrames(QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(QList<CaptureFrame>,Error)> cb){
 // Qt screen DPR may be rounded on fractional Wayland outputs. Enforce image
 // budgets against ScreenShot2 native metadata, never guessed Qt pixel counts.
 struct Batch {QList<CaptureFrame> frames;QList<CaptureFrame> frozenLayout;qint64 bytes=0,pixels=0;int index=0;std::function<void(QList<CaptureFrame>,Error)> reply;std::function<void()> next;};
 auto batch=std::make_shared<Batch>();batch->frames=frozen;batch->frozenLayout=frozen;batch->reply=std::move(cb);QPointer<KdeSourceAdapter> guard(this);const auto generation=generation_;std::weak_ptr<Batch> weak=batch;
 batch->next=[guard,batchWeak=weak,revision,limits,generation]{auto batch=batchWeak.lock();if(!batch)return;
  auto fail=[batch](Error error){auto cb=std::move(batch->reply);batch->next={};if(cb)cb({},error);};
  if(!guard||generation!=guard->generation_){fail({"cancelled","Desktop capture cancelled",{}});return;}if(!guard->unchanged(batch->frozenLayout,revision)){fail({"display_changed","Display layout changed during acquisition",{}});return;}
  if(batch->index==batch->frames.size()){auto frames=std::move(batch->frames);auto reply=std::move(batch->reply);batch->next={};reply(std::move(frames),{});return;}
  const auto index=batch->index;guard->capture_.request("CaptureScreen",{batch->frames[index].identity},limits,[guard,batch,index,revision,limits,generation](QImage image,QVariantMap metadata,Error error){
   const auto reject=[batch](Error failure){auto cb=std::move(batch->reply);batch->next={};if(cb)cb({},failure);};
   if(!guard||generation!=guard->generation_){reject({"cancelled","Desktop capture cancelled",{}});return;}
   if(error){reject(error);return;}
   if(!guard->unchanged(batch->frozenLayout,revision)){reject({"display_changed","Display layout changed during acquisition",{}});return;}
   auto& frame=batch->frames[index];
   if(metadata.value("screen").toString()!=frame.identity){reject({"capture_metadata","KWin returned a different capture output",{}});return;}
   const auto scale=metadata.value("scale").toDouble();
   if(std::abs(image.width()/scale-frame.logicalRect.width())>1.0||std::abs(image.height()/scale-frame.logicalRect.height())>1.0){reject({"display_changed","KWin native image does not match frozen output geometry",{}});return;}
   const auto pixels=qint64(image.width())*image.height();
   if(pixels>limits.imagePixels-batch->pixels){reject({"input_excessive","Desktop frame snapshots exceed image pixel limits",{}});return;}
   if(image.sizeInBytes()>limits.imageBytes-batch->bytes){reject({"input_excessive","Desktop frame snapshots exceed resource limits",{}});return;}
   batch->pixels+=pixels;batch->bytes+=image.sizeInBytes();frame.pixels=std::move(image);frame.density=scale;
   frame.nativeRect=QRect(QPoint(qRound(frame.logicalRect.x()*scale),qRound(frame.logicalRect.y()*scale)),frame.pixels.size());
   ++batch->index;auto next=batch->next;next();
  });
 };
 // The first native request retains batch until its callback; next is weak to
 // avoid a self-owning function cycle across cancellation and adapter shutdown.
 auto next=batch->next;next();
}
void KdeSourceAdapter::compose(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,QRectF selection,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
 const auto generation=generation_;auto result=std::make_shared<PayloadResult>();auto* worker=QThread::create([frames=std::move(frames),selection,limits,result]{result->payload.image=composeKdeFrames(frames,selection,std::min(limits.imagePixels,limits.imageBytes/4),result->error);result->payload.kind="image";result->payload.mime="image/png";});workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,generation,frozen=std::move(frozen),revision,result,cb=std::move(cb)]()mutable{workers_.removeOne(worker);worker->deleteLater();if(generation!=generation_)cb({{},{"cancelled","Capture acquisition cancelled",{}}});else if(!unchanged(frozen,revision))cb({{},{"display_changed","Display layout changed during capture composition",{}}});else cb(std::move(*result));});worker->start();
}
void KdeSourceAdapter::cancelAcquisition(){++generation_;capture_.cancel();clipboard_.cancel();region_.cancel();if(overlay_)overlay_->cancel();}
void KdeSourceAdapter::copy(const CopyPayload& payload,std::function<void(Error)> cb){clipboard_.copy(payload,std::move(cb));}
void KdeSourceAdapter::invalidateCopyEligibility(){clipboard_.invalidateCopyEligibility();}
void KdeSourceAdapter::bindShortcuts(const QHash<QString,QString>& desired){shortcuts_.bind(desired);}
}
