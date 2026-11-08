#include "BudgieShortcuts.h"
#include "LabwcBridgeBackend.h"
#include "platform/wayland/PortalGlobalShortcuts.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QJsonArray>

namespace putgd {
namespace {
const QString service=QStringLiteral("org.putgd.App.Budgie");
const QString objectPath=QStringLiteral("/org/putgd/App");
}
class BudgieTriggerAdaptor final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.putgd.App.Budgie")
public:
 explicit BudgieTriggerAdaptor(BudgieShortcuts* owner):QObject(owner),owner_(owner){}
public slots:
 Q_SCRIPTABLE void Trigger(const QString& action){
  if(!owner_->trigger(action)&&calledFromDBus())sendErrorReply(QStringLiteral("org.putgd.App.Budgie.Error.NotBound"),QStringLiteral("No put.gd shortcut is bound for this action"));
 }
private:
 BudgieShortcuts* owner_;
};
BudgieShortcuts::BudgieShortcuts(QObject* parent):QObject(parent){
 auto bus=QDBusConnection::sessionBus();
 if(!bus.isConnected())serviceError_="The session bus is unavailable";
 else{
  adaptor_=new BudgieTriggerAdaptor(this);
  if(!bus.registerObject(objectPath,adaptor_,QDBusConnection::ExportScriptableSlots))serviceError_="Cannot export "+objectPath+" on the session bus";
  else if(!bus.registerService(service))serviceError_=service+" is already owned on the session bus: "+bus.lastError().message();
  else registered_=true;
 }
}
BudgieShortcuts::~BudgieShortcuts(){
 if(registered_){auto bus=QDBusConnection::sessionBus();bus.unregisterService(service);bus.unregisterObject(objectPath);}
}
void BudgieShortcuts::use(ShortcutBackend* backend){
 backend_=backend;
 connect(backend,&ShortcutBackend::changed,this,&BudgieShortcuts::changed);
 connect(backend,&ShortcutBackend::activated,this,[this](const QString& action){trigger(action);});
}
void BudgieShortcuts::choose(){
 if(chosen_||choosing_)return;
 choosing_=true;
 LabwcBridgeBackend::probe([this](bool bridge,QString reason){
  if(bridge){use(new LabwcBridgeBackend(this));choosing_=false;chosen_=true;backend_->bind(requested_);if(recording_)backend_->setPaused(true,{});emit changed();return;}
  backendError_=reason;
  PortalGlobalShortcuts::probe([this,reason](bool portal,QString portalReason){
   choosing_=false;chosen_=true;
   if(portal){use(new PortalGlobalShortcuts(this));backend_->bind(requested_);if(recording_)backend_->setPaused(true,{});}
   else backendError_="No shortcut backend: "+reason+"; "+portalReason;
   emit changed();
  });
 });
}
void BudgieShortcuts::bind(const QHash<QString,QString>& desired){
 requested_=desired;
 if(!chosen_){choose();emit changed();return;}
 if(backend_)backend_->bind(desired);
 emit changed();
}
void BudgieShortcuts::setRecording(bool active,std::function<void(Error)> reply){
 recording_=active;
 if(backend_){backend_->setPaused(active,std::move(reply));return;}
 if(reply)reply({});
}
bool BudgieShortcuts::trigger(const QString& action){
 // Only an action put.gd bound fires; the recorder's keys never do.
 if(recording_||!requested_.contains(action)||requested_.value(action).isEmpty())return false;
 emit activated(action);return true;
}
void BudgieShortcuts::clear(){if(backend_)backend_->clear();}
QJsonObject BudgieShortcuts::capabilities()const{
 QJsonObject bindings;
 if(backend_)bindings=backend_->status();
 else for(auto it=requested_.cbegin();it!=requested_.cend();++it){
  QJsonObject entry{{"requested",it.value()},{"bound",false},{"actual",QJsonArray{}}};
  if(choosing_||!chosen_)entry["state"]="pending";else{entry["state"]="shortcut_unavailable";entry["error"]=backendError_;}
  bindings[it.key()]=entry;
 }
 QJsonObject out{{"bindings",bindings},{"service",service}};
 const bool available=registered_&&backend_&&backend_->available();
 // Until a backend is chosen Settings says it is waiting, not unavailable.
 if(chosen_||!registered_)out["available"]=available;
 if(backend_)out["backend"]=backend_->name();else out["backend"]=chosen_?QStringLiteral("none"):QStringLiteral("pending");
 if(!registered_)out["error"]=serviceError_;
 else if(!backend_&&chosen_)out["error"]=backendError_;
 if(!registered_)for(auto it=bindings.begin();it!=bindings.end();++it){auto entry=it.value().toObject();entry["bound"]=false;entry["state"]="shortcut_unavailable";entry["error"]=serviceError_;it.value()=entry;}
 out["bindings"]=bindings;
 return out;
}
}
#include "BudgieShortcuts.moc"
