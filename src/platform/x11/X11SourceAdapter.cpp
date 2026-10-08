#include "X11SourceAdapter.h"
#include "CinnamonRegionCapture.h"
#include <QThread>
#include <QJsonArray>
#include <QGuiApplication>
namespace putgd {
X11SourceAdapter::X11SourceAdapter(QObject* parent):QObject(parent){connect(&shortcuts_,&X11Shortcuts::activated,this,&X11SourceAdapter::actionRequested);connect(&shortcuts_,&X11Shortcuts::bindingsChanged,this,&X11SourceAdapter::capabilitiesChanged);}
X11SourceAdapter::~X11SourceAdapter(){
 cancelAcquisition();clipboard_.cancel();
 for(auto* t:workers_){t->wait();delete t;}
 // Completed selectors can still await DeferredDelete. Destroy them while the
 // adapter's state is alive, before QObject tears down children after members.
 const auto selectors=findChildren<RegionOverlay*>(QString{},Qt::FindDirectChildrenOnly);
 for(auto* selector:selectors)delete selector;
 const auto captures=findChildren<CinnamonRegionCapture*>(QString{},Qt::FindDirectChildrenOnly);
 for(auto* request:captures)delete request;
}
quint64 X11SourceAdapter::clipboardRevision()const{return clipboard_.revision();}
QJsonObject X11SourceAdapter::capabilities()const {
 const auto captureStatus=QJsonObject{{"available",capture_.available()},{"mode","visible-pixels"},{"consent","none"}};
 const auto clipboardStatus=QJsonObject{{"available",clipboard_.available()},{"types",QJsonArray{"files","image","text"}},{"import","asynchronous-x11-selection"},{"result","qt-ownership"}};
 return {{"adapter","cinnamon-x11"},{"capture",QJsonObject{{"region",captureStatus},{"window",captureStatus},{"desktop",captureStatus}}},{"clipboard",clipboardStatus},{"shortcuts",QJsonObject{{"available",shortcuts_.available()},{"bindings",shortcuts_.status()}}}};
}
SourceAdapter::PreparedAcquisition X11SourceAdapter::prepareAcquisition(const QString& action) {
 QPointer<X11SourceAdapter> guard(this);
 if(action=="clipboard"){const auto revision=clipboard_.revision();return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(guard)guard->clipboard_.acquire(limits,std::move(cb),revision);else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};}
 Error error;const auto target=capture_.freeze(action,error);
 return [guard,target,error](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(error){cb({{},error});return;}if(guard)guard->capture(target,limits,std::move(cb));else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};
}
void X11SourceAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){prepareAcquisition(action)(limits,std::move(cb));}
void X11SourceAdapter::capture(const X11Target& target,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
 if(overlay_){cb({{},{"busy_selecting","Region selection is already active",{}}});return;}
 qint64 pixels=0;for(const auto& frame:target.layout){const qint64 count=qint64(frame.nativeRect.width())*frame.nativeRect.height();if(count>limits.imagePixels-pixels){cb({{},{"image_limit","Monitor snapshot exceeds resource limits",{}}});return;}pixels+=count;}
 Error error;if(!capture_.unchanged(target,error)){cb({{},error});return;}
 if(target.action!="region"){auto frames=capture_.frames(target,error);if(error){cb({{},error});return;}compose(std::move(frames),target.logicalSelection,limits,std::move(cb));return;}
 auto* overlay=new RegionOverlay(target.layout,this);overlay_=overlay;auto callback=std::make_shared<std::function<void(PayloadResult)>>(std::move(cb));const auto generation=generation_;
 connect(overlay,&RegionOverlay::selected,this,[this,overlay,target,limits,callback,generation](const QRectF& selection){
  overlay_=nullptr;
  connect(overlay,&QObject::destroyed,this,[this,target,selection,limits,callback,generation]{captureSelected(target,selection,limits,std::move(*callback),generation);});
  overlay->deleteLater();
 });
 connect(overlay,&RegionOverlay::cancelled,this,[this,overlay,callback](const Error& error){overlay_=nullptr;overlay->deleteLater();(*callback)({{},error});});overlay->start();
}
void X11SourceAdapter::captureSelected(const X11Target& target,QRectF selection,const ResourceLimits& limits,std::function<void(PayloadResult)> cb,quint64 generation){
 if(generation!=generation_){cb({{},{"cancelled","Capture acquisition cancelled",{}}});return;}
 Error error;if(!capture_.unchanged(target,error)){cb({{},error});return;}
 if(!capture_.cinnamonCompositor()){auto frames=capture_.frames(target,error);if(error){cb({{},error});return;}compose(std::move(frames),selection,limits,std::move(cb));return;}
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
void X11SourceAdapter::cancelAcquisition(){++generation_;if(overlay_)overlay_->cancel();const auto captures=std::move(regionCaptures_);regionCaptures_.clear();for(auto* request:captures)request->cancel();}
void X11SourceAdapter::copy(const CopyPayload& payload,std::function<void(Error)> cb){clipboard_.copy(payload,std::move(cb));}
void X11SourceAdapter::bindShortcuts(const QHash<QString,QString>& shortcuts){shortcuts_.bind(shortcuts);}
}
