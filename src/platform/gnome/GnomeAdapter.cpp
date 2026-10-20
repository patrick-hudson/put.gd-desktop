#include "GnomeAdapter.h"
#include "GnomeProtocol.h"
#include "platform/NativeSession.h"
#include "core/ImageJob.h"
#include <QThread>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QGuiApplication>
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
QJsonObject parse(const QDBusMessage& message,Error& error){
 if(message.type()==QDBusMessage::ErrorMessage){error={"helper_unavailable","GNOME native helper did not complete the request",{{"dbusError",message.errorName()}}};return {};}
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
 probe_.setInterval(3000);connect(&probe_,&QTimer::timeout,this,[this]{
  if(!ready_)discover();
  // Closing the UI must not leave window hotkeys disabled after a transient
  // clear failure. Retry only while no helper is registered; a newer live PID
  // supersedes this recovery and stays fail-closed until its own registration.
  else if(!uiProcess_&&uiProcessError_)setUiProcess(0);
 });probe_.start();discover();
}
GnomeAdapter::~GnomeAdapter(){
 stopping_=true;probe_.stop();cancelAll("source_unavailable","GNOME adapter closed");
 auto pending=std::move(pending_);pending_.clear();for(auto& reply:pending)reply({"source_unavailable","GNOME adapter closed",{}},{});
 for(auto* worker:workers_){worker->wait();delete worker;}
 if(ownsName_)bus_.unregisterService(client);
}
QJsonObject GnomeAdapter::sync(const QString& method,const QVariantList& args,Error& error,int timeout){
 if(owner_.isEmpty()){error=helperError_.code.isEmpty()?Error{"helper_unavailable","Install and enable putgd@put.gd in GNOME 50",{}}:helperError_;return {};}
 auto message=QDBusMessage::createMethodCall(owner_,path,interface,method);message.setArguments(args);return parse(bus_.call(message,QDBus::Block,timeout),error);
}
void GnomeAdapter::call(const QString& method,const QVariantList& args,Reply reply,int timeout){
 if(owner_.isEmpty()||stopping_){reply({"helper_unavailable","GNOME helper is unavailable",{}},{});return;}
 auto message=QDBusMessage::createMethodCall(owner_,path,interface,method);message.setArguments(args);const auto serial=++callSerial_;pending_.insert(serial,std::move(reply));
 auto* watcher=new QDBusPendingCallWatcher(bus_.asyncCall(message,timeout),this);const auto owner=owner_;
 connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher,serial,owner]{const auto callback=pending_.take(serial);const QDBusMessage message=watcher->reply();watcher->deleteLater();if(!callback)return;Error error;const auto result=parse(message,error);if(owner!=owner_)error={"helper_unavailable","GNOME Shell connection changed",{}};callback(error,result);});
}
void GnomeAdapter::discover(){
 if(!ownsName_||stopping_)return;
 const auto owner=bus_.interface()->serviceOwner(service);if(!owner.isValid()||owner.value().isEmpty()){ready_=false;helperError_={"helper_unavailable","GNOME Shell is unavailable on the session bus",{}};invalidateShortcutBindings();emit capabilitiesChanged();return;}
 if(owner_!=owner.value()){owner_=owner.value();bus_.connect(owner_,path,interface,"Action",this,SLOT(helperAction(QString,QString)));bus_.connect(owner_,path,interface,"ClipboardChanged",this,SLOT(helperClipboardChanged(qulonglong)));bus_.connect(owner_,path,interface,"CapabilitiesChanged",this,SLOT(helperChanged()));}
 call("Hello",{},[this](Error error,QJsonObject hello){
  if(!error&&(hello["version"].toInt()!=gnome::protocolVersion||hello["shellMajor"].toInt()!=50))error={"helper_incompatible","GNOME helper requires protocol 1 and GNOME Shell 50",{}};
  if(error){ready_=false;helperError_=error;invalidateShortcutBindings();emit capabilitiesChanged();return;}
  ready_=true;helperError_={};helper_=hello;setUiProcess(uiProcess_);helperRevision_=quint64(hello["clipboardRevision"].toDouble());++revision_;bindShortcuts(requested_);emit capabilitiesChanged();
 });
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
 }
 emit capabilitiesChanged();
}
void GnomeAdapter::helperChanged(){
 if(!owner_.isEmpty()){bus_.disconnect(owner_,path,interface,"Action",this,SLOT(helperAction(QString,QString)));bus_.disconnect(owner_,path,interface,"ClipboardChanged",this,SLOT(helperClipboardChanged(qulonglong)));bus_.disconnect(owner_,path,interface,"CapabilitiesChanged",this,SLOT(helperChanged()));}
 ready_=false;invalidateShortcutBindings();owner_.clear();helper_={};++revision_;cancelAll("helper_unavailable","GNOME helper disabled or Shell restarted");emit capabilitiesChanged();discover();
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
 if(action==Settings::recordingActionName()){emit actionRequested(action);return;}
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
 auto capture=QJsonObject{{"available",ready_},{"mode","compositor-native"},{"consent","one-time-extension-setup"}};if(!ready_)capture["error"]=helperError_.toJson();
 auto window=capture;if(uiProcessError_){window["available"]=false;window["error"]=uiProcessError_.toJson();}
 auto clipboard=QJsonObject{{"available",ready_},{"types",QJsonArray{"files","image","text"}},{"import","bounded-mutter-selection"},{"result","st-clipboard-only"}};
 if(!ready_)clipboard["error"]=helperError_.toJson();
 return {{"adapter","gnome-wayland"},{"helper",QJsonObject{{"available",ready_},{"uuid","putgd@put.gd"},{"protocol",gnome::protocolVersion},{"shellMajor",helper_["shellMajor"]},{"extensionVersion",helper_["extensionVersion"]},{"packagedExtensionVersion",gnome::extensionVersion},{"outdated",outdated()},{"recordingVersion",helper_["recordingVersion"].toInt()},{"error",helperError_.toJson()}}},{"capture",QJsonObject{{"region",capture},{"window",window},{"desktop",capture}}},{"clipboard",clipboard},{"shortcuts",QJsonObject{{"available",ready_},{"bindings",bindings_}}}};
}
SourceAdapter::PreparedAcquisition GnomeAdapter::prepareAcquisition(const QString& action){
 QPointer<GnomeAdapter> guard(this);const bool hotkey=!hotkeyPrepared_[action].isEmpty();const auto id=hotkey?hotkeyPrepared_[action].takeFirst():uuid();Error error;
 if(!ready_)error=helperError_.code.isEmpty()?Error{"helper_unavailable","Enable putgd@put.gd before sharing",{}}:helperError_;
 else if(action=="window"&&uiProcessError_){error=uiProcessError_;if(hotkey)release(id);}
 else if(!hotkey){const auto result=sync("Prepare",{id,action},error);if(!error&&(result["version"].toInt()!=gnome::protocolVersion||result["requestId"].toString()!=id))error={"bridge_protocol","GNOME preparation identity is invalid",{}};if(!error){prepared_.append(id);QTimer::singleShot(120000,this,[this,id]{if(prepared_.removeOne(id))release(id);});}}
 if(hotkey&&!error)prepared_.append(id);
 return [guard,id,action,error](const ResourceLimits& limits,std::function<void(PayloadResult)> reply){if(error){reply({{},error});return;}if(!guard){reply({{},{"source_unavailable","GNOME adapter closed",{}}});return;}guard->start(id,action,limits,std::move(reply));};
}
void GnomeAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> reply){prepareAcquisition(action)(limits,std::move(reply));}
void GnomeAdapter::start(const QString& id,const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> reply){
 if(!prepared_.removeOne(id)){reply({{},{"bridge_protocol","GNOME preparation expired or was cancelled",{}}});return;}
 auto transfer=std::make_shared<Transfer>();transfer->id=id;transfer->limits=limits;transfer->reply=std::move(reply);transfer->generation=generation_;transfers_[id]=transfer;
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
 QJsonObject bounds{{"imageMimes",imageMimes},{"textBytes",double(limits.clipboardTextBytes)},{"imageBytes",double(limits.imageBytes)},{"imagePixels",double(limits.imagePixels)},{"maxFiles",limits.maxFiles}};
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
 connect(worker,&QThread::finished,this,[this,worker,transfer,result]{workers_.removeOne(worker);worker->deleteLater();if(!transfers_.remove(transfer->id))return;auto reply=std::move(transfer->reply);if(transfer->generation!=generation_)reply({{},{"cancelled","GNOME acquisition cancelled",{}}});else reply(std::move(*result));});worker->start();
}
// Recording protocol 1 adds SelectArea, ShowRecordingFrame and HideRecordingFrame.
bool GnomeAdapter::selectsRegions()const{return ready_&&helper_["recordingVersion"].toInt()>=1;}
// An extension older than the packaged one is still the one loaded at login (#131).
bool GnomeAdapter::outdated()const{return ready_&&helper_["extensionVersion"].toInt()<gnome::extensionVersion;}
Error GnomeAdapter::recordingShellError()const{
 if(outdated())return {"helper_outdated","The put.gd GNOME Shell extension was updated; log out and back in to load the new version",{{"loaded",helper_["extensionVersion"].toInt()},{"packaged",gnome::extensionVersion}}};
 if(!ready_)return {"region_unavailable","Recording a region on GNOME needs the put.gd Shell extension; enable putgd@put.gd",{{"helper",helperError_.toJson()}}};
 return {"region_unavailable","Recording a region on GNOME needs the put.gd Shell extension with recording support",{}};
}
Error GnomeAdapter::regionError()const{return selectsRegions()?Error{}:recordingShellError();}
void GnomeAdapter::selectRegion(RecordingShell::Reply reply){
 if(!selectsRegions()){reply(recordingShellError(),{});return;}
 const auto id=uuid();recordingSelection_=id;
 // The user drags at their own pace; the extension's request deadline (120 s) ends it first.
 call("SelectArea",{id},[this,id,reply](Error error,QJsonObject result){
  if(recordingSelection_==id)recordingSelection_.clear();
  if(!error&&(result["requestId"].toString()!=id||!result["rect"].isArray()||result["rect"].toArray().size()!=4))error={"bridge_protocol","GNOME helper returned an invalid selection",{}};
  reply(error,error?QJsonObject{}:QJsonObject{{"rect",result["rect"]}});
 },125000);
}
void GnomeAdapter::cancelSelection(){if(recordingSelection_.isEmpty())return;const auto id=std::exchange(recordingSelection_,QString{});release(id);}
void GnomeAdapter::showFrame(const QJsonObject& frame,RecordingShell::Reply reply){
 if(!selectsRegions()){reply(outdated()?recordingShellError():Error{"helper_incompatible","Update the GNOME extension to frame recordings",{}},{});return;}
 // The extension waits up to two seconds for the bar window to map before placing it.
 call("ShowRecordingFrame",{json(frame)},std::move(reply),4000);
}
void GnomeAdapter::hideFrame(){if(selectsRegions())call("HideRecordingFrame",{},[](Error,QJsonObject){});}
void GnomeAdapter::release(const QString& id){if(!owner_.isEmpty()&&!stopping_)call("Release",{id},[](Error,QJsonObject){});}
void GnomeAdapter::cancelAll(const QString& code,const QString& message){
 ++generation_;const auto hotkeys=std::move(hotkeyPrepared_);hotkeyPrepared_.clear();for(const auto& tickets:hotkeys)for(const auto& id:tickets)release(id);
 const auto prepared=std::move(prepared_);prepared_.clear();for(const auto& id:prepared)release(id);
 auto transfers=std::move(transfers_);transfers_.clear();for(const auto& transfer:transfers){release(transfer->id);auto reply=std::move(transfer->reply);if(reply)reply({{},{code,message,{}}});}
}
void GnomeAdapter::cancelAcquisition(){cancelAll("cancelled","GNOME acquisition cancelled");}
void GnomeAdapter::copy(const CopyPayload& payload,std::function<void(Error)> reply){
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
 if(!ready_){emit capabilitiesChanged();if(reply)reply({"helper_unavailable","GNOME global shortcuts are unavailable.",{}});return;}
 call("Bind",{json(entries)},[this,generation,reply=std::move(reply)](Error error,QJsonObject result){
  if(generation!=bindingGeneration_){if(reply)reply({"shortcut_recording_interrupted","Global shortcut configuration changed. Try recording again.",{}});return;}
  if(!ready_)invalidateShortcutBindings();else if(!error){
   bindings_=result["bindings"].toObject();
   for(const auto& action:Settings::invocableActionNames()){
    auto entry=bindings_[action].toObject();entry["requested"]=requested_.value(action);
    if(recording_){entry["state"]="recording";if(!entry["bound"].isBool()||entry["bound"].toBool())error={"shortcut_recording_failed","GNOME did not release all global shortcuts.",{}};}
    if(!recording_&&reply&&!requested_.value(action).isEmpty()&&!entry["bound"].toBool())error={"shortcut_restore_failed","A saved global shortcut could not be restored.",{}};
    bindings_[action]=entry;
   }
  }else for(const auto& action:Settings::invocableActionNames()){auto entry=bindings_[action].toObject();if(!entry.contains("error"))entry["error"]=error.code;bindings_[action]=entry;}
  emit capabilitiesChanged();if(reply)reply(error);
 });
}
}
