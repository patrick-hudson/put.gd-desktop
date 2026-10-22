#include "Log.h"
#include "CrashRecord.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace putgd {
Q_LOGGING_CATEGORY(logApp,"putgd.app",QtInfoMsg)
Q_LOGGING_CATEGORY(logOperation,"putgd.operation",QtInfoMsg)
Q_LOGGING_CATEGORY(logS3,"putgd.s3",QtInfoMsg)
Q_LOGGING_CATEGORY(logProton,"putgd.proton",QtInfoMsg)
Q_LOGGING_CATEGORY(logCapture,"putgd.capture",QtInfoMsg)
Q_LOGGING_CATEGORY(logUi,"putgd.ui",QtInfoMsg)
Q_LOGGING_CATEGORY(logIpc,"putgd.ipc",QtInfoMsg)
Q_LOGGING_CATEGORY(logSettings,"putgd.settings",QtInfoMsg)
Q_LOGGING_CATEGORY(logHistory,"putgd.history",QtInfoMsg)
Q_LOGGING_CATEGORY(logRecording,"putgd.recording",QtInfoMsg)
Q_LOGGING_CATEGORY(logClipboard,"putgd.clipboard",QtInfoMsg)
namespace logging {
namespace {
const QRegularExpression& fileName() {static const QRegularExpression value("^putgd-\\d{8}T\\d{9}Z-\\d+-\\d+\\.jsonl$");return value;}
QString levelName(QtMsgType type) {
 switch(type) {case QtDebugMsg:return "debug";case QtInfoMsg:return "info";case QtWarningMsg:return "warning";case QtCriticalMsg:return "error";case QtFatalMsg:return "fatal";}
 return "info";
}
struct Rule {QRegularExpression pattern;QString replacement;};
const QList<Rule>& rules() {
 static const QList<Rule> value{
  {QRegularExpression("-----BEGIN [A-Z ]*PRIVATE KEY-----.*?(-----END [A-Z ]*PRIVATE KEY-----|$)",QRegularExpression::DotMatchesEverythingOption),"<redacted-private-key>"},
  {QRegularExpression("(?i)\\b((?:proxy-)?authorization)(\\s*[:=]\\s*)[^\\r\\n]*"),"\\1\\2<redacted>"},
  {QRegularExpression("(?i)\\b(x-amz-security-token|x-amz-signature|x-amz-credential|x-goog-signature|signature|credential|sig)(\\s*[:=]\\s*)[^\\s&,;\"'<>]+"),"\\1\\2<redacted>"},
  // Presigned queries and share-link fragments (Proton keeps the share
  // password in the fragment). The scheme, host and path stay.
  {QRegularExpression("(?i)\\b((?:https?|wss?|s3)://[^\\s?#\"'<>]+)[?#][^\\s\"'<>]*"),"\\1?<redacted>"},
  {QRegularExpression("(?i)\\b(aws_secret_access_key|aws_session_token|secret_?access_?key|secret_?key|session_?token|security_?token|access_?token|refresh_?token|id_?token|share_?password|password|passphrase|passwd|api_?key|client_?secret|private_?key|cookie|token|secret)(\"?\\s*[:=]\\s*\"?)[^\\s\",;&}\\]]+"),"\\1\\2<redacted>"},
  {QRegularExpression("(?i)\\b(bearer|basic)\\s+[A-Za-z0-9._~+/=-]{8,}"),"\\1 <redacted>"},
  {QRegularExpression("\\b(?:AKIA|ASIA|AGPA|AIDA|AROA|AIPA|ANPA|ANVA|ABIA|ACCA)[A-Z0-9]{16}\\b"),"<access-key-id>"},
  {QRegularExpression("(?<![A-Za-z0-9/+=])[A-Za-z0-9/+=]{120,}"),"<redacted-blob>"}};
 return value;
}
// 40 characters of mixed-case base64 alphabet: the shape of an AWS secret key.
// Lowercase hex (commit and build IDs) never matches.
QString redactSecretShapes(QString text) {
 static const QRegularExpression shape("(?<![A-Za-z0-9/+=])[A-Za-z0-9/+]{40}(?![A-Za-z0-9/+=])");
 qsizetype offset=0;
 for(;;) {
  const auto match=shape.match(text,offset);if(!match.hasMatch())break;
  const auto value=match.captured();bool upper=false,lower=false,other=false;
  for(const auto c:value) {if(c.isUpper())upper=true;else if(c.isLower())lower=true;else other=true;}
  if(upper&&lower&&other) {text.replace(match.capturedStart(),match.capturedLength(),"<redacted>");offset=match.capturedStart()+10;}
  else offset=match.capturedEnd();
 }
 return text;
}
bool sensitiveKey(const QString& key) {
 static const QStringList keys{"password","passphrase","passwd","secret","secretkey","secretaccesskey","awssecretaccesskey","accesskey","accesskeyid",
  "awsaccesskeyid","sessiontoken","awssessiontoken","securitytoken","token","accesstoken","refreshtoken","authorization","credentials","cookie",
  "sharepassword","privatekey","clipboard","clipboardtext","text","content","contents","payload"};
 auto normalized=key.toLower();normalized.remove('_');normalized.remove('-');
 return keys.contains(normalized);
}
struct Record {qint64 time=0;QtMsgType type=QtInfoMsg;QString category,process,message;qint64 pid=0;QJsonObject fields;};
QByteArray encode(const Record& record) {
 QJsonObject object{{"v",1},{"time",QDateTime::fromMSecsSinceEpoch(record.time).toUTC().toString(Qt::ISODateWithMs)},{"level",levelName(record.type)},
  {"category",record.category.left(64)},{"process",record.process.left(64)},{"pid",record.pid},{"message",redact(record.message).left(4096)}};
 auto fields=redact(QJsonValue(record.fields)).toObject();
 for(const auto* key:{"operationId","itemId"})if(fields[key].isString()){object[key]=fields[key].toString().left(128);fields.remove(key);}
 if(!fields.isEmpty())object["fields"]=fields;
 auto line=QJsonDocument(object).toJson(QJsonDocument::Compact);
 if(line.size()>16384) {object["fields"]=QJsonObject{{"truncated",true}};line=QJsonDocument(object).toJson(QJsonDocument::Compact);}
 return line+'\n';
}
QString timeOf(const QString& path) {
 QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};
 const auto document=QJsonDocument::fromJson(file.readLine(32768));
 return document.object()["time"].toString();
}
struct Command {
 enum Kind {Write,Cleanup,Clear,Flush,Stop} kind=Write;
 Record record;
 std::shared_ptr<std::promise<QJsonObject>> done;
};
class Writer {
public:
 Writer(QString directory,Retention retention,QString process):directory_(std::move(directory)),process_(std::move(process)),retention_(retention) {}
 ~Writer() {if(thread_.joinable())thread_.join();}
 void start() {thread_=std::thread([this]{run();});}
 // Never blocks on IO: the queue is bounded and a full queue drops.
 void push(Command command) {
  {
   std::lock_guard lock(mutex_);
   if(stopped_){if(command.done)command.done->set_value({});return;}
   if(command.kind==Command::Write) {
    if(queued_>=maximumQueuedRecords) {++dropped_;degraded_=true;reason_="queue_full";return;}
    ++queued_;
   }
   queue_.push_back(std::move(command));
  }
  wake_.notify_one();
 }
 QJsonObject status() {
  std::lock_guard lock(mutex_);
  return {{"available",true},{"directory",directory_},{"files",files_},{"bytes",total_},{"oldestRecord",oldest_.isEmpty()?QJsonValue():QJsonValue(oldest_)},
   {"degraded",degraded_},{"degradedReason",degraded_?QJsonValue(reason_):QJsonValue()},{"droppedRecords",qint64(dropped_)},
   {"retention",QJsonObject{{"days",retention_.days},{"bytes",retention_.bytes}}},{"rotationBytes",rotationLimit()}};
 }
 void setRetention(Retention retention) {{std::lock_guard lock(mutex_);retention_=retention;}push({Command::Cleanup,{},{}});}
 QStringList files() {std::lock_guard lock(mutex_);return names_;}
 QString current() {std::lock_guard lock(mutex_);return current_.isEmpty()?QString():directory_+"/"+current_;}
 QString directory() const {return directory_;}
private:
 qint64 rotationLimit() const {return qBound(qint64(64*1024),retention_.bytes/4,rotationBytes);}
 void run() {
  prepare();
  for(;;) {
   Command command;
   {
    std::unique_lock lock(mutex_);wake_.wait(lock,[this]{return !queue_.empty();});
    command=std::move(queue_.front());queue_.pop_front();if(command.kind==Command::Write)--queued_;
   }
   QJsonObject result;
   switch(command.kind) {
   case Command::Write:
    if(writeLine(encode(command.record))) {
     std::lock_guard lock(mutex_);
     if(oldest_.isEmpty())oldest_=QDateTime::fromMSecsSinceEpoch(command.record.time).toUTC().toString(Qt::ISODateWithMs);
    }
    break;
   case Command::Cleanup:enforce();break;
   case Command::Clear:result=clearAll();break;
   case Command::Flush:break;
   case Command::Stop:
#ifdef Q_OS_UNIX
    if(fd_>=0){::fsync(fd_);::close(fd_);fd_=-1;}
#endif
    {std::lock_guard lock(mutex_);stopped_=true;for(auto& pending:queue_)if(pending.done)pending.done->set_value({});queue_.clear();}
    if(command.done)command.done->set_value({});
    return;
   }
   if(command.done)command.done->set_value(result);
  }
 }
 void fail(const QString& reason) {std::lock_guard lock(mutex_);++dropped_;degraded_=true;reason_=reason;}
 bool prepareDirectory() {
#ifdef Q_OS_UNIX
  // The folder must be a real directory of this user's, owner-only. A folder
  // created here gets owner-only parents too (the putgd state folder).
  const auto parent=QFileInfo(directory_).absolutePath();
  const bool createdParent=!QFileInfo::exists(parent);
  if(!QDir().mkpath(directory_))return false;
  if(createdParent)QFile::setPermissions(parent,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
  struct stat status{};const auto bytes=QFile::encodeName(directory_);
  if(::lstat(bytes.constData(),&status)!=0||!S_ISDIR(status.st_mode)||status.st_uid!=::getuid())return false;
  return ::chmod(bytes.constData(),0700)==0;
#else
  return QDir().mkpath(directory_);
#endif
 }
 bool openFile() {
#ifdef Q_OS_UNIX
  // After a failed open, retry at most every 5 s rather than on every record.
  const auto now=QDateTime::currentMSecsSinceEpoch();
  if(failedOpen_>0&&now-failedOpen_<5000)return false;
  failedOpen_=now;
  if(!prepareDirectory())return false;
  const auto name=QString("putgd-%1Z-%2-%3.jsonl").arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmsszzz")).arg(QCoreApplication::applicationPid()).arg(++sequence_);
  const auto path=QFile::encodeName(directory_+"/"+name);
  const int fd=::open(path.constData(),O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
  if(fd<0)return false;
  fd_=fd;fileBytes_=0;failedOpen_=0;
  std::lock_guard lock(mutex_);current_=name;
  return true;
#else
  return false;
#endif
 }
 void prepare() {
  if(!openFile()){fail("unavailable");return;}
  enforce();
 }
 bool writeLine(const QByteArray& line) {
#ifdef Q_OS_UNIX
  if(fd_<0&&!openFile()){fail("unavailable");return false;}
  if(fileBytes_>0&&fileBytes_+line.size()>rotationLimit()) {
   ::close(fd_);fd_=-1;
   if(!openFile()){fail("unavailable");return false;}
   enforce();
  }
  const char* data=line.constData();qint64 remaining=line.size();
  while(remaining>0) {
   const auto written=::write(fd_,data,size_t(remaining));
   if(written<0&&errno==EINTR)continue;
   if(written<=0) {
    const auto reason=errno==ENOSPC||errno==EDQUOT?QString("disk_full"):QString("write_failed");
    // Never leave half a record: the next one must start on its own line.
    if(remaining!=line.size()&&::ftruncate(fd_,fileBytes_)!=0){::close(fd_);fd_=-1;}
    fail(reason);return false;
   }
   data+=written;remaining-=written;
  }
  fileBytes_+=line.size();
  std::lock_guard lock(mutex_);total_+=line.size();degraded_=false;
  if(files_==0)files_=1;
  return true;
#else
  Q_UNUSED(line);fail("unavailable");return false;
#endif
 }
 // Age first, then size, oldest first; the file being written stays.
 void enforce() {
  Retention retention;QString current;{std::lock_guard lock(mutex_);retention=retention_;current=current_;}
  QDir dir(directory_);const auto cutoff=QDateTime::currentDateTimeUtc().addDays(-retention.days);
  QList<QPair<QString,qint64>> kept;qint64 total=0;
  for(const auto& name:dir.entryList({"putgd-*.jsonl"},QDir::Files,QDir::Name)) {
   if(!fileName().match(name).hasMatch())continue;
   const QFileInfo info(dir.filePath(name));
   if(name!=current&&info.lastModified().toUTC()<cutoff&&QFile::remove(info.filePath()))continue;
   kept.append({name,info.size()});total+=info.size();
  }
  for(qsizetype index=0;index<kept.size()&&total>retention.bytes;) {
   if(kept[index].first!=current&&QFile::remove(dir.filePath(kept[index].first))){total-=kept[index].second;kept.removeAt(index);}
   else ++index;
  }
  QStringList names;for(const auto& entry:kept)names.append(entry.first);
  const auto oldest=names.isEmpty()?QString():timeOf(dir.filePath(names.first()));
  std::lock_guard lock(mutex_);names_=names;files_=int(names.size());total_=total;oldest_=oldest;
 }
 QJsonObject clearAll() {
#ifdef Q_OS_UNIX
  if(fd_>=0){::close(fd_);fd_=-1;}
#endif
  QDir dir(directory_);int removed=0;qint64 bytes=0;
  for(const auto& name:dir.entryList({"putgd-*.jsonl"},QDir::Files,QDir::Name)) {
   if(!fileName().match(name).hasMatch())continue;
   const auto size=QFileInfo(dir.filePath(name)).size();
   if(QFile::remove(dir.filePath(name))){++removed;bytes+=size;}
  }
  {std::lock_guard lock(mutex_);current_.clear();names_.clear();files_=0;total_=0;oldest_.clear();}
  failedOpen_=0;
  if(openFile()) {
   Record record{QDateTime::currentMSecsSinceEpoch(),QtInfoMsg,"putgd.app",process_,"Logs cleared",QCoreApplication::applicationPid(),{{"removedFiles",removed},{"removedBytes",bytes}}};
   writeLine(encode(record));enforce();
  }else fail("unavailable");
  return {{"removedFiles",removed},{"removedBytes",bytes}};
 }
 const QString directory_,process_;
 std::thread thread_;
 std::mutex mutex_;std::condition_variable wake_;std::deque<Command> queue_;int queued_=0;bool stopped_=false;
 Retention retention_;
 QString current_,oldest_,reason_;QStringList names_;int files_=0;qint64 total_=0;quint64 dropped_=0;bool degraded_=false;
 int fd_=-1;qint64 fileBytes_=0,failedOpen_=0;int sequence_=0;
};
std::mutex stateMutex;
std::shared_ptr<Writer> instance;
QtMessageHandler previousHandler=nullptr;
std::atomic_bool debugEnabled{false};
QString processName,debugExpiry;
std::shared_ptr<Writer> current() {std::lock_guard lock(stateMutex);return instance;}
void enqueue(const QString& category,QtMsgType type,const QString& message,const QJsonObject& fields,const QString& process,qint64 pid) {
 const auto writer=current();if(!writer)return;
 Command command;command.record={QDateTime::currentMSecsSinceEpoch(),type,category,process,message,pid,fields};
 writer->push(std::move(command));
}
QJsonObject wait(Command::Kind kind,int timeoutMs,bool* completed=nullptr) {
 if(completed)*completed=false;
 const auto writer=current();if(!writer)return {};
 auto done=std::make_shared<std::promise<QJsonObject>>();auto future=done->get_future();
 writer->push({kind,{},done});
 if(future.wait_for(std::chrono::milliseconds(timeoutMs))!=std::future_status::ready)return {};
 if(completed)*completed=true;
 return future.get();
}
void handle(QtMsgType type,const QMessageLogContext& context,const QString& message) {
 if(type!=QtDebugMsg||debugEnabled)enqueue(QString::fromLatin1(context.category?context.category:"default"),type,message,{},processName,QCoreApplication::applicationPid());
 if(type==QtFatalMsg)wait(Command::Flush,1000);
 if(previousHandler)previousHandler(type,context,message);
}
}
QString stateRoot() {
#ifdef Q_OS_WIN
 return QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"))+"/putgd";
#else
 auto base=qEnvironmentVariable("XDG_STATE_HOME");
 if(base.isEmpty()||!QDir::isAbsolutePath(base))base=QDir::homePath()+"/.local/state";
 return QDir::cleanPath(base)+"/putgd";
#endif
}
void open(const QString& directory,Retention retention,const QString& process) {
 if(current())return;
 processName=process.isEmpty()?QFileInfo(QCoreApplication::applicationFilePath()).fileName():process;
 if(processName.isEmpty())processName="putgd";
 auto writer=std::make_shared<Writer>(QDir::cleanPath(directory),retention,processName);
 writer->start();
 {std::lock_guard lock(stateMutex);instance=writer;}
 previousHandler=qInstallMessageHandler(handle);
}
void close() {
 if(!current())return;
 qInstallMessageHandler(previousHandler);previousHandler=nullptr;
 bool stopped=false;wait(Command::Stop,10000,&stopped);
 std::shared_ptr<Writer> writer;{std::lock_guard lock(stateMutex);writer=std::move(instance);}
 // A writer stuck in the filesystem keeps its thread; never join it at exit.
 if(!stopped)new std::shared_ptr<Writer>(std::move(writer));
}
bool isOpen() {return bool(current());}
void setRetention(Retention retention) {if(const auto writer=current())writer->setRetention(retention);}
void setDebug(bool enabled,const QDateTime& expiresAt) {
 debugEnabled=enabled;
 {std::lock_guard lock(stateMutex);debugExpiry=enabled&&expiresAt.isValid()?expiresAt.toUTC().toString(Qt::ISODateWithMs):QString();}
 // put.gd's categories and qDebug ("default"). These rules beat a distro's
 // qtlogging.ini (Fedora's has *.debug=false, #191); QT_LOGGING_RULES is
 // applied after them, so developers keep control.
 QLoggingCategory::setFilterRules(enabled?QStringLiteral("putgd.*.debug=true\ndefault.debug=true"):QString());
}
bool debug() {return debugEnabled;}
void write(const QLoggingCategory& category,QtMsgType type,const QString& message,const QJsonObject& fields) {
 write(category,type,message,fields,processName.isEmpty()?QStringLiteral("putgd"):processName,QCoreApplication::applicationPid());
}
void write(const QLoggingCategory& category,QtMsgType type,const QString& message,const QJsonObject& fields,const QString& process,qint64 pid) {
 if(!category.isEnabled(type))return;
 enqueue(QString::fromLatin1(category.categoryName()),type,message,fields,process,pid);
}
QJsonObject errorFields(const Error& error) {
 return {{"code",error.code},{"message",error.message.left(1024)},{"details",error.details}};
}
QJsonObject status() {
 const auto writer=current();
 auto result=writer?writer->status():QJsonObject{{"available",false},{"degraded",false}};
 result["debugLogging"]=bool(debugEnabled);
 std::lock_guard lock(stateMutex);
 result["debugLoggingExpiresAt"]=debugExpiry.isEmpty()?QJsonValue():QJsonValue(debugExpiry);
 return result;
}
void cleanup() {if(const auto writer=current())writer->push({Command::Cleanup,{},{}});}
QJsonObject clear(Error& error) {
 if(!current()){error={"logging_unavailable","Logging is not running in this process",{}};return {};}
 bool completed=false;auto result=wait(Command::Clear,10000,&completed);
 if(!completed)error={"logging_busy","The log writer did not finish clearing; try again",{}};
 return result;
}
void flush() {wait(Command::Flush,10000);}
QStringList files() {const auto writer=current();return writer?writer->files():QStringList{};}
QString currentFile() {const auto writer=current();return writer?writer->current():QString{};}
QString redact(const QString& input) {
 auto text=input;
 for(const auto& rule:rules())text.replace(rule.pattern,rule.replacement);
 return redactSecretShapes(std::move(text));
}
QJsonValue redact(const QJsonValue& value) {
 if(value.isString())return redact(value.toString());
 if(value.isArray()) {QJsonArray result;for(const auto& entry:value.toArray())result.append(redact(entry));return result;}
 if(value.isObject()) {
  QJsonObject result;const auto object=value.toObject();
  for(auto it=object.begin();it!=object.end();++it)result[it.key()]=sensitiveKey(it.key())?QJsonValue("<redacted>"):redact(it.value());
  return result;
 }
 return value;
}
}
namespace {
// Settings and version lines encoder libraries print as the recording helper
// opens them (#250): not faults, so they stay out of the warning stream. A
// quiet helper (the capability probe) tries encoders that may be missing, so
// every line those libraries print there is a banner too.
bool encoderBanner(const QByteArray& text,bool quiet) {
 static const QRegularExpression banner(R"(^x265 \[info\]: |^encoded \d+ frames|^\[(libx264|libx265|libvpx[\w-]*|libaom[\w-]*|libsvtav1|libopenh264) @ 0x[0-9a-f]+\] (using cpu capabilities|profile |v\d|Neither bitrate nor constrained quality))");
 static const QRegularExpression library(R"(^(x265 \[\w+\]: |\[OpenH264\] |\[[\w-]+ @ 0x[0-9a-f]+\] ))");
 const auto line=QString::fromUtf8(text);
 return banner.match(line).hasMatch()||(quiet&&library.match(line).hasMatch());
}
}
HelperOutput::HelperOutput(QString process,const QLoggingCategory& category,bool quiet):process_(std::move(process)),category_(category),quiet_(quiet) {}
void HelperOutput::started(qint64 pid,const QJsonObject& fields) {
 pid_=pid;logged_=suppressed_=banners_=0;partial_.clear();crash_.clear();killReason_.clear();++generation_;
 auto values=fields;values["helper"]=process_;values["pid"]=pid;
 logging::write(category_,quiet_?QtDebugMsg:QtInfoMsg,"Helper started",values);
}
void HelperOutput::append(const QByteArray& bytes) {
 partial_+=bytes;
 for(auto newline=partial_.indexOf('\n');newline>=0;newline=partial_.indexOf('\n')) {line(partial_.left(newline));partial_.remove(0,newline+1);}
 if(partial_.size()>lineBytes){line(partial_);partial_.clear();}
}
void HelperOutput::line(QByteArray text) {
 if(text.endsWith('\r'))text.chop(1);
 if(text.trimmed().isEmpty())return;
 if(crash::isRecordLine(text)) {
  if(crash_.size()+text.size()<crashBytes)crash_+=text+'\n';
  if(text.startsWith("putgd-crash\tend"))crashRecord();
  return;
 }
 QtMsgType type=QtWarningMsg;
 if(encoderBanner(text,quiet_)) {
  // Counted for the exit summary; written only while debug logging is on.
  ++banners_;type=QtDebugMsg;
  if(!category_.isEnabled(QtDebugMsg))return;
 }
 if(logged_>=generationBytes){suppressed_+=text.size()+1;return;}
 if(text.size()>lineBytes)text.truncate(lineBytes);
 logged_+=text.size()+1;
 QJsonObject fields{{"stream","stderr"}};auto message=QString::fromUtf8(text);
 // The S3 worker's AWS SDK bridge: "putgd-aws<TAB>E|W|I<TAB>tag<TAB>message".
 if(text.startsWith("putgd-aws\t")) {
  const auto parts=text.split('\t');
  if(parts.size()>=4) {
   type=parts[1]=="E"?QtCriticalMsg:parts[1]=="W"?QtWarningMsg:parts[1]=="I"?QtInfoMsg:QtDebugMsg;
   fields={{"stream","aws-sdk"},{"awsTag",QString::fromUtf8(parts[2]).left(64)}};
   message=QString::fromUtf8(text.mid(parts[0].size()+parts[1].size()+parts[2].size()+3));
  }
 }
 logging::write(category_,type,message,fields,process_,pid_);
}
void HelperOutput::crashRecord() {
 const auto record=crash::parseRecord(crash_);crash_.clear();
 if(!record.isEmpty())logging::write(category_,QtCriticalMsg,"Helper crash record",{{"helper",process_},{"crash",record}},process_,pid_);
}
void HelperOutput::finished(int code,QProcess::ExitStatus status,bool requested) {
 if(!partial_.isEmpty()){line(partial_);partial_.clear();}
 if(!crash_.isEmpty())crashRecord(); // cut short: keep what arrived
 if(suppressed_>0)logging::write(category_,QtWarningMsg,"Helper output truncated",{{"suppressedBytes",suppressed_},{"keptBytes",logged_}},process_,pid_);
 if(banners_>0)logging::write(category_,quiet_?QtDebugMsg:QtInfoMsg,"Encoder banner lines logged at debug level",{{"lines",banners_}},process_,pid_);
 QJsonObject fields{{"helper",process_},{"pid",pid_},{"requested",requested}};
 // A supervisor kill names the reason its "Killing helper" record gave.
 const bool supervised=!killReason_.isEmpty();
 if(supervised){fields["supervisorKill"]=true;fields["reason"]=killReason_;}
 QtMsgType type=QtInfoMsg;QString message;
 if(status==QProcess::NormalExit){fields["exitCode"]=code;type=code==0?(quiet_?QtDebugMsg:QtInfoMsg):QtWarningMsg;message="Helper exited";}
 else {
  fields["signal"]=code;
  if(supervised){type=QtWarningMsg;message="Helper killed by put.gd";}
  else if(requested){type=QtWarningMsg;message="Helper stopped by put.gd";}
  // Nothing in put.gd sends SIGKILL without asking for the exit, and a crash
  // never ends in it: another process (an OOM killer, a window manager's
  // force quit, a user) killed this helper.
#ifdef Q_OS_UNIX
  else if(code==SIGKILL){type=QtCriticalMsg;message="Helper killed from outside put.gd";fields["supervisorKill"]=false;}
#endif
  else {type=QtCriticalMsg;message="Helper crashed";}
 }
 logging::write(category_,type,message,fields);
 suppressed_=0;logged_=0;banners_=0;pid_=0;killReason_.clear();
}
void HelperOutput::killing(const QString& reason,const QJsonObject& fields) {
 auto values=fields;values["helper"]=process_;values["pid"]=pid_;values["reason"]=reason;
 logging::write(category_,QtWarningMsg,"Killing helper",values);
 killReason_=reason;
}
void HelperOutput::failedToStart(const QString& reason) {
 logging::write(category_,QtCriticalMsg,"Helper failed to start",{{"helper",process_},{"reason",reason.left(256)}});
 pid_=0;partial_.clear();killReason_.clear();
}
}
