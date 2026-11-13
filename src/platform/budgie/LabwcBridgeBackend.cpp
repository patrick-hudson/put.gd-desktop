#include "LabwcBridgeBackend.h"
#include "core/HostEnvironment.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeySequence>
#include <QProcess>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <xkbcommon/xkbcommon.h>
#include <algorithm>
#include <optional>
#include <unistd.h>

namespace putgd {
namespace {
const QString schema=QStringLiteral("org.buddiesofbudgie.settings-daemon.plugins.media-keys");
const QString entrySchema=QStringLiteral("org.buddiesofbudgie.settings-daemon.plugins.media-keys.custom-keybinding");
const QString basePath=QStringLiteral("/org/buddiesofbudgie/settings-daemon/plugins/media-keys/custom-keybindings/");
const QString ownPrefix=QStringLiteral("customputgd");
constexpr qint64 maximumRcBytes=4*1024*1024;
constexpr int gsettingsTimeoutMs=5000;
// The bridge starts 10 s after login and rewrites rc.xml within ~120 ms.
constexpr int patienceMs=30000;
const QHash<QString,QString> labels{{"region","Region capture"},{"window","Window capture"},{"desktop","Desktop capture"},{"monitor","Monitor capture"},{"repeat","Repeat last region"},{"edit","Region capture and edit"},{"scroll","Capture scrolling region"},{"text","Copy text from region"},{"record","Record screen"},{"clipboard","Upload clipboard"}};
// Qt key names whose GTK keysym name differs.
const QHash<int,QString> namedKeys{{Qt::Key_Print,"Print"},{Qt::Key_Escape,"Escape"},{Qt::Key_Tab,"Tab"},{Qt::Key_Backspace,"BackSpace"},
 {Qt::Key_Return,"Return"},{Qt::Key_Enter,"KP_Enter"},{Qt::Key_Insert,"Insert"},{Qt::Key_Delete,"Delete"},{Qt::Key_Pause,"Pause"},
 {Qt::Key_Home,"Home"},{Qt::Key_End,"End"},{Qt::Key_Left,"Left"},{Qt::Key_Up,"Up"},{Qt::Key_Right,"Right"},{Qt::Key_Down,"Down"},
 {Qt::Key_PageUp,"Page_Up"},{Qt::Key_PageDown,"Page_Down"},{Qt::Key_Space,"space"},{Qt::Key_Menu,"Menu"},{Qt::Key_ScrollLock,"Scroll_Lock"}};
// The session's labwc: its --config-dir (startbudgielabwc passes
// $XDG_CONFIG_HOME/budgie-desktop/labwc) and the XDG_CONFIG_HOME it runs with,
// whose dconf database gsettings must read. A resident started with another
// XDG_CONFIG_HOME would otherwise read an empty database and drop the user's
// own custom keybindings when it rewrites the list.
struct LabwcSession {QString configDir,xdgConfigHome;};
std::optional<LabwcSession> runningLabwc(){
 const auto uid=getuid();
 for(const auto& entry:QDir("/proc").entryList(QDir::Dirs|QDir::NoDotAndDotDot)){
  bool numeric=false;entry.toInt(&numeric);if(!numeric)continue;
  QFile comm("/proc/"+entry+"/comm");if(!comm.open(QIODevice::ReadOnly)||comm.readAll().trimmed()!="labwc")continue;
  if(QFileInfo("/proc/"+entry).ownerId()!=uid)continue;
  QFile cmdline("/proc/"+entry+"/cmdline");QFile environ("/proc/"+entry+"/environ");
  if(!cmdline.open(QIODevice::ReadOnly)||!environ.open(QIODevice::ReadOnly))continue;
  LabwcSession session;
  const auto args=cmdline.read(65536).split('\0');
  for(qsizetype i=0;i+1<args.size();++i)if(args[i]=="--config-dir"||args[i]=="-C")session.configDir=QString::fromLocal8Bit(args[i+1]);
  QString home;
  for(const auto& line:environ.read(1<<20).split('\0')){if(line.startsWith("XDG_CONFIG_HOME="))session.xdgConfigHome=QString::fromLocal8Bit(line.mid(16));else if(line.startsWith("HOME="))home=QString::fromLocal8Bit(line.mid(5));}
  if(session.xdgConfigHome.isEmpty()&&!home.isEmpty())session.xdgConfigHome=home+"/.config";
  return session;
 }
 return {};
}
bool validAction(const QString& action){static const QRegularExpression pattern("^[a-z]{1,32}$");return pattern.match(action).hasMatch();}
QString quoted(const QString& value){QString out=value;out.replace('\\',"\\\\").replace('\'',"\\'");return "'"+out+"'";}
}
QString LabwcBridgeBackend::trigger(const QString& action){
 return QStringLiteral("dbus-send --session --type=method_call --dest=org.putgd.App.Budgie /org/putgd/App org.putgd.App.Budgie.Trigger string:")+action;
}
QString LabwcBridgeBackend::pathFor(const QString& action){return basePath+ownPrefix+action+'/';}
QStringList LabwcBridgeBackend::parseStrv(const QString& text){
 QStringList out;static const QRegularExpression item("'((?:[^'\\\\]|\\\\.)*)'");
 for(auto it=item.globalMatch(text);it.hasNext();){auto value=it.next().captured(1);value.replace("\\'","'").replace("\\\\","\\");out.append(value);}
 return out;
}
QString LabwcBridgeBackend::strv(const QStringList& values){QStringList items;for(const auto& value:values)items.append(quoted(value));return values.isEmpty()?QStringLiteral("@as []"):"["+items.join(", ")+"]";}
QString LabwcBridgeBackend::gtkAccelerator(const QString& portable,QString* error){
 if(error)error->clear();
 const QKeySequence sequence=QKeySequence::fromString(portable,QKeySequence::PortableText);
 if(sequence.count()!=1){if(error)*error="Budgie custom shortcuts hold a single key chord";return {};}
 const auto combination=sequence[0];const int key=combination.key();const auto modifiers=combination.keyboardModifiers();
 QString name;
 if(key>=Qt::Key_F1&&key<=Qt::Key_F35)name=QStringLiteral("F%1").arg(key-Qt::Key_F1+1);
 else if(namedKeys.contains(key))name=namedKeys.value(key);
 else if(key>0x20&&key<0x7f){
  // Qt names printable keys by their unshifted upper-case character.
  const auto symbol=xkb_utf32_to_keysym(uint32_t(QChar(key).toLower().unicode()));char buffer[64]={};
  if(symbol!=XKB_KEY_NoSymbol&&xkb_keysym_get_name(symbol,buffer,sizeof buffer)>0)name=QString::fromLatin1(buffer);
 }
 if(name.isEmpty()){if(error)*error="This key has no Budgie shortcut name";return {};}
 QString out;
 if(modifiers&Qt::ControlModifier)out+="<Control>";
 if(modifiers&Qt::AltModifier)out+="<Alt>";
 if(modifiers&Qt::ShiftModifier)out+="<Shift>";
 if(modifiers&Qt::MetaModifier)out+="<Super>";
 return out+name;
}
QString LabwcBridgeBackend::labwcKey(const QString& gtk){
 // The bridge's own translation (labwc_bridge/keybinds/accel.py), for the keys put.gd writes.
 QString out=gtk;
 for(const auto& [from,to]:std::initializer_list<std::pair<const char*,const char*>>{{"<Alt>","A-"},{"<Super>","W-"},{"<Control>","C-"},{"<Primary>","C-"},{"<Ctrl>","C-"},{"<Shift>","S-"}})out.replace(QString::fromLatin1(from),QString::fromLatin1(to));
 const auto split=out.lastIndexOf('-');
 const auto key=split<0?out:out.mid(split+1);
 if(key.size()==1)out=(split<0?QString():out.left(split+1))+key.toLower();
 return out;
}
QString LabwcBridgeBackend::normalizedLabwcKey(const QString& labwc){
 const auto parts=labwc.split('-');if(parts.isEmpty()||parts.last().isEmpty())return {};
 QStringList modifiers;
 for(qsizetype i=0;i+1<parts.size();++i){
  const auto m=parts[i].toUpper();
  // labwc: S Shift, C Control, A Alt, W Super, M Meta, H Hyper; Mod1..Mod5 alias them.
  const QString canonical=m=="MOD1"?"A":m=="MOD4"?"W":m;
  if(!modifiers.contains(canonical))modifiers.append(canonical);
 }
 std::sort(modifiers.begin(),modifiers.end());
 const auto symbol=xkb_keysym_from_name(parts.last().toUtf8().constData(),XKB_KEYSYM_CASE_INSENSITIVE);
 QString key=parts.last().toLower();
 if(symbol!=XKB_KEY_NoSymbol){char buffer[64]={};if(xkb_keysym_get_name(xkb_keysym_to_lower(symbol),buffer,sizeof buffer)>0)key=QString::fromLatin1(buffer).toLower();}
 return modifiers.join('-')+(modifiers.isEmpty()?"":"-")+key;
}
namespace {
// rc.xml key back to Qt portable text, so Settings compares like with like.
QString portableFromLabwc(const QString& labwc){
 const auto parts=labwc.split('-');if(parts.isEmpty())return labwc;
 QStringList out;
 for(qsizetype i=0;i+1<parts.size();++i){const auto m=parts[i].toUpper();if(m=="C")out<<"Ctrl";else if(m=="A"||m=="MOD1")out<<"Alt";else if(m=="S")out<<"Shift";else if(m=="W"||m=="MOD4")out<<"Meta";else out<<parts[i];}
 const auto keyName=parts.last();QString key=keyName;
 for(auto it=namedKeys.cbegin();it!=namedKeys.cend();++it)if(it.value().compare(keyName,Qt::CaseInsensitive)==0)key=QKeySequence(it.key()).toString(QKeySequence::PortableText);
 if(key==keyName){
  const auto symbol=xkb_keysym_from_name(keyName.toUtf8().constData(),XKB_KEYSYM_CASE_INSENSITIVE);const auto code=xkb_keysym_to_utf32(symbol);
  if(code>0x20&&code<0x7f)key=QString(QChar(char16_t(code))).toUpper();
 }
 // Qt's order: Ctrl, Alt, Shift, Meta.
 QStringList ordered;for(const char* m:{"Ctrl","Alt","Shift","Meta"})if(out.contains(QString::fromLatin1(m)))ordered<<QString::fromLatin1(m);
 for(const auto& m:out)if(!ordered.contains(m))ordered<<m;
 ordered<<key;return ordered.join('+');
}
}
QList<LabwcBridgeBackend::Keybind> LabwcBridgeBackend::readKeybinds(const QString& path,QString* error){
 if(error)error->clear();QList<Keybind> out;QFile file(path);
 if(!file.open(QIODevice::ReadOnly)){if(error)*error="Cannot read "+path;return out;}
 if(file.size()>maximumRcBytes){if(error)*error="rc.xml is larger than put.gd reads";return out;}
 QXmlStreamReader xml(&file);Keybind* current=nullptr;int depth=0,keybindDepth=-1;
 while(!xml.atEnd()){
  const auto token=xml.readNext();
  if(token==QXmlStreamReader::StartElement){
   ++depth;
   if(xml.name()==QLatin1String("keybind")){out.append({xml.attributes().value("key").toString(),xml.attributes().value("bridge").toString(),{}});current=&out.last();keybindDepth=depth;}
   else if(current&&xml.name()==QLatin1String("action")&&current->command.isEmpty())current->command=xml.attributes().value("command").toString();
  }else if(token==QXmlStreamReader::EndElement){if(depth==keybindDepth){current=nullptr;keybindDepth=-1;}--depth;}
 }
 if(xml.hasError()){if(error)*error="rc.xml is not well-formed: "+xml.errorString();out.clear();}
 return out;
}
void LabwcBridgeBackend::probe(std::function<void(bool,QString)> reply,QString configDirectory){
 auto environment=host::environment();QString rc;
 const auto session=configDirectory.isEmpty()?runningLabwc():std::nullopt;
 if(session&&!session->configDir.isEmpty())rc=session->configDir+"/rc.xml";
 if(session&&!session->xdgConfigHome.isEmpty())environment.insert("XDG_CONFIG_HOME",session->xdgConfigHome);
 if(configDirectory.isEmpty())configDirectory=session&&!session->xdgConfigHome.isEmpty()?session->xdgConfigHome:qEnvironmentVariable("XDG_CONFIG_HOME",QDir::homePath()+"/.config");
 if(rc.isEmpty())rc=configDirectory+"/budgie-desktop/labwc/rc.xml";
 auto* process=new QProcess;process->setProcessEnvironment(environment);process->setProgram("gsettings");
 process->setArguments({"get",schema,"custom-keybindings"});
 // A gsettings that blocks (dconf or the bus not answering) must not leave the backend choice waiting.
 auto timedOut=std::make_shared<bool>(false);auto* deadline=new QTimer(process);deadline->setSingleShot(true);
 QObject::connect(deadline,&QTimer::timeout,process,[process,timedOut]{*timedOut=true;process->kill();});deadline->start(gsettingsTimeoutMs);
 QObject::connect(process,&QProcess::finished,process,[process,rc,reply,timedOut](int code,QProcess::ExitStatus status){
  process->deleteLater();
  if(*timedOut){reply(false,"gsettings did not answer within "+QString::number(gsettingsTimeoutMs/1000)+" s");return;}
  if(status!=QProcess::NormalExit||code!=0){reply(false,"Budgie's media-keys schema is not installed");return;}
  if(!QFileInfo::exists(rc)){reply(false,"Budgie's labwc configuration "+rc+" does not exist");return;}
  reply(true,{});
 });
 QObject::connect(process,&QProcess::errorOccurred,process,[process,reply](QProcess::ProcessError error){if(error==QProcess::FailedToStart){process->deleteLater();reply(false,"gsettings is not installed");}});
 process->start();
}
LabwcBridgeBackend::LabwcBridgeBackend(QObject* parent,QString configDirectory):ShortcutBackend(parent),configDirectory_(std::move(configDirectory)){
 environment_=host::environment();
 const auto session=configDirectory_.isEmpty()?runningLabwc():std::nullopt;
 if(configDirectory_.isEmpty())configDirectory_=session&&!session->xdgConfigHome.isEmpty()?session->xdgConfigHome:qEnvironmentVariable("XDG_CONFIG_HOME",QDir::homePath()+"/.config");
 rcPath_=session&&!session->configDir.isEmpty()?session->configDir+"/rc.xml":configDirectory_+"/budgie-desktop/labwc/rc.xml";
 // gsettings reads the session's dconf database, wherever this process's own config lives.
 environment_.insert("XDG_CONFIG_HOME",configDirectory_);
 available_=QFileInfo::exists(rcPath_);if(!available_)lastError_="Budgie's labwc configuration "+rcPath_+" does not exist";
 // The bridge replaces rc.xml, so watch its directory as well as the file.
 watcher_.addPath(QFileInfo(rcPath_).absolutePath());if(available_)watcher_.addPath(rcPath_);
 connect(&watcher_,&QFileSystemWatcher::directoryChanged,this,[this]{if(!watcher_.files().contains(rcPath_)&&QFileInfo::exists(rcPath_))watcher_.addPath(rcPath_);reread();});
 connect(&watcher_,&QFileSystemWatcher::fileChanged,this,[this]{if(!watcher_.files().contains(rcPath_)&&QFileInfo::exists(rcPath_))watcher_.addPath(rcPath_);reread();});
 poll_.setInterval(500);connect(&poll_,&QTimer::timeout,this,[this]{reread();});
 patience_.setSingleShot(true);connect(&patience_,&QTimer::timeout,this,[this]{poll_.stop();report();});
 reread();
}
LabwcBridgeBackend::~LabwcBridgeBackend(){if(process_){process_->disconnect(this);process_->kill();process_->waitForFinished(1000);delete process_;}}
void LabwcBridgeBackend::run(QStringList arguments,std::function<void(int,QString)> done){jobs_.enqueue({std::move(arguments),std::move(done)});if(!process_)next();}
void LabwcBridgeBackend::next(){
 if(process_||jobs_.isEmpty())return;
 auto job=jobs_.dequeue();process_=new QProcess(this);process_->setProcessEnvironment(environment_);process_->setProgram("gsettings");process_->setArguments(job.arguments);
 auto* timer=new QTimer(process_);timer->setSingleShot(true);connect(timer,&QTimer::timeout,process_,[this]{if(process_)process_->kill();});timer->start(gsettingsTimeoutMs);
 auto finish=[this,done=std::move(job.done)](int code){auto* process=process_;process_=nullptr;const auto out=QString::fromUtf8(process->readAllStandardOutput()).trimmed();const auto err=QString::fromUtf8(process->readAllStandardError()).trimmed();process->deleteLater();if(code!=0&&!err.isEmpty())lastError_=err.left(300);if(done)done(code,out);next();};
 connect(process_,&QProcess::finished,this,[finish](int code,QProcess::ExitStatus status){finish(status==QProcess::NormalExit?code:-1);});
 connect(process_,&QProcess::errorOccurred,this,[this,finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart){lastError_="gsettings could not start";finish(-1);}});
 process_->start();
}
void LabwcBridgeBackend::bind(const QHash<QString,QString>& desired){
 requested_.clear();for(auto it=desired.cbegin();it!=desired.cend();++it)if(validAction(it.key()))requested_.insert(it.key(),it.value());
 if(paused_){report();return;}
 apply();
}
void LabwcBridgeBackend::apply(){
 // Paused: nothing is written; resuming applies the latest request.
 if(paused_){reapply_=false;report();return;}
 if(applying_){reapply_=true;return;}
 applying_=true;reapply_=false;
 available_=QFileInfo::exists(rcPath_);if(!available_){lastError_="Budgie's labwc configuration "+rcPath_+" does not exist";applying_=false;report();return;}
 // Its own old conflicts go first, so its reread doesn't count them as freed.
 conflicts_.clear();invalid_.clear();reread();
 run({"get",schema,"custom-keybindings"},[this](int code,QString out){
  if(code!=0){available_=false;applying_=false;report();return;}
  // A pause acknowledged while this read was queued wins: no key may reach rc.xml while recording.
  if(paused_){applying_=false;reapply_=false;report();return;}
  const auto current=parseStrv(out);QStringList others;for(const auto& path:current)if(!path.startsWith(basePath+ownPrefix))others.append(path);
  QStringList ours;QStringList removed;
  for(const auto& path:current)if(path.startsWith(basePath+ownPrefix))removed.append(path);
  auto actions=requested_.keys();std::sort(actions.begin(),actions.end());
  for(const auto& action:actions){
   const auto requested=requested_.value(action);const auto path=pathFor(action);
   if(requested.isEmpty()){written_.remove(action);pending_.remove(action);continue;}
   QString error;const auto gtk=gtkAccelerator(requested,&error);
   if(gtk.isEmpty()){invalid_.insert(action,error);written_.remove(action);pending_.remove(action);continue;}
   // The bridge never checks conflicts; Budgie's own Print keys must survive.
   const auto wanted=normalizedLabwcKey(labwcKey(gtk));
   std::optional<Keybind> owner;for(const auto& keybind:keybinds_)if(keybind.bridge!=ownPrefix+action&&normalizedLabwcKey(keybind.key)==wanted){owner=keybind;break;}
   if(owner){conflicts_.insert(action,QJsonObject{{"key",owner->key},{"owner",owner->bridge.isEmpty()?QStringLiteral("rc.xml"):owner->bridge},{"command",owner->command.left(200)}});written_.remove(action);pending_.remove(action);continue;}
   removed.removeAll(path);ours.append(path);
   const auto entry=entrySchema+':'+path;
   run({"set",entry,"name",quoted("put.gd: "+labels.value(action,action))});
   run({"set",entry,"command",quoted(trigger(action))});
   run({"set",entry,"binding",quoted(gtk)});
   written_.insert(action,labwcKey(gtk));pending_.insert(action);
  }
  // Removal clears binding and command, drops the path, then resets the keys.
  for(const auto& path:removed){const auto entry=entrySchema+':'+path;run({"set",entry,"binding","''"});run({"set",entry,"command","''"});}
  run({"set",schema,"custom-keybindings",strv(others+ours)});
  for(const auto& path:removed)run({"reset-recursively",entrySchema+':'+path});
  run({"get",schema,"custom-keybindings"},[this](int,QString){
   applying_=false;
   if(!pending_.isEmpty()||!conflicts_.isEmpty()){poll_.start();patience_.start(patienceMs);}
   reread();if(reapply_)apply();
  });
 });
 report();
}
void LabwcBridgeBackend::reread(){
 QString error;keybinds_=readKeybinds(rcPath_,&error);rcError_=error;
 // A conflict lasts only while its owner still holds the key (another put.gd action that moved, or
 // a binding changed in Control Center); once the key is free this action is written.
 // An unreadable rc.xml says nothing about the owner: keep the conflict until a read succeeds.
 bool freed=false;
 if(rcError_.isEmpty())for(auto it=conflicts_.begin();it!=conflicts_.end();){
  const auto key=normalizedLabwcKey(it.value()["key"].toString());const auto self=ownPrefix+it.key();bool held=false;
  for(const auto& keybind:keybinds_)if(keybind.bridge!=self&&normalizedLabwcKey(keybind.key)==key)held=true;
  if(held)++it;else{it=conflicts_.erase(it);freed=true;}
 }
 // During an apply the freed key is written by the re-apply that apply's last step runs.
 if(freed&&!paused_){if(applying_)reapply_=true;else QTimer::singleShot(0,this,[this]{apply();});}
 for(auto it=pending_.begin();it!=pending_.end();){
  const auto action=*it;bool found=false;
  for(const auto& keybind:keybinds_)if(keybind.bridge==ownPrefix+action&&keybind.command==trigger(action)&&normalizedLabwcKey(keybind.key)==normalizedLabwcKey(written_.value(action)))found=true;
  if(found)it=pending_.erase(it);else ++it;
 }
 if(pending_.isEmpty()){poll_.stop();patience_.stop();}
 report();
}
void LabwcBridgeBackend::report(){emit changed();}
QJsonObject LabwcBridgeBackend::status()const{
 QJsonObject out;
 for(auto it=requested_.cbegin();it!=requested_.cend();++it){
  const auto& action=it.key();const auto& requested=it.value();
  const auto effective=QKeySequence::fromString(requested,QKeySequence::PortableText).toString(QKeySequence::PortableText);
  QJsonObject entry{{"requested",requested},{"backend","labwc-bridge"},{"bound",false}};
  if(!requested.isEmpty())entry["effective"]=effective;
  QJsonArray actual;bool commandMatches=false;
  for(const auto& keybind:keybinds_)if(keybind.bridge==ownPrefix+action&&keybind.key!="undefined"){actual.append(portableFromLabwc(keybind.key));commandMatches=keybind.command==trigger(action);}
  entry["actual"]=actual;
  if(!available_){entry["state"]="shortcut_unavailable";entry["error"]=lastError_;}
  else if(paused_)entry["state"]="recording";
  else if(requested.isEmpty())entry["state"]=actual.isEmpty()?"unbound":"pending";
  else if(invalid_.contains(action)){entry["state"]="shortcut_invalid";entry["error"]=invalid_.value(action);}
  else if(conflicts_.contains(action)){entry["state"]="shortcut_conflict";entry["error"]="Budgie already uses this key";entry["conflict"]=conflicts_.value(action);}
  else if(pending_.contains(action)){entry["state"]="pending";if(!patience_.isActive()&&!applying_)entry["error"]="Budgie's labwc bridge has not applied this hotkey yet; it starts 10 seconds after login";}
  else if(!actual.isEmpty()&&!commandMatches){entry["state"]="shortcut_conflict";entry["error"]="The Budgie entry no longer runs put.gd";}
  else if(!actual.isEmpty()){entry["state"]="bound";entry["bound"]=true;}
  else{entry["state"]="pending";entry["error"]=rcError_.isEmpty()?QStringLiteral("rc.xml has no entry for this hotkey"):rcError_;}
  out[action]=entry;
 }
 return out;
}
void LabwcBridgeBackend::setPaused(bool paused,std::function<void(Error)> reply){
 if(paused==paused_){if(reply)reply({});return;}
 paused_=paused;
 if(!paused){apply();if(reply)reply({});return;}
 // labwc runs a bound key's command instead of sending it to the recorder, so
 // the keys come out of rc.xml while recording and go back afterwards.
 auto remaining=std::make_shared<int>(0);auto failure=std::make_shared<Error>();
 const auto keys=written_.keys();
 if(keys.isEmpty()){report();if(reply)reply({});return;}
 for(const auto& action:keys){
  ++*remaining;
  run({"set",entrySchema+':'+pathFor(action),"binding","''"},[remaining,failure,reply](int code,QString){if(code!=0)*failure={"shortcut_recording_failed","Budgie did not accept pausing put.gd's hotkeys.",{}};if(--*remaining==0&&reply)reply(*failure);});
 }
 report();
}
void LabwcBridgeBackend::clear(){
 // A clean quit leaves nothing behind in Budgie's settings or rc.xml. This runs
 // as the resident stops, so each call is bounded and synchronous.
 const auto call=[this](const QStringList& arguments)->QString{QProcess process;process.setProcessEnvironment(environment_);process.start("gsettings",arguments);if(!process.waitForFinished(1500)){process.kill();process.waitForFinished(500);return {};}return QString::fromUtf8(process.readAllStandardOutput()).trimmed();};
 const auto current=parseStrv(call({"get",schema,"custom-keybindings"}));
 QStringList others,ours;for(const auto& path:current)(path.startsWith(basePath+ownPrefix)?ours:others).append(path);
 if(ours.isEmpty())return;
 for(const auto& path:ours){call({"set",entrySchema+':'+path,"binding","''"});call({"set",entrySchema+':'+path,"command","''"});}
 call({"set",schema,"custom-keybindings",strv(others)});
 for(const auto& path:ours)call({"reset-recursively",entrySchema+':'+path});
 written_.clear();pending_.clear();
}
}
