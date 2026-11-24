#include "PortalGlobalShortcuts.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonArray>
#include <QKeySequence>
#include <QPointer>
#include <QUuid>

namespace putgd {
namespace {
const QString portal=QStringLiteral("org.freedesktop.portal.Desktop");
const QString portalPath=QStringLiteral("/org/freedesktop/portal/desktop");
const QString interface=QStringLiteral("org.freedesktop.portal.GlobalShortcuts");
const QHash<QString,QString> descriptions{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"monitor","Capture monitor"},{"repeat","Repeat last region"},{"edit","Capture region & edit"},{"scroll","Capture scrolling region"},{"text","Copy text from region"},{"record","Record screen"},{"clipboard","Upload clipboard"}};
struct Shortcut {QString id;QVariantMap properties;};
QDBusArgument& operator<<(QDBusArgument& argument,const Shortcut& shortcut){argument.beginStructure();argument<<shortcut.id<<shortcut.properties;argument.endStructure();return argument;}
const QDBusArgument& operator>>(const QDBusArgument& argument,Shortcut& shortcut){argument.beginStructure();argument>>shortcut.id>>shortcut.properties;argument.endStructure();return argument;}
QString token(){return QStringLiteral("putgd")+QUuid::createUuid().toString(QUuid::Id128).left(12);}
QString requestPath(const QString& handle){
 auto sender=QDBusConnection::sessionBus().baseService();sender.remove(0,1);sender.replace('.','_');
 return portalPath+"/request/"+sender+'/'+handle;
}
QList<Shortcut> shortcutsFrom(const QVariant& value){
 QList<Shortcut> out;
 if(value.canConvert<QDBusArgument>()){const auto argument=value.value<QDBusArgument>();argument.beginArray();while(!argument.atEnd()){Shortcut shortcut;argument>>shortcut;out.append(shortcut);}argument.endArray();}
 return out;
}
}
}
Q_DECLARE_METATYPE(putgd::Shortcut)
namespace putgd {
QString PortalGlobalShortcuts::xdgTrigger(const QString& portable){
 const QKeySequence sequence=QKeySequence::fromString(portable,QKeySequence::PortableText);
 if(sequence.count()!=1)return {};
 const auto combination=sequence[0];QStringList parts;const auto modifiers=combination.keyboardModifiers();
 if(modifiers&Qt::ControlModifier)parts<<"CTRL";
 if(modifiers&Qt::AltModifier)parts<<"ALT";
 if(modifiers&Qt::ShiftModifier)parts<<"SHIFT";
 if(modifiers&Qt::MetaModifier)parts<<"LOGO";
 auto key=QKeySequence(combination.key()).toString(QKeySequence::PortableText);
 if(key.size()==1)key=key.toLower();
 if(key.isEmpty())return {};
 parts<<key;return parts.join('+');
}
void PortalGlobalShortcuts::probe(std::function<void(bool,QString)> reply){
 auto message=QDBusMessage::createMethodCall(portal,portalPath,"org.freedesktop.DBus.Properties","Get");message.setArguments({interface,QStringLiteral("version")});
 auto* watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,3000));
 QObject::connect(watcher,&QDBusPendingCallWatcher::finished,watcher,[watcher,reply]{
  QDBusPendingReply<QVariant> result=*watcher;watcher->deleteLater();
  if(result.isError())reply(false,"The desktop portal offers no GlobalShortcuts interface");else reply(true,{});
 });
}
PortalGlobalShortcuts::PortalGlobalShortcuts(QObject* parent):ShortcutBackend(parent){
 qDBusRegisterMetaType<Shortcut>();qDBusRegisterMetaType<QList<Shortcut>>();
 auto bus=QDBusConnection::sessionBus();
 bus.connect(portal,portalPath,interface,"Activated",this,SLOT(activatedSignal(QDBusObjectPath,QString,qulonglong,QVariantMap)));
 bus.connect(portal,portalPath,interface,"ShortcutsChanged",this,SLOT(shortcutsChanged(QDBusObjectPath,QDBusArgument)));
}
PortalGlobalShortcuts::~PortalGlobalShortcuts(){close();}
void PortalGlobalShortcuts::request(const QString& method,const QList<QVariant>& arguments,const QString& handle){
 auto bus=QDBusConnection::sessionBus();
 if(!pendingRequest_.isEmpty())bus.disconnect(portal,pendingRequest_,"org.freedesktop.portal.Request","Response",this,SLOT(response(uint,QVariantMap)));
 pendingRequest_=requestPath(handle);
 // Subscribe before calling, so a fast Response is never missed.
 bus.connect(portal,pendingRequest_,"org.freedesktop.portal.Request","Response",this,SLOT(response(uint,QVariantMap)));
 auto message=QDBusMessage::createMethodCall(portal,portalPath,interface,method);message.setArguments(arguments);
 auto* watcher=new QDBusPendingCallWatcher(bus.asyncCall(message,10000),this);const auto generation=generation_;
 connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher,generation,method]{
  QDBusPendingReply<QDBusObjectPath> result=*watcher;watcher->deleteLater();
  if(generation!=generation_)return;
  // No Response follows a failed call: end the stage so status() shows the error and a re-bind retries.
  if(result.isError()){error_=method+" failed: "+result.error().message();if(method=="CreateSession")available_=false;stage_.clear();bound_=false;emit changed();}
 });
}
void PortalGlobalShortcuts::open(){
 if(!session_.isEmpty()||paused_)return;
 bool any=false;for(const auto& value:std::as_const(requested_))any|=!value.isEmpty();
 if(!any){emit changed();return;}
 stage_="create";bound_=false;triggers_.clear();
 // An unsandboxed caller names its app ID first, or xdg-desktop-portal-hyprland
 // refuses with "An app id is required". Older portals lack the registry; the
 // call is harmless there. putgd.desktop is the installed entry.
 if(!registered_){
  registered_=true;
  auto message=QDBusMessage::createMethodCall(portal,portalPath,QStringLiteral("org.freedesktop.host.portal.Registry"),QStringLiteral("Register"));
  message.setArguments({QStringLiteral("putgd"),QVariantMap{}});
  QDBusConnection::sessionBus().call(message,QDBus::Block,2000);
 }
 // The request path's last segment is the handle token sent with the call.
 const auto handle=token();
 request("CreateSession",{QVariantMap{{"handle_token",handle},{"session_handle_token",token()}}},handle);
 emit changed();
}
void PortalGlobalShortcuts::close(std::function<void()> closed){
 ++generation_;
 if(!session_.isEmpty()){
  auto message=QDBusMessage::createMethodCall(portal,session_,"org.freedesktop.portal.Session","Close");
  auto* watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,5000),this);
  // The portal keeps the session's keys until Close returns; a pause waits for it.
  connect(watcher,&QDBusPendingCallWatcher::finished,this,[watcher,closed]{watcher->deleteLater();if(closed)closed();});
  session_.clear();
 }else if(closed)closed();
 bound_=false;stage_.clear();
}
void PortalGlobalShortcuts::response(uint code,const QVariantMap& results){
 if(stage_=="create"){
  if(code!=0){error_="The portal refused a GlobalShortcuts session";available_=false;emit changed();return;}
  session_=results.value("session_handle").toString();stage_="bind";available_=true;error_.clear();
  QList<Shortcut> shortcuts;for(auto it=requested_.cbegin();it!=requested_.cend();++it)if(!it.value().isEmpty())shortcuts.append({it.key(),QVariantMap{{"description",descriptions.value(it.key(),it.key())},{"preferred_trigger",xdgTrigger(it.value())}}});
  const auto handle=token();
  request("BindShortcuts",{QVariant::fromValue(QDBusObjectPath(session_)),QVariant::fromValue(shortcuts),QString(),QVariantMap{{"handle_token",handle}}},handle);
  return;
 }
 if(stage_=="bind"){
  stage_.clear();
  if(code!=0){error_=code==1?"The hotkeys were declined in the desktop's dialog":"The portal did not bind the hotkeys";emit changed();return;}
  bound_=true;error_.clear();
  for(const auto& shortcut:shortcutsFrom(results.value("shortcuts")))triggers_.insert(shortcut.id,shortcut.properties.value("trigger_description").toString());
  emit changed();
 }
}
void PortalGlobalShortcuts::activatedSignal(const QDBusObjectPath& session,const QString& id,qulonglong,const QVariantMap&){
 if(paused_||session.path()!=session_||!requested_.contains(id))return;
 emit activated(id);
}
void PortalGlobalShortcuts::shortcutsChanged(const QDBusObjectPath& session,const QDBusArgument& argument){
 if(session.path()!=session_)return;
 triggers_.clear();argument.beginArray();while(!argument.atEnd()){Shortcut shortcut;argument>>shortcut;triggers_.insert(shortcut.id,shortcut.properties.value("trigger_description").toString());}argument.endArray();
 emit changed();
}
void PortalGlobalShortcuts::bind(const QHash<QString,QString>& desired){
 if(desired==requested_&&(bound_||!stage_.isEmpty()))return;
 requested_=desired;close();open();
}
QJsonObject PortalGlobalShortcuts::status()const{
 QJsonObject out;
 for(auto it=requested_.cbegin();it!=requested_.cend();++it){
  QJsonObject entry{{"requested",it.value()},{"backend","portal-global-shortcuts"},{"bound",false}};
  if(!it.value().isEmpty())entry["effective"]=QKeySequence::fromString(it.value(),QKeySequence::PortableText).toString(QKeySequence::PortableText);
  QJsonArray actual;if(triggers_.contains(it.key())&&!triggers_.value(it.key()).isEmpty())actual.append(triggers_.value(it.key()));entry["actual"]=actual;
  if(!available_){entry["state"]="shortcut_unavailable";entry["error"]=error_;}
  else if(paused_)entry["state"]="recording";
  else if(it.value().isEmpty())entry["state"]="unbound";
  else if(!stage_.isEmpty())entry["state"]="pending";
  else if(bound_&&triggers_.contains(it.key())&&triggers_.value(it.key()).isEmpty()){entry["state"]="pending";entry["error"]="The desktop registered this hotkey without a key; assign one in the desktop's shortcut settings";}
  else if(bound_&&triggers_.contains(it.key())){entry["state"]="bound";entry["bound"]=true;}
  else{entry["state"]="shortcut_unavailable";entry["error"]=error_.isEmpty()?QStringLiteral("The portal did not report this hotkey"):error_;}
  out[it.key()]=entry;
 }
 return out;
}
void PortalGlobalShortcuts::setPaused(bool paused,std::function<void(Error)> reply){
 if(paused==paused_){if(reply)reply({});return;}
 paused_=paused;
 if(!paused){open();emit changed();if(reply)reply({});return;}
 // Acknowledge only once the portal has let go of the keys, so they reach the recorder.
 close([guard=QPointer<PortalGlobalShortcuts>(this),reply]{if(guard)emit guard->changed();if(reply)reply({});});
 emit changed();
}
}
