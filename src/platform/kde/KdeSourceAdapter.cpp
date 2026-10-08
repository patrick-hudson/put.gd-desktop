#include "KdeSourceAdapter.h"
#include <QGuiApplication>
#include <QScreen>
#include <QJsonArray>
#include <cmath>
namespace putgd {
namespace {
QList<CaptureFrame> layout(){QList<CaptureFrame> out;for(auto* s:QGuiApplication::screens()){CaptureFrame frame;frame.identity=s->name();frame.logicalRect=s->geometry();frame.density=s->devicePixelRatio();frame.rotation=s->angleBetween(s->nativeOrientation(),s->orientation());frame.nativeRect=QRect(QPoint(qRound(s->geometry().x()*frame.density),qRound(s->geometry().y()*frame.density)),QSize(qRound(s->geometry().width()*frame.density),qRound(s->geometry().height()*frame.density)));out.append(frame);}return out;}
bool unchanged(const QList<CaptureFrame>& before){const auto now=layout();if(now.size()!=before.size())return false;for(const auto& frame:before){bool found=false;for(const auto& current:now)if(current.identity==frame.identity&&current.logicalRect==frame.logicalRect&&current.nativeRect==frame.nativeRect&&current.density==frame.density&&current.rotation==frame.rotation)found=true;if(!found)return false;}return true;}
struct WindowSnapshot {QImage image;Error error;bool done=false;std::function<void(PayloadResult)> accepted;ResourceLimits acceptedLimits;};
PayloadResult imageResult(const QImage& image,const ResourceLimits& limits,Error error={}){if(error)return {{},error};if(image.isNull())return {{},{"capture_failed","KWin provided no image",{}}};if(qint64(image.width())*image.height()>limits.imagePixels||image.sizeInBytes()>limits.imageBytes)return {{},{"input_excessive","Capture exceeds accepted operation resource limits",{}}};ClipboardPayload payload;payload.kind="image";payload.mime="image/png";payload.image=image;return {payload,{}};}
}
KdeSourceAdapter::KdeSourceAdapter(QObject* parent):QObject(parent){connect(&capture_,&KWinCapture::changed,this,&KdeSourceAdapter::capabilitiesChanged);connect(&clipboard_,&DataControlClipboard::changed,this,&KdeSourceAdapter::capabilitiesChanged);connect(&shortcuts_,&KdeShortcuts::changed,this,&KdeSourceAdapter::capabilitiesChanged);connect(&shortcuts_,&KdeShortcuts::activated,this,&KdeSourceAdapter::actionRequested);}
KdeSourceAdapter::~KdeSourceAdapter(){cancelAcquisition();clipboard_.cancel();for(auto* worker:workers_){worker->wait();delete worker;}const auto selectors=findChildren<KdeRegionOverlay*>(QString{},Qt::FindDirectChildrenOnly);for(auto* selector:selectors)delete selector;}
quint64 KdeSourceAdapter::clipboardRevision()const{return clipboard_.revision();}
QJsonObject KdeSourceAdapter::capabilities()const{
 auto region=capture_.status();
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 region["available"]=false;region["error"]="Native region selection requires current distro per-window LayerShellQt and Qt 6.5 or later";
#endif
 return {{"adapter","kde-wayland"},{"capture",QJsonObject{{"region",region},{"window",capture_.status()},{"desktop",capture_.status()}}},{"clipboard",clipboard_.status()},{"shortcuts",QJsonObject{{"available",shortcuts_.available()},{"bindings",shortcuts_.status()}}}};
}
SourceAdapter::PreparedAcquisition KdeSourceAdapter::prepareAcquisition(const QString& action){
 QPointer<KdeSourceAdapter> guard(this);
 if(action=="clipboard"){const auto revision=clipboard_.revision();return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(guard)guard->clipboard_.acquire(limits,std::move(cb),revision);else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};}
 if(action=="window"){
  if(QGuiApplication::applicationState()==Qt::ApplicationActive&&QGuiApplication::focusWindow())return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"target_unavailable","Focus an external window and invoke the window shortcut or CLI",{}}});};
  // ScreenShot2 freezes active focus at method invocation. Starting this bounded
  // request now prevents persistence or GUI focus changes from retargeting it.
  auto snapshot=std::make_shared<WindowSnapshot>();const auto generation=generation_;
  capture_.request("CaptureActiveWindow",{},limits_,[guard,snapshot,generation](QImage image,QVariantMap,Error error){snapshot->done=true;if(!guard||guard->generation_!=generation)snapshot->error={"cancelled","Window acquisition cancelled",{}};else{snapshot->image=std::move(image);snapshot->error=std::move(error);}if(snapshot->accepted){auto cb=std::move(snapshot->accepted);cb(imageResult(snapshot->image,snapshot->acceptedLimits,snapshot->error));}});
  return [guard,snapshot,generation](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(!guard||guard->generation_!=generation){cb({{},{"cancelled","Window acquisition cancelled",{}}});return;}if(snapshot->done)cb(imageResult(snapshot->image,limits,snapshot->error));else{snapshot->acceptedLimits=limits;snapshot->accepted=std::move(cb);}};
 }
 const auto frozen=layout();return [guard,action,frozen](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(!guard){cb({{},{"source_unavailable","Desktop adapter closed",{}}});return;}guard->captureScreens(action,frozen,limits,std::move(cb));};
}
void KdeSourceAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){prepareAcquisition(action)(limits,std::move(cb));}
void KdeSourceAdapter::captureScreens(QString action,QList<CaptureFrame> frozen,ResourceLimits limits,std::function<void(PayloadResult)> cb){
 if(action!="region"&&action!="desktop"){cb({{},{"source_unsupported","Unknown capture action",{}}});return;}
#if !defined(PUTGD_HAVE_LAYER_SHELL) || QT_VERSION < QT_VERSION_CHECK(6,5,0)
 if(action=="region"){cb({{},{"region_unavailable","Native region selection requires current distro per-window LayerShellQt",{}}});return;}
#endif
 if(overlay_){cb({{},{"busy_selecting","Region selection is already active",{}}});return;}
 if(frozen.isEmpty()||!unchanged(frozen)){cb({{},{"display_changed","Display layout changed after invocation",{}}});return;}
 // Qt screen DPR may be rounded on fractional Wayland outputs. Enforce image
 // budgets against ScreenShot2 native metadata, never guessed Qt pixel counts.
 struct Batch {QList<CaptureFrame> frames;QList<CaptureFrame> frozenLayout;qint64 bytes=0,pixels=0;int index=0;std::function<void(PayloadResult)> reply;std::function<void()> next;};
 auto batch=std::make_shared<Batch>();batch->frames=frozen;batch->frozenLayout=frozen;batch->reply=std::move(cb);QPointer<KdeSourceAdapter> guard(this);const auto generation=generation_;std::weak_ptr<Batch> weak=batch;
 batch->next=[guard,batchWeak=weak,action,limits,generation]{auto batch=batchWeak.lock();if(!batch)return;
  auto fail=[batch](Error error){auto cb=std::move(batch->reply);batch->next={};if(cb)cb({{},error});};
  if(!guard||generation!=guard->generation_){fail({"cancelled","Desktop capture cancelled",{}});return;}if(!unchanged(batch->frozenLayout)){fail({"display_changed","Display layout changed during acquisition",{}});return;}
  if(batch->index==batch->frames.size()){QRectF bounds;for(const auto& frame:batch->frames)bounds=bounds.united(frame.logicalRect);auto frames=batch->frames;const auto frozenLayout=batch->frozenLayout;auto reply=std::move(batch->reply);batch->next={};
   if(action=="desktop"){guard->compose(std::move(frames),bounds,limits,std::move(reply));return;}
   auto* selector=new KdeRegionOverlay(frames,guard);guard->overlay_=selector;auto cb=std::make_shared<std::function<void(PayloadResult)>>(std::move(reply));
   QObject::connect(selector,&KdeRegionOverlay::selected,guard,[guard,selector,frames,frozenLayout,limits,cb](QRectF selection){guard->overlay_=nullptr;selector->deleteLater();if(!unchanged(frozenLayout)){(*cb)({{},{"display_changed","Display layout changed during selection",{}}});return;}guard->compose(frames,selection,limits,std::move(*cb));});
   QObject::connect(selector,&KdeRegionOverlay::cancelled,guard,[guard,selector,cb](Error error){guard->overlay_=nullptr;selector->deleteLater();(*cb)({{},error});});selector->start();return;
  }
  const auto index=batch->index;guard->capture_.request("CaptureScreen",{batch->frames[index].identity},limits,[guard,batch,index,limits,generation](QImage image,QVariantMap metadata,Error error){
   const auto reject=[batch](Error failure){auto cb=std::move(batch->reply);batch->next={};if(cb)cb({{},failure});};
   if(!guard||generation!=guard->generation_){reject({"cancelled","Desktop capture cancelled",{}});return;}
   if(error){reject(error);return;}
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
void KdeSourceAdapter::compose(QList<CaptureFrame> frames,QRectF selection,const ResourceLimits& limits,std::function<void(PayloadResult)> cb){
 const auto generation=generation_;auto result=std::make_shared<PayloadResult>();auto* worker=QThread::create([frames=std::move(frames),selection,limits,result]{result->payload.image=composeFrames(frames,selection,std::min(limits.imagePixels,limits.imageBytes/4),result->error);result->payload.kind="image";result->payload.mime="image/png";});workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,generation,result,cb=std::move(cb)]()mutable{workers_.removeOne(worker);worker->deleteLater();if(generation!=generation_)cb({{},{"cancelled","Capture acquisition cancelled",{}}});else cb(std::move(*result));});worker->start();
}
void KdeSourceAdapter::cancelAcquisition(){++generation_;capture_.cancel();clipboard_.cancel();if(overlay_)overlay_->cancel();}
void KdeSourceAdapter::copy(const CopyPayload& payload,std::function<void(Error)> cb){clipboard_.copy(payload,std::move(cb));}
void KdeSourceAdapter::invalidateCopyEligibility(){clipboard_.invalidateCopyEligibility();}
void KdeSourceAdapter::bindShortcuts(const QHash<QString,QString>& desired){shortcuts_.bind(desired);}
}
