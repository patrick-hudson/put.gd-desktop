#include "OutputBudget.h"
#include "ProtonProvider.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
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
bool runtimeVerified() {
 QFile file(runtimePath());QCryptographicHash hash(QCryptographicHash::Sha512);
 return !QString::fromUtf8(PUTGD_PROTON_RUNTIME_SHA512).isEmpty() && file.open(QIODevice::ReadOnly)
   && hash.addData(&file) && hash.result().toHex()==QByteArray(PUTGD_PROTON_RUNTIME_SHA512);
}
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
 if(!runtimeVerified()){error={"proton_runtime_unavailable","Install a matching put.gd package with the bundled Proton runtime",{}};return {};}
 const auto reference=destination.credentialReference;
 if(reference["mode"]!="keychain"||reference["reference"].toString().isEmpty()){error={"proton_credentials_invalid","Proton requires an explicit account keychain reference",{}};return {};}
 // Separate helper processes share a rotating auth session. Serialize the
 // whole request by keychain reference, including setup and token refresh.
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
 QLockFile accountLock(QDir(lockRoot).filePath(accountKey+".lock"));accountLock.setStaleLockTime(0);
 if(!accountLock.tryLock(2000)){error={"proton_account_busy","Another request is using this Proton account; wait and retry",{}};return {};}
 if(stopping_.load()||(cancelled&&cancelled->load())){error={"operation_cancelled","Request cancelled before the helper started",{}};return {};}
 QTemporaryDir privateRoot(QDir::tempPath()+"/putgd-proton-XXXXXX");
 if(!privateRoot.isValid()){error={"proton_private_storage","Private helper storage is unavailable",{}};return {};}
 QFile::setPermissions(privateRoot.path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 params["schemaVersion"]=1;params["method"]=method;params["destination"]=destination.toJson();
 auto env=QProcessEnvironment::systemEnvironment();for(const auto& key:env.keys())if(key.startsWith("PROTON_DRIVE_")||key.startsWith("PUTGD_PROTON_"))env.remove(key);
 env.insert("PUTGD_PROTON_DATA_DIR",privateRoot.path());
 QProcess process;process.setProcessEnvironment(env);process.setProcessChannelMode(QProcess::SeparateChannels);
 ++active_;struct Release{std::atomic_int& count;~Release(){--count;}} release{active_};
 process.start(runtimePath(),{"--stdio"});
 if(!process.waitForStarted(5000)){error={"proton_runtime_start","The bundled Proton runtime could not start",{}};return {};}
 auto bytes=QJsonDocument(params).toJson(QJsonDocument::Compact);bytes.append('\n');
 if(bytes.size()>128*1024){process.kill();process.waitForFinished(5000);error={"proton_protocol","Helper request exceeded its bound",{}};return {};}
 // Once any request bytes are handed to the helper, a lost or malformed
 // completion cannot establish whether a remote mutation ran.
 const auto delivered=process.write(bytes);if(outcomeUncertain)*outcomeUncertain=delivered>0;
 if(delivered!=bytes.size()){process.kill();process.waitForFinished(5000);error={"proton_result_unknown","Helper request could not be delivered",{}};return {};}
 process.closeWriteChannel();bytes.fill('\0');bytes.clear();
 QElapsedTimer elapsed;elapsed.start();QByteArray pending;QJsonObject final;bool gotResult=false,gotAuth=false;ProtonOutputBudget outputBudget;
 const int timeout=method=="connect"?600000:params["requestTimeoutMs"].toInt(destination.settings["requestTimeoutMs"].toInt(120000));
 const auto consume=[&]{
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
  if(stopping_.load()||(cancelled&&cancelled->load())){error={"proton_result_unknown","The request was cancelled after dispatch; reconcile its saved identity before retrying",{{"reason",stopping_.load()?"shutdown":"cancelled"}}};process.kill();break;}
  if(elapsed.elapsed()>timeout){error={"proton_request_timeout",QString("Proton exceeded the configured %1-second request deadline; the outcome is unknown. Reconcile its saved identity before retrying.").arg(timeout/1000.0),{{"reason","deadline"},{"timeoutMs",timeout},{"method",method}}};process.kill();break;}
 }
 process.waitForFinished(5000);consume();
 if(!pending.trimmed().isEmpty()&&!error)error={"proton_protocol","Incomplete helper response",{}};
 pending.fill('\0');pending.clear();
 if(error)return {};
 if(!gotResult||process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0){error={"proton_result_unknown","The helper ended without a confirmed result",{}};return {};}
 if(!final["ok"].toBool()){
  error=parseError(final["error"].toObject());if(!error)error={"proton_failed","Proton operation failed",{}};
  // Only protocol-defined pre-mutation failures prove no remote side effect.
  if(outcomeUncertain&&QStringList{"invalid_request","authentication_required","account_changed","artifact_changed","keychain_unavailable","invalid_destination","invalid_receipt","keychain_required","unsupported_request","unsupported_condition","unsupported_safe_removal","unsafe_data_directory","destination_unavailable"}.contains(error.code))*outcomeUncertain=false;
  return {};
 }
 return final["result"].toObject();
}
void ProtonProvider::checkName(const Destination& d,const QString& name,std::function<void(NameCheck)> reply){Error e;auto r=call("checkName",d,{{"candidate",name}},e);reply({e?"inconclusive":r["status"].toString("inconclusive"),r["objectReference"].toObject(),r["concurrencyToken"].toString(),e?e:parseError(r["error"].toObject())});}
void ProtonProvider::upload(const UploadRequest& req,std::function<void(UploadResult)> reply){
 if(req.condition!="absent"){reply({"failed",{}, {"unsupported_capability","Proton cannot safely replace a recorded file revision",{}}});return;}
 Error e;bool uncertain=false;auto r=call("upload",req.destination,{{"requestTimeoutMs",req.requestTimeoutMs?QJsonValue(*req.requestTimeoutMs):QJsonValue(req.destination.settings["requestTimeoutMs"])},{"artifact",req.artifact.toJson()},{"candidate",req.candidate},{"publicationId",req.publicationId},{"condition",req.condition},{"objectReference",req.objectReference},{"concurrencyToken",req.concurrencyToken}},e,req.cancellation,{},&uncertain,req.progress);
 reply({e?(uncertain?"unknown":"failed"):r["status"].toString("unknown"),StorageReceipt::fromJson(r["receipt"].toObject()),e?e:parseError(r["error"].toObject())});
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
 reply({e?(uncertain?"unknown":"failed"):r["status"].toString("unknown"),receipt,r["progress"].toObject(),e?e:parseError(r["error"].toObject())});
}
QJsonObject ProtonProvider::setup(const QString& method,const Destination& d,const QJsonObject& input,Error& e,SetupEvent event){return call(method,d,{{"options",input}},e,{},std::move(event));}
}
