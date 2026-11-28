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
#include <QHash>
#include <cmath>
#include "editor/FalseAlarmReport.h"
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
// The reports folder as it stands: a directory of this user's, not a link. Nothing is followed or changed first.
bool usableFolder(const QString& path){
#ifdef Q_OS_UNIX
 struct stat s{};return ::lstat(QFile::encodeName(path).constData(),&s)==0&&S_ISDIR(s.st_mode)&&s.st_uid==::getuid();
#else
 return QFileInfo(path).isDir();
#endif
}
// The folder, owner-only, a directory of this user's and not a link: checked before it is made private, so a link
// or someone else's folder is refused, never followed.
bool ownedFolder(const QString& path){
#ifdef Q_OS_UNIX
 struct stat s{};
 if(::lstat(QFile::encodeName(path).constData(),&s)==0){if(!S_ISDIR(s.st_mode)||s.st_uid!=::getuid())return false;}
 else if(errno!=ENOENT||!QDir().mkpath(path))return false;
 if(!usableFolder(path)||!QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner))return false;
 return ::lstat(QFile::encodeName(path).constData(),&s)==0&&S_ISDIR(s.st_mode)&&s.st_uid==::getuid()&&(s.st_mode&0077)==0;
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
// Removes a report folder by its known files, then the folder; a folder holding anything else stays. `gone` says
// whether the folder is gone afterwards.
qint64 removeReport(const QString& path,bool* gone=nullptr){
 qint64 bytes=0;QDir folder(path);
 for(const auto& file:reportFiles()){const QFileInfo info(folder.filePath(file));if(info.isFile()&&!info.isSymLink()){const auto size=info.size();if(QFile::remove(info.filePath()))bytes+=size;}}
 QDir().rmdir(path);
 if(gone)*gone=!QFileInfo::exists(path)&&!QFileInfo(path).isSymLink();
 return bytes;
}
// Hidden folders a write left (".putgd-false-alarm-…"), by name only.
QStringList partialFolders(const QString& directory){
 QStringList out;
 for(const auto& info:QDir(directory).entryInfoList(QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot|QDir::NoSymLinks))
  if(info.fileName().startsWith('.')&&namePattern().match(info.fileName().mid(1)).hasMatch())out.append(info.absoluteFilePath());
 return out;
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
 // Known names only: an unexpected value is "unknown", never the raw variable.
 static const QStringList desktops{"KDE","GNOME","X-Cinnamon","Cinnamon","Budgie","Hyprland","XFCE","MATE","LXQt","LXDE","Unity","ubuntu","Pantheon","Deepin","sway","wlroots","niri","COSMIC","Enlightenment"};
 QStringList desktop;for(const auto& part:qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':',Qt::SkipEmptyParts))desktop.append(desktops.contains(part)?part:QStringLiteral("unknown"));
 const auto session=qEnvironmentVariable("XDG_SESSION_TYPE");
 static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
 for(auto it=os.begin();it!=os.end();++it)if(!token.match(it.value().toString()).hasMatch())it.value()=QStringLiteral("unknown");
 return {{"desktop",desktop.isEmpty()?QStringLiteral("unknown"):desktop.mid(0,4).join(':')},{"sessionType",QStringList{"x11","wayland","tty","mir"}.contains(session)?session:QStringLiteral("unknown")},{"os",os},
  {"kernel",QSysInfo::kernelVersion().left(128)},{"architecture",QSysInfo::currentCpuArchitecture()},{"qtRuntime",QString::fromLatin1(qVersion())}};
}
namespace {
// The helper's part, rebuilt field by field from its schema (FalseAlarmReport.h): only known fields, each of its
// type and within its length; anything else is dropped. Never stored as the helper sent it.
bool whole(const QJsonValue& value,qint64 low,qint64 high){const double d=value.toDouble(-1e300);return value.isDouble()&&d==std::floor(d)&&d>=double(low)&&d<=double(high);}
QJsonValue integer(const QJsonValue& value,qint64 low,qint64 high){return whole(value,low,high)?QJsonValue(value.toDouble()):QJsonValue();}
QJsonValue number(const QJsonValue& value,double low,double high){const double d=value.toDouble(-1e300);return value.isDouble()&&std::isfinite(d)&&d>=low&&d<=high?QJsonValue(d):QJsonValue();}
QString text(const QJsonValue& value,qsizetype most){return value.isString()?value.toString().left(most):QString();}
// A box [x, y, w, h], or Undefined: a bad one is dropped, never kept as null.
QJsonValue box(const QJsonValue& value){
 const auto array=value.toArray();if(!value.isArray()||array.size()!=4)return QJsonValue(QJsonValue::Undefined);
 QJsonArray out;for(int i=0;i<4;++i){if(!whole(array.at(i),i<2?-1000000:0,1000000))return QJsonValue(QJsonValue::Undefined);out.append(array.at(i));}
 return out;
}
const QStringList& kinds(){static const QStringList list{"email","phone","ip","card","key","name","list"};return list;}
QJsonObject counts(const QJsonValue& value,const QStringList& names){
 QJsonObject out;const auto object=value.toObject();
 for(const auto& name:names)if(whole(object.value(name),1,1000000))out[name]=object.value(name);
 return out;
}
QJsonArray kindList(const QJsonValue& value){QJsonArray out;for(const auto& entry:value.toArray())if(kinds().contains(entry.toString())&&!out.contains(entry))out.append(entry);return out;}
QJsonArray strings(const QJsonValue& value,const QRegularExpression& pattern,int most){
 QJsonArray out;for(const auto& entry:value.toArray()){if(out.size()>=most)break;if(entry.isString()&&pattern.match(entry.toString()).hasMatch())out.append(entry);}
 return out;
}
void put(QJsonObject& into,const char* key,const QJsonValue& value){if(!value.isUndefined()&&!value.isNull())into[key]=value;}
QJsonObject cleanCheck(const QJsonObject& in){
 QJsonObject out;
 out["categories"]=kindList(in.value("categories"));out["automatic"]=kindList(in.value("automatic"));
 if(const auto codes=counts(in.value("codes"),{"otp","wifi","key"});!codes.isEmpty())out["codes"]=codes;
 return out;
}
QJsonObject cleanRecognition(const QJsonObject& in){
 static const QRegularExpression language(QStringLiteral("^[A-Za-z0-9_+-]{1,32}$")),printable(QStringLiteral("^[ -~]{1,64}$"));
 QJsonObject out;const auto engine=in.value("engine").toObject();
 QJsonObject cleanEngine{{"name","tesseract"}};
 if(printable.match(text(engine.value("version"),64)).hasMatch())cleanEngine["version"]=text(engine.value("version"),64);
 put(cleanEngine,"scale",number(engine.value("scale"),0,64));put(cleanEngine,"bands",integer(engine.value("bands"),0,100000));
 if(engine.value("banded").isBool())cleanEngine["banded"]=engine.value("banded");
 out["engine"]=cleanEngine;
 out["languagesUsed"]=strings(in.value("languagesUsed"),language,16);out["languagesSkipped"]=strings(in.value("languagesSkipped"),language,16);
 if(in.value("detection").isObject()){
  const auto d=in.value("detection").toObject();QJsonObject detection;
  for(const auto* key:{"script","language","outcome"})if(printable.match(text(d.value(key),64)).hasMatch())detection[key]=text(d.value(key),64);
  out["detection"]=detection;
 }
 if(in.value("truncated").isBool())out["truncated"]=in.value("truncated");
 for(const auto* key:{"engineStartMs","recognizeMs","totalMs"})put(out,key,integer(in.value(key),0,3600000));
 for(const auto* key:{"candidates","codes"})put(out,key,integer(in.value(key),0,10000000));
 if(const auto offset=in.value("cropOffset").toArray();offset.size()==2&&whole(offset.at(0),0,1000000)&&whole(offset.at(1),0,1000000))out["cropOffset"]=offset;
 return out;
}
QJsonArray cleanFindings(const QJsonArray& in,int words){
 static const QRegularExpression rule(QStringLiteral("^[a-z0-9_.,]{1,200}$"));
 QJsonArray out;
 for(const auto& value:in){
  if(out.size()>=10000)break;
  const auto f=value.toObject();if(!kinds().contains(f.value("kind").toString()))continue;
  QJsonObject clean{{"kind",f.value("kind")},{"automatic",f.value("automatic").toBool(false)},{"covered",f.value("covered").toBool(false)},{"text",text(f.value("text"),4096)}};
  const auto r=text(f.value("rule"),200);clean["rule"]=rule.match(r).hasMatch()?r:f.value("kind").toString();
  QJsonArray indexes;for(const auto& w:f.value("words").toArray()){if(indexes.size()>=10000)break;if(whole(w,-1,words-1))indexes.append(w);}clean["words"]=indexes;
  QJsonArray lines;for(const auto& l:f.value("lines").toArray()){if(lines.size()>=1000)break;if(whole(l,0,10000000))lines.append(l);}clean["lines"]=lines;
  put(clean,"box",box(f.value("box")));
  QJsonArray marks;for(const auto& m:f.value("marks").toArray()){if(marks.size()>=100)break;if(const auto b=box(m);!b.isUndefined())marks.append(b);}clean["marks"]=marks;
  const auto confidence=f.value("confidence").toObject();QJsonObject c;put(c,"lowest",integer(confidence.value("lowest"),0,100));put(c,"mean",number(confidence.value("mean"),0,100));clean["confidence"]=c;
  out.append(clean);
 }
 return out;
}
QJsonObject cleanWords(const QJsonObject& in){
 QJsonArray list;
 for(const auto& value:in.value("list").toArray()){
  if(list.size()>=editor::falseAlarm::maximumWords)break;
  const auto w=value.toObject();const auto b=box(w.value("box"));if(b.isUndefined()||!w.value("text").isString())continue;
  QJsonObject clean{{"text",text(w.value("text"),512)},{"box",b},{"confidence",integer(w.value("confidence"),0,100).toDouble(0)},{"line",integer(w.value("line"),0,10000000).toDouble(0)}};
  if(w.value("second").toBool(false))clean["second"]=true;
  list.append(clean);
 }
 QJsonObject out{{"list",list},{"listed",list.size()}};
 put(out,"total",integer(in.value("total"),0,100000000));put(out,"omitted",integer(in.value("omitted"),0,100000000));
 return out;
}
}
QJsonObject FalseAlarmReports::compose(const QJsonObject& helper,const QJsonObject& resident,Error& error){
 static const QStringList keys{"reportedFrom","image","check","recognition","findings","words","codes"};
 bool shaped=helper.size()==keys.size();
 for(const auto& key:keys)shaped=shaped&&helper.contains(key);
 shaped=shaped&&(helper.value("reportedFrom")=="prompt"||helper.value("reportedFrom")=="editor")&&helper.value("image").isObject()&&helper.value("check").isObject()
  &&helper.value("recognition").isObject()&&helper.value("findings").isArray()&&helper.value("words").isObject()&&helper.value("codes").isArray();
 const auto image=helper.value("image").toObject();
 shaped=shaped&&whole(image.value("width"),1,1000000)&&whole(image.value("height"),1,1000000);
 if(!shaped){error={"edit_output_invalid","The editor's report is not in the expected form",{}};return {};}
 const auto words=cleanWords(helper.value("words").toObject());
 QJsonArray codes;
 for(const auto& value:helper.value("codes").toArray()){if(codes.size()>=64)break;const auto c=value.toObject();const auto b=box(c.value("box"));if(QStringList{"otp","wifi","key"}.contains(c.value("kind").toString())&&!b.isUndefined())codes.append(QJsonObject{{"kind",c.value("kind")},{"box",b}});}
 QJsonObject report{{"reportedFrom",helper.value("reportedFrom")},{"image",QJsonObject{{"width",image.value("width")},{"height",image.value("height")}}},
  {"check",cleanCheck(helper.value("check").toObject())},{"recognition",cleanRecognition(helper.value("recognition").toObject())},
  {"findings",cleanFindings(helper.value("findings").toArray(),int(words.value("list").toArray().size()))},{"words",words},{"codes",codes}};
 // The resident's own fields win; its `check` (the findings it validated itself) joins the helper's.
 for(auto it=resident.begin();it!=resident.end();++it){
  if(it.key()=="check"){auto check=report.value("check").toObject();const auto mine=it.value().toObject();for(auto c=mine.begin();c!=mine.end();++c)check[c.key()]=c.value();report["check"]=check;}
  else report[it.key()]=it.value();
 }
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
 if(!usableFolder(directory_))return 0;
 const int days=days_.load();const qint64 budget=bytes_.load();int removed=0;
 auto list=entries(directory_);
 // The newest report always stays, like the log file being written.
 const auto newest=keep.isEmpty()&&!list.isEmpty()?list.last().name:keep;
 for(auto it=list.begin();it!=list.end();){if(it->name!=newest&&it->created<now.addDays(-days)){removeReport(it->path);++removed;it=list.erase(it);}else ++it;}
 qint64 total=0;for(const auto& entry:list)total+=entry.bytes;
 for(auto it=list.begin();it!=list.end()&&total>budget;){if(it->name==newest){++it;continue;}removeReport(it->path);total-=it->bytes;++removed;it=list.erase(it);}
 // A hidden folder left by a write that never finished (a crash), after an hour.
 for(const auto& path:partialFolders(directory_))if(QFileInfo(path).lastModified().toUTC()<now.addSecs(-3600))removeReport(path);
 return removed;
}
FalseAlarmReports::Removed FalseAlarmReports::clear() const {
 std::lock_guard guard(lock());Removed removed;
 if(!QFileInfo::exists(directory_)&&!QFileInfo(directory_).isSymLink())return removed;
 // A folder that isn't this user's own directory is never followed: everything in it counts as left.
 if(!usableFolder(directory_)){removed.left=-1;return removed;}
 QStringList folders;for(const auto& entry:entries(directory_))folders.append(entry.path);
 const auto partial=partialFolders(directory_);
 for(const auto& path:folders+partial){
  bool gone=false;const auto bytes=removeReport(path,&gone);removed.bytes+=bytes;
  if(!gone)++removed.left;else if(!partial.contains(path))++removed.reports;
 }
 return removed;
}
QJsonObject FalseAlarmReports::list() const {
 std::lock_guard guard(lock());
 QJsonArray reports;qint64 total=0;
 auto all=usableFolder(directory_)?entries(directory_):QList<Entry>{};
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
FalseAlarmReports::Selection FalseAlarmReports::select(const QString& directory,const QDateTime& from,const QDateTime& to,const QString& operationId,qint64 budget,bool read,const QStringList* only){
 std::lock_guard guard(lock());Selection selection;
 if(!usableFolder(directory))return selection;
 auto all=entries(directory);std::reverse(all.begin(),all.end());
 for(const auto& entry:std::as_const(all)){
  if(operationId.isEmpty()&&(entry.created<from||entry.created>to))continue;
  if(!operationId.isEmpty()&&readReport(entry.path)["operationId"].toString()!=operationId)continue;
  if(only&&!only->contains(entry.name))continue;
  ++selection.eligible;
  // Whole reports only: one that doesn't fit what is left is counted, and an older, smaller one may still fit.
  if(selection.bytes+entry.bytes>budget){++selection.omitted;continue;}
  Packed packed{entry.name,{},entry.bytes};
  // A whole report only: report.json, covered.png and exactly one original, each as report.json records it.
  const QDir folder(entry.path);
  const bool png=QFileInfo(folder.filePath("original.png")).isFile(),jpg=QFileInfo(folder.filePath("original.jpg")).isFile();
  const auto report=readReport(entry.path);
  QHash<QString,QJsonObject> recorded;for(const auto& value:report.value("files").toArray())recorded.insert(value.toObject().value("name").toString(),value.toObject());
  const auto original=png?QStringLiteral("original.png"):QStringLiteral("original.jpg");
  if(png==jpg||report.isEmpty()||!recorded.contains(original)||!recorded.contains("covered.png")||recorded.size()!=2){++selection.invalid;continue;}
  if(read){
   bool sound=true;
   for(const auto& name:{QStringLiteral("report.json"),original,QStringLiteral("covered.png")}){
    QFile part(folder.filePath(name));
    if(QFileInfo(part.fileName()).isSymLink()||!part.open(QIODevice::ReadOnly)||part.size()>budget){sound=false;break;}
    auto bytes=part.readAll();
    if(name!="report.json"&&(double(bytes.size())!=recorded.value(name).value("bytes").toDouble()||sha256(bytes)!=recorded.value(name).value("sha256").toString())){sound=false;break;}
    packed.files.append({name,std::move(bytes)});
   }
   if(!sound){++selection.invalid;continue;}
   qint64 bytes=0;for(const auto& [name,data]:packed.files)bytes+=data.size();packed.bytes=bytes;
   if(selection.bytes+bytes>budget){++selection.omitted;continue;}
  }
  selection.bytes+=packed.bytes;selection.reports.append(std::move(packed));
 }
 return selection;
}
}
