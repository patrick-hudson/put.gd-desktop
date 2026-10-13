#include "KdeShortcuts.h"
#include "KdeShortcutNormalization.h"
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusObjectPath>
#include <QJsonArray>
#include <memory>
namespace putgd {
QDBusArgument& operator<<(QDBusArgument& a,const KdeKeySequence& s){a.beginStructure();a<<s.keys;a.endStructure();return a;}
const QDBusArgument& operator>>(const QDBusArgument& a,KdeKeySequence& s){s.keys.clear();a.beginStructure();a.beginArray();while(!a.atEnd()){int key;a>>key;if(s.keys.size()<4)s.keys.append(key);}a.endArray();a.endStructure();while(s.keys.size()<4)s.keys.append(0);return a;}
namespace {
const QString service="org.kde.kglobalaccel",path="/kglobalaccel",iface="org.kde.KGlobalAccel",component="putgd";
const QString layoutService="org.kde.keyboard",layoutPath="/Layouts",layoutInterface="org.kde.KeyboardLayouts";
QStringList actionId(const QString& action){return {component,action,"put.gd",action};}
KdeKeySequence wire(const QKeySequence& seq){KdeKeySequence out;for(int i=0;i<4;++i)out.keys.append(i<seq.count()?seq[i].toCombined():0);return out;}
QString text(const KdeKeySequence& s){int keys[4]={};for(int i=0;i<std::min(4,int(s.keys.size()));++i)keys[i]=s.keys[i];return QKeySequence(keys[0],keys[1],keys[2],keys[3]).toString(QKeySequence::PortableText);}
QDBusPendingCall call(const QString& method,const QVariantList& args={}){auto m=QDBusMessage::createMethodCall(service,path,iface,method);m.setArguments(args);m.setAutoStartService(false);return QDBusConnection::sessionBus().asyncCall(m,3000);}
}
KdeShortcuts::KdeShortcuts(QObject* parent):QObject(parent),watcher_(service,QDBusConnection::sessionBus(),QDBusServiceWatcher::WatchForOwnerChange,this),layoutWatcher_(layoutService,QDBusConnection::sessionBus(),QDBusServiceWatcher::WatchForOwnerChange,this){
 layoutRetry_.setSingleShot(true);layoutRetry_.setInterval(3000);connect(&layoutRetry_,&QTimer::timeout,this,&KdeShortcuts::queryLayout);
 qDBusRegisterMetaType<KdeKeySequence>();qDBusRegisterMetaType<QList<KdeKeySequence>>();
 QDBusConnection::sessionBus().connect(service,path,iface,"yourShortcutsChanged",this,SLOT(shortcutsChanged(QStringList,QList<putgd::KdeKeySequence>)));
 QDBusConnection::sessionBus().connect(layoutService,layoutPath,layoutInterface,"layoutChanged",this,SLOT(layoutChanged(uint)));
 QDBusConnection::sessionBus().connect(layoutService,layoutPath,layoutInterface,"layoutListChanged",this,SLOT(layoutsChanged()));
 connect(&layoutWatcher_,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{refreshLayout();});
 connect(&watcher_,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{available_=false;apiReady_=false;if(!componentPath_.isEmpty())QDBusConnection::sessionBus().disconnect(service,componentPath_,"org.kde.kglobalaccel.Component","globalShortcutPressed",this,SLOT(pressed(QString,QString,qlonglong)));componentPath_.clear();if(recording_)setRecording(true,[](Error){});else bind(requested_);});
}
KdeShortcuts::~KdeShortcuts(){if(keymap_)xkb_keymap_unref(keymap_);for(auto it=requested_.cbegin();it!=requested_.cend();++it)call("setInactive",{actionId(it.key())});}
void KdeShortcuts::setKeyboardMap(xkb_keymap* map){
 if(map==keymap_)return;
 if(map)xkb_keymap_ref(map);
 if(keymap_)xkb_keymap_unref(keymap_);
 keymap_=map;refreshLayout();
}
void KdeShortcuts::layoutChanged(uint index){
 layoutRetry_.stop();layoutRetryMs_=3000;++layoutGeneration_;
 if(keymap_&&index<xkb_keymap_num_layouts(keymap_)){applyLayout(index);return;}
 refreshLayout();
}
void KdeShortcuts::layoutsChanged(){refreshLayout();}
void KdeShortcuts::refreshLayout(){
 layoutRetry_.stop();layoutRetryMs_=3000;++layoutGeneration_;
 // A sole compositor group is unambiguous, independent of its language or
 // whether Plasma provides a layout-switching D-Bus service.
 if(keymap_&&xkb_keymap_num_layouts(keymap_)==1){applyLayout(0);return;}
 layout_=XKB_LAYOUT_INVALID;bind(requested_);
 if(keymap_)queryLayout();
}
void KdeShortcuts::queryLayout(){
 if(!keymap_)return;
 const auto generation=layoutGeneration_;
 auto message=QDBusMessage::createMethodCall(layoutService,layoutPath,layoutInterface,"getLayout");
 message.setAutoStartService(false);
 auto* pending=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,3000),this);
 connect(pending,&QDBusPendingCallWatcher::finished,this,[this,pending,generation]{
  const QDBusPendingReply<uint> result=*pending;pending->deleteLater();
  if(generation!=layoutGeneration_||!keymap_)return;
  if(result.isError()||result.value()>=xkb_keymap_num_layouts(keymap_)){
   // Retrying discovery does not repeat teardown or diagnostics publication.
   layoutRetry_.start(layoutRetryMs_);layoutRetryMs_=std::min(60000,layoutRetryMs_*2);return;
  }
  layoutRetry_.stop();layoutRetryMs_=3000;applyLayout(result.value());
 });
}
void KdeShortcuts::applyLayout(xkb_layout_index_t layout){
 layout_=layout;
 if(recording_){bind(requested_);return;}
 if(!apiReady_){bind(requested_);return;}
 const auto generation=++generation_;available_=true;
 for(auto it=requested_.cbegin();it!=requested_.cend();++it){
  const auto normalized=normalizeKdeShortcut(QKeySequence::fromString(it.value(),QKeySequence::PortableText),keymap_,layout_);
  auto item=status_.value(it.key()).toObject();const auto state=item["state"].toString();
  if(normalized.error.isEmpty()&&effective_.contains(it.key())&&effective_[it.key()]==normalized.sequence&&
     (state=="bound"||state=="unbound"||state=="shortcut_conflict")){
   item["layout"]=int(layout_);status_[it.key()]=item;continue;
  }
  // Keep unaffected confirmed registrations live; only changed or incomplete
  // registrations need a new request. Generation guards fence old replies.
  call("setInactive",{actionId(it.key())});effective_.remove(it.key());
  status_[it.key()]=QJsonObject{{"requested",it.value()},{"state","pending"},{"bound",false}};
  bindOne(it.key(),it.value(),generation);
 }
 // Preserved bindings still need a live event subscription. A layout change
 // invalidates any older pending component lookup as well as setter replies.
 if(componentPath_.isEmpty())connectComponent(generation);
 emit changed();
}
void KdeShortcuts::setRecording(bool active,std::function<void(Error)> reply){
 if(!active){recording_=false;bind(requested_,std::move(reply));return;}
 recording_=true;const auto generation=++generation_;
 for(auto it=requested_.cbegin();it!=requested_.cend();++it)status_[it.key()]=QJsonObject{{"requested",it.value()},{"bound",false},{"state","recording"}};
 emit changed();
 if(requested_.isEmpty()){reply({});return;}
 auto remaining=std::make_shared<int>(requested_.size());auto error=std::make_shared<Error>();
 for(auto it=requested_.cbegin();it!=requested_.cend();++it){
  auto* pending=new QDBusPendingCallWatcher(call("setInactive",{actionId(it.key())}),this);
  connect(pending,&QDBusPendingCallWatcher::finished,this,[this,pending,generation,remaining,error,reply]{
   QDBusPendingReply<> result=*pending;pending->deleteLater();
   if(generation!=generation_)*error={"shortcut_recording_interrupted","Global shortcut configuration changed. Try recording again.",{}};
   else if(result.isError())*error={"shortcut_recording_failed","KDE did not acknowledge pausing global shortcuts.",{}};
   if(--*remaining==0)reply(*error);
  });
 }
}
void KdeShortcuts::bind(const QHash<QString,QString>& desired,std::function<void(Error)> reply){
 if(recording_){requested_=desired;status_={};for(auto it=desired.cbegin();it!=desired.cend();++it)status_[it.key()]=QJsonObject{{"requested",it.value()},{"bound",false},{"state","recording"}};emit changed();if(reply)reply({});return;}
 ++generation_;const auto generation=generation_;for(auto it=requested_.cbegin();it!=requested_.cend();++it)call("setInactive",{actionId(it.key())});requested_=desired;status_={};effective_.clear();available_=false;
 for(auto it=desired.cbegin();it!=desired.cend();++it)status_[it.key()]=QJsonObject{{"requested",it.value()},{"state","pending"},{"bound",false}};
 if(!keymap_||layout_==XKB_LAYOUT_INVALID){for(auto it=desired.cbegin();it!=desired.cend();++it)status_[it.key()]=QJsonObject{{"requested",it.value()},{"state","shortcut_unavailable"},{"bound",false},{"error","Waiting for the compositor keyboard map and current layout"}};emit changed();if(reply)reply({"shortcut_unavailable","Waiting for the compositor keyboard map and current layout.",{}});return;}
 auto m=QDBusMessage::createMethodCall(service,path,"org.freedesktop.DBus.Introspectable","Introspect");m.setAutoStartService(false);auto* pending=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m,3000),this);
 connect(pending,&QDBusPendingCallWatcher::finished,this,[this,pending,generation,complete=std::move(reply)]{QDBusPendingReply<QString> reply=*pending;pending->deleteLater();if(generation!=generation_){if(complete)complete({"shortcut_recording_interrupted","Global shortcut configuration changed.",{}});return;}
  available_=!reply.isError()&&reply.value().contains("name=\"setShortcutKeys\"");apiReady_=available_;
  if(!available_){for(auto it=requested_.cbegin();it!=requested_.cend();++it)status_[it.key()]=QJsonObject{{"requested",it.value()},{"state","shortcut_unavailable"},{"bound",false},{"error","Current KGlobalAccel shortcut API is unavailable"}};emit changed();if(complete)complete({"shortcut_unavailable","KDE global shortcuts are unavailable.",{}});return;}
  if(requested_.isEmpty()){if(complete)complete({});}
  auto remaining=std::make_shared<int>(requested_.size());auto error=std::make_shared<Error>();
  for(auto it=requested_.cbegin();it!=requested_.cend();++it)bindOne(it.key(),it.value(),generation,[remaining,error,complete](Error result){if(result)*error=result;if(--*remaining==0&&complete)complete(*error);});
  emit changed();
 });emit changed();
}
void KdeShortcuts::bindOne(QString action,QString requested,quint64 generation,std::function<void(Error)> complete){
 const QKeySequence configured=QKeySequence::fromString(requested,QKeySequence::PortableText);
 const auto normalized=normalizeKdeShortcut(configured,keymap_,layout_);
 const auto seq=normalized.sequence;
 const bool invalid=!requested.isEmpty()&&(configured.isEmpty()||configured[0].key()==Qt::Key_unknown||configured[0].key()==0);
 if(invalid||!normalized.error.isEmpty()){
  const auto error=invalid?QString("shortcut_invalid"):normalized.error;
  const auto state=error=="shortcut_ambiguous"?QString("shortcut_unmapped"):error;
  effective_.remove(action);status_[action]=QJsonObject{{"requested",requested},{"bound",false},{"state",state},{"error",error}};emit changed();if(complete)complete({error,"A saved global shortcut could not be restored.",{}});return;
 }
 effective_[action]=seq;
 auto finish=[this,action,requested,seq,complete](const QString& state,const QList<KdeKeySequence>& keys={}){QJsonArray actual;for(const auto& key:keys)actual.append(text(key));status_[action]=QJsonObject{{"requested",requested},{"effective",seq.toString(QKeySequence::PortableText)},{"layout",int(layout_)},{"actual",actual},{"bound",state=="bound"},{"state",state}};emit changed();if(complete)complete(state=="shortcut_unavailable"||state=="shortcut_conflict"?Error{state,"A saved global shortcut could not be restored.",{}}:Error{});};
 auto set=[this,action,seq,generation,finish,complete]{auto* reg=new QDBusPendingCallWatcher(call("doRegister",{actionId(action)}),this);
  connect(reg,&QDBusPendingCallWatcher::finished,this,[this,reg,action,seq,generation,finish,complete]{QDBusPendingReply<> r=*reg;reg->deleteLater();if(generation!=generation_){if(complete)complete({"shortcut_recording_interrupted","Global shortcut configuration changed.",{}});return;}if(r.isError()){finish("shortcut_unavailable");return;}
   const auto desired=seq.isEmpty()?QList<KdeKeySequence>{}:QList<KdeKeySequence>{wire(seq)};
   auto* setter=new QDBusPendingCallWatcher(call("setShortcutKeys",{actionId(action),QVariant::fromValue(desired),uint(6)}),this);
   connect(setter,&QDBusPendingCallWatcher::finished,this,[this,setter,desired,generation,finish,complete]{QDBusPendingReply<QList<KdeKeySequence>> result=*setter;setter->deleteLater();if(generation!=generation_){if(complete)complete({"shortcut_recording_interrupted","Global shortcut configuration changed.",{}});return;}if(result.isError()){finish("shortcut_unavailable");return;}
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
void KdeShortcuts::pressed(const QString& c,const QString& action,qlonglong){if(!recording_&&c==component&&requested_.contains(action)&&status_.value(action).toObject().value("bound").toBool())emit activated(action);}
void KdeShortcuts::shortcutsChanged(const QStringList& id,const QList<KdeKeySequence>& keys){if(recording_||id.size()<2||id[0]!=component||!requested_.contains(id[1]))return;QJsonArray actual;for(const auto& key:keys)actual.append(text(key));auto item=status_.value(id[1]).toObject();item["actual"]=actual;const auto expected=effective_.value(id[1]);const bool matches=!expected.isEmpty()&&keys.size()==1&&keys.first().keys==wire(expected).keys;item["bound"]=matches;item["state"]=keys.isEmpty()?"unbound":matches?"bound":"shortcut_conflict";status_[id[1]]=item;emit changed();}
}
