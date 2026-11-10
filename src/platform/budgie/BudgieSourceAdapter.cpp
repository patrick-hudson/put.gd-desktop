#include "BudgieSourceAdapter.h"
#include "platform/wayland/CaptureCompose.h"
#include "platform/wayland/SurfaceVisibility.h"
#include <QCoreApplication>
#include <QJsonArray>
namespace putgd {
namespace {
Error visibilityError(){return {"session_locked","The screen is locked, blanked or not responding. Unlock it and try again.",{}};}
const QString cleanupMessage="Region helper exited before confirming surface cleanup; restart put.gd before capturing again";
struct WindowSnapshot {QImage image;Error error;bool done=false;std::function<void(PayloadResult)> accepted;ResourceLimits acceptedLimits;};
PayloadResult imageResult(const QImage& image,const ResourceLimits& limits,Error error={}){
 if(error)return {{},error};
 if(image.isNull())return {{},{"capture_failed","The compositor provided no image",{}}};
 if(qint64(image.width())*image.height()>limits.imagePixels||image.sizeInBytes()>limits.imageBytes)return {{},{"input_excessive","Capture exceeds accepted operation resource limits",{}}};
 ClipboardPayload payload;payload.kind="image";payload.mime="image/png";payload.image=image;return {payload,{}};
}
}
// labwc exposes no client-held session-lock boundary. Witness both sides of
// the copy and discard its pixels if the final frame is withheld. Only a lock
// and unlock entirely between these witnesses can escape this bounded check.
void BudgieSourceAdapter::queryVisibility(std::function<void(bool)> reply){
 if(visibilityQuery_)visibilityQuery_(std::move(reply));
 else querySurfaceVisible(this,capture_.connection(),std::move(reply));
}
bool BudgieSourceAdapter::ownAppId(const QString& appId){return putgdAppId(appId);}
BudgieSourceAdapter::BudgieSourceAdapter(QObject* parent):QObject(parent){
 connect(&outputs_,&WaylandOutputs::changed,this,[this]{if(region_.busy())region_.cancel({"display_changed","Display layout changed during region selection",{}});emit capabilitiesChanged();});
 connect(&capture_,&ImageCopyCapture::changed,this,&BudgieSourceAdapter::capabilitiesChanged);
 connect(&clipboard_,&DataControlClipboard::changed,this,&BudgieSourceAdapter::capabilitiesChanged);
 connect(&shortcuts_,&BudgieShortcuts::changed,this,&BudgieSourceAdapter::capabilitiesChanged);
 connect(&shortcuts_,&BudgieShortcuts::activated,this,&BudgieSourceAdapter::actionRequested);
}
BudgieSourceAdapter::~BudgieSourceAdapter(){region_.shutdown();cancelAcquisition();clipboard_.cancel();for(auto* worker:workers_){worker->wait();delete worker;}clipboard_.keepSelectionAfterExit();}
quint64 BudgieSourceAdapter::clipboardRevision()const{return clipboard_.revision();}
bool BudgieSourceAdapter::unchanged(const QList<CaptureFrame>& before,quint64 revision)const{
 return outputs_.revision()==revision&&sameOutputLayout(before,layout());
}
void BudgieSourceAdapter::listWindows(RecordingWindows::Reply reply){
 QString reason;
 if(!capture_.windowsAvailable(&reason)){reply({"recording_unsupported",reason,{{"protocol","ext-foreign-toplevel-image-capture-source-v1"}}},{});return;}
 QJsonArray out;bool activeExcluded=false;
 for(const auto& window:capture_.toplevels()){
  if(ownAppId(window.appId)){activeExcluded|=window.activated;continue;}
  out.append(QJsonObject{{"identity",window.identifier},{"title",window.title},{"appId",window.appId},{"rect",QJsonArray{}},{"active",window.activated},{"minimized",false}});
 }
 reply({},{{"windows",out},{"activeExcluded",activeExcluded}});
}
QJsonObject BudgieSourceAdapter::capabilities()const{
 const auto pixels=capture_.status();const auto outputs=outputs_.status();
 QString outputReason,windowReason;const bool outputsReady=capture_.outputsAvailable(&outputReason);const bool windowsReady=capture_.windowsAvailable(&windowReason);
 QJsonObject desktop{{"available",outputsReady},{"protocol","ext-image-copy-capture-v1"}};if(!outputsReady)desktop["error"]=outputReason;
 QJsonObject region=desktop;
 QJsonObject window{{"available",windowsReady},{"protocol","ext-foreign-toplevel-image-capture-source-v1"},{"content","client-only"},{"occlusion","ignored"}};
 if(!windowsReady){window["error"]=windowReason;window["code"]="window_unavailable";}
 if(!outputs["available"].toBool()){const auto error=outputs["error"];for(auto* status:{&region,&desktop}){(*status)["available"]=false;(*status)["error"]=error;}}
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 region["available"]=false;region["error"]="Native region selection requires current distro per-window LayerShellQt and Qt 6.5 or later";
#endif
 if(region_.cleanupRequired()){for(auto* status:{&region,&window,&desktop}){(*status)["available"]=false;(*status)["error"]=cleanupMessage;(*status)["code"]="capture_cleanup_pending";}}
 const QJsonObject helper=region_.diagnostics();region["helper"]=helper;
 return {{"adapter","budgie-wayland"},{"resident","qt-core"},{"captureHelper",helper},{"outputs",outputs},{"imageCopy",pixels},
  {"capture",QJsonObject{{"region",region},{"window",window},{"desktop",desktop}}},{"clipboard",clipboard_.status()},
  {"shortcuts",shortcuts_.capabilities()}};
}
SourceAdapter::PreparedAcquisition BudgieSourceAdapter::prepareAcquisition(const QString& action){
 QPointer<BudgieSourceAdapter> guard(this);
 if(action=="clipboard"){const auto revision=clipboard_.revision();return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(guard)guard->clipboard_.acquire(limits,std::move(cb),revision);else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};}
 if(action=="window"){
  if(region_.cleanupRequired())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"capture_cleanup_pending",cleanupMessage,{}}});};
  if(region_.busy())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"busy_selecting","Region selection is already active",{}}});};
  // Freeze the active window at invocation, then read its pixels now: a focus
  // change or put.gd's own window opening later cannot retarget the request.
  auto snapshot=std::make_shared<WindowSnapshot>();const auto generation=generation_;const auto limits=limits_;
  auto complete=[guard,snapshot,generation](QImage image,Error error){
   snapshot->done=true;
   if(!guard||guard->generation_!=generation)snapshot->error={"cancelled","Window acquisition cancelled",{}};
   else{snapshot->error=std::move(error);if(!snapshot->error)snapshot->image=std::move(image);}
   if(snapshot->accepted){auto cb=std::move(snapshot->accepted);cb(imageResult(snapshot->image,snapshot->acceptedLimits,snapshot->error));}
  };
  capture_.freezeActive([guard,complete,limits,generation](ImageCopyCapture::Toplevel target,Error error){
   if(error){if(error.code=="capture_target"&&error.message.startsWith("No window"))error={"target_unavailable","Focus an external window and invoke the window shortcut or CLI",error.details};complete({},std::move(error));return;}
   if(!guard){complete({},{"cancelled","Window acquisition cancelled",{}});return;}
   if(ownAppId(target.appId)){complete({},{"target_unavailable","Focus an external window and invoke the window shortcut or CLI",{}});return;}
   guard->queryVisibility([guard,complete,limits,generation,id=target.identifier](bool visible){
    if(!guard||guard->generation_!=generation){complete({},{"cancelled","Window acquisition cancelled",{}});return;}
    if(!visible){complete({},visibilityError());return;}
    guard->capture_.captureToplevel(id,limits,[guard,generation,complete](QImage image,Error error){
     if(error){complete({},std::move(error));return;}
     if(!guard||guard->generation_!=generation){complete({},{"cancelled","Window acquisition cancelled",{}});return;}
     guard->queryVisibility([guard,generation,complete,image=std::move(image)](bool visible)mutable{
      if(!guard||guard->generation_!=generation){complete({},{"cancelled","Window acquisition cancelled",{}});return;}
      if(!visible){complete({},visibilityError());return;}
      complete(std::move(image),{});
     });
    });
   });
  });
  return [guard,snapshot,generation](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Window acquisition cancelled",{}}});return;}if(snapshot->done)cb(imageResult(snapshot->image,limits,snapshot->error));else{snapshot->acceptedLimits=limits;snapshot->accepted=std::move(cb);}};
 }
 const auto frozen=layout();const auto revision=outputs_.revision();
 return [guard,action,frozen,revision,outcome=captureOutcome_,screenshot=action=="region"&&regionScreenshotFirst_](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(!guard){cb({{},{"source_unavailable","Desktop adapter closed",{}}});return;}guard->capture(action,frozen,revision,limits,std::move(cb),outcome,screenshot);};
}
SourceAdapter::PreparedAcquisition BudgieSourceAdapter::prepareAcquisition(const QString& action,const ResourceLimits& acceptedLimits,std::function<void(Error)> terminal,std::function<void()> released){
#if defined(PUTGD_HAVE_LAYER_SHELL) && QT_VERSION >= QT_VERSION_CHECK(6,5,0)
 // A live selection shows before the journal saves; its pixels still wait.
 const auto frozen=action=="region"&&!regionScreenshotFirst_?layout():QList<CaptureFrame>{};
 const auto revision=outputs_.revision();
 if(!frozen.isEmpty()&&!region_.busy()&&!region_.cleanupRequired()&&unchanged(frozen,revision)){
  QPointer<BudgieSourceAdapter> guard(this);const auto generation=generation_;
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
void BudgieSourceAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){prepareAcquisition(action)(limits,std::move(cb));}
void BudgieSourceAdapter::capture(QString action,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(PayloadResult)> cb,const QString& outcome,bool screenshotFirst){
 if(action!="region"&&action!="desktop"){cb({{},{"source_unsupported","Unknown capture action",{}}});return;}
 if(region_.cleanupRequired()){cb({{},{"capture_cleanup_pending",cleanupMessage,{}}});return;}
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 if(action=="region"){cb({{},{"region_unavailable","Native region selection requires current distro per-window LayerShellQt",{}}});return;}
#endif
 if(region_.busy()){cb({{},{"busy_selecting","Region selection is already active",{}}});return;}
 if(frozen.isEmpty()||!unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed after invocation",{}}});return;}
 if(action=="region"&&screenshotFirst){selectFrozen(std::move(frozen),revision,limits,std::move(cb),outcome);return;}
 if(action=="region"){
  const auto generation=generation_;QPointer<BudgieSourceAdapter> guard(this);
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
void BudgieSourceAdapter::captureScreens(QList<CaptureFrame> frozen,quint64 revision,QRectF selection,ResourceLimits limits,std::function<void(PayloadResult)> cb){
 QPointer<BudgieSourceAdapter> guard(this);auto layout=frozen;
 // Only the outputs the selection touches are read.
 QList<CaptureFrame> needed;for(const auto& frame:frozen)if(frame.logicalRect.intersects(selection))needed.append(frame);
 if(needed.isEmpty()){cb({{},{"capture_geometry","The selection lies outside every output",{}}});return;}
 captureFrames(std::move(needed),layout,revision,limits,[guard,layout,revision,selection,limits,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  if(error){cb({{},error});return;}
  if(!guard){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
  guard->compose(std::move(frames),std::move(layout),revision,selection,limits,std::move(cb));
 });
}
void BudgieSourceAdapter::selectFrozen(QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(PayloadResult)> cb,const QString& outcome){
 QPointer<BudgieSourceAdapter> guard(this);const auto generation=generation_;auto layout=frozen;
 // Read every output before any selector surface maps; open menus stay in frame.
 captureFrames(std::move(frozen),layout,revision,limits,[guard,layout,revision,limits,generation,outcome,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
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
void BudgieSourceAdapter::captureFrames(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(QList<CaptureFrame>,Error)> cb){
 QPointer<BudgieSourceAdapter> guard(this);const auto generation=generation_;
 queryVisibility([guard,generation,frames=std::move(frames),frozen=std::move(frozen),revision,limits,cb=std::move(cb)](bool visible)mutable{
  if(!guard||guard->generation_!=generation){cb({},{"cancelled","Desktop capture cancelled",{}});return;}
  if(!visible){cb({},visibilityError());return;}
  guard->captureVisibleFrames(std::move(frames),frozen,revision,limits,[guard,generation,frozen,revision,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
   if(error){cb({},std::move(error));return;}
   if(!guard||guard->generation_!=generation){cb({},{"cancelled","Desktop capture cancelled",{}});return;}
   guard->queryVisibility([guard,generation,frozen,revision,frames=std::move(frames),cb=std::move(cb)](bool visible)mutable{
    if(!guard||guard->generation_!=generation){cb({},{"cancelled","Desktop capture cancelled",{}});return;}
    if(!visible){cb({},visibilityError());return;}
    if(!guard->unchanged(frozen,revision)){cb({},{"display_changed","Display layout changed during acquisition",{}});return;}
    cb(std::move(frames),{});
   });
  });
 });
}
void BudgieSourceAdapter::captureVisibleFrames(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(QList<CaptureFrame>,Error)> cb){
 // frames: the outputs to read; frozen: the whole layout, which must not change.
 struct Batch {QList<CaptureFrame> frames,frozenLayout;qint64 bytes=0,pixels=0;int index=0;std::function<void(QList<CaptureFrame>,Error)> reply;std::function<void()> next;};
 auto batch=std::make_shared<Batch>();batch->frames=std::move(frames);batch->frozenLayout=std::move(frozen);batch->reply=std::move(cb);QPointer<BudgieSourceAdapter> guard(this);const auto generation=generation_;std::weak_ptr<Batch> weak=batch;
 batch->next=[guard,batchWeak=weak,revision,limits,generation]{auto batch=batchWeak.lock();if(!batch)return;
  auto fail=[batch](Error error){auto cb=std::move(batch->reply);batch->next={};if(cb)cb({},error);};
  if(!guard||generation!=guard->generation_){fail({"cancelled","Desktop capture cancelled",{}});return;}
  if(!guard->unchanged(batch->frozenLayout,revision)){fail({"display_changed","Display layout changed during acquisition",{}});return;}
  if(batch->index==batch->frames.size()){auto frames=std::move(batch->frames);auto reply=std::move(batch->reply);batch->next={};reply(std::move(frames),{});return;}
  const auto index=batch->index;
  // Each output's budget is what the earlier outputs left.
  ResourceLimits remaining=limits;remaining.imagePixels=limits.imagePixels-batch->pixels;remaining.imageBytes=limits.imageBytes-batch->bytes;
  guard->capture_.captureOutput(batch->frames[index].identity,remaining,[guard,batch,index,revision,limits,generation](QImage image,Error error){
   const auto reject=[batch](Error failure){auto cb=std::move(batch->reply);batch->next={};if(cb)cb({},failure);};
   if(!guard||generation!=guard->generation_){reject({"cancelled","Desktop capture cancelled",{}});return;}
   if(error){reject(error);return;}
   if(!guard->unchanged(batch->frozenLayout,revision)){reject({"display_changed","Display layout changed during acquisition",{}});return;}
   auto& frame=batch->frames[index];
   // The buffer is the output's native, upright size; anything else means the
   // mode, scale or transform changed after the layout froze.
   if(image.size()!=frame.nativeRect.size()){reject({"display_changed","Captured output does not match frozen output geometry",{{"captured",QStringLiteral("%1x%2").arg(image.width()).arg(image.height())},{"expected",QStringLiteral("%1x%2").arg(frame.nativeRect.width()).arg(frame.nativeRect.height())}}});return;}
   const auto pixels=qint64(image.width())*image.height();
   if(pixels>limits.imagePixels-batch->pixels||image.sizeInBytes()>limits.imageBytes-batch->bytes){reject({"input_excessive","Desktop frame snapshots exceed resource limits",{}});return;}
   batch->pixels+=pixels;batch->bytes+=image.sizeInBytes();image.setDevicePixelRatio(frame.density);frame.pixels=std::move(image);
   ++batch->index;auto next=batch->next;next();
  });
 };
 auto next=batch->next;next();
}
void BudgieSourceAdapter::compose(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,QRectF selection,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
 const auto generation=generation_;auto result=std::make_shared<PayloadResult>();
 auto* worker=QThread::create([frames=std::move(frames),selection,limits,result]{result->payload.image=composeOutputFrames(frames,selection,std::min(limits.imagePixels,limits.imageBytes/4),result->error);result->payload.kind="image";result->payload.mime="image/png";});workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,generation,frozen=std::move(frozen),revision,result,cb=std::move(cb)]()mutable{workers_.removeOne(worker);worker->deleteLater();if(generation!=generation_)cb({{},{"cancelled","Capture acquisition cancelled",{}}});else if(!unchanged(frozen,revision))cb({{},{"display_changed","Display layout changed during capture composition",{}}});else cb(std::move(*result));});
 worker->start();
}
void BudgieSourceAdapter::cancelAcquisition(){++generation_;capture_.cancel();clipboard_.cancel();region_.cancel();}
void BudgieSourceAdapter::copy(const CopyPayload& payload,std::function<void(Error)> cb){clipboard_.copy(payload,std::move(cb));}
void BudgieSourceAdapter::invalidateCopyEligibility(){clipboard_.invalidateCopyEligibility();}
void BudgieSourceAdapter::bindShortcuts(const QHash<QString,QString>& desired){shortcuts_.bind(desired);}
}
