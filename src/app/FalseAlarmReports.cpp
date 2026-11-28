#include "FalseAlarmReports.h"
#include <QSysInfo>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimeZone>
#include <algorithm>
#include <mutex>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace putgd {
namespace {
std::mutex& lock(){static std::mutex mutex;return mutex;}
const QRegularExpression& namePattern(){static const QRegularExpression pattern(QStringLiteral("^putgd-false-alarm-(\\d{8}T\\d{9})Z-[0-9a-f]{8}$"));return pattern;}
// The files a report folder may hold: nothing else is ever removed from it.
const QStringList& reportFiles(){static const QStringList files{"original.png","original.jpg","covered.png","report.json"};return files;}
QString iso(const QDateTime& time){return time.toUTC().toString(Qt::ISODateWithMs);}
QDateTime createdOf(const QString& name){
 const auto match=namePattern().match(name);if(!match.hasMatch())return {};
 const auto stamp=match.captured(1);
 const auto date=QDate::fromString(stamp.left(8),QStringLiteral("yyyyMMdd"));const auto time=QTime::fromString(stamp.mid(9),QStringLiteral("HHmmsszzz"));
 return date.isValid()&&time.isValid()?QDateTime(date,time,QTimeZone::utc()):QDateTime();
}
// The folder, owner-only, a directory of this user's and not a link.
bool ownedFolder(const QString& path){
#ifdef Q_OS_UNIX
 if(!QDir().mkpath(path)||!QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner))return false;
 struct stat s{};return ::lstat(QFile::encodeName(path).constData(),&s)==0&&S_ISDIR(s.st_mode)&&s.st_uid==::getuid()&&(s.st_mode&0077)==0;
#else
 return QDir().mkpath(path);
#endif
}
bool writePrivate(const QString& path,const QByteArray& bytes){
#ifdef Q_OS_UNIX
 const int fd=::open(QFile::encodeName(path).constData(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
 if(fd<0)return false;
 qint64 written=0;
 while(written<bytes.size()){const auto n=::write(fd,bytes.constData()+written,size_t(bytes.size()-written));if(n<0&&errno==EINTR)continue;if(n<=0)break;written+=n;}
 const bool ok=written==bytes.size()&&::fchmod(fd,0600)==0&&::fsync(fd)==0;
 ::close(fd);return ok;
#else
 QFile file(path);return file.open(QIODevice::WriteOnly|QIODevice::NewOnly)&&file.write(bytes)==bytes.size();
#endif
}
// Removes a report folder by its known files, then the folder; a folder holding anything else stays.
qint64 removeReport(const QString& path){
 qint64 bytes=0;QDir folder(path);
 for(const auto& file:reportFiles()){const QFileInfo info(folder.filePath(file));if(info.isFile()&&!info.isSymLink()){const auto size=info.size();if(QFile::remove(info.filePath()))bytes+=size;}}
 QDir().rmdir(path);
 return bytes;
}
struct Entry {QString name,path;QDateTime created;qint64 bytes=0;};
QList<Entry> entries(const QString& directory){
 QList<Entry> out;
 QDir dir(directory);
 for(const auto& info:dir.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks,QDir::Name)){
  const auto created=createdOf(info.fileName());if(!created.isValid())continue;
  Entry entry{info.fileName(),info.absoluteFilePath(),created,0};
  for(const auto& file:reportFiles()){const QFileInfo part(QDir(entry.path).filePath(file));if(part.isFile()&&!part.isSymLink())entry.bytes+=part.size();}
  out.append(entry);
 }
 // Oldest first: names sort by time.
 return out;
}
QJsonObject readReport(const QString& path){
 QFile file(QDir(path).filePath("report.json"));
 if(QFileInfo(file.fileName()).isSymLink()||!file.open(QIODevice::ReadOnly)||file.size()>16LL*1024*1024)return {};
 return QJsonDocument::fromJson(file.readAll()).object();
}
QString sha256(const QByteArray& bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
}
bool FalseAlarmReports::reportName(const QString& name){return namePattern().match(name).hasMatch();}
QJsonObject FalseAlarmReports::settingsSummary(const Settings& settings,const QString& provider){
 QJsonObject kinds;
 for(const auto& category:Settings::sensitiveTextCategoryNames())
  kinds[category]=settings.sensitiveTextCover.contains(category)?QStringLiteral("redact"):settings.sensitiveTextCategories.contains(category)?QStringLiteral("warn"):QStringLiteral("off");
 return {{"sensitiveTextCheck",QJsonObject{{"enabled",settings.sensitiveTextCheck},{"kinds",kinds}}},
  {"textRecognitionLanguages",QJsonArray::fromStringList(settings.textRecognitionLanguages)},{"unhingedMode",settings.unhingedMode},
  // The user's own words and patterns: how many, never what.
  {"quickRedact",QJsonObject{{"terms",settings.quickRedactTerms.size()},{"patterns",settings.quickRedactPatterns.size()}}},
  {"captureStyle",settings.captureStyle.enabled},{"keepLocalCopy",settings.archive.keepUploads},
  {"destination",QJsonObject{{"provider",provider}}}};
}
QJsonObject FalseAlarmReports::environment(){
 QJsonObject os;
 QFile release(QStringLiteral("/etc/os-release"));
 if(release.open(QIODevice::ReadOnly)&&release.size()<65536)for(const auto& raw:release.readAll().split('\n')){
  const auto line=QString::fromUtf8(raw).trimmed();const auto at=line.indexOf('=');if(at<1)continue;
  const auto key=line.left(at);if(key!="ID"&&key!="VERSION_ID")continue;
  auto value=line.mid(at+1);if(value.size()>=2&&(value.startsWith('"')||value.startsWith('\'')))value=value.mid(1,value.size()-2);
  os[key]=value.left(64);
 }
 return {{"desktop",qEnvironmentVariable("XDG_CURRENT_DESKTOP").left(64)},{"sessionType",qEnvironmentVariable("XDG_SESSION_TYPE").left(32)},{"os",os},
  {"kernel",QSysInfo::kernelVersion().left(128)},{"architecture",QSysInfo::currentCpuArchitecture()},{"qtRuntime",QString::fromLatin1(qVersion())}};
}
QJsonObject FalseAlarmReports::compose(const QJsonObject& helper,const QJsonObject& resident,Error& error){
 static const QStringList keys{"reportedFrom","image","check","recognition","findings","words","codes"};
 bool shaped=helper.size()==keys.size();
 for(const auto& key:keys)shaped=shaped&&helper.contains(key);
 shaped=shaped&&(helper.value("reportedFrom")=="prompt"||helper.value("reportedFrom")=="editor")&&helper.value("image").isObject()&&helper.value("check").isObject()
  &&helper.value("recognition").isObject()&&helper.value("findings").isArray()&&helper.value("words").isObject()&&helper.value("codes").isArray();
 if(!shaped){error={"edit_output_invalid","The editor's report is not in the expected form",{}};return {};}
 auto report=helper;
 for(auto it=resident.begin();it!=resident.end();++it)report[it.key()]=it.value();
 return report;
}
FalseAlarmReports::FalseAlarmReports(QString directory):directory_(std::move(directory)){}
FalseAlarmReports::Saved FalseAlarmReports::save(const QByteArray& original,const QString& originalSuffix,const QByteArray& covered,QJsonObject report,Error& error,const QDateTime& now) const {
 Saved saved;
 if(original.isEmpty()||(originalSuffix!="png"&&originalSuffix!="jpg")){error={"invalid_request","A false-alarm report needs the original image",{}};return saved;}
 std::lock_guard guard(lock());
 if(!ownedFolder(directory_)){error={"false_alarm_unavailable","Couldn't save the report: the diagnostics folder isn't available.",{}};return saved;}
 const auto stamp=now.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz"));
 QString name;
 for(int attempt=0;attempt<16&&name.isEmpty();++attempt){
  const auto candidate=QStringLiteral("putgd-false-alarm-%1Z-%2").arg(stamp).arg(QRandomGenerator::system()->generate(),8,16,QLatin1Char('0'));
  if(!QFileInfo::exists(QDir(directory_).filePath(candidate))&&!QFileInfo::exists(QDir(directory_).filePath("."+candidate)))name=candidate;
 }
 if(name.isEmpty()){error={"false_alarm_unavailable","Couldn't save the report. Try again.",{}};return saved;}
 // Written under a hidden name, then renamed: a list or a package never sees half a report.
 const auto partial=QDir(directory_).filePath("."+name),final=QDir(directory_).filePath(name);
 if(!QDir().mkdir(partial)||!QFile::setPermissions(partial,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)){error={"false_alarm_unavailable","Couldn't save the report. Check free disk space and try again.",{}};return saved;}
 const auto originalName=QStringLiteral("original.")+originalSuffix;
 QJsonArray files{QJsonObject{{"name",originalName},{"bytes",double(original.size())},{"sha256",sha256(original)}}};
 if(!covered.isEmpty())files.append(QJsonObject{{"name","covered.png"},{"bytes",double(covered.size())},{"sha256",sha256(covered)}});
 report["files"]=files;report["name"]=name;
 const auto json=QJsonDocument(report).toJson(QJsonDocument::Indented);
 const bool ok=writePrivate(QDir(partial).filePath(originalName),original)&&(covered.isEmpty()||writePrivate(QDir(partial).filePath("covered.png"),covered))
  &&writePrivate(QDir(partial).filePath("report.json"),json)&&QDir().rename(partial,final);
 if(!ok){removeReport(partial);error={"false_alarm_write_failed","Couldn't save the report. Check free disk space and try again.",{}};return saved;}
 saved.name=name;saved.path=final;saved.bytes=original.size()+covered.size()+json.size();
 saved.pruned=[&]{
  // Retention as for the logs, with the lock already held.
  const int days=days_.load();const qint64 budget=bytes_.load();int removed=0;
  auto list=entries(directory_);
  for(auto it=list.begin();it!=list.end();){if(it->name!=name&&it->created<now.addDays(-days)){removeReport(it->path);++removed;it=list.erase(it);}else ++it;}
  qint64 total=0;for(const auto& entry:list)total+=entry.bytes;
  for(auto it=list.begin();it!=list.end()&&total>budget;){if(it->name==name){++it;continue;}removeReport(it->path);total-=it->bytes;++removed;it=list.erase(it);}
  return removed;
 }();
 return saved;
}
int FalseAlarmReports::prune(const QDateTime& now,const QString& keep) const {
 std::lock_guard guard(lock());
 if(!QFileInfo(directory_).isDir())return 0;
 const int days=days_.load();const qint64 budget=bytes_.load();int removed=0;
 auto list=entries(directory_);
 // The newest report always stays, like the log file being written.
 const auto newest=keep.isEmpty()&&!list.isEmpty()?list.last().name:keep;
 for(auto it=list.begin();it!=list.end();){if(it->name!=newest&&it->created<now.addDays(-days)){removeReport(it->path);++removed;it=list.erase(it);}else ++it;}
 qint64 total=0;for(const auto& entry:list)total+=entry.bytes;
 for(auto it=list.begin();it!=list.end()&&total>budget;){if(it->name==newest){++it;continue;}removeReport(it->path);total-=it->bytes;++removed;it=list.erase(it);}
 // A hidden folder left by a write that never finished (a crash), after an hour.
 QDir dir(directory_);
 for(const auto& info:dir.entryInfoList(QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot|QDir::NoSymLinks)){
  if(!info.fileName().startsWith('.')||!reportName(info.fileName().mid(1)))continue;
  if(info.lastModified().toUTC()<now.addSecs(-3600))removeReport(info.absoluteFilePath());
 }
 return removed;
}
FalseAlarmReports::Removed FalseAlarmReports::clear() const {
 std::lock_guard guard(lock());Removed removed;
 if(!QFileInfo(directory_).isDir())return removed;
 for(const auto& entry:entries(directory_)){removed.bytes+=removeReport(entry.path);++removed.reports;}
 return removed;
}
QJsonObject FalseAlarmReports::list() const {
 std::lock_guard guard(lock());
 QJsonArray reports;qint64 total=0;
 auto all=QFileInfo(directory_).isDir()?entries(directory_):QList<Entry>{};
 std::reverse(all.begin(),all.end());
 for(const auto& entry:std::as_const(all)){
  total+=entry.bytes;const auto report=readReport(entry.path);
  QJsonObject item{{"name",entry.name},{"created",iso(entry.created)},{"bytes",double(entry.bytes)}};
  for(const auto* key:{"operationId","reportedFrom"})if(report[key].isString())item[key]=report[key];
  const auto check=report["check"].toObject();
  if(check["found"].isObject())item["found"]=check["found"];
  if(check["covered"].isObject())item["covered"]=check["covered"];
  reports.append(item);
 }
 return {{"directory",directory_},{"reports",reports},{"count",reports.size()},{"bytes",double(total)},
  {"retention",QJsonObject{{"days",days_.load()},{"bytes",double(bytes_.load())}}}};
}
FalseAlarmReports::Selection FalseAlarmReports::select(const QString& directory,const QDateTime& from,const QDateTime& to,const QString& operationId,qint64 budget,bool read){
 std::lock_guard guard(lock());Selection selection;
 if(!QFileInfo(directory).isDir())return selection;
 auto all=entries(directory);std::reverse(all.begin(),all.end());
 for(const auto& entry:std::as_const(all)){
  if(operationId.isEmpty()&&(entry.created<from||entry.created>to))continue;
  if(!operationId.isEmpty()&&readReport(entry.path)["operationId"].toString()!=operationId)continue;
  ++selection.eligible;
  // Whole reports only: one that doesn't fit what is left is counted, and an older, smaller one may still fit.
  if(selection.bytes+entry.bytes>budget){++selection.omitted;continue;}
  Packed packed{entry.name,{},entry.bytes};
  if(read){
   bool whole=true;
   for(const auto& file:reportFiles()){
    QFile part(QDir(entry.path).filePath(file));
    if(!part.exists())continue;
    if(QFileInfo(part.fileName()).isSymLink()||!part.open(QIODevice::ReadOnly)){whole=false;break;}
    packed.files.append({file,part.readAll()});
   }
   if(!whole||packed.files.isEmpty()){++selection.omitted;continue;}
   qint64 bytes=0;for(const auto& [name,data]:packed.files)bytes+=data.size();packed.bytes=bytes;
   if(selection.bytes+bytes>budget){++selection.omitted;continue;}
  }
  selection.bytes+=packed.bytes;selection.reports.append(std::move(packed));
 }
 return selection;
}
}
