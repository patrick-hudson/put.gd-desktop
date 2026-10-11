#include "KdeShortcuts.h"
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusObjectPath>
#include <QJsonArray>
namespace putgd {
QDBusArgument& operator<<(QDBusArgument& a,const KdeKeySequence& s){a.beginStructure();a<<s.keys;a.endStructure();return a;}
const QDBusArgument& operator>>(const QDBusArgument& a,KdeKeySequence& s){s.keys.clear();a.beginStructure();a.beginArray();while(!a.atEnd()){int key;a>>key;if(s.keys.size()<4)s.keys.append(key);}a.endArray();a.endStructure();while(s.keys.size()<4)s.keys.append(0);return a;}
namespace {
const QString service="org.kde.kglobalaccel",path="/kglobalaccel",iface="org.kde.KGlobalAccel",component="putgd";
QStringList actionId(const QString& action){return {component,action,"put.gd",action};}
KdeKeySequence wire(const QKeySequence& seq){KdeKeySequence out;for(int i=0;i<4;++i)out.keys.append(i<seq.count()?seq[i].toCombined():0);return out;}
QString text(const KdeKeySequence& s){int keys[4]={};for(int i=0;i<std::min(4,int(s.keys.size()));++i)keys[i]=s.keys[i];return QKeySequence(keys[0],keys[1],keys[2],keys[3]).toString(QKeySequence::PortableText);}
QDBusPendingCall call(const QString& method,const QVariantList& args={}){auto m=QDBusMessage::createMethodCall(service,path,iface,method);m.setArguments(args);m.setAutoStartService(false);return QDBusConnection::sessionBus().asyncCall(m,3000);}
}
KdeShortcuts::KdeShortcuts(QObject* parent):QObject(parent),watcher_(service,QDBusConnection::sessionBus(),QDBusServiceWatcher::WatchForOwnerChange,this){
 qDBusRegisterMetaType<KdeKeySequence>();qDBusRegisterMetaType<QList<KdeKeySequence>>();
 QDBusConnection::sessionBus().connect(service,path,iface,"yourShortcutsChanged",this,SLOT(shortcutsChanged(QStringList,QList<putgd::KdeKeySequence>)));
 connect(&watcher_,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{available_=false;componentPath_.clear();bind(requested_);});
}
KdeShortcuts::~KdeShortcuts(){for(auto it=requested_.cbegin();it!=requested_.cend();++it)call("setInactive",{actionId(it.key())});}
void KdeShortcuts::bind(const QHash<QString,QString>& desired){
 ++generation_;const auto generation=generation_;for(auto it=requested_.cbegin();it!=requested_.cend();++it)call("setInactive",{actionId(it.key())});requested_=desired;status_={};available_=false;
 for(auto it=desired.cbegin();it!=desired.cend();++it)status_[it.key()]=QJsonObject{{"requested",it.value()},{"state","pending"},{"bound",false}};
 auto m=QDBusMessage::createMethodCall(service,path,"org.freedesktop.DBus.Introspectable","Introspect");m.setAutoStartService(false);auto* pending=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m,3000),this);
 connect(pending,&QDBusPendingCallWatcher::finished,this,[this,pending,generation]{QDBusPendingReply<QString> reply=*pending;pending->deleteLater();if(generation!=generation_)return;
  available_=!reply.isError()&&reply.value().contains("name=\"setShortcutKeys\"");
  if(!available_){for(auto it=requested_.cbegin();it!=requested_.cend();++it)status_[it.key()]=QJsonObject{{"requested",it.value()},{"state","shortcut_unavailable"},{"bound",false},{"error","Current KGlobalAccel shortcut API is unavailable"}};emit changed();return;}
  for(auto it=requested_.cbegin();it!=requested_.cend();++it)bindOne(it.key(),it.value(),generation);
  emit changed();
 });emit changed();
}
void KdeShortcuts::bindOne(QString action,QString requested,quint64 generation){
 const QKeySequence seq=QKeySequence::fromString(requested,QKeySequence::PortableText);
 if(!requested.isEmpty()&&seq.isEmpty()){status_[action]=QJsonObject{{"requested",requested},{"bound",false},{"state","shortcut_invalid"}};emit changed();return;}
 auto finish=[this,action,requested](const QString& state,const QList<KdeKeySequence>& keys={}){QJsonArray actual;for(const auto& key:keys)actual.append(text(key));status_[action]=QJsonObject{{"requested",requested},{"actual",actual},{"bound",!keys.isEmpty()&&!text(keys.first()).isEmpty()},{"state",state}};emit changed();};
 auto set=[this,action,seq,generation,finish]{auto* reg=new QDBusPendingCallWatcher(call("doRegister",{actionId(action)}),this);
  connect(reg,&QDBusPendingCallWatcher::finished,this,[this,reg,action,seq,generation,finish]{QDBusPendingReply<> r=*reg;reg->deleteLater();if(generation!=generation_)return;if(r.isError()){finish("shortcut_unavailable");return;}
   const auto desired=seq.isEmpty()?QList<KdeKeySequence>{}:QList<KdeKeySequence>{wire(seq)};
   auto* setter=new QDBusPendingCallWatcher(call("setShortcutKeys",{actionId(action),QVariant::fromValue(desired),uint(6)}),this);
   connect(setter,&QDBusPendingCallWatcher::finished,this,[this,setter,desired,generation,finish]{QDBusPendingReply<QList<KdeKeySequence>> result=*setter;setter->deleteLater();if(generation!=generation_)return;if(result.isError()){finish("shortcut_unavailable");return;}
    const auto actual=result.value();bool matches=actual.size()==desired.size();for(int i=0;matches&&i<actual.size();++i)matches=actual[i].keys==desired[i].keys;
    finish(desired.isEmpty()?"unbound":matches?"bound":"shortcut_conflict",actual);connectComponent(generation);
   });
  });
 };
 // Availability includes this action's saved keys, even while inactive. The
 // setter excludes its own old keys and rejects keys owned by other actions.
 set();
}
void KdeShortcuts::connectComponent(quint64 generation){
 auto* pending=new QDBusPendingCallWatcher(call("getComponent",{component}),this);connect(pending,&QDBusPendingCallWatcher::finished,this,[this,pending,generation]{QDBusPendingReply<QDBusObjectPath> result=*pending;pending->deleteLater();if(generation!=generation_||result.isError())return;const auto next=result.value().path();if(next==componentPath_)return;
  if(!componentPath_.isEmpty())QDBusConnection::sessionBus().disconnect(service,componentPath_,"org.kde.kglobalaccel.Component","globalShortcutPressed",this,SLOT(pressed(QString,QString,qlonglong)));
  componentPath_=next;QDBusConnection::sessionBus().connect(service,next,"org.kde.kglobalaccel.Component","globalShortcutPressed",this,SLOT(pressed(QString,QString,qlonglong)));
 });
}
void KdeShortcuts::pressed(const QString& c,const QString& action,qlonglong){if(c==component&&requested_.contains(action)&&status_.value(action).toObject().value("bound").toBool())emit activated(action);}
void KdeShortcuts::shortcutsChanged(const QStringList& id,const QList<KdeKeySequence>& keys){if(id.size()<2||id[0]!=component||!requested_.contains(id[1]))return;QJsonArray actual;for(const auto& key:keys)actual.append(text(key));auto item=status_.value(id[1]).toObject();item["actual"]=actual;item["bound"]=!keys.isEmpty()&&!text(keys.first()).isEmpty();item["state"]=keys.isEmpty()?"unbound":"bound";status_[id[1]]=item;emit changed();}
}
