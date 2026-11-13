#include "HelperProcess.h"
#include "OutputBudget.h"
#include "ProtonProvider.h"
#include "core/HostEnvironment.h"
#include "core/ByteSize.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLocale>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QLockFile>
#include <QFileInfo>
#include <QStandardPaths>
#ifdef Q_OS_UNIX
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#endif
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace putgd {
namespace {
Error parseError(const QJsonObject& value) {auto code=value["code"].toString();if(code=="request_timeout")code="proton_request_timeout";return {code,value["message"].toString(),value["details"].toObject()};}
bool nodeUid(const QString& value) {
 static const QRegularExpression pattern("^([A-Za-z0-9=_-]{88,108}|[A-Za-z0-9_-]{22})~([A-Za-z0-9=_-]{88,108}|[A-Za-z0-9_-]{22})$");
 return pattern.match(value).hasMatch();
}
bool storageHost(const QString& value) {
 static const QRegularExpression pattern("^[a-z0-9-]{1,48}\\.proton\\.me$");
 return value!=QLatin1String("drive-api.proton.me")&&pattern.match(value).hasMatch();
}
QString runtimePath() {
 const auto app=QCoreApplication::applicationDirPath();
 if(app==QString::fromUtf8(PUTGD_PROTON_BUILD_APP_DIR))return QString::fromUtf8(PUTGD_PROTON_BUILD_RUNTIME);
 return QDir(app).filePath(PUTGD_PROTON_INSTALL_RELATIVE);
}
bool runtimeHashMatches() {
 QFile file(runtimePath());QCryptographicHash hash(QCryptographicHash::Sha512);
 return !QString::fromUtf8(PUTGD_PROTON_RUNTIME_SHA512).isEmpty() && file.open(QIODevice::ReadOnly)
   && hash.addData(&file) && hash.result().toHex()==QByteArray(PUTGD_PROTON_RUNTIME_SHA512);
}
// Hashing the runtime (about 110 MB) takes most of a second, so one verdict stands for
// as long as the file is the same one: a replaced or rewritten file has another inode,
// size or change time, and is hashed again. A caller waits for a hash already under way,
// unless it may not hash: it then learns only whether the file is already known good.
bool runtimeVerified(bool mayHash=true) {
#ifdef Q_OS_UNIX
 struct Identity {
  dev_t device=0;ino_t inode=0;off_t size=0;timespec modified{},changed{};
  bool operator==(const Identity& other) const {return device==other.device&&inode==other.inode&&size==other.size&&modified.tv_sec==other.modified.tv_sec&&modified.tv_nsec==other.modified.tv_nsec&&changed.tv_sec==other.changed.tv_sec&&changed.tv_nsec==other.changed.tv_nsec;}
 };
 const auto identify=[](Identity& identity){
  struct stat info;if(::stat(QFile::encodeName(runtimePath()).constData(),&info)!=0)return false;
  identity={info.st_dev,info.st_ino,info.st_size,info.st_mtim,info.st_ctim};return true;
 };
 static std::mutex mutex;static std::optional<Identity> verified;
 std::unique_lock lock(mutex,std::defer_lock);
 if(mayHash)lock.lock();else if(!lock.try_lock())return false;
 Identity before,after;if(!identify(before))return false;
 if(verified&&*verified==before)return true;
 if(!mayHash||!runtimeHashMatches())return false;
 // Only a file that stayed the same while it was hashed is remembered.
 if(identify(after)&&after==before)verified=after;
 return true;
#else
 return mayHash&&runtimeHashMatches();
#endif
}
}
struct ProtonProvider::Helper {
 QLockFile lock;QTemporaryDir root;HelperProcess process;
 // It answers request lines in turn (--session) instead of one request to the end of its input.
 bool lines=false;
 // It was sent a warm-up request whose answer has not been read yet.
 bool warming=false;
 // Which helper started ahead this is: an address can be used again, a number is not.
 quint64 serial=0;
 // A finished session passed it on to an operation that had asked for the account meanwhile.
 bool passedOn=false;
 // Lets a helper nobody will ask again end by itself. It finishes what it is doing first:
 // a helper killed while it refreshes the session loses the account. `hurry` ends the wait.
 void finish(const std::function<bool()>& hurry){
  process.closeInput();
  for(int waited=0;waited<30000&&!hurry()&&!process.waitForFinished(50);waited+=50){}
  if(process.running()){process.kill();process.waitForFinished(5000);}
 }
 explicit Helper(const QString& lockPath):lock(lockPath),root(QDir::tempPath()+"/putgd-proton-XXXXXX"){}
 // A helper waiting for a request ends when its input closes.
 ~Helper(){
  if(!process.running())return;
  process.closeInput();if(!process.waitForFinished(2000)){process.kill();process.waitForFinished(5000);}
 }
};
// Helpers that wait for a request, by keychain reference: one started ahead of its operation,
// or one a finished session passed on. `asked` counts the operations that wanted a helper
// started ahead while the account's session had it.
struct ProtonProvider::Waiting {
 std::mutex mutex;std::map<QString,std::unique_ptr<Helper>> helpers;std::map<QString,int> asked;bool closed=false;
};
struct ProtonProvider::Session {
 QString reference;std::unique_ptr<Helper> helper;std::shared_ptr<Waiting> waiting;
 // An operation that has registered for this account (see anticipate) gets the session's
 // helper, signed in and connected, instead of a new one. Only a helper that waits for its
 // next request, and has said nothing since its last clean answer, is passed on; any other
 // ends here, as does every helper no registered operation is left to take. The session's
 // own operation is registered too, so its helper waits until that operation ends and is
 // retired then if no other took it.
 ~Session(){
  if(!helper||!waiting||!helper->lines)return;
  auto& process=helper->process;
  if(!process.running()||!process.outputOpen()||process.waitForReadable(0))return;
  std::lock_guard lock(waiting->mutex);
  if(waiting->closed||!waiting->asked.count(reference)||waiting->helpers.count(reference))return;
  helper->warming=false;helper->serial=0;helper->passedOn=true;
  waiting->helpers[reference]=std::move(helper);
 }
};
namespace {
// One session per provider and thread: an operation's requests all run on its own thread.
QHash<const ProtonProvider*,std::weak_ptr<void>>& threadSessions() {thread_local QHash<const ProtonProvider*,std::weak_ptr<void>> sessions;return sessions;}
// Reads a waiting helper's warm-up answer. True when it is clean and nothing follows it.
bool settled(HelperProcess& process,int milliseconds,const std::function<bool()>& abandoned) {
 QElapsedTimer waited;waited.start();QByteArray answer;
 while(!answer.contains('\n')){
  if(!process.outputOpen()||waited.elapsed()>milliseconds||abandoned())return false;
  process.waitForReadable(100);answer+=process.read(64*1024);if(answer.size()>=64*1024)return false;
 }
 const auto value=QJsonDocument::fromJson(answer.left(answer.indexOf('\n'))).object();
 return answer.endsWith('\n')&&answer.count('\n')==1&&value.size()==2&&value["ok"].toBool()&&value["result"].isObject()&&value["result"].toObject()["error"].toObject().isEmpty();
}
}
ProtonProvider::ProtonProvider():waiting_(std::make_shared<Waiting>()){}
ProtonProvider::~ProtonProvider() {
 stopping_.store(true);
 if(verifier_.joinable())verifier_.join();
 std::map<QString,std::unique_ptr<Helper>> unused;
 {std::lock_guard lock(waiting_->mutex);waiting_->closed=true;unused.swap(waiting_->helpers);}
 for(auto& entry:unused)retire(std::move(entry.second));
 std::unique_lock lock(retiringMutex_);retired_.wait(lock,[this]{return retiring_==0;});
}
void ProtonProvider::prepare() {
 if(prepared_.exchange(true))return;
 verifier_=std::thread([]{runtimeVerified();});
}
std::shared_ptr<void> ProtonProvider::openSession(const Destination& destination) {
 const auto reference=destination.credentialReference["reference"].toString();
 if(reference.isEmpty()||threadSessions().value(this).lock())return {};
 auto opened=std::make_shared<Session>();opened->reference=reference;opened->waiting=waiting_;threadSessions().insert(this,opened);return opened;
}
std::shared_ptr<ProtonProvider::Session> ProtonProvider::session(const QString& reference) const {
 auto current=std::static_pointer_cast<Session>(threadSessions().value(this).lock());
 return current&&current->reference==reference?current:nullptr;
}
std::unique_ptr<ProtonProvider::Helper> ProtonProvider::start(const QString& reference,bool lines,int lockWaitMs,bool mayHash,const std::shared_ptr<std::atomic_bool>& cancelled,Error& error) {
 if(!runtimeVerified(mayHash)){error={"proton_runtime_unavailable","Install a matching put.gd package with the bundled Proton runtime",{}};return {};}
 // Separate helper processes share a rotating auth session. Serialize each
 // helper's whole life by keychain reference, including setup and token refresh.
 const auto lockRoot=QString("/tmp/putgd-proton-accounts-%1").arg(getuid());
 // mkdir is atomic and does not follow a pre-existing /tmp symlink.
 if(::mkdir(QFile::encodeName(lockRoot).constData(),0700)!=0&&errno!=EEXIST){error={"proton_lock_unavailable","Private account coordination is unavailable",{}};return {};}
 const QFileInfo lockInfo(lockRoot);
 if(lockInfo.ownerId()!=getuid()||lockInfo.permissions()&(QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|QFile::ReadOther|QFile::WriteOther|QFile::ExeOther)){error={"proton_lock_unavailable","Account coordination directory is not private",{}};return {};}
 if(!lockInfo.isDir()||lockInfo.isSymLink()||lockInfo.canonicalFilePath()!=lockInfo.absoluteFilePath()){error={"proton_lock_unavailable","Account coordination directory is invalid",{}};return {};}
 const auto accountKey=QString::fromLatin1(QCryptographicHash::hash(reference.toUtf8(),QCryptographicHash::Sha256).toHex());
 auto started=std::make_unique<Helper>(QDir(lockRoot).filePath(accountKey+".lock"));started->lock.setStaleLockTime(0);started->lines=lines;
 if(!started->lock.tryLock(lockWaitMs)){error={"proton_account_busy","Another request is using this Proton account; wait and retry",{}};return {};}
 if(stopping_.load()||(cancelled&&cancelled->load())){error={"operation_cancelled","Request cancelled before the helper started",{}};return {};}
 if(!started->root.isValid()){error={"proton_private_storage","Private helper storage is unavailable",{}};return {};}
 QFile::setPermissions(started->root.path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 if(!started->process.start(runtimePath(),{lines?"--session":"--stdio"},helperEnvironment(host::environment(),started->root.path()))){error={"proton_runtime_start","The bundled Proton runtime could not start",{}};return {};}
 return started;
}
// Ends a helper nobody will ask again, without making the caller wait for it.
void ProtonProvider::retire(std::unique_ptr<Helper> helper) {
 if(!helper)return;
 helper->process.closeInput();
 {std::lock_guard lock(retiringMutex_);++retiring_;}
 std::thread([this,helper=std::move(helper)]() mutable {
  helper->finish([this]{return stopping_.load();});
  helper.reset();
  std::lock_guard lock(retiringMutex_);--retiring_;retired_.notify_all();
 }).detach();
}
std::unique_ptr<ProtonProvider::Helper> ProtonProvider::takeStandby(const QString& reference) {
 std::lock_guard lock(waiting_->mutex);
 const auto found=waiting_->helpers.find(reference);if(found==waiting_->helpers.end())return {};
 auto helper=std::move(found->second);waiting_->helpers.erase(found);return helper;
}
std::shared_ptr<void> ProtonProvider::anticipate(const Destination& destination) {
 const auto reference=destination.credentialReference["reference"].toString();
 if(stopping_.load()||destination.credentialReference["mode"]!="keychain"||reference.isEmpty())return {};
 // Every operation registers that it will ask, for as long as it lives: a session's helper is
 // passed on when the session ends (see Session) while a registered operation may take it.
 // That includes the operation that starts a helper below, because another operation's
 // session may take that helper first, and this one then asks for it afterwards. It registers
 // before it looks, so a session that ends meanwhile still passes its helper on.
 {std::lock_guard lock(waiting_->mutex);++waiting_->asked[reference];}
 std::shared_ptr<void> registered(this,[this,waiting=waiting_,reference](void*){
  std::unique_ptr<Helper> unused;
  {
   std::lock_guard lock(waiting->mutex);
   const auto asked=waiting->asked.find(reference);
   if(asked==waiting->asked.end()||--asked->second>0)return;
   waiting->asked.erase(asked);
   // Nobody is left to ask a helper that was passed on and not taken.
   const auto found=waiting->helpers.find(reference);
   if(found!=waiting->helpers.end()&&found->second->passedOn){unused=std::move(found->second);waiting->helpers.erase(found);}
  }
  retire(std::move(unused));
 });
 bool waitingAlready=false;
 {std::lock_guard lock(waiting_->mutex);waitingAlready=waiting_->helpers.count(reference)>0;}
 if(waitingAlready)return registered;
 // Never waits: not for the account, and not for the runtime's hash if it is not known yet.
 Error refused;auto helper=start(reference,true,0,false,{},refused);
 if(!helper)return refused.code=="proton_account_busy"?registered:std::shared_ptr<void>{};
 const auto warm=QJsonDocument(QJsonObject{{"schemaVersion",1},{"method","warm"},{"destination",destination.toJson()}}).toJson(QJsonDocument::Compact)+'\n';
 if(helper->process.write(warm,0)!=warm.size()){helper->process.kill();return {};}
 static std::atomic<quint64> serials{0};
 helper->warming=true;const auto mine=helper->serial=++serials;
 {std::lock_guard lock(waiting_->mutex);waiting_->helpers[reference]=std::move(helper);}
 // The helper it started ends with the operation if nobody took it; then its registration ends.
 return std::shared_ptr<void>(this,[this,waiting=waiting_,reference,mine,registered](void*) mutable {
  std::unique_ptr<Helper> unused;
  {std::lock_guard lock(waiting->mutex);const auto found=waiting->helpers.find(reference);if(found!=waiting->helpers.end()&&found->second->serial==mine){unused=std::move(found->second);waiting->helpers.erase(found);}}
  retire(std::move(unused));
  registered.reset();
 });
}
QProcessEnvironment ProtonProvider::helperEnvironment(const QProcessEnvironment& host,const QString& dataDirectory) {
 auto env=host;for(const auto& key:env.keys())if(key.startsWith("PROTON_DRIVE_")||key.startsWith("PUTGD_PROTON_"))env.remove(key);
 env.insert("PUTGD_PROTON_DATA_DIR",dataDirectory);
 return env;
}
QJsonObject ProtonProvider::defaults() {
 return {{"accountRootUid",""},{"folderUid",""},{"shareByDefault",true},{"linkExpirationDays",0},
  {"sharePasswordReference",QJsonValue::Null},{"requestTimeoutMs",120000}};
}
std::optional<int> ProtonProvider::retryRequestTimeout(const Destination& accepted,const QJsonObject& profile) const {
 if(profile["id"]!=accepted.profileId||profile["provider"]!=accepted.providerId||profile["credentials"].toObject()!=accepted.credentialReference)return {};
 const auto current=profile["settings"].toObject();if(validateSettings(current))return {};
 for(const auto& field:QStringList{"accountRootUid","folderUid"})if(current[field]!=accepted.settings[field])return {};
 return std::max(accepted.settings["requestTimeoutMs"].toInt(120000),current["requestTimeoutMs"].toInt(120000));
}
ProviderCapabilities ProtonProvider::capabilities(const Destination&) const {
 ProviderCapabilities caps;caps.conditionalCreate=true;caps.stat=true;caps.urls=true;caps.maximumNameBytes=255;caps.sharing=true;caps.privateUploads=true;
 // The pinned Drive SDK (0.22.2) streams 4 MiB encrypted blocks and sets no file
 // size limit of its own. Proton enforces plan limits server-side (error 200003,
 // MAX_FILE_SIZE_FOR_FREE_USER, reported as destination_file_too_large_for_plan),
 // so none is declared.
 caps.maximumObjectBytes=0;
 caps.deletionEffect="Safe remote deletion is unavailable: Proton does not expose an atomic revision-guarded file deletion. Revoke share links separately.";return caps;
}
QString ProtonProvider::defaultPublicationMode(const Destination& destination) const {return destination.settings["shareByDefault"].toBool(true)?"share":"private";}
QString ProtonProvider::serializationKey(const Destination& d,const QJsonObject& ref,const QString& candidate) const {
 const auto root=ref["accountRootUid"].toString(d.settings["accountRootUid"].toString());
 const auto folder=ref["folderUid"].toString(d.settings["folderUid"].toString());
 const auto name=ref["name"].toString(candidate);
 return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonArray{root,folder,name}).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
Error ProtonProvider::validateSettings(const QJsonObject& s) const {
 const auto expected=defaults().keys();auto required=s;required.remove("folderName");if(required.keys()!=expected)return {"config_invalid","Proton settings have missing or unsupported fields",{}};
 const auto root=s["accountRootUid"].toString(),folder=s["folderUid"].toString();
 if(!s["accountRootUid"].isString()||!s["folderUid"].isString()||(!root.isEmpty()&&!nodeUid(root))||(!folder.isEmpty()&&!nodeUid(folder))||(root.isEmpty()&&!folder.isEmpty()))return {"config_invalid","Proton account and folder identifiers are invalid",{}};
 // The optional name is a display hint; storage identity remains the folder UID.
 const auto name=s["folderName"];
 static const QRegularExpression controlCharacter("[\\x00-\\x1f\\x7f]");
 if(!name.isUndefined()&&(!name.isString()||name.toString().isEmpty()||name.toString().toUtf8().size()>255||name.toString().contains(controlCharacter)||folder.isEmpty()))return {"config_invalid","Proton folder display name is invalid",{}};
 const auto days=s["linkExpirationDays"].toDouble(-1),timeout=s["requestTimeoutMs"].toDouble(-1);
 if(!s["shareByDefault"].isBool()||days<0||days>365||days!=int(days)||timeout<1000||timeout>MaximumRequestTimeoutMs||timeout!=int(timeout))return {"config_invalid","Proton sharing or timeout settings are invalid",{}};
 const auto password=s["sharePasswordReference"];
 if(!password.isNull()&&(!password.isString()||password.toString().isEmpty()||password.toString().size()>256))return {"config_invalid","Use a secret-store reference for the share password",{}};
 return {};
}
Error ProtonProvider::validateCredentials(const QJsonObject& reference) const {
 if(reference["mode"]!="keychain"||!reference["reference"].isString()||reference["reference"].toString().isEmpty())
  return {"config_invalid","Proton requires a keychain account reference",{}};
 return {};
}
QJsonObject ProtonProvider::diagnostics() const {return {{"activeHelpers",active_.load()},{"protocolVersion",1}};}
// A helper that found no session in the keychain says `session_unread` and changes nothing:
// the desktop's secret service now and then answers a new process with nothing, or refuses
// it, for an entry that is there, and goes on doing so for as long as that process lives
// (#1418). The request is made once more, by a new helper that is told so and believes what
// it finds: an account that really is not connected answers as before, one helper later.
QJsonObject ProtonProvider::call(const QString& method,const Destination& destination,QJsonObject params,Error& error,std::shared_ptr<std::atomic_bool> cancelled,SetupEvent event,bool* outcomeUncertain,std::function<void(const UploadProgress&)> progress) {
 auto result=callOnce(method,destination,params,error,cancelled,event,outcomeUncertain,progress);
 if(error.code!="session_unread")return result;
 error={};params["sessionRetry"]=true;
 return callOnce(method,destination,params,error,cancelled,event,outcomeUncertain,progress);
}
QJsonObject ProtonProvider::callOnce(const QString& method,const Destination& destination,QJsonObject params,Error& error,std::shared_ptr<std::atomic_bool> cancelled,SetupEvent event,bool* outcomeUncertain,std::function<void(const UploadProgress&)> progress) {
 if(outcomeUncertain)*outcomeUncertain=false;
 if(stopping_.load()){error={"operation_cancelled","The application is stopping",{}};return {};}
 const auto reference=destination.credentialReference;
 if(reference["mode"]!="keychain"||reference["reference"].toString().isEmpty()){error={"proton_credentials_invalid","Proton requires an explicit account keychain reference",{}};return {};}
 // An open session's helper answers this request too, if it is still running. A helper
 // started ahead (see anticipate) is used next. Any other request starts its own.
 const bool carried=QStringList{"checkName","upload","stat","reconcile","share"}.contains(method);
 const auto kept=carried?session(reference["reference"].toString()):nullptr;
 std::unique_ptr<Helper> single;
 // A kept helper that ended, or said anything since its last answer, is not asked again.
 if(kept&&kept->helper){
  auto& waiting=kept->helper->process;
  if(waiting.waitForReadable(0)||!waiting.outputOpen()||!waiting.running()){
   if(waiting.running()){waiting.kill();waiting.waitForFinished(5000);}
   kept->helper.reset();
  }
 }
 QElapsedTimer elapsed;elapsed.start();
 const int timeout=method=="connect"?600000:params["requestTimeoutMs"].toInt(destination.settings["requestTimeoutMs"].toInt(120000));
 if(!kept||!kept->helper){
  auto ready=takeStandby(reference["reference"].toString());
  if(ready&&!carried){
   // Account setup takes the account for itself, once the waiting helper has ended.
   ready->finish([&]{return stopping_.load()||(cancelled&&cancelled->load());});ready.reset();
  }else if(ready){
   // A helper started ahead answers its warm-up before our request: one that is late, refused
   // or unclear ends that helper. One a session passed on has nothing left to say.
   const bool usable=ready->warming?settled(ready->process,qMin(timeout,30000),[&]{return stopping_.load()||(cancelled&&cancelled->load());})
     :ready->process.outputOpen()&&!ready->process.waitForReadable(0);
   if(usable&&ready->process.running())ready->warming=false;
   else{ready->process.kill();ready->process.waitForFinished(5000);ready.reset();}
  }
  if(!ready)ready=start(reference["reference"].toString(),kept!=nullptr,2000,true,cancelled,error);
  if(!ready)return {};
  (kept?kept->helper:single)=std::move(ready);
 }
 if(stopping_.load()||(cancelled&&cancelled->load())){error={"operation_cancelled","Request cancelled before it was sent",{}};return {};}
 auto& helper=*(kept?kept->helper:single);auto& process=helper.process;
 // A helper is asked again only after a clean answer; anything else ends it.
 const auto end=[&]{if(kept)kept->helper.reset();};
 params["schemaVersion"]=1;params["method"]=method;params["destination"]=destination.toJson();
 ++active_;struct Release{std::atomic_int& count;~Release(){--count;}} release{active_};
 auto bytes=QJsonDocument(params).toJson(QJsonDocument::Compact);bytes.append('\n');
 elapsed.restart(); // The request's deadline runs from its dispatch.
 if(bytes.size()>128*1024){process.kill();process.waitForFinished(5000);end();error={"proton_protocol","Helper request exceeded its bound",{}};return {};}
 // Once any request bytes are handed to the helper, a lost or malformed
 // completion cannot establish whether a remote mutation ran.
 const auto delivered=process.write(bytes);if(outcomeUncertain)*outcomeUncertain=delivered>0;
 if(delivered!=bytes.size()){process.kill();process.waitForFinished(5000);end();error={"proton_result_unknown","Helper request could not be delivered",{}};return {};}
 if(!helper.lines)process.closeInput();
 bytes.fill('\0');bytes.clear();
 QByteArray pending;QJsonObject final;bool gotResult=false,gotAuth=false;ProtonOutputBudget outputBudget;
 const auto consume=[&]{
  // The first violation decides the outcome; output left after a kill is not parsed.
  if(error)return;
  auto chunk=process.read(ProtonOutputBudget::MaximumBurstBytes+1);pending+=chunk;
  if(pending.size()>ProtonOutputBudget::MaximumBurstBytes||!outputBudget.consume(chunk.size(),elapsed.elapsed())){error={"proton_protocol","Helper response exceeded its bound",{}};return;}
  while(pending.contains('\n')){
   const auto line=pending.left(pending.indexOf('\n'));pending.remove(0,line.size()+1);QJsonParseError parse;const auto doc=QJsonDocument::fromJson(line,&parse);
   if(parse.error!=QJsonParseError::NoError||!doc.isObject()){error={"proton_protocol","Malformed helper response",{}};return;}
   const auto value=doc.object();
   if(value["event"]=="auth"){
    if(method!="connect"||!event||gotAuth||gotResult||value.size()!=2||!value["url"].isString()){error={"proton_protocol","Unexpected authentication event",{}};return;}
    gotAuth=true;event(value);
   }else if(value["event"]=="progress"){
    auto fields=value;fields.remove("event");const auto report=UploadProgress::fromJson(fields);
    if(method!="upload"||gotResult||!report){error={"proton_protocol","Unexpected or invalid upload progress",{}};return;}
    if(progress)progress(*report);
   }else if(gotResult||!value["ok"].isBool()||value.size()!=2||(value["ok"].toBool()?!value["result"].isObject():!value["error"].isObject())){error={"proton_protocol","Invalid helper completion",{}};return;}
   else{final=value;gotResult=true;}
  }
 };
 while(process.running()){
  process.waitForReadable(100);consume();if(error){process.kill();break;}
  // A helper that answers in turn has answered, and waits for its next request.
  if(helper.lines&&gotResult)break;
  if(stopping_.load()||(cancelled&&cancelled->load())){error={"proton_result_unknown","The request was cancelled after dispatch; reconcile its saved identity before retrying",{{"reason",stopping_.load()?"shutdown":"cancelled"}}};process.kill();break;}
  if(elapsed.elapsed()>timeout){error={"proton_request_timeout",QString("Proton exceeded the configured %1-second request deadline; the outcome is unknown. Reconcile its saved identity before retrying.").arg(timeout/1000.0),{{"reason","deadline"},{"timeoutMs",timeout},{"method",method}}};process.kill();break;}
 }
 const bool waiting=helper.lines&&!error&&gotResult&&process.running();
 if(!waiting){process.waitForFinished(5000);consume();}
 if(!pending.trimmed().isEmpty()&&!error)error={"proton_protocol","Incomplete helper response",{}};
 pending.fill('\0');pending.clear();
 if(error){if(process.running()){process.kill();process.waitForFinished(5000);}end();return {};}
 if(!gotResult||(!waiting&&!process.exitedCleanly())){end();error={"proton_result_unknown","The helper ended without a confirmed result",{}};return {};}
 if(!final["ok"].toBool()||!final["result"].toObject()["error"].toObject().isEmpty())end();
 if(!final["ok"].toBool()){
  error=parseError(final["error"].toObject());if(!error)error={"proton_failed","Proton operation failed",{}};
  // Only protocol-defined pre-mutation failures prove no remote side effect.
  if(outcomeUncertain&&QStringList{"invalid_request","authentication_required","session_unread","account_changed","artifact_changed","keychain_unavailable","invalid_destination","invalid_receipt","keychain_required","unsupported_request","unsupported_condition","unsupported_safe_removal","unsafe_data_directory","destination_unavailable"}.contains(error.code))*outcomeUncertain=false;
  return {};
 }
 return final["result"].toObject();
}
void ProtonProvider::checkName(const Destination& d,const QString& name,std::function<void(NameCheck)> reply,std::shared_ptr<std::atomic_bool> cancellation){Error e;auto r=call("checkName",d,{{"candidate",name}},e,cancellation);reply({e?"inconclusive":r["status"].toString("inconclusive"),r["objectReference"].toObject(),r["concurrencyToken"].toString(),e?e:parseError(r["error"].toObject())});}
void ProtonProvider::upload(const UploadRequest& req,std::function<void(UploadResult)> reply){
 if(req.condition!="absent"){reply({"failed",{}, {"unsupported_capability","Proton cannot safely replace a recorded file revision",{}}});return;}
 QJsonObject params{{"requestTimeoutMs",req.requestTimeoutMs?QJsonValue(*req.requestTimeoutMs):QJsonValue(req.destination.settings["requestTimeoutMs"])},{"artifact",req.artifact.toJson()},{"candidate",req.candidate},{"publicationId",req.publicationId},{"condition",req.condition},{"objectReference",req.objectReference},{"concurrencyToken",req.concurrencyToken}};
 if(req.linkFollows)params["linkFollows"]=true;
 const auto account=req.destination.credentialReference["reference"].toString();
 {std::lock_guard lock(transferHostsMutex_);const auto known=transferHosts_.value(account);if(!known.isEmpty())params["transferHostHint"]=known;}
 Error e;bool uncertain=false;auto r=call("upload",req.destination,params,e,req.cancellation,{},&uncertain,req.progress);
 // Only a Proton storage host is remembered, whatever the answer names.
 if(const auto used=r["transferHost"].toString();!e&&storageHost(used)){std::lock_guard lock(transferHostsMutex_);transferHosts_.insert(account,used);}
 auto error=e?e:parseError(r["error"].toObject());
 // The plan's per-file limit is Proton's own (#147): name the file's size beside it.
 if(error.code=="destination_file_too_large_for_plan"){
  error.message=QStringLiteral("Proton refused this %1 file: it is larger than your Proton plan allows for a single file. Upload a smaller file, or upgrade the Proton plan, then Retry.").arg(ByteSize::localized(req.artifact.bytes,1,QLocale::c()));
  error.details["bytes"]=double(req.artifact.bytes);
 }
 reply({e?(uncertain?"unknown":"failed"):r["status"].toString("unknown"),StorageReceipt::fromJson(r["receipt"].toObject()),error});
}
void ProtonProvider::stat(const StorageReceipt& receipt,std::function<void(StatResult)> reply){Error e;auto r=call("stat",receipt.destination,{{"receipt",receipt.toJson()}},e);reply({e?"unknown":r["status"].toString("unknown"),StorageReceipt::fromJson(r["receipt"].toObject()),e?e:parseError(r["error"].toObject())});}
void ProtonProvider::reconcile(const UploadRequest& req,std::function<void(StatResult)> reply){Error e;auto r=call("reconcile",req.destination,{{"requestTimeoutMs",req.requestTimeoutMs?QJsonValue(*req.requestTimeoutMs):QJsonValue(req.destination.settings["requestTimeoutMs"])},{"artifact",req.artifact.toJson()},{"candidate",req.candidate},{"publicationId",req.publicationId},{"objectReference",req.objectReference}},e,req.cancellation);reply({e?"unknown":r["status"].toString("unknown"),StorageReceipt::fromJson(r["receipt"].toObject()),e?e:parseError(r["error"].toObject())});}
void ProtonProvider::remove(const DeleteRequest&,DurableCheckpoint,std::function<void(DeleteResult)> reply){reply({"unsupported_safe_removal",{}, {"unsupported_capability","Proton does not expose atomic revision-guarded file deletion; revoke the share link instead",{}}});}
void ProtonProvider::share(const ShareRequest& req,DurableCheckpoint checkpoint,std::function<void(ShareResult)> reply){
 const QJsonObject progress{{"schemaVersion",1},{"phase",req.action=="inspect"?"inspect":"intent"},{"operationId",req.id},{"action",req.action}};
 if(req.action!="inspect"){
  auto promise=std::make_shared<std::promise<Error>>();auto future=promise->get_future();checkpoint(progress,[promise](Error e){promise->set_value(e);});
  if(auto e=future.get()){reply({"failed",req.receipt,req.progress,e});return;}
 }
 auto options=req.options;options["action"]=req.action;Error e;bool uncertain=false;auto r=call("share",req.receipt.destination,{{"receipt",req.receipt.toJson()},{"options",options}},e,req.cancellation,{},&uncertain);
 auto receipt=r.contains("receipt")?StorageReceipt::fromJson(r["receipt"].toObject()):req.receipt;
 if(e)receipt.directUrl.clear();
 ShareResult result{e?(uncertain?"unknown":"failed"):r["status"].toString("unknown"),receipt,r["progress"].toObject(),e?e:parseError(r["error"].toObject())};
 const auto expires=QDateTime::fromString(r.value("expiresAt").toString(),Qt::ISODateWithMs);
 if(!e&&expires.isValid())result.expiresAt=expires.toUTC().toString(Qt::ISODateWithMs);
 reply(result);
}
QJsonObject ProtonProvider::setup(const QString& method,const Destination& d,const QJsonObject& input,Error& e,SetupEvent event){
 if(method!="testDestination")return call(method,d,{{"options",input}},e,{},std::move(event));
 const auto root=d.settings["accountRootUid"].toString(),folder=d.settings["folderUid"].toString();
 if(root.isEmpty()||folder.isEmpty()){
  e={"proton_setup_required","Connect your Proton account and choose an upload folder before testing",{}};return {};
 }
 const auto account=call("inspectAccount",d,{{"options",input}},e,{},std::move(event));
 if(e)return {};
 if(account["accountRootUid"]!=root){e={"proton_account_changed","Reconnect the saved Proton account before testing",{}};return {};}
 bool found=folder==root;
 for(const auto& value:account["folders"].toArray())if(value.toObject()["uid"]==folder)found=true;
 if(!found){e={"proton_folder_unavailable","The selected Proton folder is unavailable. Refresh folders and choose an upload folder",{}};return {};}
 return {{"testScope","account-folder"}};
}
}
