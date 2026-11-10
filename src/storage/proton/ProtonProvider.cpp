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
#include <QProcess>
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
#include <future>
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
// size or change time, and is hashed again. A caller waits for a hash already under way.
bool runtimeVerified() {
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
 std::lock_guard lock(mutex);
 Identity before,after;if(!identify(before))return false;
 if(verified&&*verified==before)return true;
 if(!runtimeHashMatches())return false;
 // Only a file that stayed the same while it was hashed is remembered.
 if(identify(after)&&after==before)verified=after;
 return true;
#else
 return runtimeHashMatches();
#endif
}
}
struct ProtonProvider::Helper {
 QLockFile lock;QTemporaryDir root;QProcess process;
 explicit Helper(const QString& lockPath):lock(lockPath),root(QDir::tempPath()+"/putgd-proton-XXXXXX"){}
 // A helper still waiting for a request ends when its input closes.
 ~Helper(){
  if(process.state()==QProcess::NotRunning)return;
  process.closeWriteChannel();if(!process.waitForFinished(2000)){process.kill();process.waitForFinished(5000);}
 }
};
struct ProtonProvider::Session {QString reference;std::unique_ptr<Helper> helper;};
namespace {
// A QProcess belongs to the thread that made it, so a session is found only by its own thread.
QHash<const ProtonProvider*,std::weak_ptr<void>>& threadSessions() {thread_local QHash<const ProtonProvider*,std::weak_ptr<void>> sessions;return sessions;}
}
ProtonProvider::~ProtonProvider() {if(verifier_.joinable())verifier_.join();}
void ProtonProvider::prepare() {
 if(prepared_.exchange(true))return;
 verifier_=std::thread([]{runtimeVerified();});
}
std::shared_ptr<void> ProtonProvider::openSession(const Destination& destination) {
 const auto reference=destination.credentialReference["reference"].toString();
 if(reference.isEmpty()||threadSessions().value(this).lock())return {};
 auto opened=std::make_shared<Session>();opened->reference=reference;threadSessions().insert(this,opened);return opened;
}
std::shared_ptr<ProtonProvider::Session> ProtonProvider::session(const QString& reference) const {
 auto current=std::static_pointer_cast<Session>(threadSessions().value(this).lock());
 return current&&current->reference==reference?current:nullptr;
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
 if(!name.isUndefined()&&(!name.isString()||name.toString().isEmpty()||name.toString().toUtf8().size()>255||name.toString().contains(QRegularExpression("[\\x00-\\x1f\\x7f]"))||folder.isEmpty()))return {"config_invalid","Proton folder display name is invalid",{}};
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
QJsonObject ProtonProvider::call(const QString& method,const Destination& destination,QJsonObject params,Error& error,std::shared_ptr<std::atomic_bool> cancelled,SetupEvent event,bool* outcomeUncertain,std::function<void(const UploadProgress&)> progress) {
 if(outcomeUncertain)*outcomeUncertain=false;
 if(stopping_.load()){error={"operation_cancelled","The application is stopping",{}};return {};}
 const auto reference=destination.credentialReference;
 if(reference["mode"]!="keychain"||reference["reference"].toString().isEmpty()){error={"proton_credentials_invalid","Proton requires an explicit account keychain reference",{}};return {};}
 // An open session's helper answers this request too, if it is still running; any
 // other request starts a helper of its own that ends with it.
 const auto kept=QStringList{"checkName","upload","stat","reconcile","share"}.contains(method)?session(reference["reference"].toString()):nullptr;
 std::unique_ptr<Helper> single;
 // A kept helper that ended, or said anything since its last answer, is not asked again.
 if(kept&&kept->helper){
  auto& waiting=kept->helper->process;
  if(waiting.waitForReadyRead(0)||waiting.bytesAvailable()>0||waiting.waitForFinished(0)||waiting.state()!=QProcess::Running){
   if(waiting.state()!=QProcess::NotRunning){waiting.kill();waiting.waitForFinished(5000);}
   kept->helper.reset();
  }
 }
 if(!kept||!kept->helper){
  if(!runtimeVerified()){error={"proton_runtime_unavailable","Install a matching put.gd package with the bundled Proton runtime",{}};return {};}
  // Separate helper processes share a rotating auth session. Serialize the
  // whole helper by keychain reference, including setup and token refresh.
#ifdef Q_OS_UNIX
  const auto lockRoot=QString("/tmp/putgd-proton-accounts-%1").arg(getuid());
#else
  const auto lockRoot=QDir::homePath()+"/AppData/Local/putgd/proton-accounts";
#endif
#ifdef Q_OS_UNIX
  // mkdir is atomic and does not follow a pre-existing /tmp symlink.
  if(::mkdir(QFile::encodeName(lockRoot).constData(),0700)!=0&&errno!=EEXIST){error={"proton_lock_unavailable","Private account coordination is unavailable",{}};return {};}
#else
  if(!QDir().mkpath(lockRoot)){error={"proton_lock_unavailable","Private account coordination is unavailable",{}};return {};}
#endif
  const QFileInfo lockInfo(lockRoot);
#ifdef Q_OS_UNIX
  if(lockInfo.ownerId()!=getuid()||lockInfo.permissions()&(QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|QFile::ReadOther|QFile::WriteOther|QFile::ExeOther)){error={"proton_lock_unavailable","Account coordination directory is not private",{}};return {};}
#endif
  if(!lockInfo.isDir()||lockInfo.isSymLink()||lockInfo.canonicalFilePath()!=lockInfo.absoluteFilePath()){error={"proton_lock_unavailable","Account coordination directory is invalid",{}};return {};}
  const auto accountKey=QString::fromLatin1(QCryptographicHash::hash(reference["reference"].toString().toUtf8(),QCryptographicHash::Sha256).toHex());
  auto started=std::make_unique<Helper>(QDir(lockRoot).filePath(accountKey+".lock"));started->lock.setStaleLockTime(0);
  if(!started->lock.tryLock(2000)){error={"proton_account_busy","Another request is using this Proton account; wait and retry",{}};return {};}
  if(stopping_.load()||(cancelled&&cancelled->load())){error={"operation_cancelled","Request cancelled before the helper started",{}};return {};}
  if(!started->root.isValid()){error={"proton_private_storage","Private helper storage is unavailable",{}};return {};}
  QFile::setPermissions(started->root.path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
  started->process.setProcessEnvironment(helperEnvironment(host::environment(),started->root.path()));started->process.setProcessChannelMode(QProcess::SeparateChannels);
  started->process.start(runtimePath(),{kept?"--session":"--stdio"});
  if(!started->process.waitForStarted(5000)){error={"proton_runtime_start","The bundled Proton runtime could not start",{}};return {};}
  (kept?kept->helper:single)=std::move(started);
 }
 else if(stopping_.load()||(cancelled&&cancelled->load())){error={"operation_cancelled","Request cancelled before it was sent",{}};return {};}
 auto& process=(kept?kept->helper:single)->process;
 // A kept helper is reused only after a clean answer; anything else ends it.
 const auto end=[&]{if(kept)kept->helper.reset();};
 params["schemaVersion"]=1;params["method"]=method;params["destination"]=destination.toJson();
 ++active_;struct Release{std::atomic_int& count;~Release(){--count;}} release{active_};
 auto bytes=QJsonDocument(params).toJson(QJsonDocument::Compact);bytes.append('\n');
 if(bytes.size()>128*1024){process.kill();process.waitForFinished(5000);end();error={"proton_protocol","Helper request exceeded its bound",{}};return {};}
 // Once any request bytes are handed to the helper, a lost or malformed
 // completion cannot establish whether a remote mutation ran.
 const auto delivered=process.write(bytes);if(outcomeUncertain)*outcomeUncertain=delivered>0;
 if(delivered!=bytes.size()){process.kill();process.waitForFinished(5000);end();error={"proton_result_unknown","Helper request could not be delivered",{}};return {};}
 if(!kept)process.closeWriteChannel();
 bytes.fill('\0');bytes.clear();
 QElapsedTimer elapsed;elapsed.start();QByteArray pending;QJsonObject final;bool gotResult=false,gotAuth=false;ProtonOutputBudget outputBudget;
 const int timeout=method=="connect"?600000:params["requestTimeoutMs"].toInt(destination.settings["requestTimeoutMs"].toInt(120000));
 const auto consume=[&]{
  // The first violation decides the outcome; output left after a kill is not parsed.
  if(error)return;
  process.readAllStandardError();process.setReadChannel(QProcess::StandardOutput);auto chunk=process.read(ProtonOutputBudget::MaximumBurstBytes+1);pending+=chunk;
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
 while(process.state()!=QProcess::NotRunning){
  process.waitForReadyRead(100);consume();if(error){process.kill();break;}
  // A kept helper has answered and now waits for its next request.
  if(kept&&gotResult)break;
  if(stopping_.load()||(cancelled&&cancelled->load())){error={"proton_result_unknown","The request was cancelled after dispatch; reconcile its saved identity before retrying",{{"reason",stopping_.load()?"shutdown":"cancelled"}}};process.kill();break;}
  if(elapsed.elapsed()>timeout){error={"proton_request_timeout",QString("Proton exceeded the configured %1-second request deadline; the outcome is unknown. Reconcile its saved identity before retrying.").arg(timeout/1000.0),{{"reason","deadline"},{"timeoutMs",timeout},{"method",method}}};process.kill();break;}
 }
 const bool waiting=kept&&!error&&gotResult&&process.state()==QProcess::Running;
 if(!waiting){process.waitForFinished(5000);consume();}
 if(!pending.trimmed().isEmpty()&&!error)error={"proton_protocol","Incomplete helper response",{}};
 pending.fill('\0');pending.clear();
 if(error){if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(5000);}end();return {};}
 if(!gotResult||(!waiting&&(process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0))){end();error={"proton_result_unknown","The helper ended without a confirmed result",{}};return {};}
 if(!final["ok"].toBool()||!final["result"].toObject()["error"].toObject().isEmpty())end();
 if(!final["ok"].toBool()){
  error=parseError(final["error"].toObject());if(!error)error={"proton_failed","Proton operation failed",{}};
  // Only protocol-defined pre-mutation failures prove no remote side effect.
  if(outcomeUncertain&&QStringList{"invalid_request","authentication_required","account_changed","artifact_changed","keychain_unavailable","invalid_destination","invalid_receipt","keychain_required","unsupported_request","unsupported_condition","unsupported_safe_removal","unsafe_data_directory","destination_unavailable"}.contains(error.code))*outcomeUncertain=false;
  return {};
 }
 return final["result"].toObject();
}
void ProtonProvider::checkName(const Destination& d,const QString& name,std::function<void(NameCheck)> reply,std::shared_ptr<std::atomic_bool> cancellation){Error e;auto r=call("checkName",d,{{"candidate",name}},e,cancellation);reply({e?"inconclusive":r["status"].toString("inconclusive"),r["objectReference"].toObject(),r["concurrencyToken"].toString(),e?e:parseError(r["error"].toObject())});}
void ProtonProvider::upload(const UploadRequest& req,std::function<void(UploadResult)> reply){
 if(req.condition!="absent"){reply({"failed",{}, {"unsupported_capability","Proton cannot safely replace a recorded file revision",{}}});return;}
 Error e;bool uncertain=false;auto r=call("upload",req.destination,{{"requestTimeoutMs",req.requestTimeoutMs?QJsonValue(*req.requestTimeoutMs):QJsonValue(req.destination.settings["requestTimeoutMs"])},{"artifact",req.artifact.toJson()},{"candidate",req.candidate},{"publicationId",req.publicationId},{"condition",req.condition},{"objectReference",req.objectReference},{"concurrencyToken",req.concurrencyToken}},e,req.cancellation,{},&uncertain,req.progress);
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
