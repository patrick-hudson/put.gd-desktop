#include "SystemUpdates.h"
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
namespace putgd::update {
namespace {
constexpr qint64 maximumRepositoryFile=64*1024;
const QStringList debTargets{"ubuntu24.04","ubuntu26.04","debian13"};
const QList<Updater> updaters{
 {"mintupdate","Update Manager","mintupdate",{}},
 {"plasma-discover","Discover","plasma-discover",{"--mode","update"}},
 {"gnome-software","GNOME Software","gnome-software",{"--mode=updates"}}};
QString readSmall(const QString& path,bool& exists) {
 QFile file(path);exists=file.exists();
 if(!exists||!file.open(QIODevice::ReadOnly)||file.size()>maximumRepositoryFile)return {};
 return QString::fromUtf8(file.readAll());
}
// deb822: stanzas separated by blank lines; continuation lines start with a space.
QList<QHash<QString,QString>> stanzas(const QString& text) {
 QList<QHash<QString,QString>> result;QHash<QString,QString> current;QString key;
 for(const auto& line:text.split('\n')) {
  if(line.trimmed().isEmpty()){if(!current.isEmpty())result.append(current);current.clear();key.clear();continue;}
  if(line.startsWith('#'))continue;
  if(line.startsWith(' ')||line.startsWith('\t')){if(!key.isEmpty())current[key]+='\n'+line.trimmed();continue;}
  const auto colon=line.indexOf(':');if(colon<=0)continue;
  key=line.left(colon).trimmed().toLower();current[key]=line.mid(colon+1).trimmed();
 }
 if(!current.isEmpty())result.append(current);
 return result;
}
// The INI sections of a .repo file, keys lowercased.
QList<QHash<QString,QString>> sections(const QString& text) {
 QList<QHash<QString,QString>> result;
 for(const auto& raw:text.split('\n')) {
  const auto line=raw.trimmed();
  if(line.isEmpty()||line.startsWith('#')||line.startsWith(';'))continue;
  if(line.startsWith('[')&&line.endsWith(']')){result.append({{"[id]",line.mid(1,line.size()-2)}});continue;}
  const auto equals=line.indexOf('=');
  if(equals>0&&!result.isEmpty())result.last()[line.left(equals).trimmed().toLower()]=line.mid(equals+1).trimmed();
 }
 return result;
}
bool falseValue(const QString& value) {return QStringList{"no","false","0","off"}.contains(value.trimmed().toLower());}
}
UpdaterChoice softwareUpdater(const QString& currentDesktop,const std::function<QString(const QString&)>& which) {
 const auto desktops=currentDesktop.toLower().split(':',Qt::SkipEmptyParts);
 const auto preferred=desktops.contains("x-cinnamon")||desktops.contains("cinnamon")?QString("mintupdate"):
  desktops.contains("kde")?QString("plasma-discover"):desktops.contains("gnome")?QString("gnome-software"):QString();
 QList<Updater> order;
 for(const auto& updater:updaters)if(updater.id==preferred)order.append(updater);
 for(const auto& updater:updaters)if(updater.id!=preferred)order.append(updater);
 for(auto updater:order) {
  const auto path=which(updater.program);
  if(!path.isEmpty()){updater.program=path;return {true,updater};}
 }
 return {};
}
QJsonObject systemAction(const QString& target,const QString& root,const QString& repository,const UpdaterChoice& choice) {
 const bool deb=debTargets.contains(target),rpm=target=="fedora44";
 if(!deb&&!rpm)return {{"kind","download"},{"repository","none"}};
 const QString file=deb?"/etc/apt/sources.list.d/putgd.sources":"/etc/yum.repos.d/putgd.repo";
 bool exists=false;const auto text=readSmall(root+file,exists);
 if(!exists)return {{"kind","download"},{"repository","absent"},{"file",file}};
 // Only this build's repository counts; anything else is the user's own setup.
 const auto base=repository+(deb?"/apt/"+target:QString("/rpm/fedora/44/x86_64/"));
 QString state="other",channel;
 if(deb) {
  for(const auto& stanza:stanzas(text)) {
   if(!stanza.value("types").split(' ',Qt::SkipEmptyParts).contains("deb"))continue;
   if(!stanza.value("uris").split(QRegularExpression("\\s+"),Qt::SkipEmptyParts).contains(base))continue;
   channel=stanza.value("suites").section(' ',0,0);
   state=stanza.contains("enabled")&&falseValue(stanza.value("enabled"))?"disabled":"managed";
   if(state=="managed")break;
  }
 } else {
  for(const auto& section:sections(text)) {
   const auto url=section.value("baseurl");
   if(!url.startsWith(base))continue;
   channel=url.mid(base.size()).section('/',0,0);
   state=section.contains("enabled")&&falseValue(section.value("enabled"))?"disabled":"managed";
   if(state=="managed")break;
  }
 }
 if(state!="managed")return {{"kind","download"},{"repository",state},{"file",file}};
 QJsonObject result{{"kind","software-updates"},{"manager",deb?"apt":"dnf"},{"repository","managed"},{"file",file},{"channel",channel},
  {"command",deb?"sudo apt update && sudo apt upgrade":"sudo dnf upgrade --refresh"},{"updater",QJsonValue()}};
 if(choice.found)result["updater"]=QJsonObject{{"id",choice.updater.id},{"name",choice.updater.name},{"program",choice.updater.program},
  {"arguments",QJsonArray::fromStringList(choice.updater.arguments)}};
 return result;
}
QJsonObject systemAction(const QString& target) {
 const auto choice=softwareUpdater(qEnvironmentVariable("XDG_CURRENT_DESKTOP"),[](const QString& program){return QStandardPaths::findExecutable(program);});
 return systemAction(target,qEnvironmentVariable("PUTGD_UPDATE_SYSTEM_ROOT"),packageRepository(),choice);
}
bool openSoftwareUpdates(const QJsonObject& action) {
 const auto updater=action["updater"].toObject();
 if(action["kind"]!="software-updates"||updater.isEmpty())return false;
 // Only the three updaters this file knows, at the path it found.
 const auto id=updater["id"].toString();
 const auto known=std::find_if(updaters.cbegin(),updaters.cend(),[&](const Updater& u){return u.id==id;});
 if(known==updaters.cend())return false;
 // The updater outlives the short-lived putgd-cli child that starts it, so it
 // must not inherit that child's pipes: Discover logs to stderr and died of
 // SIGPIPE once the resident closed them (k72).
 QProcess process;process.setProgram(updater["program"].toString());process.setArguments(known->arguments);
 process.setStandardInputFile(QProcess::nullDevice());process.setStandardOutputFile(QProcess::nullDevice());process.setStandardErrorFile(QProcess::nullDevice());
 return process.startDetached();
}
}
