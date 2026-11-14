#include "platform/ProcessStat.h"
#include "HyprlandSourceAdapter.h"
#include "platform/SessionLock.h"
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
const QString focusMessage="Focus an external window and invoke the window hotkey or CLI";
// The parent of a process, from /proc/<pid>/stat (the field after the command's closing parenthesis).
qint64 parentOf(qint64 pid){return parentProcess(pid);}
}
HyprlandSourceAdapter::HyprlandSourceAdapter(QObject* parent):WaylandCaptureAdapter(parent),shortcuts_(&ipc_){
 connect(&shortcuts_,&ShortcutBackend::changed,this,&HyprlandSourceAdapter::capabilitiesChanged);
 // A drag that starts before the selector has surfaces reaches it through the catcher.
 // Up at the hotkey itself, ahead of the operation that will ask for the selector.
 catcherUnclaimed_.setSingleShot(true);catcherUnclaimed_.setInterval(3000);
 connect(&catcherUnclaimed_,&QTimer::timeout,this,[this]{if(!region_.busy()){catcher_.stop();region_.disarmPointerRelay();}});
 connect(&shortcuts_,&ShortcutBackend::activated,this,[this](const QString& action){
  if((action=="region"||action=="text")&&!region_.busy()&&!region_.cleanupRequired()){catchEarlyPress(layout());if(catcher_.running())catcherUnclaimed_.start();}
 });
 connect(&shortcuts_,&ShortcutBackend::activated,this,&HyprlandSourceAdapter::actionRequested);
 connect(&region_,&WaylandRegionProcess::ended,this,[this]{catcherUnclaimed_.stop();catcher_.stop();});
 connect(&catcher_,&PressCatcher::pointer,this,[this](int kind,QPointF logical){
  if(kind==PressCatcher::Cancel){
   catcher_.stop();
   // Not cancelAcquisition(): a clipboard import in flight is not part of this.
   if(region_.busy())region_.cancel();else{++generation_;capture_.cancel();earlyCancel_.start();}
   return;
  }
  region_.relayPointer(kind==PressCatcher::Press?QStringLiteral("press"):kind==PressCatcher::Move?QStringLiteral("move"):QStringLiteral("release"),logical);
 });
 connect(&catcher_,&PressCatcher::key,this,[this](int key,int modifiers){if(region_.grantKeyboard(key,modifiers))catcher_.stop();});
}
void HyprlandSourceAdapter::catchEarlyPress(const QList<CaptureFrame>& frames){
 catcherUnclaimed_.stop();
 QList<PressCatcher::Area> areas;for(const auto& frame:frames)areas.append({frame.identity,frame.logicalRect});
 if(!catcher_.running()&&catcher_.start(areas))region_.armPointerRelay();
 // Without a catcher nothing holds the keyboard, so the selector takes it at once.
 region_.setDeferKeyboard(catcher_.running());
}
HyprlandSourceAdapter::~HyprlandSourceAdapter(){shutdownCapture();}
bool HyprlandSourceAdapter::ownWindow(qint64 pid,const QString& appId)const{
 if(putgdAppId(appId))return true;
 if(pid<=0)return false;
 const qint64 self=QCoreApplication::applicationPid();
 if(pid==self||(uiProcess_&&pid==uiProcess_))return true;
 // Keep Hyprland's original bounded descendant rule (#1275). Native helpers
 // are direct QProcess children; this also covers intermediate launchers.
 constexpr int residentAncestorDepth=8;
 qint64 current=pid;
 for(int depth=0;depth<residentAncestorDepth&&current>1;++depth){current=parentOf(current);if(current==self)return true;}
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
 if(region_.cleanupRequired()){for(auto* status:{&region,&window,&desktop}){(*status)["available"]=false;(*status)["error"]=cleanupMessage();(*status)["code"]="capture_cleanup_pending";}}
 const QJsonObject helper=region_.diagnostics();
 // One monitor and a repeat read outputs as a desktop capture does; neither needs
 // the selector. The pointer's output comes from Hyprland's cursorpos request.
 auto monitor=desktop;monitor["pointer"]="hyprland-cursorpos";auto repeat=desktop;repeat["remembered"]=lastRegion_.has_value();
 QJsonObject delay{{"available",region["available"].toBool()},{"seconds",QJsonArray{3,5,10}},{"indicator","layer-surface"}};if(!region["available"].toBool())delay["error"]=region["error"];
 region["helper"]=helper;
 QJsonObject shortcuts{{"available",shortcuts_.available()},{"backend",shortcuts_.name()},{"bindings",shortcuts_.status()},{"hyprland",shortcuts_.diagnostics()}};
 if(!shortcuts_.error().isEmpty())shortcuts["error"]=shortcuts_.error();
 return {{"adapter","hyprland-wayland"},{"resident","qt-core"},{"captureHelper",helper},{"outputs",outputs},{"imageCopy",pixels},
  {"capture",QJsonObject{{"region",region},{"text",region},{"window",window},{"desktop",desktop},{"monitor",monitor},{"repeat",repeat}}},{"delay",delay},{"clipboard",clipboard_.status()},
  {"shortcuts",shortcuts}};
}
void HyprlandSourceAdapter::captureActiveWindow(const ResourceLimits& limits,quint64 generation,std::function<void(QImage,Error)> complete){
 // Freeze the active window at invocation through IPC, then read its pixels
 // now: a focus change or put.gd's own window opening later cannot retarget it.
 QPointer<HyprlandSourceAdapter> guard(this);
 freezeActiveWindow([guard,complete,limits,generation](QJsonObject target,Error error){
  if(error){complete({},std::move(error));return;}
  // Cancelled while the window was being mapped: start no capture.
  if(!guard||guard->generation_!=generation){complete({},{"cancelled","Window acquisition cancelled",{}});return;}
  guard->capture_.captureToplevel(target["identity"].toString(),limits,[complete](QImage image,Error error){
   if(!error&&refusedFrame(image)){complete({},permissionError());return;}
   complete(std::move(image),std::move(error));
  });
 });
}
void HyprlandSourceAdapter::probePointer(const QList<CaptureFrame>& frozen,std::function<void(QRectF,Error)> reply){
 ipc_.json("j/cursorpos",[frozen,reply=std::move(reply)](QJsonDocument document,Error error){
  const auto position=document.object();
  if(error||!position["x"].isDouble()||!position["y"].isDouble())reply({},{"capture_target","Hyprland did not report the pointer position",{{"reason",error.message}}});
  else if(const auto* frame=outputAt(frozen,QPointF(position["x"].toDouble(),position["y"].toDouble())))reply(frame->logicalRect,{});
  else reply({},{"capture_target","The pointer is not over a monitor",{}});
 });
}
void HyprlandSourceAdapter::sessionLocked(std::function<void(bool)> reply){
 // Hyprland knows whether a session lock (hyprlock, a quickshell lock) is up;
 // an answer of true is final. Otherwise the session's own services decide.
 QPointer<HyprlandSourceAdapter> guard(this);
 ipc_.json("j/locked",[guard,reply=std::move(reply)](QJsonDocument document,Error error)mutable{
  if(!error&&document.object()["locked"].toBool()){reply(true);return;}
  if(!guard){reply(false);return;}
  querySessionLocked(guard,std::move(reply));
 });
}
}
