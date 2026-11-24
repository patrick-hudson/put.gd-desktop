#include "platform/wayland/EventPump.h"
#include "HyprlandShortcuts.h"
#include "HyprlandIpc.h"
#include "hyprland-global-shortcuts-v1-client-protocol.h"
#include <QJsonDocument>
#include <QKeySequence>
#include <QSocketNotifier>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <poll.h>
#include <cerrno>
#include <cstring>

namespace putgd {
namespace {
const QString ownPrefix=QStringLiteral("put.gd: ");
const QHash<QString,QString> labels{{"region","Region capture"},{"window","Window capture"},{"desktop","Desktop capture"},{"monitor","Monitor capture"},{"repeat","Repeat last region"},
 {"edit","Region capture and edit"},{"scroll","Capture scrolling region"},{"text","Copy text from region"},{"record","Record screen"},{"clipboard","Upload clipboard"}};
// Qt keys whose xkb keysym name is not the character itself.
const QHash<int,QString> namedKeys{{Qt::Key_Print,"Print"},{Qt::Key_Escape,"Escape"},{Qt::Key_Tab,"Tab"},{Qt::Key_Backspace,"BackSpace"},
 {Qt::Key_Return,"Return"},{Qt::Key_Enter,"KP_Enter"},{Qt::Key_Insert,"Insert"},{Qt::Key_Delete,"Delete"},{Qt::Key_Pause,"Pause"},
 {Qt::Key_Home,"Home"},{Qt::Key_End,"End"},{Qt::Key_Left,"Left"},{Qt::Key_Up,"Up"},{Qt::Key_Right,"Right"},{Qt::Key_Down,"Down"},
 {Qt::Key_PageUp,"Prior"},{Qt::Key_PageDown,"Next"},{Qt::Key_Space,"space"},{Qt::Key_Menu,"Menu"},{Qt::Key_ScrollLock,"Scroll_Lock"}};
constexpr int debounceMs=300;
constexpr int maximumReloadsPerApply=2;
// Lower-case keysym name, or empty for a name xkb does not know.
QString canonicalKey(const QString& name){
 if(name.isEmpty())return {};
 const auto symbol=xkb_keysym_from_name(name.toUtf8().constData(),XKB_KEYSYM_CASE_INSENSITIVE);
 if(symbol==XKB_KEY_NoSymbol)return {};
 char buffer[64]={};
 if(xkb_keysym_get_name(xkb_keysym_to_lower(symbol),buffer,sizeof buffer)<=0)return {};
 return QString::fromLatin1(buffer).toLower();
}
// The level-one keysym of an xkb keycode in the default keymap, for code: binds.
QString keycodeKey(int keycode){
 static xkb_keymap* keymap=[]{
  auto* context=xkb_context_new(XKB_CONTEXT_NO_FLAGS);if(!context)return static_cast<xkb_keymap*>(nullptr);
  auto* map=xkb_keymap_new_from_names(context,nullptr,XKB_KEYMAP_COMPILE_NO_FLAGS);xkb_context_unref(context);return map;
 }();
 if(!keymap||keycode<=0)return {};
 const xkb_keysym_t* symbols=nullptr;
 if(xkb_keymap_key_get_syms_by_level(keymap,xkb_keycode_t(keycode),0,0,&symbols)<1||!symbols)return {};
 char buffer[64]={};if(xkb_keysym_get_name(xkb_keysym_to_lower(symbols[0]),buffer,sizeof buffer)<=0)return {};
 return QString::fromLatin1(buffer).toLower();
}
QString describeBind(const QJsonObject& bind){
 const auto description=bind["description"].toString();
 if(!description.isEmpty())return description;
 const auto dispatcher=bind["dispatcher"].toString();
 return dispatcher=="__lua"?QStringLiteral("a Lua bind"):dispatcher+' '+bind["arg"].toString();
}
}

// Registration ---------------------------------------------------------------
struct HyprlandShortcuts::Registry:QObject {
 struct Entry {Registry* owner=nullptr;QString action;hyprland_global_shortcut_v1* native=nullptr;};
 HyprlandShortcuts& q;
 wl_display* display=nullptr;wl_registry* registry=nullptr;hyprland_global_shortcuts_manager_v1* manager=nullptr;
 QHash<QString,Entry*> entries;QSocketNotifier* notifier=nullptr;QSocketNotifier* writeNotifier=nullptr;QTimer reconnect;
 QString error=QStringLiteral("The Hyprland shortcut protocol has not connected");bool connected=false,dispatching=false,closing=false,everConnected=false;int failures=0;
 explicit Registry(HyprlandShortcuts& q):q(q){reconnect.setSingleShot(true);connect(&reconnect,&QTimer::timeout,this,[this]{open();});open();}
 ~Registry() override{closing=true;reset();}
 void open(){
  if(closing||display)return;
  display=wl_display_connect(nullptr);
  if(!display){error="Cannot connect to the Wayland compositor for hotkeys";retry();return;}
  registry=wl_display_get_registry(display);
  static const wl_registry_listener listener{[](void* data,wl_registry* r,uint32_t name,const char* interface,uint32_t){
   auto* self=static_cast<Registry*>(data);
   if(std::strcmp(interface,hyprland_global_shortcuts_manager_v1_interface.name)==0&&!self->manager)
    self->manager=static_cast<hyprland_global_shortcuts_manager_v1*>(wl_registry_bind(r,name,&hyprland_global_shortcuts_manager_v1_interface,1));
  },[](void*,wl_registry*,uint32_t){}};
  wl_registry_add_listener(registry,&listener,this);
  // One bounded round trip for the globals; Hyprland answers in microseconds.
  if(!roundTrip()){fail("The compositor did not answer the shortcut protocol query");return;}
  if(!manager){error="The compositor does not offer hyprland-global-shortcuts-v1";reset();retry(30000);q.changed();return;}
  notifier=new QSocketNotifier(wl_display_get_fd(display),QSocketNotifier::Read,this);
  writeNotifier=new QSocketNotifier(wl_display_get_fd(display),QSocketNotifier::Write,this);writeNotifier->setEnabled(false);
  QObject::connect(writeNotifier,&QSocketNotifier::activated,this,[this]{flush();});
  connect(notifier,&QSocketNotifier::activated,this,[this]{events();});
  connected=true;error.clear();failures=0;
  for(auto it=q.requested_.cbegin();it!=q.requested_.cend();++it)if(!it.value().isEmpty())ensure(it.key());
  flush();
  // A reconnect registers the shortcuts again; read j/binds and put back any bind that is gone.
  if(everConnected)q.schedule(0);
  everConnected=true;emit q.changed();
 }
 bool roundTrip(){
  // wl_display_roundtrip with a deadline, before the notifier exists.
  bool done=false;auto* callback=wl_display_sync(display);
  static const wl_callback_listener synced{[](void* data,wl_callback*,uint32_t){*static_cast<bool*>(data)=true;}};
  wl_callback_add_listener(callback,&synced,&done);
  for(int waited=0;!done&&waited<2000;waited+=100){
   bool pending=false;if(!wayland::flush(display,pending))break;
   pollfd ready{wl_display_get_fd(display),short(POLLIN|(pending?POLLOUT:0)),0};
   if(!wayland::pump(display,&ready,1,100))break;
  }
  wl_callback_destroy(callback);return done;
 }
 void ensure(const QString& action){
  if(!manager||entries.contains(action))return;
  auto* entry=new Entry{this,action,nullptr};
  entry->native=hyprland_global_shortcuts_manager_v1_register_shortcut(manager,action.toUtf8().constData(),"putgd",
   description(action).toUtf8().constData(),"");
  static const hyprland_global_shortcut_v1_listener listener{
   [](void* data,hyprland_global_shortcut_v1*,uint32_t,uint32_t,uint32_t){
    auto* entry=static_cast<Entry*>(data);
    // Leave Wayland dispatch before any capture work starts.
    QMetaObject::invokeMethod(&entry->owner->q,[guard=QPointer<HyprlandShortcuts>(&entry->owner->q),action=entry->action]{if(guard&&!guard->paused_&&guard->requested_.value(action).size())emit guard->activated(action);},Qt::QueuedConnection);
   },
   [](void*,hyprland_global_shortcut_v1*,uint32_t,uint32_t,uint32_t){}};
  hyprland_global_shortcut_v1_add_listener(entry->native,&listener,entry);
  entries.insert(action,entry);flush();
 }
 void flush(){if(!display)return;bool pending=false;if(!wayland::flush(display,pending)){lost();return;}if(writeNotifier)writeNotifier->setEnabled(pending);}
 void events(){
  if(!display||dispatching)return;dispatching=true;
  const bool ok=wayland::pump(display);
  dispatching=false;
  if(!ok){lost();return;}
  flush();
 }

 void lost(){
  QString reason=QStringLiteral("The Wayland connection for hotkeys was lost");
  if(display){
   const hyprland_global_shortcuts_manager_v1* source=nullptr;uint32_t id=0;const wl_interface* interface=nullptr;
   const auto code=wl_display_get_protocol_error(display,&interface,&id);Q_UNUSED(source);
   if(interface&&std::strcmp(interface->name,hyprland_global_shortcuts_manager_v1_interface.name)==0&&code==HYPRLAND_GLOBAL_SHORTCUTS_MANAGER_V1_ERROR_ALREADY_TAKEN)
    reason=QStringLiteral("Another client already registered put.gd's hotkeys (putgd:*) in this Hyprland session");
  }
  fail(reason);
 }
 void fail(const QString& reason){
  error=reason;reset();retry();emit q.changed();
 }
 void retry(int delayMs=0){
  if(closing)return;
  ++failures;reconnect.start(delayMs>0?delayMs:std::min(30000,1000*failures));
 }
 void reset(){
  if(writeNotifier){writeNotifier->setEnabled(false);writeNotifier->deleteLater();writeNotifier=nullptr;}
  connected=false;
  if(notifier){notifier->setEnabled(false);notifier->disconnect(this);notifier->deleteLater();notifier=nullptr;}
  for(auto* entry:entries){if(entry->native&&display)hyprland_global_shortcut_v1_destroy(entry->native);delete entry;}entries.clear();
  if(manager){hyprland_global_shortcuts_manager_v1_destroy(manager);manager=nullptr;}
  if(registry){wl_registry_destroy(registry);registry=nullptr;}
  if(display){wl_display_disconnect(display);display=nullptr;}
 }
};

// Keys -----------------------------------------------------------------------
std::optional<HyprlandShortcuts::Chord> HyprlandShortcuts::chordFromPortable(const QString& portableText,QString* error){
 if(error)error->clear();
 const QKeySequence sequence=QKeySequence::fromString(portableText,QKeySequence::PortableText);
 if(sequence.count()!=1){if(error)*error="Hyprland binds hold a single key chord";return {};}
 const auto combination=sequence[0];const int key=combination.key();const auto modifiers=combination.keyboardModifiers();
 // A keypad key would otherwise bind the ordinary key of the same name.
 if(modifiers&Qt::KeypadModifier){if(error)*error="Keypad keys cannot be bound on Hyprland";return {};}
 QString name;
 if(key>=Qt::Key_F1&&key<=Qt::Key_F35)name=QStringLiteral("F%1").arg(key-Qt::Key_F1+1);
 else if(namedKeys.contains(key))name=namedKeys.value(key);
 else if(key>0x20&&key<0x7f){
  const auto symbol=xkb_utf32_to_keysym(uint32_t(QChar(key).toLower().unicode()));char buffer[64]={};
  if(symbol!=XKB_KEY_NoSymbol&&xkb_keysym_get_name(symbol,buffer,sizeof buffer)>0)name=QString::fromLatin1(buffer);
 }
 Chord chord;chord.key=canonicalKey(name);
 if(chord.key.isEmpty()){if(error)*error="This key has no Hyprland key name";return {};}
 if(modifiers&Qt::ControlModifier)chord.modmask|=control;
 if(modifiers&Qt::AltModifier)chord.modmask|=alt;
 if(modifiers&Qt::ShiftModifier)chord.modmask|=shift;
 if(modifiers&Qt::MetaModifier)chord.modmask|=super;
 return chord;
}
std::optional<HyprlandShortcuts::Chord> HyprlandShortcuts::chordFromBind(const QJsonObject& bind){
 // Only plain key binds in the global map can fire with put.gd's.
 if(bind["mouse"].toBool()||bind["catch_all"].toBool()||!bind["submap"].toString().isEmpty())return {};
 Chord chord;chord.modmask=bind["modmask"].toInt();
 const auto key=bind["key"].toString();
 if(!key.isEmpty())chord.key=canonicalKey(key);
 else{
  int keycode=bind["keycode"].toInt();
  // "SUPER + SHIFT + code:10" from the text reply: the last part names the key.
  const auto text=bind["keyText"].toString().section('+',-1).trimmed();
  if(keycode<=0&&text.startsWith(QStringLiteral("code:"),Qt::CaseInsensitive))keycode=text.mid(5).toInt();
  else if(keycode<=0&&!text.isEmpty())chord.key=canonicalKey(text);
  if(chord.key.isEmpty())chord.key=keycodeKey(keycode);
 }
 if(chord.key.isEmpty())return {};
 return chord;
}
QJsonArray HyprlandShortcuts::withTextKeys(QJsonArray binds,const QByteArray& text){
 // Blocks start with an unindented "bind..." line; "key:" is one of its fields.
 QStringList keys;bool inBlock=false;
 for(const auto& raw:text.split('\n')){
  const auto line=QString::fromUtf8(raw);
  if(line.startsWith(QStringLiteral("bind"))){keys.append(QString());inBlock=true;continue;}
  if(inBlock&&line.trimmed().startsWith(QStringLiteral("key:")))keys.last()=line.trimmed().mid(4).trimmed();
 }
 if(keys.size()!=binds.size())return binds;
 for(qsizetype i=0;i<binds.size();++i){
  auto bind=binds[i].toObject();
  if(bind["key"].toString().isEmpty()&&!keys[i].isEmpty()){bind["keyText"]=keys[i];binds[i]=bind;}
 }
 return binds;
}
QString HyprlandShortcuts::hyprlandKeys(const Chord& chord){
 QStringList parts;
 if(chord.modmask&super)parts<<"SUPER";
 if(chord.modmask&control)parts<<"CTRL";
 if(chord.modmask&alt)parts<<"ALT";
 if(chord.modmask&shift)parts<<"SHIFT";
 parts<<chord.key;
 return parts.join(QStringLiteral(" + "));
}
QString HyprlandShortcuts::portable(const Chord& chord){
 QString key;
 const auto symbol=xkb_keysym_from_name(chord.key.toUtf8().constData(),XKB_KEYSYM_CASE_INSENSITIVE);
 for(auto it=namedKeys.cbegin();it!=namedKeys.cend();++it)if(canonicalKey(it.value())==chord.key)key=QKeySequence(it.key()).toString(QKeySequence::PortableText);
 if(key.isEmpty()&&chord.key.size()>1&&chord.key.startsWith('f')){bool ok=false;const int n=chord.key.mid(1).toInt(&ok);if(ok&&n>=1&&n<=35)key=QStringLiteral("F%1").arg(n);}
 if(key.isEmpty()){const auto code=xkb_keysym_to_utf32(symbol);if(code>0x20&&code<0x7f)key=QString(QChar(char16_t(code))).toUpper();}
 if(key.isEmpty())key=chord.key;
 QStringList parts;
 if(chord.modmask&control)parts<<"Ctrl";
 if(chord.modmask&alt)parts<<"Alt";
 if(chord.modmask&shift)parts<<"Shift";
 if(chord.modmask&super)parts<<"Meta";
 parts<<key;
 // Qt's own order and spelling (Meta first), so Settings compares like with like.
 const auto joined=parts.join('+');const auto sequence=QKeySequence::fromString(joined,QKeySequence::PortableText);
 return sequence.isEmpty()?joined:sequence.toString(QKeySequence::PortableText);
}
QString HyprlandShortcuts::description(const QString& action){return ownPrefix+labels.value(action,action);}
bool HyprlandShortcuts::ownDescription(const QString& text){return text.startsWith(ownPrefix);}
QString HyprlandShortcuts::bindScript(const QString& action,const Chord& chord){
 return QStringLiteral("hl.bind(%1, hl.dsp.global(%2), { description = %3 })")
  .arg(HyprlandIpc::luaString(hyprlandKeys(chord)),HyprlandIpc::luaString(globalId(action)),HyprlandIpc::luaString(description(action)));
}
QString HyprlandShortcuts::unbindScript(const Chord& chord){return QStringLiteral("hl.unbind(%1)").arg(HyprlandIpc::luaString(hyprlandKeys(chord)));}
QString HyprlandShortcuts::layerRuleScript(){
 return QStringLiteral("hl.layer_rule({ match = { namespace = \"^putgd-(region-selector|region-catcher|record|capture-countdown)$\" }, no_anim = true })");
}
HyprlandShortcuts::Plan HyprlandShortcuts::plan(const QJsonArray& binds,const QHash<QString,QString>& desired){
 Plan out;
 // put.gd's binds now: description -> chords.
 QHash<QString,QList<Chord>> ours;QList<QPair<Chord,QJsonObject>> others;
 for(const auto& value:binds){
  const auto bind=value.toObject();const auto text=bind["description"].toString();
  if(ownDescription(text)){
   // Even an unreadable put.gd bind is put.gd's to replace.
   const auto chord=chordFromBind(bind);ours[text].append(chord.value_or(Chord{-1,{}}));continue;
  }
  if(const auto chord=chordFromBind(bind))others.append({*chord,bind});
 }
 QSet<QString> wanted;
 for(auto it=desired.cbegin();it!=desired.cend();++it){
  const auto& action=it.key();
  if(it.value().isEmpty())continue;
  QString reason;const auto chord=chordFromPortable(it.value(),&reason);
  if(!chord){out.invalid.insert(action,reason);continue;}
  bool conflict=false;
  for(const auto& [other,bind]:others)if(other==*chord){out.conflicts.insert(action,bind);conflict=true;break;}
  // Two put.gd actions on one key would both fire; the second is a conflict.
  for(auto applied=out.apply.cbegin();!conflict&&applied!=out.apply.cend();++applied)
   if(applied.value()==*chord){out.conflicts.insert(action,QJsonObject{{"description",description(applied.key())}});conflict=true;}
  for(const auto& present:std::as_const(out.present))
   if(!conflict&&chordFromPortable(desired.value(present))==chord){out.conflicts.insert(action,QJsonObject{{"description",description(present)}});conflict=true;}
  if(conflict)continue;
  wanted.insert(description(action));
  const auto existing=ours.value(description(action));
  if(existing.size()==1&&existing.first()==*chord)out.present.insert(action);
  else out.apply.insert(action,*chord);
 }
 // Anything of put.gd's that is not exactly one wanted bind must go first.
 QList<Chord> leaving;
 for(auto it=ours.cbegin();it!=ours.cend();++it)if(!wanted.contains(it.key())||it.value().size()!=1)leaving+=it.value();
 for(auto it=out.apply.cbegin();it!=out.apply.cend();++it)leaving+=ours.value(description(it.key()));
 // hl.unbind takes every bind on the chord in every submap, so anything else on it
 // (or a put.gd bind whose key can't be read) needs the reload.
 const auto heldByOthers=[&](const Chord& chord){
  for(const auto& value:binds){
   const auto bind=value.toObject();
   if(ownDescription(bind["description"].toString()))continue;
   if(bind["modmask"].toInt()==chord.modmask&&canonicalKey(bind["key"].toString())==chord.key)return true;
   if(chordFromBind(bind)==chord)return true;
  }
  return false;
 };
 for(const auto& chord:std::as_const(leaving)){
  if(chord.modmask<0||heldByOthers(chord)){out.reloadFirst=true;out.remove.clear();break;}
  if(!out.remove.contains(chord))out.remove.append(chord);
 }
 if(out.reloadFirst){for(const auto& action:std::as_const(out.present)){QString ignored;out.apply.insert(action,*chordFromPortable(desired.value(action),&ignored));}out.present.clear();}
 return out;
}

// Backend --------------------------------------------------------------------
HyprlandShortcuts::HyprlandShortcuts(HyprlandIpc* ipc,QObject* parent):ShortcutBackend(parent),ipc_(ipc){
 debounce_.setSingleShot(true);connect(&debounce_,&QTimer::timeout,this,&HyprlandShortcuts::apply);
 connect(ipc_,&HyprlandIpc::event,this,[this](const QString& name,const QString&){if(name=="configreloaded")configReloaded();});
 // A reload while the event socket was away went unseen: read j/binds and put back what's gone.
 connect(ipc_,&HyprlandIpc::listeningChanged,this,[this](bool listening){if(listening&&applied_){layerRule_=false;schedule(debounceMs);}});
 ipc_->listen();
 registry_=std::make_unique<Registry>(*this);
}
HyprlandShortcuts::~HyprlandShortcuts()=default;
bool HyprlandShortcuts::available()const{return registry_->connected&&evalError_.isEmpty()&&ipcError_.isEmpty();}
QString HyprlandShortcuts::error()const{
 if(!ipcError_.isEmpty())return ipcError_;
 if(!evalError_.isEmpty())return evalError_;
 if(!registry_->connected)return registry_->error;
 return {};
}
void HyprlandShortcuts::bind(const QHash<QString,QString>& desired){
 requested_=desired;
 for(auto it=desired.cbegin();it!=desired.cend();++it)if(!it.value().isEmpty())registry_->ensure(it.key());
 schedule();emit changed();
}
void HyprlandShortcuts::schedule(int delayMs){
 if(applying_){reapply_=true;return;}
 debounce_.start(delayMs);
}
void HyprlandShortcuts::configReloaded(){
 ++reloadsSeen_;layerRule_=false;
 // Our own binds and rule are gone; put back what is still wanted.
 schedule(debounceMs);
}
void HyprlandShortcuts::ensureLayerRule(std::function<void()> next){
 if(layerRule_){next();return;}
 ipc_->request("eval "+layerRuleScript(),[this,next=std::move(next)](QByteArray reply,Error error){
  if(!error&&HyprlandIpc::okReply(reply))layerRule_=true;
  next();
 });
}
void HyprlandShortcuts::apply(){
 if(applying_){reapply_=true;return;}
 applying_=true;reapply_=false;++applies_;
 ensureLayerRule([this]{
  readBinds([this](QJsonArray binds,Error error){
   if(error){ipcError_=error.message;finish();return;}
   ipcError_.clear();
   applyBinds(std::move(binds),0);
  });
 });
}
void HyprlandShortcuts::readBinds(std::function<void(QJsonArray,Error)> reply){
 ipc_->json("j/binds",[this,reply=std::move(reply)](QJsonDocument document,Error error){
  if(error){reply({},std::move(error));return;}
  auto binds=document.array();
  // Only code: binds need the text reply; skip it when there are none.
  bool unnamed=false;for(const auto& value:std::as_const(binds))if(value.toObject()["key"].toString().isEmpty())unnamed=true;
  if(!unnamed){reply(std::move(binds),{});return;}
  ipc_->request("binds",[binds,reply](QByteArray text,Error error)mutable{
   // Without the text the code: binds stay unchecked; the JSON is still right.
   reply(error?binds:withTextKeys(binds,text),{});
  });
 });
}
void HyprlandShortcuts::reloadThen(std::function<void(Error)> next){
 ++reloadsIssued_;
 ipc_->request("reload",[this,next=std::move(next)](QByteArray reply,Error error){
  if(!error&&!HyprlandIpc::okReply(reply))error={"shortcut_unavailable","Hyprland refused reload: "+QString::fromUtf8(reply.left(200)).trimmed(),{}};
  layerRule_=false;
  next(error);
 });
}
void HyprlandShortcuts::unbindThen(QList<Chord> chords,std::function<void()> next){
 if(chords.isEmpty()){next();return;}
 const auto chord=chords.takeFirst();++unbinds_;
 // A refused or failed unbind shows up when j/binds is read again, and the reload takes over.
 ipc_->request("eval "+unbindScript(chord),[this,chords=std::move(chords),next=std::move(next)](QByteArray,Error)mutable{
  unbindThen(std::move(chords),std::move(next));
 });
}
void HyprlandShortcuts::applyBinds(QJsonArray binds,int reloads){
 auto wanted=paused_?QHash<QString,QString>{}:requested_;
 auto planned=plan(binds,wanted);
 if(!planned.reloadFirst&&!planned.remove.isEmpty()&&reloads==0){
  unbindThen(planned.remove,[this,reloads]{
   readBinds([this,reloads](QJsonArray binds,Error error){
    if(error){ipcError_=error.message;finish();return;}
    applyBinds(std::move(binds),reloads+1);
   });
  });
  return;
 }
 if(planned.reloadFirst||!planned.remove.isEmpty()){
  if(reloads>=maximumReloadsPerApply){ipcError_="put.gd's Hyprland binds did not clear after reload";finish();return;}
  reloadThen([this,reloads](Error error){
   if(error){ipcError_=error.message;finish();return;}
   ensureLayerRule([this,reloads]{
    readBinds([this,reloads](QJsonArray binds,Error error){
     if(error){ipcError_=error.message;finish();return;}
     applyBinds(std::move(binds),reloads+1);
    });
   });
  });
  return;
 }
 conflicts_=planned.conflicts;invalid_=planned.invalid;failed_.clear();
 QList<QPair<QString,Chord>> queue;
 for(auto it=planned.apply.cbegin();it!=planned.apply.cend();++it){registry_->ensure(it.key());queue.append({it.key(),it.value()});}
 std::sort(queue.begin(),queue.end(),[](const auto& a,const auto& b){return a.first<b.first;});
 evalNext(std::move(queue));
}
void HyprlandShortcuts::evalNext(QList<QPair<QString,Chord>> queue){
 if(queue.isEmpty()){readBack();return;}
 const auto [action,chord]=queue.takeFirst();
 ipc_->request("eval "+bindScript(action,chord),[this,action,queue=std::move(queue)](QByteArray reply,Error error)mutable{
  if(error){ipcError_=error.message;finish();return;}
  if(!HyprlandIpc::okReply(reply)){
   const auto text=QString::fromUtf8(reply.left(300)).trimmed();
   // The legacy hyprland.conf parser has no Lua: nothing can be bound at runtime.
   if(text.contains("lua",Qt::CaseInsensitive)||text.contains("legacy",Qt::CaseInsensitive)||text.contains("eval",Qt::CaseInsensitive))
    evalError_="Hyprland did not run put.gd's Lua bind (a session on the legacy hyprland.conf parser has no put.gd hotkeys): "+text;
   else failed_.insert(action,"Hyprland refused the bind: "+text);
  }else evalError_.clear();
  evalNext(std::move(queue));
 });
}
void HyprlandShortcuts::readBack(){
 ipc_->json("j/binds",[this](QJsonDocument document,Error error){
  if(error){ipcError_=error.message;finish();return;}
  bound_.clear();actual_.clear();
  for(const auto& value:document.array()){
   const auto bind=value.toObject();const auto text=bind["description"].toString();
   if(!ownDescription(text))continue;
   for(auto it=requested_.cbegin();it!=requested_.cend();++it)if(description(it.key())==text){
    const auto chord=chordFromBind(bind);
    actual_[it.key()].append(chord?portable(*chord):bind["key"].toString());
    if(!paused_&&chord&&chordFromPortable(it.value())==chord)bound_.insert(it.key());
   }
  }
  applied_=true;finish();
 });
}
void HyprlandShortcuts::finish(){
 applying_=false;
 // A change that came during this apply still has to run. Whoever waits for it,
 // a pause above all, is answered by the apply that makes it (#1308).
 if(reapply_){reapply_=false;emit changed();debounce_.start(0);return;}
 auto replies=std::move(pauseReplies_);pauseReplies_.clear();
 const Error result=ipcError_.isEmpty()?Error{}:Error{"shortcut_unavailable",ipcError_,{}};
 for(auto& reply:replies)if(reply)reply(result);
 emit changed();
 // A Hyprland that did not answer (restarting, reloading) is asked again, more slowly each time.
 if(!ipcError_.isEmpty()){ipcRetryMs_=std::min(30000,std::max(1000,ipcRetryMs_*2));debounce_.start(ipcRetryMs_);}
 else ipcRetryMs_=0;
}
void HyprlandShortcuts::setPaused(bool paused,std::function<void(Error)> reply){
 if(paused==paused_&&!applying_){if(reply)reply({});return;}
 paused_=paused;
 // Paused: every put.gd bind leaves so the recorder gets the keys.
 if(reply)pauseReplies_.append(std::move(reply));
 schedule();emit changed();
}
void HyprlandShortcuts::clear(){
 debounce_.stop();
 QByteArray reply;
 if(HyprlandIpc::requestBlocking("j/binds",&reply))return;
 const auto planned=plan(QJsonDocument::fromJson(reply).array(),{});
 if(planned.reloadFirst){HyprlandIpc::requestBlocking("reload",&reply);++reloadsIssued_;}
 else for(const auto& chord:planned.remove){
  ++unbinds_;
  // Binds left behind on exit point at a shortcut nobody registered, so a failure gets the reload.
  if(HyprlandIpc::requestBlocking("eval "+unbindScript(chord),&reply)||!HyprlandIpc::okReply(reply)){HyprlandIpc::requestBlocking("reload",&reply);++reloadsIssued_;break;}
 }
 bound_.clear();actual_.clear();
}
QJsonObject HyprlandShortcuts::status()const{
 QJsonObject out;
 const auto unavailable=error();
 for(auto it=requested_.cbegin();it!=requested_.cend();++it){
  const auto& action=it.key();const auto& requested=it.value();
  QJsonObject entry{{"requested",requested},{"backend",name()},{"bound",false}};
  if(!requested.isEmpty())entry["effective"]=QKeySequence::fromString(requested,QKeySequence::PortableText).toString(QKeySequence::PortableText);
  QJsonArray actual;for(const auto& key:actual_.value(action))actual.append(key);entry["actual"]=actual;
  if(!unavailable.isEmpty()){entry["state"]="shortcut_unavailable";entry["error"]=unavailable;}
  else if(paused_)entry["state"]="recording";
  else if(requested.isEmpty())entry["state"]=actual.isEmpty()?"unbound":"pending";
  else if(invalid_.contains(action)){entry["state"]="shortcut_invalid";entry["error"]=invalid_.value(action);}
  else if(conflicts_.contains(action)){
   entry["state"]="shortcut_conflict";entry["error"]="Hyprland already binds this key: "+describeBind(conflicts_.value(action));
   entry["conflict"]=QJsonObject{{"description",conflicts_.value(action)["description"]},{"dispatcher",conflicts_.value(action)["dispatcher"]},{"modmask",conflicts_.value(action)["modmask"]},{"key",conflicts_.value(action)["key"]}};
  }
  else if(failed_.contains(action)){entry["state"]="shortcut_unavailable";entry["error"]=failed_.value(action);}
  else if(bound_.contains(action)){entry["state"]="bound";entry["bound"]=true;}
  else{entry["state"]="pending";if(applied_&&!applying_)entry["error"]="Hyprland's bind list does not show this hotkey";}
  out[action]=entry;
 }
 return out;
}
QJsonObject HyprlandShortcuts::diagnostics()const{
 QJsonObject out{{"protocol","hyprland-global-shortcuts-v1"},{"registered",registry_->connected},{"listening",ipc_->listening()},
  {"reloadsIssued",double(reloadsIssued_)},{"unbinds",double(unbinds_)},{"reloadsSeen",double(reloadsSeen_)},{"applies",double(applies_)},{"layerRule",layerRule_}};
 if(!registry_->connected)out["registryError"]=registry_->error;
 if(!evalError_.isEmpty())out["evalError"]=evalError_;
 if(!ipcError_.isEmpty())out["ipcError"]=ipcError_;
 return out;
}
}
