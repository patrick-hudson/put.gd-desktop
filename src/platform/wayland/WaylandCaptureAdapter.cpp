#include "WaylandCaptureAdapter.h"
#include "platform/wayland/CaptureCompose.h"
#include "platform/wayland/CaptureSequence.h"
namespace putgd {
namespace {
struct WindowSnapshot {QImage image;Error error;bool done=false;std::function<void(PayloadResult)> accepted;ResourceLimits acceptedLimits;};
PayloadResult imageResult(const QImage& image,const ResourceLimits& limits,Error error={}){
 if(error)return {{},error};
 if(image.isNull())return {{},{"capture_failed","The compositor provided no image",{}}};
 if(qint64(image.width())*image.height()>limits.imagePixels||image.sizeInBytes()>limits.imageBytes)return {{},{"input_excessive","Capture exceeds accepted operation resource limits",{}}};
 ClipboardPayload payload;payload.kind="image";payload.mime="image/png";payload.image=image;return {payload,{}};
}
}
QString WaylandCaptureAdapter::cleanupMessage(){return QStringLiteral("Region helper exited before confirming surface cleanup; restart put.gd before capturing again");}
WaylandCaptureAdapter::WaylandCaptureAdapter(QObject* parent):QObject(parent){
 connect(&outputs_,&WaylandOutputs::changed,this,[this]{if(region_.busy())region_.cancel({"display_changed","Display layout changed during region selection",{}});emit capabilitiesChanged();});
 connect(&capture_,&ImageCopyCapture::changed,this,&WaylandCaptureAdapter::capabilitiesChanged);
 connect(&clipboard_,&DataControlClipboard::changed,this,&WaylandCaptureAdapter::capabilitiesChanged);
}
void WaylandCaptureAdapter::shutdownCapture(){region_.shutdown();cancelAcquisition();clipboard_.cancel();for(auto* worker:workers_){worker->wait();delete worker;}clipboard_.keepSelectionAfterExit();}
quint64 WaylandCaptureAdapter::clipboardRevision()const{return clipboard_.revision();}
bool WaylandCaptureAdapter::unchanged(const QList<CaptureFrame>& before,quint64 revision)const{
 return outputs_.revision()==revision&&sameOutputLayout(before,layout());
}
SourceAdapter::PreparedAcquisition WaylandCaptureAdapter::prepareAcquisition(const QString& requested){
 // Copy text (#1428) selects exactly as a region does, with the same selector, cleanup
 // and lock witness; only Repeat's memory is left alone.
 const bool text=requested==QLatin1String("text");const QString action=text?QStringLiteral("region"):requested;
 QPointer<WaylandCaptureAdapter> guard(this);
 if(action=="clipboard"){const auto revision=clipboard_.revision();return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(guard)guard->clipboard_.acquire(limits,std::move(cb),revision);else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};}
 if(action=="region"&&takeEarlyCancel())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"cancelled","Region selection cancelled",{}}});};
 if(action=="window"){
  if(region_.cleanupRequired())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"capture_cleanup_pending",cleanupMessage(),{}}});};
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
  captureActiveWindow(limits,generation,std::move(complete));
  return [guard,snapshot,generation](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Window acquisition cancelled",{}}});return;}if(snapshot->done)cb(imageResult(snapshot->image,limits,snapshot->error));else{snapshot->acceptedLimits=limits;snapshot->accepted=std::move(cb);}};
 }
 const auto frozen=layout();const auto revision=outputs_.revision();
 if(action=="monitor"){
  // The pointer's output is frozen now, at invocation, as a window's focus is.
  struct PointerSnapshot {QRectF selection;Error error;bool done=false;std::function<void()> ready;};
  auto snapshot=std::make_shared<PointerSnapshot>();const auto generation=generation_;
  probePointer(frozen,[snapshot](QRectF selection,Error error){
   snapshot->done=true;
   if(error)snapshot->error=std::move(error);else snapshot->selection=selection;
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
void WaylandCaptureAdapter::captureArea(QList<CaptureFrame> frozen,quint64 revision,QRectF selection,ResourceLimits limits,std::function<void(PayloadResult)> cb){
 if(region_.cleanupRequired()){cb({{},{"capture_cleanup_pending",cleanupMessage(),{}}});return;}
 if(region_.busy()){cb({{},{"busy_selecting","Region selection is already active",{}}});return;}
 if(frozen.isEmpty()||!unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed after invocation",{}}});return;}
 captureScreens(std::move(frozen),revision,selection,limits,std::move(cb));
}
std::function<void(PayloadResult)> WaylandCaptureAdapter::remembering(QRectF selection,QList<CaptureFrame> layout,std::function<void(PayloadResult)> cb,bool remember){
 if(!remember)return cb;
 QPointer<WaylandCaptureAdapter> guard(this);for(auto& frame:layout)frame.pixels={};
 return [guard,selection,layout=std::move(layout),cb=std::move(cb)](PayloadResult result)mutable{
  if(guard&&!result.error)guard->lastRegion_=RegionMemory{selection,std::move(layout)};
  cb(std::move(result));
 };
}
void WaylandCaptureAdapter::lockGate(int delay,std::function<void(Error)> next){
 if(delay<=0){next({});return;}
 sessionLocked([next=std::move(next)](bool locked){next(locked?lockedError():Error{});});
}
void WaylandCaptureAdapter::countdown(int seconds,std::function<void(Error)> reply){
 if(region_.busy()){reply({"busy_selecting","Region selection is already active",{}});return;}
 countdown_.start(seconds,captureAppearance_,std::move(reply));
}
SourceAdapter::PreparedAcquisition WaylandCaptureAdapter::prepareAcquisition(const QString& requested,const ResourceLimits& acceptedLimits,std::function<void(Error)> terminal,std::function<void()> released){
 const bool text=requested==QLatin1String("text");const QString action=text?QStringLiteral("region"):requested;
 if(action=="region"&&takeEarlyCancel()){
  const Error cancelled{"cancelled","Region selection cancelled",{}};
  if(terminal)terminal(cancelled);
  if(released)released();
  return [cancelled](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},cancelled});};
 }
#if defined(PUTGD_HAVE_LAYER_SHELL) && QT_VERSION >= QT_VERSION_CHECK(6,5,0)
 // A live selection shows before the journal saves; its pixels still wait.
 const auto frozen=action=="region"&&!regionScreenshotFirst_?layout():QList<CaptureFrame>{};
 const auto revision=outputs_.revision();
 if(!frozen.isEmpty()&&!region_.busy()&&!region_.cleanupRequired()&&unchanged(frozen,revision)){
  QPointer<WaylandCaptureAdapter> guard(this);const auto generation=generation_;const int delay=captureDelay_;
  catchEarlyPress(frozen);
  region_.setDelaySeconds(delay);
  auto ticket=region_.prepare(frozen,captureOutcome_,captureAppearance_,std::move(terminal),std::move(released));
  selectorStarted();
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
void WaylandCaptureAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){prepareAcquisition(action)(limits,std::move(cb));}
void WaylandCaptureAdapter::capture(QString action,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(PayloadResult)> cb,const QString& outcome,bool screenshotFirst,int delay,bool remember){
 if(action!="region"&&action!="desktop"){cb({{},{"source_unsupported","Unknown capture action",{}}});return;}
 // Cancelled on the catcher while this operation was still being accepted.
 if(action=="region"&&takeEarlyCancel()){cb({{},{"cancelled","Region selection cancelled",{}}});return;}
 if(region_.cleanupRequired()){cb({{},{"capture_cleanup_pending",cleanupMessage(),{}}});return;}
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 if(action=="region"){cb({{},{"region_unavailable","Native region selection requires current distro per-window LayerShellQt",{}}});return;}
#endif
 if(region_.busy()){cb({{},{"busy_selecting","Region selection is already active",{}}});return;}
 if(frozen.isEmpty()||!unchanged(frozen,revision)){cb({{},{"display_changed","Display layout changed after invocation",{}}});return;}
 if(action=="region"&&screenshotFirst){selectFrozen(std::move(frozen),revision,limits,std::move(cb),outcome,remember);return;}
 if(action=="region"){
  const auto generation=generation_;QPointer<WaylandCaptureAdapter> guard(this);
  catchEarlyPress(frozen);
  region_.setDelaySeconds(delay);
  region_.start(frozen,CaptureSequence::afterSelection(guard,frozen,revision,limits,generation,delay,remember,std::move(cb)),outcome,{},captureAppearance_);
  selectorStarted();
  return;
 }
 QRectF bounds;for(const auto& frame:frozen)bounds=bounds.united(frame.logicalRect);captureScreens(std::move(frozen),revision,bounds,limits,std::move(cb));
}
void WaylandCaptureAdapter::captureScreens(QList<CaptureFrame> frozen,quint64 revision,QRectF selection,ResourceLimits limits,std::function<void(PayloadResult)> cb){
 QPointer<WaylandCaptureAdapter> guard(this);auto layout=frozen;
 // Only the outputs the selection touches are read.
 QList<CaptureFrame> needed;for(const auto& frame:frozen)if(frame.logicalRect.intersects(selection))needed.append(frame);
 if(needed.isEmpty()){cb({{},{"capture_geometry","The selection lies outside every output",{}}});return;}
 captureFrames(std::move(needed),layout,revision,limits,[guard,layout,revision,selection,limits,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  if(error){cb({{},error});return;}
  if(!guard){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
  guard->compose(std::move(frames),std::move(layout),revision,selection,limits,std::move(cb));
 });
}
void WaylandCaptureAdapter::selectFrozen(QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(PayloadResult)> cb,const QString& outcome,bool remember){
 QPointer<WaylandCaptureAdapter> guard(this);const auto generation=generation_;auto layout=frozen;
 // Read every output before any selector surface maps; open menus stay in frame.
 captureFrames(std::move(frozen),layout,revision,limits,[guard,layout,revision,limits,generation,outcome,remember,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  if(error){if(!error.details.contains("stage"))error.details["stage"]="freeze";cb({{},error});return;}
  if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Desktop capture cancelled",{}}});return;}
  auto pixels=std::make_shared<QList<CaptureFrame>>(std::move(frames));auto reply=std::make_shared<std::function<void(PayloadResult)>>(std::move(cb));
  guard->catchEarlyPress(layout);
  guard->region_.start(layout,[guard,layout,revision,limits,generation,pixels,reply,remember](QRectF selection,Error error){
   if(!guard||guard->generation_!=generation){(*reply)({{},{"cancelled","Desktop capture cancelled",{}}});return;}
   if(error){(*reply)({{},std::move(error)});return;}
   if(!guard->unchanged(layout,revision)){(*reply)({{},{"display_changed","Display layout changed during region selection",{}}});return;}
   guard->compose(std::move(*pixels),layout,revision,selection,limits,guard->remembering(selection,layout,std::move(*reply),remember));
  },outcome,*pixels,guard->captureAppearance_);
  if(guard)guard->selectorStarted();
 });
}
void WaylandCaptureAdapter::captureFrames(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(QList<CaptureFrame>,Error)> cb){
 readOutputs(std::move(frames),std::move(frozen),revision,limits,std::move(cb));
}
void WaylandCaptureAdapter::readOutputs(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(QList<CaptureFrame>,Error)> cb){
 QPointer<WaylandCaptureAdapter> guard(this);
 CaptureSequence::readFrames(guard,std::move(frames),std::move(frozen),revision,limits,
  [guard](const CaptureFrame& frame,const ResourceLimits& remaining,CaptureSequence::FrameReply reply){
   guard->capture_.captureOutput(frame.identity,remaining,[reply=std::move(reply)](QImage image,Error error){reply(std::move(image),{},std::move(error));});
  },
  [guard](CaptureFrame& frame,QImage image,const QVariantMap&,const ResourceLimits& remaining)->Error{
   if(auto refused=guard->checkOutput(image))return refused;
   // The buffer is the output's native, upright size; anything else means the
   // mode, scale or transform changed after the layout froze.
   if(image.size()!=frame.nativeRect.size())return {"display_changed","Captured output does not match frozen output geometry",{{"captured",QStringLiteral("%1x%2").arg(image.width()).arg(image.height())},{"expected",QStringLiteral("%1x%2").arg(frame.nativeRect.width()).arg(frame.nativeRect.height())}}};
   const auto pixels=qint64(image.width())*image.height();
   if(pixels>remaining.imagePixels||image.sizeInBytes()>remaining.imageBytes)return {"input_excessive","Desktop frame snapshots exceed resource limits",{}};
   image.setDevicePixelRatio(frame.density);frame.pixels=std::move(image);return {};
  },std::move(cb));
}
void WaylandCaptureAdapter::compose(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,QRectF selection,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
 const auto generation=generation_;auto result=std::make_shared<PayloadResult>();
 auto* worker=QThread::create([frames=std::move(frames),selection,limits,result]{result->payload.image=composeOutputFrames(frames,selection,std::min(limits.imagePixels,limits.imageBytes/4),result->error);result->payload.kind="image";result->payload.mime="image/png";});workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,generation,frozen=std::move(frozen),revision,result,cb=std::move(cb)]()mutable{workers_.removeOne(worker);worker->deleteLater();if(generation!=generation_)cb({{},{"cancelled","Capture acquisition cancelled",{}}});else if(!unchanged(frozen,revision))cb({{},{"display_changed","Display layout changed during capture composition",{}}});else cb(std::move(*result));});
 worker->start();
}
void WaylandCaptureAdapter::cancelAcquisition(){countdown_.cancel();cancelPointer();++generation_;capture_.cancel();clipboard_.cancel();region_.cancel();}
void WaylandCaptureAdapter::copy(const CopyPayload& payload,std::function<void(Error)> cb){clipboard_.copy(payload,std::move(cb));}
void WaylandCaptureAdapter::invalidateCopyEligibility(){clipboard_.invalidateCopyEligibility();}
}
