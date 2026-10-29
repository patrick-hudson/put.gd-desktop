#include "X11SourceAdapter.h"
#include "CinnamonRegionCapture.h"
#include "X11CompositorSettle.h"
#include "NativeRegionOverlay.h"
#include "ui/RegionOverlay.h"
#include <QThread>
#include <QJsonArray>
#include <QGuiApplication>
#include <cmath>
namespace putgd {
namespace {
Error selectionError(const X11Target& target,const QRectF& rectangle,const ResourceLimits& limits){
 QRectF bounds;qreal density=0;
 for(const auto& frame:target.layout){bounds=bounds.united(frame.logicalRect);if(frame.logicalRect.intersects(rectangle))density=std::max(density,frame.density);}
 const bool finite=std::isfinite(rectangle.x())&&std::isfinite(rectangle.y())&&std::isfinite(rectangle.width())&&std::isfinite(rectangle.height())&&std::isfinite(rectangle.right())&&std::isfinite(rectangle.bottom());
 if(target.action!="region"||!finite||rectangle.isEmpty()||!bounds.contains(rectangle)||!std::isfinite(density)||density<=0)return {"capture_geometry","Invalid region selection",{}};
 const auto width=std::ceil(rectangle.right()*density)-std::floor(rectangle.left()*density),height=std::ceil(rectangle.bottom()*density)-std::floor(rectangle.top()*density);
 if(!std::isfinite(width)||!std::isfinite(height)||width*height>std::min(limits.imagePixels,limits.imageBytes/4))return {"image_limit","Capture exceeds image pixel limit",{}};
 return {};
}
bool sameTarget(const X11Target& a,const X11Target& b){
 if(a.action!=b.action||a.screenshotFirst!=b.screenshotFirst||a.window!=b.window||a.nativeWindowRect!=b.nativeWindowRect||a.logicalSelection!=b.logicalSelection||a.layout.size()!=b.layout.size())return false;
 for(int i=0;i<a.layout.size();++i){const auto& x=a.layout[i];const auto& y=b.layout[i];if(x.identity!=y.identity||x.logicalRect!=y.logicalRect||x.nativeRect!=y.nativeRect||x.density!=y.density||x.rotation!=y.rotation)return false;}
 return true;
}
}

X11SourceAdapter::X11SourceAdapter(QObject* parent,bool nativeRegionSelector):QObject(parent),nativeRegionSelector_(nativeRegionSelector){connect(&shortcuts_,&X11Shortcuts::activated,this,&X11SourceAdapter::actionRequested);connect(&shortcuts_,&X11Shortcuts::bindingsChanged,this,&X11SourceAdapter::capabilitiesChanged);connect(&shortcuts_,&X11Shortcuts::connectionLost,this,&X11SourceAdapter::displayLost);}
X11SourceAdapter::~X11SourceAdapter(){
 cancelAcquisition();clipboard_.cancel();
 for(auto* t:workers_){t->wait();delete t;}
 // Completed selectors can still await DeferredDelete. Destroy them while the
 // adapter's state is alive, before QObject tears down children after members.
 const auto selectors=findChildren<RegionSelector*>(QString{},Qt::FindDirectChildrenOnly);
 for(auto* selector:selectors){
  // The event loop may have stopped before DeferredDelete. Never deliver a
  // delayed selection/cancellation to external captures during owner teardown.
  QObject::disconnect(selector,nullptr,this,nullptr);
  delete selector;
 }
 const auto captures=findChildren<CinnamonRegionCapture*>(QString{},Qt::FindDirectChildrenOnly);
 for(auto* request:captures)delete request;
 const auto settles=findChildren<X11CompositorSettle*>(QString{},Qt::FindDirectChildrenOnly);
 for(auto* settle:settles)delete settle;
}
quint64 X11SourceAdapter::clipboardRevision()const{return clipboard_.revision();}
QJsonObject X11SourceAdapter::capabilities()const {
 const auto captureStatus=QJsonObject{{"available",capture_.available()},{"mode","visible-pixels"},{"consent","none"}};
 const auto clipboardStatus=QJsonObject{{"available",clipboard_.available()},{"types",QJsonArray{"files","image","text"}},{"import","asynchronous-x11-selection"},{"result","qt-ownership"}};
 return {{"adapter","cinnamon-x11"},{"capture",QJsonObject{{"region",captureStatus},{"window",captureStatus},{"desktop",captureStatus}}},{"clipboard",clipboardStatus},{"shortcuts",QJsonObject{{"available",shortcuts_.available()},{"bindings",shortcuts_.status()},{"keyPaths",shortcuts_.keyPaths()}}}};
}
SourceAdapter::PreparedAcquisition X11SourceAdapter::prepareAcquisition(const QString& action) {
 QPointer<X11SourceAdapter> guard(this);
 if(action=="clipboard"){const auto revision=clipboard_.revision();return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(guard)guard->clipboard_.acquire(limits,std::move(cb),revision);else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};}
 Error error;auto target=capture_.freeze(action,error);target.outcome=captureOutcome_;target.screenshotFirst=action=="region"&&regionScreenshotFirst_;
 return [guard,target,error](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(error){cb({{},error});return;}if(guard)guard->capture(target,limits,std::move(cb));else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};
}
void X11SourceAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){prepareAcquisition(action)(limits,std::move(cb));}
void X11SourceAdapter::acquireFrozen(const X11Target& target,const ResourceLimits& limits,std::function<void(PayloadResult)> callback){capture(target,limits,std::move(callback));}
void X11SourceAdapter::capture(const X11Target& target,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
 if(overlay_){cb({{},{"busy_selecting","Region selection is already active",{}}});return;}
 qint64 pixels=0;for(const auto& frame:target.layout){const qint64 count=qint64(frame.nativeRect.width())*frame.nativeRect.height();if(count>limits.imagePixels-pixels){cb({{},{"image_limit","Monitor snapshot exceeds resource limits",{}}});return;}pixels+=count;}
 Error error;if(!capture_.unchanged(target,error)){cb({{},error});return;}
 if(target.action!="region"){auto frames=capture_.frames(target,error);if(error){cb({{},error});return;}compose(std::move(frames),target.logicalSelection,limits,std::move(cb));return;}
 selectRegionFrozen(target,limits,[this,target,limits,cb=std::move(cb)](RegionSelectionResult selection)mutable{
  if(selection.error){cb({{},selection.error});return;}
  acquireSelectedRegionFrozen(target,selection,limits,std::move(cb));
 });
}
void X11SourceAdapter::selectRegionFrozen(const X11Target& target,const ResourceLimits& limits,std::function<void(RegionSelectionResult)> cb){
 if(target.action!="region"){cb({{},0,{"capture_target","Region selection requires a region target",{}}});return;}
 if(overlay_){cb({{},0,{"busy_selecting","Region selection is already active",{}}});return;}
 qint64 pixels=0;for(const auto& frame:target.layout){const qint64 count=qint64(frame.nativeRect.width())*frame.nativeRect.height();if(count<=0||count>limits.imagePixels-pixels){cb({{},0,{"image_limit","Monitor snapshot exceeds resource limits",{}}});return;}pixels+=count;}
 Error error;if(!capture_.unchanged(target,error)){cb({{},0,error});return;}
 // A fresh selection invalidates older geometry. A live selection reads no
 // pixels; Screenshot first reads every monitor once, before the selector maps.
 selectedGeneration_=0;selectedTarget_={};frozenFrames_.clear();const auto generation=++generation_;
 auto callback=std::make_shared<std::function<void(RegionSelectionResult)>>(std::move(cb));
 if(!target.screenshotFirst){openSelector(target,limits,generation,{},[callback](RegionSelectionResult result){(*callback)(std::move(result));});return;}
 captureLayout(target,limits,generation,[this,target,limits,generation,callback](QList<CaptureFrame> frames,Error error){
  if(generation!=generation_){(*callback)({{},0,{"cancelled","Capture acquisition cancelled",{}}});return;}
  if(error){if(!error.details.contains("stage"))error.details["stage"]="freeze";(*callback)({{},0,error});return;}
  openSelector(target,limits,generation,std::move(frames),[callback](RegionSelectionResult result){(*callback)(std::move(result));});
 });
}
void X11SourceAdapter::captureLayout(const X11Target& target,const ResourceLimits& limits,quint64 generation,std::function<void(QList<CaptureFrame>,Error)> cb){
 if(!capture_.cinnamonCompositor()){Error error;auto frames=capture_.frames(target,error);cb(std::move(frames),error);return;}
 auto* request=new CinnamonRegionCapture(target.layout,limits,this);regionCaptures_.insert(request);
 connect(request,&CinnamonRegionCapture::acquired,this,[this,request,target,generation,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  regionCaptures_.remove(request);
  if(generation!=generation_){cb({},{"cancelled","Capture acquisition cancelled",{}});return;}
  if(!error)capture_.unchanged(target,error);
  cb(error?QList<CaptureFrame>{}:std::move(frames),error);
 });
 request->start();
}
void X11SourceAdapter::openSelector(const X11Target& target,const ResourceLimits& limits,quint64 generation,QList<CaptureFrame> frozen,std::function<void(RegionSelectionResult)> cb){
 if(overlay_){cb({{},0,{"busy_selecting","Region selection is already active",{}}});return;}
 const bool screenshot=!frozen.isEmpty();
 auto layout=screenshot?frozen:target.layout;
 RegionSelector* overlay=nativeRegionSelector_&&NativeRegionOverlay::supportsLayout(target.layout)?static_cast<RegionSelector*>(new NativeRegionOverlay(layout,this,residentRegionCursor_,target.outcome,screenshot)):static_cast<RegionSelector*>(new RegionOverlay(layout,this,target.outcome,screenshot));overlay_=overlay;
 auto callback=std::make_shared<std::function<void(RegionSelectionResult)>>(std::move(cb));
 auto pixels=std::make_shared<QList<CaptureFrame>>(std::move(frozen));
 connect(overlay,&RegionSelector::selected,this,[this,overlay,target,limits,callback,generation,pixels](const QRectF& rectangle){
  // Both selector implementations withdraw before emitting; wait for object
  // destruction too, so no later acquisition can include a selector surface.
  connect(overlay,&QObject::destroyed,this,[this,overlay,target,rectangle,limits,callback,generation,pixels]{
   if(overlay_==overlay)overlay_=nullptr;
   if(generation!=generation_){(*callback)({{},0,{"cancelled","Capture acquisition cancelled",{}}});return;}
   const auto error=selectionError(target,rectangle,limits);if(error){(*callback)({{},0,error});return;}
   selectedGeneration_=generation;selectedTarget_=target;frozenFrames_=std::move(*pixels);
   for(auto& frame:selectedTarget_.layout)frame.pixels={};
   (*callback)({rectangle,generation,{}});
  });
  overlay->deleteLater();
 });
 connect(overlay,&RegionSelector::cancelled,this,[this,overlay,callback](const Error& error){
  connect(overlay,&QObject::destroyed,this,[this,overlay,callback,error]{if(overlay_==overlay)overlay_=nullptr;(*callback)({{},0,error});});
  overlay->deleteLater();
 });
 overlay->start();
}
void X11SourceAdapter::acquireSelectedRegionFrozen(const X11Target& target,const RegionSelectionResult& selection,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
 if(selection.error){cb({{},selection.error});return;}
 if(!selection.generation||selection.generation!=generation_||selection.generation!=selectedGeneration_){cb({{},{"cancelled","Region selection expired or was already acquired",{}}});return;}
 // Consume once even on validation failure; a selection cannot be replayed
 // against a different invocation target, even if that layout still exists.
 const bool matchingTarget=sameTarget(target,selectedTarget_);
 auto frozen=std::move(frozenFrames_);
 selectedGeneration_=0;selectedTarget_={};frozenFrames_.clear();
 if(!matchingTarget){cb({{},{"capture_target","Region selection target changed",{}}});return;}
 qint64 pixels=0;for(const auto& frame:target.layout){const qint64 count=qint64(frame.nativeRect.width())*frame.nativeRect.height();if(count<=0||count>limits.imagePixels-pixels){cb({{},{"image_limit","Monitor snapshot exceeds resource limits",{}}});return;}pixels+=count;}
 const auto error=selectionError(target,selection.rectangle,limits);if(error){cb({{},error});return;}
 // Screenshot first: cut the result from the frames the user selected over.
 if(!frozen.isEmpty()){Error changed;if(!capture_.unchanged(target,changed)){cb({{},changed});return;}compose(std::move(frozen),selection.rectangle,limits,std::move(cb));return;}
 // captureSelected rechecks the frozen display/target before its first read.
 captureSelected(target,selection.rectangle,limits,std::move(cb),selection.generation);
}
void X11SourceAdapter::captureSelected(const X11Target& target,QRectF selection,const ResourceLimits& limits,std::function<void(PayloadResult)> cb,quint64 generation){
 if(generation!=generation_){cb({{},{"cancelled","Capture acquisition cancelled",{}}});return;}
 Error error;if(!capture_.unchanged(target,error)){cb({{},error});return;}
 if(!capture_.cinnamonCompositor()){
  // The server has processed the selector's unmap, but a compositor presents
  // that later. Read the root only once its repaints over the selection settle.
  QGuiApplication::sync();
  QList<QRect> areas;
  for(const auto& frame:target.layout){const auto part=selection.intersected(frame.logicalRect);if(!part.isEmpty())areas.append(QRectF(QPointF(frame.nativeRect.topLeft())+(part.topLeft()-frame.logicalRect.topLeft())*frame.density,part.size()*frame.density).toAlignedRect());}
  auto* settle=new X11CompositorSettle(areas,this);settles_.insert(settle);
  connect(settle,&X11CompositorSettle::settled,this,[this,settle,target,selection,limits,generation,cb=std::move(cb)]()mutable{
   settles_.remove(settle);settle->deleteLater();
   if(generation!=generation_){cb({{},{"cancelled","Capture acquisition cancelled",{}}});return;}
   Error error;auto frames=capture_.frames(target,error);if(error){cb({{},error});return;}
   compose(std::move(frames),selection,limits,std::move(cb));
  });
  settle->start();
  return;
 }
 // A root read can precede Muffin's presentation of the surface unmaps. Read
 // Cinnamon's freshly painted scene directly, using its native-coordinate API.
 QGuiApplication::sync();
 if(generation!=generation_){cb({{},{"cancelled","Capture acquisition cancelled",{}}});return;}
 auto* request=new CinnamonRegionCapture(target.layout,limits,this);regionCaptures_.insert(request);
 connect(request,&CinnamonRegionCapture::acquired,this,[this,request,target,selection,limits,generation,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
  regionCaptures_.remove(request);
  if(generation!=generation_){cb({{},{"cancelled","Capture acquisition cancelled",{}}});return;}
  if(error){cb({{},error});return;}
  if(!capture_.unchanged(target,error)){cb({{},error});return;}
  compose(std::move(frames),selection,limits,std::move(cb));
 });
 request->start();
}
void X11SourceAdapter::compose(QList<CaptureFrame> frames,QRectF selection,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
 const auto generation=generation_;auto result=std::make_shared<PayloadResult>();auto* worker=QThread::create([frames=std::move(frames),selection,limits,result]{result->payload.image=composeFrames(frames,selection,std::min(limits.imagePixels,limits.imageBytes/4),result->error);result->payload.kind="image";result->payload.mime="image/png";});workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,generation,result,cb=std::move(cb)]()mutable{workers_.removeOne(worker);worker->deleteLater();if(generation!=generation_)cb({{},{"cancelled","Capture acquisition cancelled",{}}});else cb(std::move(*result));});worker->start();
}
void X11SourceAdapter::cancelAcquisition(){++generation_;selectedGeneration_=0;selectedTarget_={};frozenFrames_.clear();if(overlay_)overlay_->cancel();const auto captures=std::move(regionCaptures_);regionCaptures_.clear();for(auto* request:captures)request->cancel();const auto settles=std::move(settles_);settles_.clear();for(auto* settle:settles)settle->cancel();}
void X11SourceAdapter::copy(const CopyPayload& payload,std::function<void(Error)> cb){clipboard_.copy(payload,std::move(cb));}
void X11SourceAdapter::bindShortcuts(const QHash<QString,QString>& shortcuts){shortcuts_.bind(shortcuts);}
}
