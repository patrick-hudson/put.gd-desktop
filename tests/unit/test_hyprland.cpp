// Hyprland adapter contracts (#1275): key and bind normalisation against a real
// Omarchy 4.5 bind list, the bind plan (conflicts, stale and doubled put.gd
// binds), Lua quoting, permission refusal frames and rules, and the shortcut
// backend driven against a fake Hyprland IPC server that records every request.
#include "platform/hyprland/HyprlandClicks.h"
#include "platform/hyprland/HyprlandIpc.h"
#include "platform/hyprland/HyprlandShortcuts.h"
#include "platform/hyprland/HyprlandSourceAdapter.h"
#include "platform/hyprland/ToplevelMapping.h"
#include "fixtures/Waits.h"
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <atomic>
#include <mutex>
#include <thread>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using putgd::HyprlandShortcuts;
using putgd::HyprlandClicks;
namespace {
// The fixtures store Hyprland's "key" field as "sym": key names such as
// XF86Calculator next to "key" read as credentials to secret scanners.
QJsonArray fixtureBinds(){
 QFile file(QStringLiteral(PUTGD_TEST_FIXTURES "/hyprland/omarchy-4.5-binds.json"));
 if(!file.open(QIODevice::ReadOnly))return {};
 QJsonArray out;
 for(const auto& value:QJsonDocument::fromJson(file.readAll()).array()){auto bind=value.toObject();bind.insert(QStringLiteral("key"),bind.take(QStringLiteral("sym")));out.append(bind);}
 return out;
}
QByteArray fixtureText(){
 QFile file(QStringLiteral(PUTGD_TEST_FIXTURES "/hyprland/omarchy-4.5-binds.txt"));
 return file.open(QIODevice::ReadOnly)?file.readAll().replace("\tsym: ","\tkey: "):QByteArray();
}
QJsonObject ownBind(const QString& action,int modmask,const QString& key){
 return {{"modmask",modmask},{"key",key},{"keycode",0},{"submap",""},{"mouse",false},{"catch_all",false},{"description",HyprlandShortcuts::description(action)},{"dispatcher","__lua"},{"arg","900"}};
}
int listenOn(const QByteArray& path){
 const int fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
 sockaddr_un address{};address.sun_family=AF_UNIX;std::memcpy(address.sun_path,path.constData(),size_t(path.size()));
 if(::bind(fd,reinterpret_cast<const sockaddr*>(&address),sizeof address)!=0||::listen(fd,16)!=0){::close(fd);return -1;}
 return fd;
}
// Hyprland's two sockets, answering like Hyprland 0.56 on Omarchy: j/binds,
// binds, eval hl.bind / hl.unbind / hl.layer_rule, reload (which drops runtime
// binds and sends configreloaded>> on the event socket).
class FakeHyprland {
public:
 explicit FakeHyprland(const QString& directory){
  base_=fixtureBinds();
  binds_=base_;
  request_=listenOn(QFile::encodeName(directory+"/.socket.sock"));
  events_=listenOn(QFile::encodeName(directory+"/.socket2.sock"));
  thread_=std::thread([this]{run();});
 }
 ~FakeHyprland(){stop_=true;thread_.join();::close(request_);::close(events_);for(int fd:subscribers_)::close(fd);}
 bool ok()const{return request_>=0&&events_>=0;}
 QStringList requests(){std::lock_guard lock(mutex_);return log_;}
 QJsonArray binds(){std::lock_guard lock(mutex_);return binds_;}
 int ownCount(){int n=0;for(const auto& b:binds())if(HyprlandShortcuts::ownDescription(b.toObject()["description"].toString()))++n;return n;}
 int count(const QString& prefix){int n=0;for(const auto& r:requests())if(r.startsWith(prefix))++n;return n;}
 // An Omarchy theme change: Hyprland reloads by itself.
 void externalReload(){std::lock_guard lock(mutex_);reload();}
 void addBind(const QJsonObject& bind){std::lock_guard lock(mutex_);binds_.append(bind);}
 void refuseUnbinds(bool refuse){std::lock_guard lock(mutex_);refuseUnbinds_=refuse;}
 // The event socket goes away and a reload happens while it is gone.
 void reloadUnseen(){std::lock_guard lock(mutex_);for(int fd:subscribers_)::close(fd);subscribers_.clear();binds_=base_;}
private:
 void reload(){
  binds_=base_;
  for(int fd:subscribers_)(void)::send(fd,"configreloaded>>\n",17,MSG_NOSIGNAL);
 }
 QByteArray text(){
  // The base list's text form, plus a block per added bind (in list order).
  QByteArray out=fixtureText();
  for(qsizetype i=base_.size();i<binds_.size();++i){
   const auto b=binds_[i].toObject();
   out+="bindd\n\tmodmask: "+QByteArray::number(b["modmask"].toInt())+"\n\tsubmap: \n\tkey: "+b["key"].toString().toUtf8()+"\n\tkeycode: 0\n\tcatchall: false\n\tdescription: "+b["description"].toString().toUtf8()+"\n\tdispatcher: __lua\n\targ: 1\n\n";
  }
  return out;
 }
 QByteArray answer(const QByteArray& request){
  std::lock_guard lock(mutex_);
  log_.append(QString::fromUtf8(request));
  if(request=="j/binds")return QJsonDocument(binds_).toJson(QJsonDocument::Compact);
  if(request=="binds")return text();
  if(request=="reload"){reload();return "ok";}
  if(request.startsWith("eval ")){
   const auto lua=QString::fromUtf8(request.mid(5));
   if(lua.contains(":remove"))return "error: :remove crashes Hyprland";
   static const QRegularExpression unbind(QStringLiteral("^hl\\.unbind\\(\"([^\"]*)\"\\)$"));
   if(const auto match=unbind.match(lua);match.hasMatch()){
    if(refuseUnbinds_)return "error: refused";
    int modmask=0;QString key;
    for(const auto& part:match.captured(1).split(" + ")){
     if(part=="SUPER")modmask|=64;else if(part=="CTRL")modmask|=4;else if(part=="ALT")modmask|=8;else if(part=="SHIFT")modmask|=1;else key=part;
    }
    // Exact on the chord, whatever the submap: as measured on 0.56.2.
    QJsonArray kept;for(const auto& value:binds_){const auto b=value.toObject();if(!(b["modmask"].toInt()==modmask&&b["key"].toString().compare(key,Qt::CaseInsensitive)==0))kept.append(b);}
    binds_=kept;return "ok";
   }
   static const QRegularExpression bind(QStringLiteral("^hl\\.bind\\(\"([^\"]*)\", hl\\.dsp\\.global\\(\"putgd:([a-z]+)\"\\), \\{ description = \"([^\"]*)\" \\}\\)$"));
   const auto match=bind.match(lua);
   if(match.hasMatch()){
    int modmask=0;QString key;
    for(const auto& part:match.captured(1).split(" + ")){
     if(part=="SUPER")modmask|=64;else if(part=="CTRL")modmask|=4;else if(part=="ALT")modmask|=8;else if(part=="SHIFT")modmask|=1;else key=part;
    }
    binds_.append(QJsonObject{{"modmask",modmask},{"key",key},{"keycode",0},{"submap",""},{"mouse",false},{"catch_all",false},{"description",match.captured(3)},{"dispatcher","__lua"},{"arg","901"}});
    return "ok";
   }
   if(lua.startsWith("hl.layer_rule("))return "ok";
   return "error: unexpected eval";
  }
  return "unknown request";
 }
 void run(){
  while(!stop_){
   pollfd fds[2]{{request_,POLLIN,0},{events_,POLLIN,0}};
   if(::poll(fds,2,50)<=0)continue;
   if(fds[1].revents&POLLIN){const int fd=::accept4(events_,nullptr,nullptr,SOCK_CLOEXEC);std::lock_guard lock(mutex_);if(fd>=0)subscribers_.append(fd);}
   if(fds[0].revents&POLLIN){
    const int fd=::accept4(request_,nullptr,nullptr,SOCK_CLOEXEC);if(fd<0)continue;
    QByteArray in;char buffer[4096];
    for(;;){pollfd p{fd,POLLIN,0};if(::poll(&p,1,1000)<=0)break;const auto n=::recv(fd,buffer,sizeof buffer,0);if(n<=0)break;in.append(buffer,n);}
    const auto out=answer(in);qsizetype sent=0;
    while(sent<out.size()){const auto n=::send(fd,out.constData()+sent,size_t(out.size()-sent),MSG_NOSIGNAL);if(n<=0)break;sent+=n;}
    ::close(fd);
   }
  }
 }
 QJsonArray base_,binds_;QStringList log_;QList<int> subscribers_;bool refuseUnbinds_=false;
 int request_=-1,events_=-1;std::thread thread_;std::atomic_bool stop_{false};std::mutex mutex_;
};
}

class HyprlandTest final:public QObject {
 Q_OBJECT
private slots:
 void portableChords_data(){
  QTest::addColumn<QString>("portable");QTest::addColumn<int>("modmask");QTest::addColumn<QString>("key");QTest::addColumn<QString>("hyprland");
  QTest::newRow("Ctrl+Shift+1")<<"Ctrl+Shift+1"<<5<<"1"<<"CTRL + SHIFT + 1";
  QTest::newRow("Print")<<"Print"<<0<<"print"<<"print";
  QTest::newRow("Meta+Shift+S")<<"Meta+Shift+S"<<65<<"s"<<"SUPER + SHIFT + s";
  QTest::newRow("Ctrl+Alt+F9")<<"Ctrl+Alt+F9"<<12<<"f9"<<"CTRL + ALT + f9";
  QTest::newRow("PgUp")<<"Alt+PgUp"<<8<<"prior"<<"ALT + prior";
  QTest::newRow("slash")<<"Ctrl+/"<<4<<"slash"<<"CTRL + slash";
 }
 void portableChords(){
  QFETCH(QString,portable);QFETCH(int,modmask);QFETCH(QString,key);QFETCH(QString,hyprland);
  const auto chord=HyprlandShortcuts::chordFromPortable(portable);QVERIFY(chord);
  QCOMPARE(chord->modmask,modmask);QCOMPARE(chord->key,key);
  QCOMPARE(HyprlandShortcuts::hyprlandKeys(*chord),hyprland);
  QCOMPARE(HyprlandShortcuts::portable(*chord),QKeySequence::fromString(portable,QKeySequence::PortableText).toString(QKeySequence::PortableText));
 }
 void rejectsSequences(){
  QString error;QVERIFY(!HyprlandShortcuts::chordFromPortable("Ctrl+1, Ctrl+2",&error));QVERIFY(!error.isEmpty());
 }
 void omarchyBinds(){
  const auto binds=fixtureBinds();QCOMPARE(binds.size(),251);
  const auto merged=HyprlandShortcuts::withTextKeys(binds,fixtureText());
  int named=0;std::optional<HyprlandShortcuts::Chord> print,workspace1,maps;
  for(const auto& value:merged){
   const auto bind=value.toObject();if(bind.contains("keyText"))++named;
   if(bind["description"]=="Screenshot"&&bind["key"]=="PRINT")print=HyprlandShortcuts::chordFromBind(bind);
   if(bind["description"]=="Switch to workspace 1")workspace1=HyprlandShortcuts::chordFromBind(bind);
   if(bind["description"]=="Google Maps")maps=HyprlandShortcuts::chordFromBind(bind);
  }
  // Every code: bind gets its key from the text reply.
  QCOMPARE(named,59);
  QVERIFY(print);QCOMPARE(print->modmask,0);QCOMPARE(print->key,QString("print"));
  QVERIFY(workspace1);QCOMPARE(workspace1->modmask,64);QCOMPARE(workspace1->key,QString("1"));
  QVERIFY(maps);QCOMPARE(maps->modmask,65);QCOMPARE(maps->key,QString("s"));
  // A text list that does not line up is ignored rather than misread.
  QVERIFY(!HyprlandShortcuts::withTextKeys(binds,"bindd\n\tkey: SUPER + code:10\n")[0].toObject().contains("keyText"));
 }
 void planOnOmarchy(){
  const auto binds=HyprlandShortcuts::withTextKeys(fixtureBinds(),fixtureText());
  const QHash<QString,QString> desired{{"region","Ctrl+Shift+1"},{"window","Ctrl+Shift+2"},{"desktop","Ctrl+Shift+3"},
   {"record","Print"},{"edit","Meta+Shift+S"},{"clipboard",""},{"workspace","Meta+1"}};
  const auto plan=HyprlandShortcuts::plan(binds,desired);
  QVERIFY(!plan.reloadFirst);
  QCOMPARE(QSet<QString>(plan.apply.keyBegin(),plan.apply.keyEnd()),(QSet<QString>{"region","window","desktop"}));
  QCOMPARE(plan.conflicts.value("record")["description"].toString(),QString("Screenshot"));
  QCOMPARE(plan.conflicts.value("edit")["description"].toString(),QString("Google Maps"));
  QCOMPARE(plan.conflicts.value("workspace")["description"].toString(),QString("Switch to workspace 1"));
  QVERIFY(!plan.apply.contains("clipboard")&&!plan.conflicts.contains("clipboard"));
 }
 void planOwnBinds(){
  auto binds=fixtureBinds();
  binds.append(ownBind("region",5,"1"));
  // Already there with the same key: nothing to do.
  auto plan=HyprlandShortcuts::plan(binds,{{"region","Ctrl+Shift+1"}});
  QVERIFY(!plan.reloadFirst);QVERIFY(plan.present.contains("region"));QVERIFY(plan.apply.isEmpty());
  // A new key, an unbound action, a paused set and a doubled bind all unbind put.gd's chord; nothing reloads.
  const QList<HyprlandShortcuts::Chord> own{{5,"1"}};
  plan=HyprlandShortcuts::plan(binds,{{"region","Ctrl+Shift+4"}});QVERIFY(!plan.reloadFirst);QCOMPARE(plan.remove,own);QVERIFY(plan.apply.contains("region"));
  plan=HyprlandShortcuts::plan(binds,{{"region",""}});QVERIFY(!plan.reloadFirst);QCOMPARE(plan.remove,own);QVERIFY(plan.apply.isEmpty());
  plan=HyprlandShortcuts::plan(binds,{});QVERIFY(!plan.reloadFirst);QCOMPARE(plan.remove,own);
  auto doubled=binds;doubled.append(ownBind("region",5,"1"));
  plan=HyprlandShortcuts::plan(doubled,{{"region","Ctrl+Shift+1"},{"window","Ctrl+Shift+2"}});QVERIFY(!plan.reloadFirst);QCOMPARE(plan.remove,own);
  QCOMPARE(QSet<QString>(plan.apply.keyBegin(),plan.apply.keyEnd()),(QSet<QString>{"region","window"}));
  // put.gd's own binds never count as conflicts, and two actions on one key do.
  plan=HyprlandShortcuts::plan(binds,{{"region","Ctrl+Shift+1"},{"window","Ctrl+Shift+1"}});
  QCOMPARE(plan.present.size()+plan.apply.size(),1);QCOMPARE(plan.conflicts.size(),1);
 }
 // Anything else on put.gd's chord would go with hl.unbind, so only a reload (which restores it) is safe.
 void planReloadsWhenUnbindWouldTakeSomeoneElse_data(){
  QTest::addColumn<QJsonObject>("other");
  const QJsonObject base{{"modmask",5},{"key","1"},{"keycode",0},{"submap",""},{"mouse",false},{"catch_all",false},{"description","Desktop's own"},{"dispatcher","exec"},{"arg","x"}};
  QTest::newRow("same chord")<<base;
  auto submap=base;submap["submap"]="resize";QTest::newRow("same chord in a submap")<<submap;
  QJsonObject code=base;code["key"]="";code["keycode"]=10;QTest::newRow("same physical key by code")<<code;
 }
 void planReloadsWhenUnbindWouldTakeSomeoneElse(){
  QFETCH(QJsonObject,other);
  auto binds=fixtureBinds();binds.append(ownBind("region",5,"1"));binds.append(other);
  auto plan=HyprlandShortcuts::plan(binds,{});QVERIFY(plan.reloadFirst);QVERIFY(plan.remove.isEmpty());
  // Elsewhere on the keyboard the same bind changes nothing.
  auto apart=fixtureBinds();apart.append(ownBind("region",5,"1"));other["modmask"]=4;apart.append(other);
  plan=HyprlandShortcuts::plan(apart,{});QVERIFY(!plan.reloadFirst);QCOMPARE(plan.remove.size(),1);
 }
 void planReloadsForABindItCannotRead(){
  auto binds=fixtureBinds();auto unreadable=ownBind("region",5,"");unreadable["key"]="";binds.append(unreadable);
  const auto plan=HyprlandShortcuts::plan(binds,{});QVERIFY(plan.reloadFirst);QVERIFY(plan.remove.isEmpty());
 }
 void luaQuoting(){
  QCOMPARE(putgd::HyprlandIpc::luaString("a\"b\\c\nd"),QString("\"a\\\"b\\\\c\\010d\""));
  QCOMPARE(putgd::HyprlandIpc::luaString(QString::fromUtf8("é]]")),QString("\"\\195\\169]]\""));
  const auto chord=HyprlandShortcuts::chordFromPortable("Ctrl+Shift+1");
  QCOMPARE(HyprlandShortcuts::bindScript("region",*chord),QString("hl.bind(\"CTRL + SHIFT + 1\", hl.dsp.global(\"putgd:region\"), { description = \"put.gd: Region capture\" })"));
  QVERIFY(HyprlandShortcuts::layerRuleScript().contains("putgd-(region-selector|region-catcher|record|capture-countdown)"));
  QVERIFY(HyprlandShortcuts::layerRuleScript().contains("no_anim = true"));
  QString keypad;QVERIFY(!HyprlandShortcuts::chordFromPortable("Num+1",&keypad));QVERIFY(keypad.contains("Keypad"));
  QVERIFY(putgd::HyprlandIpc::okReply("ok\n"));QVERIFY(!putgd::HyprlandIpc::okReply("error: x"));
 }
 void refusalFrames(){
  QImage transparent(64,32,QImage::Format_ARGB32_Premultiplied);transparent.fill(0);
  QVERIFY(putgd::HyprlandSourceAdapter::refusedFrame(transparent));
  // The cursor Hyprland still draws into a denied frame (under 1% of it).
  QImage denied(400,300,QImage::Format_ARGB32_Premultiplied);denied.fill(0);
  for(int y=100;y<132;++y)for(int x=200;x<232;++x)denied.setPixel(x,y,qRgba(255,255,255,255));
  QVERIFY(putgd::HyprlandSourceAdapter::refusedFrame(denied));
  auto content=denied;for(int y=0;y<40;++y)for(int x=0;x<40;++x)content.setPixel(x,y,qRgba(0,0,0,255));
  QVERIFY(!putgd::HyprlandSourceAdapter::refusedFrame(content));
  QImage black(64,32,QImage::Format_RGB32);black.fill(Qt::black);QVERIFY(!putgd::HyprlandSourceAdapter::refusedFrame(black));
  QVERIFY(!putgd::HyprlandSourceAdapter::refusedFrame(QImage()));
 }
 void permissionRules(){
  QCOMPARE(putgd::HyprlandSourceAdapter::permissionRule("/usr/bin/putgd"),QString("hl.permission({ binary = [[/usr/bin/putgd]], type = \"screencopy\", mode = \"allow\" })"));
  QCOMPARE(putgd::HyprlandSourceAdapter::permissionRule("/tmp/.mount_putgdAb12Cd/usr/bin/putgd-record"),
   QString("hl.permission({ binary = [[/tmp/\\.mount_[^/]+/usr/bin/putgd-record]], type = \"screencopy\", mode = \"allow\" })"));
  QCOMPARE(putgd::HyprlandSourceAdapter::permissionRule("/opt/put.gd/bin/putgd"),QString("hl.permission({ binary = [[/opt/put\\.gd/bin/putgd]], type = \"screencopy\", mode = \"allow\" })"));
 }
 void addresses(){
  QVERIFY(putgd::ToplevelMapping::sameAddress("0x55d0C0ffee10","55d0c0ffee10"));
  QVERIFY(!putgd::ToplevelMapping::sameAddress("0x55d0c0ffee10","0x55d0c0ffee11"));
  QVERIFY(!putgd::ToplevelMapping::sameAddress("0x0","0"));
  QVERIFY(!putgd::ToplevelMapping::sameAddress("","0x1"));
 }
 // The backend against a fake Hyprland: binds only free keys, never unbinds,
 // reloads to change or remove its binds, re-applies after any reload, and
 // leaves exactly the configuration's binds when paused or cleared.
 // #1364: click highlights. The binds never consume the click, a hostile token
 // cannot leave the Lua string, and a chord is unbound only when nothing of the
 // user's shares it (hl.unbind would take the user's bind too).
 void clickBindsAreNonConsumingTokenGatedAndRemovedOnlyWhenAlone(){
  const auto script=HyprlandClicks::armScript("abc123",true);
  QCOMPARE(script.count("non_consuming = true"),3);
  const QString hostile="x\"; os.exit() --";
  QCOMPARE(HyprlandClicks::armScript(hostile,false),"_G.putgd_clicks = "+putgd::HyprlandIpc::luaString(hostile));
  QVERIFY(putgd::HyprlandIpc::luaString(hostile).contains("\\\""));
  const auto bind=[](const QString& key,int mods,const QString& description){return QJsonObject{{"key",key},{"modmask",mods},{"description",description},{"dispatcher","__lua"}};};
  const QString ours=HyprlandClicks::description;
  QJsonArray binds{bind("mouse:272",0,ours),bind("mouse:274",0,ours),bind("mouse:273",0,ours),bind("mouse:272",64,"move window"),bind("mouse:273",0,"my menu")};
  QCOMPARE(HyprlandClicks::removable(binds),QStringList({"mouse:272","mouse:274"}));
  QCOMPARE(HyprlandClicks::removable(QJsonArray{bind("mouse:272",0,"someone else")}),QStringList{});
 }
 void backendAgainstFakeHyprland(){
  QTemporaryDir runtime;QVERIFY(runtime.isValid());
  const QString signature="fake_1_2";QVERIFY(QDir(runtime.path()).mkpath("hypr/"+signature));
  const QList<QByteArray> names{"XDG_RUNTIME_DIR","HYPRLAND_INSTANCE_SIGNATURE","WAYLAND_DISPLAY"};
  struct Previous{QByteArray name,value;bool present;};QList<Previous> saved;
  for(const auto& name:names)saved.append({name,qgetenv(name),qEnvironmentVariableIsSet(name.constData())});
  const auto restore=qScopeGuard([&]{for(const auto& p:saved){if(p.present)qputenv(p.name,p.value);else qunsetenv(p.name);}});
  qputenv("XDG_RUNTIME_DIR",QFile::encodeName(runtime.path()));qputenv("HYPRLAND_INSTANCE_SIGNATURE",signature.toUtf8());
  // No compositor here: the protocol registration fails and retries; the IPC side still runs.
  qputenv("WAYLAND_DISPLAY","putgd-test-no-compositor");
  FakeHyprland hyprland(runtime.path()+"/hypr/"+signature);QVERIFY(hyprland.ok());
  // A put.gd bind left by a resident that crashed, on a key no longer wanted.
  hyprland.addBind(ownBind("desktop",5,"9"));
  putgd::HyprlandIpc ipc;
  auto shortcuts=std::make_unique<HyprlandShortcuts>(&ipc);
  PUTGD_TRY_VERIFY(ipc.listening());
  shortcuts->bind({{"region","Ctrl+Shift+1"},{"window","Ctrl+Shift+2"},{"record","Print"},{"edit","Meta+Shift+S"},{"clipboard",""}});
  PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
  PUTGD_TRY_VERIFY(hyprland.count("j/binds")>=3);
  QCOMPARE(hyprland.count("eval hl.unbind"),1);  // the stale bind went by itself, without a reload
  QCOMPARE(hyprland.count("reload"),0);
  QCOMPARE(hyprland.binds().size(),251+2);
  // Another program's runtime bind: gone if put.gd reloaded, so it must outlive what follows.
  hyprland.addBind(QJsonObject{{"modmask",8},{"key","F7"},{"keycode",0},{"submap",""},{"mouse",false},{"catch_all",false},{"description","Another program"},{"dispatcher","exec"},{"arg","x"}});
  const auto otherProgram=[&]{for(const auto& b:hyprland.binds())if(b.toObject()["description"].toString()=="Another program")return true;return false;};
  // Rebinding one action removes just that action's old chord.
  shortcuts->bind({{"region","Ctrl+Shift+1"},{"window","Ctrl+Shift+4"},{"record","Print"}});
  PUTGD_TRY_COMPARE(hyprland.count("eval hl.unbind"),2);
  PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
  QCOMPARE(hyprland.count("reload"),0);QVERIFY(otherProgram());
  bool heldForRecorder=false;shortcuts->setPaused(true,[&](putgd::Error){heldForRecorder=true;});
  PUTGD_TRY_VERIFY(heldForRecorder);QCOMPARE(hyprland.ownCount(),0);QCOMPARE(hyprland.count("reload"),0);QVERIFY(otherProgram());
  shortcuts->setPaused(false,{});PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
  // Something else takes put.gd's key: unbind would remove it too, so this one reloads.
  hyprland.addBind(QJsonObject{{"modmask",5},{"key","1"},{"keycode",0},{"submap",""},{"mouse",false},{"catch_all",false},{"description","Second on this key"},{"dispatcher","exec"},{"arg","x"}});
  const int unbindsBefore=hyprland.count("eval hl.unbind");
  shortcuts->bind({{"window","Ctrl+Shift+4"},{"record","Print"}});
  PUTGD_TRY_COMPARE(hyprland.count("reload"),1);PUTGD_TRY_COMPARE(hyprland.ownCount(),1);QCOMPARE(hyprland.count("eval hl.unbind"),unbindsBefore);
  shortcuts->bind({{"region","Ctrl+Shift+1"},{"window","Ctrl+Shift+4"},{"record","Print"}});PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
  // The event socket drops and Hyprland reloads meanwhile: reconnecting puts the binds back.
  hyprland.reloadUnseen();QCOMPARE(hyprland.ownCount(),0);PUTGD_TRY_VERIFY(!ipc.listening());
  PUTGD_TRY_VERIFY(ipc.listening());PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
  // An Omarchy theme change reloads Hyprland: put.gd's binds come back.
  hyprland.externalReload();QCOMPARE(hyprland.ownCount(),0);
  PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
  // Paused for the shortcut recorder: nothing of put.gd's is bound.
  bool paused=false;shortcuts->setPaused(true,[&](putgd::Error){paused=true;});
  PUTGD_TRY_VERIFY(paused);QCOMPARE(hyprland.ownCount(),0);
  shortcuts->setPaused(false,{});PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
  // #1308: a pause that lands while the re-apply is finishing is answered only once the binds are gone.
  for(int round=0;round<25;++round){
   hyprland.externalReload();PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
   bool quiet=false;shortcuts->setPaused(true,[&](putgd::Error){quiet=true;});
   PUTGD_TRY_VERIFY(quiet);QCOMPARE(hyprland.ownCount(),0);
   shortcuts->setPaused(false,{});PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
  }
  PUTGD_TRY_VERIFY(shortcuts->status()["window"].toObject()["actual"].toArray().contains("Ctrl+Shift+4"));
  // Quit: only put.gd's chords are unbound and everything else stays where it was.
  const int reloadsBeforeQuit=hyprland.count("reload");
  shortcuts->clear();QCOMPARE(hyprland.ownCount(),0);QCOMPARE(hyprland.binds().size(),251);
  QCOMPARE(hyprland.count("reload"),reloadsBeforeQuit);
  // A refused unbind falls back to the reload, while running and at quit.
  shortcuts->bind({{"region","Ctrl+Shift+1"},{"window","Ctrl+Shift+4"}});PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
  hyprland.refuseUnbinds(true);
  shortcuts->bind({{"region","Ctrl+Shift+7"},{"window","Ctrl+Shift+4"}});
  PUTGD_TRY_COMPARE(hyprland.count("reload"),reloadsBeforeQuit+1);PUTGD_TRY_COMPARE(hyprland.ownCount(),2);
  shortcuts->clear();QCOMPARE(hyprland.ownCount(),0);QCOMPARE(hyprland.count("reload"),reloadsBeforeQuit+2);
  // Never a handle's :remove() (a stale handle crashes Hyprland), never keyword binds.
  for(const auto& request:hyprland.requests()){QVERIFY2(!request.contains(":remove"),qPrintable(request));QVERIFY(!request.startsWith("keyword"));}
  QVERIFY(hyprland.count("eval hl.layer_rule(")>=2);
 }
};
QTEST_GUILESS_MAIN(HyprlandTest)
#include "test_hyprland.moc"
