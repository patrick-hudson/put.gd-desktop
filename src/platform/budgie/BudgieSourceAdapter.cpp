#include "BudgieSourceAdapter.h"
#include "platform/wayland/CaptureCompose.h"
#include "platform/wayland/SurfaceVisibility.h"
#include "platform/SessionLock.h"
#include <QCoreApplication>
#include <QJsonArray>
namespace putgd {
namespace {
Error visibilityError(){return {"session_locked","The screen is locked, blanked or not responding. Unlock it and try again.",{}};}
}
// labwc exposes no client-held session-lock boundary. Witness both sides of
// the copy and discard its pixels if the final frame is withheld. Only a lock
// and unlock entirely between these witnesses can escape this bounded check.
void BudgieSourceAdapter::queryVisibility(std::function<void(bool)> reply){
 if(visibilityQuery_)visibilityQuery_(std::move(reply));
 else querySurfaceVisible(this,capture_.connection(),std::move(reply));
}
bool BudgieSourceAdapter::ownAppId(const QString& appId){return putgdAppId(appId);}
BudgieSourceAdapter::BudgieSourceAdapter(QObject* parent):WaylandCaptureAdapter(parent){
 connect(&shortcuts_,&BudgieShortcuts::changed,this,&BudgieSourceAdapter::capabilitiesChanged);
 connect(&shortcuts_,&BudgieShortcuts::activated,this,&BudgieSourceAdapter::actionRequested);
}
BudgieSourceAdapter::~BudgieSourceAdapter(){shutdownCapture();}
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
 if(region_.cleanupRequired()){for(auto* status:{&region,&window,&desktop}){(*status)["available"]=false;(*status)["error"]=cleanupMessage();(*status)["code"]="capture_cleanup_pending";}}
 const QJsonObject helper=region_.diagnostics();
 // One monitor and a repeat read outputs as a desktop capture does; neither needs
 // the selector. The pointer's output comes from a cursor session of the same protocol.
 auto monitor=desktop;monitor["pointer"]="ext-image-copy-capture-cursor-session";auto repeat=desktop;repeat["remembered"]=lastRegion_.has_value();
 QJsonObject delay{{"available",region["available"].toBool()},{"seconds",QJsonArray{3,5,10}},{"indicator","layer-surface"}};if(!region["available"].toBool())delay["error"]=region["error"];
 region["helper"]=helper;
 return {{"adapter","budgie-wayland"},{"resident","qt-core"},{"captureHelper",helper},{"outputs",outputs},{"imageCopy",pixels},
  {"capture",QJsonObject{{"region",region},{"text",region},{"window",window},{"desktop",desktop},{"monitor",monitor},{"repeat",repeat}}},{"delay",delay},{"clipboard",clipboard_.status()},
  {"shortcuts",shortcuts_.capabilities()}};
}
void BudgieSourceAdapter::captureActiveWindow(const ResourceLimits& limits,quint64 generation,std::function<void(QImage,Error)> complete){
 QPointer<BudgieSourceAdapter> guard(this);
 capture_.freezeActive([guard,complete,limits,generation](ImageCopyCapture::Toplevel target,Error error){
  if(error){if(error.code=="capture_target"&&error.message.startsWith("No window"))error={"target_unavailable","Focus an external window and invoke the window hotkey or CLI",error.details};complete({},std::move(error));return;}
  if(!guard){complete({},{"cancelled","Window acquisition cancelled",{}});return;}
  if(ownAppId(target.appId)){complete({},{"target_unavailable","Focus an external window and invoke the window hotkey or CLI",{}});return;}
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
}
void BudgieSourceAdapter::probePointer(const QList<CaptureFrame>& frozen,std::function<void(QRectF,Error)> reply){
 pointer_.probe([frozen,reply=std::move(reply)](QList<CursorOutputProbe::Position> positions,Error error){
  if(error){reply({},std::move(error));return;}
  // The cursor image can straddle two outputs; the hotspot lies in one buffer.
  const CaptureFrame* found=nullptr;
  for(const auto& position:positions)for(const auto& frame:frozen)if(frame.identity==position.output&&QRect(QPoint(),frame.nativeRect.size()).contains(position.buffer))found=&frame;
  if(found)reply(found->logicalRect,{});else reply({},{"capture_target","The pointer is not over a monitor",{}});
 });
}
void BudgieSourceAdapter::sessionLocked(std::function<void(bool)> reply){
 // Countdown expiry must use the same real visibility signal before target
 // freezing; gtklock does not set logind or publish a screensaver query.
 queryVisibility([reply=std::move(reply)](bool visible){reply(!visible);});
}
void BudgieSourceAdapter::captureFrames(QList<CaptureFrame> frames,QList<CaptureFrame> frozen,quint64 revision,ResourceLimits limits,std::function<void(QList<CaptureFrame>,Error)> cb){
 QPointer<BudgieSourceAdapter> guard(this);const auto generation=generation_;
 queryVisibility([guard,generation,frames=std::move(frames),frozen=std::move(frozen),revision,limits,cb=std::move(cb)](bool visible)mutable{
  if(!guard||guard->generation_!=generation){cb({},{"cancelled","Desktop capture cancelled",{}});return;}
  if(!visible){cb({},visibilityError());return;}
  guard->readOutputs(std::move(frames),frozen,revision,limits,[guard,generation,frozen,revision,cb=std::move(cb)](QList<CaptureFrame> frames,Error error)mutable{
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
void BudgieSourceAdapter::bindShortcuts(const QHash<QString,QString>& desired){shortcuts_.bind(desired);}
}
