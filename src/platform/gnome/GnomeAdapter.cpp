#include "GnomeAdapter.h"
#include "platform/SessionLock.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include "GnomeProtocol.h"
#include "GnomeExtensionSetup.h"
#include "platform/NativeSession.h"
#include "core/ImageJob.h"
#include "core/Log.h"
#include <QThread>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QUrl>
#include <QUuid>
#include <cmath>
#include <limits>
#include <utility>
namespace putgd {
namespace {
const QString service="org.gnome.Shell",client="org.putgd.App.Gnome50",path="/org/gnome/Shell/Extensions/Putgd",interface="org.putgd.Gnome50";
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
QString json(const QJsonObject& value){return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));}
bool missingEndpoint(const QDBusMessage& message){
 return message.type()==QDBusMessage::ErrorMessage&&(message.errorName()=="org.freedesktop.DBus.Error.UnknownMethod"||message.errorName()=="org.freedesktop.DBus.Error.UnknownObject"||message.errorName()=="org.freedesktop.DBus.Error.UnknownInterface");
}
QJsonObject parse(const QDBusMessage& message,Error& error,bool hello=false){
 if(message.type()==QDBusMessage::ErrorMessage){
  const auto guidance="GNOME capture needs the put.gd Shell extension, which is not running yet.";
  error={"helper_unavailable",hello&&missingEndpoint(message)?guidance:"GNOME native helper did not complete the request",{{"dbusError",message.errorName()}}};
  if(hello&&missingEndpoint(message))error.details["helperEndpointMissing"]=true;return {};
 }
 if(message.arguments().size()!=1||message.arguments().first().metaType().id()!=QMetaType::QString){error={"bridge_protocol","GNOME helper returned an invalid reply",{}};return {};}
 const auto bytes=message.arguments().first().toString().toUtf8();if(bytes.size()>262144){error={"bridge_protocol","GNOME reply exceeds the control-message limit",{}};return {};}
 QJsonParseError e;const auto document=QJsonDocument::fromJson(bytes,&e);if(e.error!=QJsonParseError::NoError||!document.isObject()){error={"bridge_protocol","GNOME helper reply is not a JSON object",{}};return {};}
 auto result=document.object();if(result.contains("error")){const auto value=result["error"].toObject();error={value["code"].toString("helper_unavailable"),value["message"].toString("GNOME helper request failed"),value["details"].toObject()};}return result;
}
QString accelerator(const QString& text){
 const auto seq=QKeySequence::fromString(text,QKeySequence::PortableText);if(seq.count()!=1)return {};
 const auto key=seq[0];const auto mods=key.keyboardModifiers();if(mods.testFlag(Qt::KeypadModifier)||mods.testFlag(Qt::GroupSwitchModifier))return {};
 QString result;if(mods&Qt::ControlModifier)result+="<Control>";if(mods&Qt::ShiftModifier)result+="<Shift>";if(mods&Qt::AltModifier)result+="<Alt>";if(mods&Qt::MetaModifier)result+="<Super>";
 const auto k=key.key();QString symbol;
 if(k>=Qt::Key_A&&k<=Qt::Key_Z)symbol=QChar('a'+int(k-Qt::Key_A));else if(k>=Qt::Key_0&&k<=Qt::Key_9)symbol=QChar('0'+int(k-Qt::Key_0));else if(k>=Qt::Key_F1&&k<=Qt::Key_F35)symbol="F"+QString::number(k-Qt::Key_F1+1);
 else if(k>=32&&k<=126){
  static const QHash<int,QString> names{{32,"space"},{33,"exclam"},{34,"quotedbl"},{35,"numbersign"},{36,"dollar"},{37,"percent"},{38,"ampersand"},{39,"apostrophe"},{40,"parenleft"},{41,"parenright"},{42,"asterisk"},{43,"plus"},{44,"comma"},{45,"minus"},{46,"period"},{47,"slash"},{58,"colon"},{59,"semicolon"},{60,"less"},{61,"equal"},{62,"greater"},{63,"question"},{64,"at"},{91,"bracketleft"},{92,"backslash"},{93,"bracketright"},{94,"asciicircum"},{95,"underscore"},{96,"grave"},{123,"braceleft"},{124,"bar"},{125,"braceright"},{126,"asciitilde"}};
  symbol=names.value(k);if(symbol.isEmpty())return {};
 }else {switch(k){case Qt::Key_Print:symbol="Print";break;case Qt::Key_Space:symbol="space";break;case Qt::Key_Return:symbol="Return";break;case Qt::Key_Escape:symbol="Escape";break;case Qt::Key_Tab:symbol="Tab";break;case Qt::Key_Backspace:symbol="BackSpace";break;case Qt::Key_Delete:symbol="Delete";break;case Qt::Key_Insert:symbol="Insert";break;case Qt::Key_Home:symbol="Home";break;case Qt::Key_End:symbol="End";break;case Qt::Key_PageUp:symbol="Page_Up";break;case Qt::Key_PageDown:symbol="Page_Down";break;case Qt::Key_Left:symbol="Left";break;case Qt::Key_Right:symbol="Right";break;case Qt::Key_Up:symbol="Up";break;case Qt::Key_Down:symbol="Down";break;default:return {};}}
 return result+symbol;
}
}
GnomeAdapter::GnomeAdapter(QObject* parent):QObject(parent),bus_(QDBusConnection::sessionBus()),watcher_(service,bus_,QDBusServiceWatcher::WatchForOwnerChange,this){
 const auto graphical=qobject_cast<QGuiApplication*>(QCoreApplication::instance());
 const bool supported=graphical?QGuiApplication::platformName().startsWith("wayland"):gnomeCoreSessionEnvironment();
 if(!supported){helperError_={"session_unsupported","GNOME adapter requires a native GNOME Wayland session",{}};return;}
 ownsName_=bus_.registerService(client);if(!ownsName_){helperError_={"helper_unavailable","Another resident process owns the GNOME bridge client name",{}};return;}
 connect(&watcher_,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{helperChanged();});
 // Discovery follows events: the Shell's owner change and the extension's
 // CapabilitiesChanged, which it sends after exporting on every enable and unlock.
 // A failed Hello still retries, backing off to a slow safety probe, so a lost
 // signal or a transient failure recovers without waking an idle resident every
 // few seconds. Each retry reports itself, so the resident can release the pages
 // it mapped again (#1187).
 discoveryRetry_.setSingleShot(true);connect(&discoveryRetry_,&QTimer::timeout,this,[this]{if(!ready_){discover();emit discoveryRetried();}});
 uiProcessRetry_.setSingleShot(true);uiProcessRetry_.setInterval(firstRetryMs);
 connect(&uiProcessRetry_,&QTimer::timeout,this,[this]{
  // Closing the UI must not leave window hotkeys disabled after a transient
  // clear failure. Retry only while no helper is registered; a newer live PID
  // supersedes this recovery and stays fail-closed until its own registration.
  if(ready_&&!uiProcess_&&uiProcessError_)setUiProcess(0);
 });
 discover();
}
void GnomeAdapter::setDiscoveryRetryForTesting(int firstMs,int capMs){
 retryFirstMs_=firstMs;retryCapMs_=capMs;retryMs_=firstMs;uiProcessRetry_.setInterval(firstMs);
 if(discoveryRetry_.isActive())discoveryRetry_.start(retryMs_);
}
GnomeAdapter::~GnomeAdapter(){
 stopping_=true;discoveryRetry_.stop();uiProcessRetry_.stop();cancelAll("source_unavailable","GNOME adapter closed");
 auto pending=std::move(pending_);pending_.clear();for(auto& reply:pending)reply({"source_unavailable","GNOME adapter closed",{}},{});
 for(auto* worker:workers_){worker->wait();delete worker;}
 if(ownsName_)bus_.unregisterService(client);
}
QJsonObject GnomeAdapter::sync(const QString& method,const QVariantList& args,Error& error,int timeout){
 if(owner_.isEmpty()){error=helperAvailabilityError(true);return {};}
 auto message=QDBusMessage::createMethodCall(owner_,path,interface,method);message.setArguments(args);const auto response=bus_.call(message,QDBus::Block,timeout);
 auto result=parse(response,error,method=="Hello");
 if(method!="Hello"&&missingEndpoint(response)){
  // The extension may have been disabled since discovery. Only a missing Hello
  // confirms an absent endpoint; a missing method on a live helper is different.
  Error probe;sync("Hello",{},probe);if(probe.details["helperEndpointMissing"].toBool())error.message=probe.message;
 }
 return result;
}
void GnomeAdapter::call(const QString& method,const QVariantList& args,Reply reply,int timeout){
 if(owner_.isEmpty()||stopping_){reply({"helper_unavailable","GNOME helper is unavailable",{}},{});return;}
 auto message=QDBusMessage::createMethodCall(owner_,path,interface,method);message.setArguments(args);const auto serial=++callSerial_;pending_.insert(serial,std::move(reply));
 auto* watcher=new QDBusPendingCallWatcher(bus_.asyncCall(message,timeout),this);const auto owner=owner_;
 connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher,serial,owner,method]{const auto callback=pending_.take(serial);const QDBusMessage message=watcher->reply();watcher->deleteLater();if(!callback)return;Error error;const auto result=parse(message,error,method=="Hello");
  if(owner!=owner_)error={"helper_unavailable","GNOME Shell connection changed",{}};
  else if(method!="Hello"&&missingEndpoint(message)){Error probe;sync("Hello",{},probe);if(probe.details["helperEndpointMissing"].toBool())error.message=probe.message;}
  callback(error,result);
 });
}
void GnomeAdapter::followOwner(const QString& owner){
 if(owner_==owner)return;
 // Signals stay subscribed while the Shell keeps its connection, so none can be
 // lost between an extension disable and the next enable.
 if(!owner_.isEmpty()){bus_.disconnect(owner_,path,interface,"Action",this,SLOT(helperAction(QString,QString)));bus_.disconnect(owner_,path,interface,"ClipboardChanged",this,SLOT(helperClipboardChanged(qulonglong)));bus_.disconnect(owner_,path,interface,"CapabilitiesChanged",this,SLOT(helperChanged()));bus_.disconnect(owner_,path,interface,"Click",this,SLOT(helperClick(QString)));}
 // A Shell that went away took its click watch with it.
 clickSink_={};
 owner_=owner;
 if(!owner_.isEmpty()){bus_.connect(owner_,path,interface,"Action",this,SLOT(helperAction(QString,QString)));bus_.connect(owner_,path,interface,"ClipboardChanged",this,SLOT(helperClipboardChanged(qulonglong)));bus_.connect(owner_,path,interface,"CapabilitiesChanged",this,SLOT(helperChanged()));}
}
void GnomeAdapter::discover(){
 if(!ownsName_||stopping_)return;
 // The match rules are sent before Hello, so an enable the Shell handles after
 // this Hello still reaches the adapter as CapabilitiesChanged.
 const auto owner=bus_.interface()->serviceOwner(service);followOwner(owner.isValid()?owner.value():QString());
 if(owner_.isEmpty()){helperUnavailable({"helper_unavailable","GNOME Shell is unavailable on the session bus",{}});return;}
 call("Hello",{},[this](Error error,QJsonObject hello){
  if(!error&&(hello["version"].toInt()!=gnome::protocolVersion||hello["shellMajor"].toInt()!=50))error={"helper_incompatible","GNOME helper requires protocol 1 and GNOME Shell 50",{}};
  if(error){helperUnavailable(error);evaluateSetup();return;}
  discoveryRetry_.stop();retryMs_=retryFirstMs_;
  ready_=true;helperError_={};helper_=hello;setUiProcess(uiProcess_);helperRevision_=quint64(hello["clipboardRevision"].toDouble());++revision_;bindShortcuts(requested_);emit capabilitiesChanged();
  evaluateSetup();
  // A run that just started reports when it ends; until then the failure it may
  // find is unknown and the notice would ask for a logout that can't help.
  if(outdated()&&!setupRunning_)emitSetupNotice(currentSetup());
 });
}
// The retry runs for as long as the helper stays away, each wait twice the last
// up to the safety probe's. Only a change may reach the UI: every signal costs it
// a new snapshot and a reload of the History page.
void GnomeAdapter::helperUnavailable(const Error& error){
 const auto before=capabilities();ready_=false;helperError_=error;invalidateShortcutBindings();
 if(capabilities()!=before)emit capabilitiesChanged();
 if(!stopping_&&!discoveryRetry_.isActive()){discoveryRetry_.start(retryMs_);retryMs_=std::min(retryCapMs_,retryMs_*2);}
}
void GnomeAdapter::setUiProcess(qint64 pid){
 uiProcess_=pid>0&&pid<=std::numeric_limits<quint32>::max()?pid:0;uiProcessError_={};
 if(!ready_)return;
 if(helper_["uiProcessVersion"].toInt()!=1){
  if(uiProcess_)uiProcessError_={"helper_incompatible","Update the GNOME extension to exclude the UI process from window capture",{}};
 }else{
  // Ack registration before the supervisor can let its child display a window.
  // A failed update disables window acquisition, including Shell hotkey tickets.
  sync("SetUiProcess",{quint32(uiProcess_)},uiProcessError_);
  if(!uiProcess_&&uiProcessError_&&!stopping_)uiProcessRetry_.start();
 }
 emit capabilitiesChanged();
}
void GnomeAdapter::helperChanged(){
 // A real change: rediscover now and restart the backoff from its first step.
 discoveryRetry_.stop();retryMs_=retryFirstMs_;setupCheckedForOwner_=false;
 ready_=false;invalidateShortcutBindings();helper_={};++revision_;cancelAll("helper_unavailable","GNOME helper disabled or Shell restarted");emit capabilitiesChanged();discover();
}
void GnomeAdapter::invalidateShortcutBindings(){
 bindings_={};for(const auto& action:Settings::invocableActionNames()){
  const auto text=requested_.value(action);
  bindings_[action]=QJsonObject{{"requested",text},{"bound",false},{"error",text.isEmpty()?"disabled":"helper_unavailable"}};
 }
}
void GnomeAdapter::helperAction(const QString& action,const QString& ticket){
 if(!ready_||!Settings::invocableActionNames().contains(action)||ticket.size()!=36)return;
 if(recording_){release(ticket);return;}
 // Recording prepares its own region later; the ticket carries no frozen source.
 if(action=="scroll"||action==Settings::recordingActionName()){emit actionRequested(action);return;}
 hotkeyPrepared_[action].append(ticket);emit actionRequested(action);
 // Rejected actions do not leave unused frozen targets behind.
 if(hotkeyPrepared_[action].removeOne(ticket))release(ticket);
}
void GnomeAdapter::helperClipboardChanged(qulonglong revision){if(helperRevision_!=revision){helperRevision_=revision;++revision_;}}
quint64 GnomeAdapter::clipboardRevision()const{
 if(ready_){Error error;const auto result=const_cast<GnomeAdapter*>(this)->sync("Revision",{},error);if(!error)const_cast<GnomeAdapter*>(this)->helperClipboardChanged(qulonglong(result["clipboardRevision"].toDouble()));}
 return revision_;
}
QJsonObject GnomeAdapter::capabilities()const{
 const auto helperError=ready_?Error{}:helperAvailabilityError();
 auto capture=QJsonObject{{"available",ready_},{"mode","compositor-native"},{"consent","one-time-extension-setup"}};if(!ready_)capture["error"]=helperError.toJson();
 auto window=capture;if(uiProcessError_){window["available"]=false;window["error"]=uiProcessError_.toJson();}
 auto clipboard=QJsonObject{{"available",ready_},{"types",QJsonArray{"files","image","text"}},{"import","bounded-mutter-selection"},{"result","st-clipboard-only"}};
 if(!ready_)clipboard["error"]=helperError.toJson();
 // Monitor, repeat and the countdown came with capture version 2 (#1363): an
 // extension loaded before them is reported until the next login loads it.
 auto monitor=capture;const auto versionError=ready_?captureVersionError():Error{};
 if(versionError){monitor["available"]=false;monitor["error"]=versionError.toJson();}
 auto repeat=monitor;repeat["remembered"]=lastRegion_.has_value();
 // Copy text selects a region with the packaged extension only: one loaded before it asks for the login.
 auto text=capture;if(ready_&&outdated()){text["available"]=false;text["error"]=recordingShellError().toJson();}
 QJsonObject delay{{"available",monitor["available"].toBool()},{"seconds",QJsonArray{3,5,10}},{"indicator","shell-actor"}};if(!monitor["available"].toBool())delay["error"]=monitor["error"];
 return {{"adapter","gnome-wayland"},{"helper",QJsonObject{{"available",ready_},{"uuid","putgd@put.gd"},{"protocol",gnome::protocolVersion},{"shellMajor",helper_["shellMajor"]},{"extensionVersion",helper_["extensionVersion"]},{"packagedExtensionVersion",gnome::extensionVersion},{"outdated",outdated()},{"recordingVersion",helper_["recordingVersion"].toInt()},{"captureVersion",helper_["captureVersion"].toInt(1)},{"error",helperError.toJson()},{"setup",currentSetup().toJson()}}},{"capture",QJsonObject{{"region",capture},{"text",text},{"window",window},{"desktop",capture},{"monitor",monitor},{"repeat",repeat}}},{"delay",delay},{"clipboard",clipboard},{"shortcuts",QJsonObject{{"available",ready_},{"bindings",bindings_}}}};
}
SourceAdapter::PreparedAcquisition GnomeAdapter::prepareAcquisition(const QString& requested){
 // Copy text (#1428) selects exactly as a region does, from its own hotkey
 // ticket; only Repeat's memory is left alone. An extension loaded before it
 // is refused below with the region's "log out once" step.
 const bool text=requested==QLatin1String("text");const QString action=text?QStringLiteral("region"):requested;
 QPointer<GnomeAdapter> guard(this);const bool hotkey=!hotkeyPrepared_[requested].isEmpty();const auto id=hotkey?hotkeyPrepared_[requested].takeFirst():uuid();Error error;
 if(!ready_)error=helperAvailabilityError(true);
 else if(action=="window"&&uiProcessError_){error=uiProcessError_;if(hotkey)release(id);}
 // Both region modes require the packaged appearance protocol; an older
 // loaded extension silently ignores its tokens. Ask for re-login before preparing.
 else if(action=="region"&&outdated()){error=recordingShellError();if(hotkey)release(id);}
 // Monitor and repeat exist only in capture version 2; a repeat needs its region.
 else if((action=="monitor"||action=="repeat")&&captureVersionError()){error=captureVersionError();if(hotkey)release(id);}
 else if(action=="repeat"&&!lastRegion_){error=noLastRegion();if(hotkey)release(id);}
 else if(!hotkey){const auto result=sync("Prepare",{id,action},error);if(!error&&(result["version"].toInt()!=gnome::protocolVersion||result["requestId"].toString()!=id))error={"bridge_protocol","GNOME preparation identity is invalid",{}};if(!error){prepared_.append(id);QTimer::singleShot(120000,this,[this,id]{if(prepared_.removeOne(id))release(id);});}}
 if(hotkey&&!error)prepared_.append(id);
 // Show a live selection at the press; Acquire waits for the journal and takes
 // up its geometry. Screenshot first freezes the stage only after both saves.
 // An extension loaded before this method selects from Acquire, as it always did.
 // The journal may fail or the operation stop before this continuation runs. It
 // can die on a journal worker; the selector must not stay up until the bridge's
 // 120 s timeout, so an unconsumed preview is released on the session thread.
 auto started=std::make_shared<std::atomic_bool>(false);std::shared_ptr<void> preview;
 if(!error&&action=="region"&&!regionScreenshotFirst_&&helper_["previewVersion"].toInt()==1){
  call("Preview",{id,json({{"appearance",captureAppearance_},{"outcome",captureOutcome_}})},[](Error,QJsonObject){});
  preview=std::shared_ptr<void>(nullptr,[guard,id,started](void*){
   if(started->load()||!guard)return;
   QMetaObject::invokeMethod(guard,[guard,id]{if(guard&&guard->prepared_.removeOne(id))guard->release(id);},Qt::QueuedConnection);
  });
 }
 return [guard,id,action,error,text,outcome=captureOutcome_,screenshot=action=="region"&&regionScreenshotFirst_,started,preview,delay=action=="region"&&!regionScreenshotFirst_?captureDelay_:0,repeat=action=="repeat"?lastRegion_:std::optional<RegionMemory>{}](const ResourceLimits& limits,std::function<void(PayloadResult)> reply){started->store(true);if(error){reply({{},error});return;}if(!guard){reply({{},{"source_unavailable","GNOME adapter closed",{}}});return;}guard->start(id,action,limits,std::move(reply),outcome,screenshot,delay,repeat,!text);};
}
Error GnomeAdapter::captureVersionError()const{
 if(!ready_)return helperAvailabilityError(true);
 if(helper_["captureVersion"].toInt(1)>=2)return {};
 // The setup state decides the wording: "log out" only when a login would help.
 return {"helper_outdated",outdated()?currentSetup().message:QStringLiteral("Update the put.gd GNOME extension to capture one monitor, repeat a region or delay a capture"),{{"loaded",helper_["extensionVersion"].toInt()},{"packaged",gnome::extensionVersion}}};
}
void GnomeAdapter::countdown(int seconds,std::function<void(Error)> reply){
 if(const auto error=captureVersionError()){reply(error);return;}
 if(!countdownId_.isEmpty()){reply({"busy_selecting","Another capture countdown is running",{}});return;}
 const auto id=uuid();countdownId_=id;const auto generation=generation_;
 // The Shell draws the pill, removes it and answers; a click on it answers cancelled.
 call("Countdown",{id,json({{"seconds",seconds},{"appearance",captureAppearance_}})},[this,id,generation,reply=std::move(reply)](Error error,QJsonObject result){
  if(countdownId_==id)countdownId_.clear();
  if(!error&&generation!=generation_)error={"cancelled","Capture countdown cancelled",{}};
  if(!error&&(result["version"].toInt()!=gnome::protocolVersion||result["requestId"].toString()!=id))error={"bridge_protocol","GNOME countdown identity is invalid",{}};
  lockAware(std::move(error),reply);
 },(seconds+15)*1000);
}
// The Shell disables its extensions when it locks, and that can beat the
// extension's own session_locked answer: the request then fails as a helper that
// went away. On a locked session that is the lock, not a broken helper. logind's
// hint follows the lock screen's own animation, so the question is asked for up
// to two seconds before the helper's error stands.
void GnomeAdapter::lockAware(Error error,std::function<void(Error)> reply,int attempt){
 if(!error||error.code=="cancelled"||error.code=="session_locked"||stopping_){reply(std::move(error));return;}
 QTimer::singleShot(500,this,[this,error=std::move(error),reply=std::move(reply),attempt]{
  querySessionLocked(this,[this,error,reply,attempt](bool locked){
   if(locked)reply(SourceAdapter::lockedError());else if(attempt<3)lockAware(error,reply,attempt+1);else reply(error);
  });
 });
}
void GnomeAdapter::sessionLocked(std::function<void(bool)> reply){querySessionLocked(this,std::move(reply));}
void GnomeAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> reply){prepareAcquisition(action)(limits,std::move(reply));}
void GnomeAdapter::start(const QString& id,const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> reply,const QString& outcome,bool screenshotFirst,int delaySeconds,const std::optional<RegionMemory>& repeat,bool remember){
 if(!prepared_.removeOne(id)){reply({{},{"bridge_protocol","GNOME preparation expired or was cancelled",{}}});return;}
 // A delayed live region counts down inside Acquire: the same lock question as countdown().
 if(delaySeconds>0){auto inner=std::move(reply);reply=[this,inner=std::move(inner)](PayloadResult result){if(!result.error){inner(std::move(result));return;}lockAware(std::move(result.error),[inner](Error error){inner({{},std::move(error)});});};}
 auto transfer=std::make_shared<Transfer>();transfer->id=id;transfer->outcome=outcome;transfer->action=action;transfer->delaySeconds=delaySeconds;if(repeat){transfer->area=repeat->area;transfer->layoutKey=repeat->layoutKey;}transfer->screenshotFirst=screenshotFirst;transfer->remember=remember;transfer->limits=limits;transfer->reply=std::move(reply);transfer->generation=generation_;transfers_[id]=transfer;
 // Only clipboard imports choose among image types; screen captures are PNG.
 // Listing Qt's readers instantiates every installed image plugin (30 plugins,
 // 12 MiB with KDE's kimageformats), so the transient image job lists them,
 // once and off the session thread, and the Core resident never loads them.
 if(action!="clipboard"){requestTransfer(transfer,QJsonArray{"image/png"});return;}
 if(clipboardImageMimes_){requestTransfer(transfer,*clipboardImageMimes_);return;}
 auto types=std::make_shared<QList<QByteArray>>();auto error=std::make_shared<Error>();
 auto* worker=QThread::create([types,error]{*types=ImageJob().discover(*error);});workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,types,error,transfer]{
  workers_.removeOne(worker);worker->deleteLater();
  if(stopping_||!transfers_.contains(transfer->id))return;
  if(*error){transfers_.remove(transfer->id);release(transfer->id);auto reply=std::move(transfer->reply);reply({{},*error});return;}
  // The bridge accepts at most 128 image/* types; Qt can also list application/*.
  QJsonArray imageMimes{"image/png"};
  for(const auto& mime:*types)if(mime.startsWith("image/")&&mime!="image/png"&&mime.size()<=128&&imageMimes.size()<128)imageMimes.append(QString::fromLatin1(mime));
  clipboardImageMimes_=imageMimes;requestTransfer(transfer,imageMimes);
 });
 worker->start();
}
void GnomeAdapter::requestTransfer(const std::shared_ptr<Transfer>& transfer,const QJsonArray& imageMimes){
 const auto limits=transfer->limits;
 QJsonObject bounds{{"appearance",captureAppearance_},{"outcome",transfer->outcome},{"imageMimes",imageMimes},{"textBytes",double(limits.clipboardTextBytes)},{"imageBytes",double(limits.imageBytes)},{"imagePixels",double(limits.imagePixels)},{"maxFiles",limits.maxFiles}};
 // Screenshot first (#629): the Shell freezes the stage before selecting.
 if(transfer->screenshotFirst)bounds["regionMode"]="screenshot_first";
 // Capture version 2 (#1363): a live region's delay, and a repeat's remembered area.
 if(transfer->delaySeconds>0&&!transfer->screenshotFirst)bounds["delaySeconds"]=transfer->delaySeconds;
 if(transfer->action=="repeat"){bounds["area"]=transfer->area;bounds["layoutKey"]=transfer->layoutKey;}
 call("Acquire",{transfer->id,json(bounds)},[this,transfer](Error error,QJsonObject metadata){
  if(!transfers_.contains(transfer->id))return;
  const auto bytes=metadata["bytes"].toDouble(-1);const auto bound=std::max(transfer->limits.clipboardTextBytes,transfer->limits.imageBytes);
  if(!error&&(metadata["version"].toInt()!=gnome::protocolVersion||metadata["requestId"].toString()!=transfer->id||bytes<0||bytes>bound||bytes!=std::floor(bytes)))error={"bridge_protocol","GNOME transfer metadata exceeds limits or has invalid identity",{}};
  if(error){transfers_.remove(transfer->id);release(transfer->id);auto reply=std::move(transfer->reply);reply({{},error});return;}
  transfer->metadata=metadata;read(transfer);
 },125000);
}
void GnomeAdapter::read(const std::shared_ptr<Transfer>& transfer){
 if(transfer->bytes.size()==transfer->metadata["bytes"].toDouble()){release(transfer->id);decode(transfer);return;}
 const auto offset=transfer->bytes.size();call("Read",{transfer->id,qulonglong(offset),uint(gnome::chunkBytes)},[this,transfer,offset](Error error,QJsonObject result){
  if(!transfers_.contains(transfer->id))return;
  const auto encoded=result["data"].toString().toLatin1();const auto bytes=QByteArray::fromBase64(encoded,QByteArray::AbortOnBase64DecodingErrors);
  if(!error&&(result["version"].toInt()!=gnome::protocolVersion||result["requestId"].toString()!=transfer->id||result["offset"].toDouble(-1)!=offset||encoded.size()>4*((gnome::chunkBytes+2)/3)||bytes.isEmpty()||bytes.size()>gnome::chunkBytes||bytes.toBase64()!=encoded||bytes.size()>transfer->metadata["bytes"].toDouble()-offset))error={"bridge_protocol","GNOME transfer chunk is invalid",{}};
  if(error){transfers_.remove(transfer->id);release(transfer->id);auto reply=std::move(transfer->reply);reply({{},error});return;}
  transfer->bytes.append(bytes);read(transfer);
 });
}
void GnomeAdapter::decode(const std::shared_ptr<Transfer>& transfer){
 auto result=std::make_shared<PayloadResult>();auto* worker=QThread::create([transfer,result]{*result=gnome::decodeTransfer(transfer->metadata,transfer->bytes,transfer->id,transfer->limits);});workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,transfer,result]{workers_.removeOne(worker);worker->deleteLater();if(!transfers_.remove(transfer->id))return;auto reply=std::move(transfer->reply);if(transfer->generation!=generation_){reply({{},{"cancelled","GNOME acquisition cancelled",{}}});return;}
  // Repeat last region remembers a region that was captured, here in memory only.
  const auto selection=transfer->metadata["selection"].toArray();const auto key=transfer->metadata["layoutKey"].toString();
  if(!result->error&&transfer->remember&&transfer->action=="region"&&selection.size()==4&&!key.isEmpty())lastRegion_=RegionMemory{selection,key};
  reply(std::move(*result));});worker->start();
}
// Recording protocol 1 adds SelectArea, ShowRecordingFrame and HideRecordingFrame.
bool GnomeAdapter::selectsRegions()const{return ready_&&!outdated()&&helper_["recordingVersion"].toInt()>=1;}
// An extension older than the packaged one is still the one loaded at login (#131).
bool GnomeAdapter::outdated()const{return ready_&&helper_["extensionVersion"].toInt()<gnome::extensionVersion;}
Error GnomeAdapter::recordingShellError()const{
 // The setup state decides the wording: "log out" only when a login would help.
 if(outdated())return {"helper_outdated",currentSetup().message,{{"loaded",helper_["extensionVersion"].toInt()},{"packaged",gnome::extensionVersion}}};
 if(!ready_)return {"region_unavailable",setup_.needsUser()?setup_.message:QString("Recording a region on GNOME needs the put.gd Shell extension."),{{"helper",helperAvailabilityError().toJson()}}};
 return {"region_unavailable","Recording a region on GNOME needs the put.gd Shell extension with recording support",{}};
}
Error GnomeAdapter::regionError()const{return selectsRegions()?Error{}:recordingShellError();}
void GnomeAdapter::selectRegion(RecordingShell::Reply reply){selectRegionImpl(std::move(reply),false);}
void GnomeAdapter::selectRegionImpl(RecordingShell::Reply reply,bool scroll){
 if(!(scroll?scrollShell_.selectsRegions():selectsRegions())){reply(recordingShellError(),{});return;}
 const auto id=uuid();recordingSelection_=id;
 // The user drags at their own pace; the extension's request deadline (120 s) ends it first.
 call("SelectArea",{json(QJsonObject{{"id",id},{"appearance",captureAppearance_}})},[this,id,reply](Error error,QJsonObject result){
  if(recordingSelection_==id)recordingSelection_.clear();
  if(!error&&(result["requestId"].toString()!=id||!result["rect"].isArray()||result["rect"].toArray().size()!=4))error={"bridge_protocol","GNOME helper returned an invalid selection",{}};
  reply(error,error?QJsonObject{}:QJsonObject{{"rect",result["rect"]}});
 },125000);
}
void GnomeAdapter::cancelSelection(){if(recordingSelection_.isEmpty())return;const auto id=std::exchange(recordingSelection_,QString{});release(id);}
void GnomeAdapter::showFrame(const QJsonObject& frame,RecordingShell::Reply reply){showFrameImpl(frame,std::move(reply),false);}
void GnomeAdapter::showFrameImpl(const QJsonObject& shown,RecordingShell::Reply reply,bool scroll){
 if(!(scroll?scrollShell_.selectsRegions():selectsRegions())){reply(outdated()?recordingShellError():Error{"helper_incompatible","Update the GNOME extension to frame recordings",{}},{});return;}
 // A scroll capture reads its frames from Mutter's area stream, which sends
 // one only for damage inside the area: the Shell keeps one pixel of it
 // repainting while the frame shows, or a page that has come to rest gives
 // no frame to prove it (the extension's RecordingFrame has the measurement).
 auto frame=shown;if(scroll)frame["beat"]=true;
 // The extension waits up to two seconds for the bar window to map before placing it.
 call("ShowRecordingFrame",{json(frame)},std::move(reply),4000);
}
void GnomeAdapter::hideFrame(){if(selectsRegions())call("HideRecordingFrame",{},[](Error,QJsonObject){});}
// Recording protocol 2 adds RecordingWindows. An extension loaded before the
// upgrade that brought it is still the one running until the user logs out.
void GnomeAdapter::listWindows(RecordingWindows::Reply reply){
 if(outdated()){reply(recordingShellError(),{});return;}
 if(!ready_){reply(helperAvailabilityError(true),{});return;}
 if(helper_["recordingVersion"].toInt()<2){reply({"helper_incompatible","Update the put.gd GNOME extension to record windows",{}},{});return;}
 call("RecordingWindows",{},[reply](Error error,QJsonObject result){
  if(!error&&!result["windows"].isArray())error={"bridge_protocol","GNOME helper returned an invalid window list",{}};
  if(error){reply(error,{});return;}
  QJsonArray windows;
  for(const auto& value:result["windows"].toArray()){
   const auto w=value.toObject();const auto r=w["rect"].toArray();
   if(!w["identity"].isString()||w["identity"].toString().isEmpty()||w["identity"].toString().size()>32||r.size()!=4)continue;
   windows.append(QJsonObject{{"identity",w["identity"].toString()},{"title",w["title"].toString().left(512)},{"appId",w["appId"].toString().left(256)},{"rect",r},{"active",w["active"].toBool()},{"minimized",w["minimized"].toBool()}});
  }
  reply({},{{"windows",windows},{"activeExcluded",result["activeExcluded"].toBool()}});
 });
}
QJsonObject GnomeAdapter::clickCapability()const{
 const QJsonArray targets{"monitor","desktop","region","window"};
 QJsonObject capability{{"route","gnome-shell-extension"},{"drawnBy","recorder"},{"targets",targets}};
 Error error;
 if(outdated())error=recordingShellError();
 else if(!ready_)error=helperAvailabilityError(true);
 else if(helper_["recordingVersion"].toInt()<3)error={"helper_incompatible","Update the put.gd GNOME extension to highlight clicks",{}};
 capability["available"]=!error;
 if(error){capability["reason"]=error.code;capability["message"]=error.message;}
 return capability;
}
void GnomeAdapter::watchClicks(const QJsonObject&,RecordingClicks::Sink sink,RecordingClicks::Reply reply){
 const auto capability=clickCapability();
 if(!capability["available"].toBool()){reply({capability["reason"].toString("helper_unavailable"),capability["message"].toString("The GNOME extension cannot report clicks"),{}},{});return;}
 clickSink_=std::move(sink);
 bus_.connect(owner_,path,interface,"Click",this,SLOT(helperClick(QString)));
 call("WatchClicks",{},[this,reply](Error error,QJsonObject result){if(error)unwatchClicks();reply(error,result);});
}
void GnomeAdapter::unwatchClicks(){
 if(!clickSink_)return;
 clickSink_={};
 if(owner_.isEmpty())return;
 bus_.disconnect(owner_,path,interface,"Click",this,SLOT(helperClick(QString)));
 if(ready_&&!stopping_)call("UnwatchClicks",{},[](Error,QJsonObject){});
}
void GnomeAdapter::helperClick(const QString& json){
 // From the Shell's own connection (the signal match names it); bounded and typed before use.
 if(!clickSink_||json.size()>512)return;
 const auto click=QJsonDocument::fromJson(json.toUtf8()).object();
 const int button=click["button"].toInt();
 if(!click["x"].isDouble()||!click["y"].isDouble()||button<1||button>3)return;
 QJsonObject out{{"x",click["x"].toInt()},{"y",click["y"].toInt()},{"button",button}};
 const auto window=click["window"].toObject();const auto rect=window["rect"].toArray();
 if(window["identity"].isString()&&window["identity"].toString().size()<=32&&rect.size()==4)out["window"]=QJsonObject{{"identity",window["identity"].toString()},{"rect",rect}};
 clickSink_(out);
}
void GnomeAdapter::release(const QString& id){if(!owner_.isEmpty()&&!stopping_)call("Release",{id},[](Error,QJsonObject){});}
void GnomeAdapter::cancelAll(const QString& code,const QString& message){
 ++generation_;const auto hotkeys=std::move(hotkeyPrepared_);hotkeyPrepared_.clear();for(const auto& tickets:hotkeys)for(const auto& id:tickets)release(id);
 const auto prepared=std::move(prepared_);prepared_.clear();for(const auto& id:prepared)release(id);
 auto transfers=std::move(transfers_);transfers_.clear();for(const auto& transfer:transfers){release(transfer->id);auto reply=std::move(transfer->reply);if(reply)reply({{},{code,message,{}}});}
}
void GnomeAdapter::cancelAcquisition(){
 // Releasing the countdown's request removes its pill; its call then answers cancelled.
 if(!countdownId_.isEmpty())release(std::exchange(countdownId_,{}));
 cancelAll("cancelled","GNOME acquisition cancelled");
}
void GnomeAdapter::copy(const CopyPayload& payload,std::function<void(Error)> answer){
 auto reply=[answer=std::move(answer)](Error e){logging::debug(logClipboard(),"Native copy finished",{{"writer","gnome-helper"},{"ok",!e},{"code",e.code}});answer(std::move(e));};
 if(!ready_){reply({"clipboard_unavailable","GNOME helper cannot own CLIPBOARD",{}});return;}
 QByteArray bytes;QString mime;
 if(payload.kind=="image"){mime="image/png";bytes=payload.encodedImage;}else if(payload.kind=="text"){mime="text/plain;charset=utf-8";bytes=payload.text.toUtf8();}else if(payload.kind=="files"){mime="text/uri-list";for(const auto& file:payload.files)bytes+=QUrl::fromLocalFile(file).toEncoded()+"\r\n";}else {reply({"source_unsupported","GNOME cannot copy this result type",{}});return;}
 const auto ticket=++copyGeneration_;const auto id=uuid();call("BeginCopy",{id,mime,qulonglong(bytes.size())},[this,id,bytes,ticket,reply=std::move(reply)](Error error,QJsonObject)mutable{if(error){reply(error);return;}writeCopy(id,bytes,0,ticket,std::move(reply));});
}
void GnomeAdapter::writeCopy(const QString& id,const QByteArray& bytes,qsizetype offset,quint64 ticket,std::function<void(Error)> reply){
 if(ticket!=copyGeneration_){release(id);reply({"copy_suppressed","A newer invocation superseded GNOME result copying",{}});return;}
 if(offset==bytes.size()){call("CommitCopy",{id},[this,id,reply=std::move(reply)](Error error,QJsonObject result){release(id);if(!error){helperClipboardChanged(qulonglong(result["clipboardRevision"].toDouble()));lastCopyRevision_=revision_;}reply(error);});return;}
 const auto chunk=bytes.mid(offset,gnome::chunkBytes);call("WriteCopy",{id,qulonglong(offset),QString::fromLatin1(chunk.toBase64())},[this,id,bytes,offset,ticket,count=chunk.size(),reply=std::move(reply)](Error error,QJsonObject)mutable{if(error){release(id);reply(error);return;}writeCopy(id,bytes,offset+count,ticket,std::move(reply));});
}
void GnomeAdapter::invalidateCopyEligibility(){++copyGeneration_;if(ready_)call("InvalidateCopy",{},[](Error,QJsonObject){});}
void GnomeAdapter::bindShortcuts(const QHash<QString,QString>& requested){
 requested_=requested;applyShortcutBindings();
}
void GnomeAdapter::setShortcutRecording(bool active,std::function<void(Error)> reply){
 recording_=active;applyShortcutBindings(std::move(reply));
}
void GnomeAdapter::applyShortcutBindings(std::function<void(Error)> reply){
 const auto generation=++bindingGeneration_;QJsonObject entries;bindings_={};
 for(const auto& action:Settings::invocableActionNames()){
  const auto saved=requested_.value(action);const auto text=recording_?QString{}:saved;
  const auto gtk=text.isEmpty()?QString{}:accelerator(text);QJsonObject entry{{"requested",saved},{"bound",false}};
  if(recording_)entry["state"]="recording";
  else if(text.isEmpty())entry["error"]="disabled";else if(gtk.isEmpty())entry["error"]="shortcut_unsupported";else if(!ready_)entry["error"]="helper_unavailable";
  bindings_[action]=entry;entries[action]=QJsonObject{{"requested",text},{"accelerator",gtk}};
 }
 if(!ready_){emit capabilitiesChanged();if(reply)reply({"helper_unavailable","put.gd's hotkeys are unavailable on GNOME.",{}});return;}
 call("Bind",{json(entries)},[this,generation,reply=std::move(reply)](Error error,QJsonObject result){
  if(generation!=bindingGeneration_){if(reply)reply({"shortcut_recording_interrupted","Hotkey configuration changed. Try recording again.",{}});return;}
  if(!ready_)invalidateShortcutBindings();else if(!error){
   bindings_=result["bindings"].toObject();
   for(const auto& action:Settings::invocableActionNames()){
    auto entry=bindings_[action].toObject();entry["requested"]=requested_.value(action);
    if(recording_){entry["state"]="recording";if(!entry["bound"].isBool()||entry["bound"].toBool())error={"shortcut_recording_failed","GNOME did not release all of put.gd's hotkeys.",{}};}
    // An extension loaded before Copy text (#1428, version 23) does not know its key; that is the login step's to fix, not a failed restore.
    const bool textUnknown=action==Settings::textActionName()&&helper_["extensionVersion"].toInt()<23;
    if(!recording_&&reply&&!textUnknown&&!requested_.value(action).isEmpty()&&!entry["bound"].toBool())error={"shortcut_restore_failed","A saved hotkey could not be restored.",{}};
    if(action=="scroll"&&helper_["extensionVersion"].toInt()<22&&!recording_){entry["bound"]=false;entry["error"]="helper_outdated";}
    if(textUnknown&&!recording_){entry["bound"]=false;entry["error"]=requested_.value(action).isEmpty()?"disabled":"helper_outdated";}
    bindings_[action]=entry;
   }
  }else for(const auto& action:Settings::invocableActionNames()){auto entry=bindings_[action].toObject();if(!entry.contains("error"))entry["error"]=error.code;bindings_[action]=entry;}
  emit capabilitiesChanged();if(reply)reply(error);
 });
}
// What capture reports while the helper is away: the setup step the user has to
// take when there is one, otherwise the last discovery error.
Error GnomeAdapter::helperAvailabilityError(bool fallback)const{
 if(setup_.needsUser())return {setup_.code,setup_.message,setup_.details};
 if(fallback&&helperError_.code.isEmpty())return {"helper_unavailable","GNOME capture needs the put.gd Shell extension.",{}};
 return helperError_;
}
gnome::SetupStatus GnomeAdapter::currentSetup()const{
 // The first run hasn't reported yet, so a logout prompt could be the wrong answer.
 if(setupRunning_&&setup_.state=="checking"&&outdated())return {"checking",{},QStringLiteral("Checking put.gd's GNOME extension."),{}};
 return gnome::reportedSetup(setup_,ready_,helper_["extensionVersion"].toInt(),gnome::extensionVersion);
}
void GnomeAdapter::emitSetupNotice(const gnome::SetupStatus& status){
 if(!status.needsUser()||setupNotified_.contains(status.code))return;
 setupNotified_.append(status.code);
 emit setupNotice(status.code,status.message);
}
void GnomeAdapter::evaluateSetup(bool force){
 if(!ownsName_||stopping_)return;
 if(!force&&setupCheckedForOwner_)return;
 // A request during a run (a recheck, or a new Shell) needs a run of its own.
 if(setupRunning_){setupAgain_=true;return;}
 setupCheckedForOwner_=true;
 setupRunning_=true;
 auto result=std::make_shared<gnome::SetupStatus>();
 // Loaded and current with no owned copy to retire: nothing to do. The check needs
 // the session's data directory, which only the worker may look up.
 const bool idle=ready_&&!outdated()&&!force;
 auto* worker=QThread::create([idle,result]{
  const auto inputs=gnome::defaultSetupInputs();
  if(idle&&!gnome::ownedCopyMarkerExists(inputs)){*result={"ready",{},{},{}};return;}
  *result=gnome::runExtensionSetup(inputs);
 });
 workers_.append(worker);
 connect(worker,&QThread::finished,this,[this,worker,result]{
  workers_.removeOne(worker);worker->deleteLater();setupRunning_=false;
  if(stopping_)return;
  const bool changed=setup_.state!=result->state||setup_.code!=result->code||setup_.message!=result->message;
  setup_=*result;
  if(changed)emit capabilitiesChanged();
  emitSetupNotice(currentSetup());
  // Waiters ask for the state after their request, so a queued run answers them.
  if(setupAgain_){setupAgain_=false;evaluateSetup(true);return;}
  auto waiters=std::move(setupWaiters_);setupWaiters_.clear();
  for(auto& waiter:waiters)waiter(desktopSetup());
 });
 worker->start();
}
QJsonObject GnomeAdapter::desktopSetup()const{return currentSetup().toJson();}
void GnomeAdapter::recheckDesktopSetup(std::function<void(QJsonObject)> reply){
 setupWaiters_.append(std::move(reply));
 if(!ownsName_||stopping_){auto waiters=std::move(setupWaiters_);setupWaiters_.clear();for(auto& waiter:waiters)waiter(desktopSetup());return;}
 evaluateSetup(true);
}
}
