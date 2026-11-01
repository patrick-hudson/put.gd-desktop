#include "SetupService.h"
#include <QPointer>
#include <QUuid>

namespace putgd {
namespace {
Error invalid() {return {"invalid_request","Invalid setup fields or parameter types",{}};}
bool validInput(const QString& kind,const QJsonObject& input) {
 if(kind=="createFolder")return input.size()==1&&input["name"].isString()&&!input["name"].toString().isEmpty()&&input["name"].toString().size()<=255;
 if(kind=="storeSharePassword")return input.size()==2&&input["password"].isString()&&input["password"].toString().size()<=4096&&input["reference"].isString()&&!input["reference"].toString().isEmpty()&&input["reference"].toString().size()<=256;
 if(kind!="storeCredentials")return input.isEmpty();
 if(input.size()<2||input.size()>3)return false;
 for(auto it=input.begin();it!=input.end();++it)
  if(it.key()!="accessKey"&&it.key()!="secretKey"&&it.key()!="sessionToken")return false;
 for(const auto& key:{"accessKey","secretKey"})
  if(!input[key].isString()||input[key].toString().isEmpty()||input[key].toString().size()>4096)return false;
 return !input.contains("sessionToken")||(input["sessionToken"].isString()&&input["sessionToken"].toString().size()<=16384);
}
QString activeKey(const QString& profileId,const QString& kind) {return profileId+"/"+kind;}
}
SetupService::SetupService(SettingsService& settings,CredentialStore& credentials,DestinationTester tester,QObject* parent)
 :QObject(parent),settings_(settings),credentials_(credentials),tester_(std::move(tester)),completion_(std::make_shared<CompletionContext>()) {
 completion_->owner=this;
}
SetupService::~SetupService() {
 std::lock_guard lock(completion_->mutex);completion_->owner=nullptr;
}
void SetupService::start(const QString& kind,const QString& profileId,const QString& expectedRevision,const QJsonObject& input,JsonReply reply) {
 if(!QStringList{"storeCredentials","unlockCredentials","testDestination","connectAccount","inspectAccount","createFolder","storeSharePassword"}.contains(kind)||!validInput(kind,input)) {reply(invalid(),{});return;}
 const auto metadata=settings_.metadata();
 if(expectedRevision!=settings_.revision()||expectedRevision!=metadata["observedRevision"].toString()) {
  reply({"config_conflict","Configuration changed; save or reload before setup",{}},{});return;
 }
 if(!metadata["validation"].toObject()["ok"].toBool()) {
  reply({"config_invalid","Resolve the configuration error before setup",{}},{});return;
 }
 const auto profiles=settings_.snapshot().profiles;
 if(!profiles.contains(profileId)) {reply({"profile_not_found","Save the destination before setup",{}},{});return;}
 const auto profile=profiles[profileId];const auto reference=profile["credentials"].toObject();
 const auto key=activeKey(profileId,kind);
 if(active_.contains(key)||active_.size()>=32) {reply({"setup_busy","Wait for the active setup job to complete",{}},{});return;}
 const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
 jobs_.insert(id,{{"setupId",id},{"kind",kind},{"profileId",profileId},{"state","running"},{"error",Error{}.toJson()}});
 active_.insert(key);
 // Only a safe job ID and lifetime context cross the asynchronous boundary.
 // Queue under the lock so destruction cannot race the context passed to Qt.
 const auto complete=[context=completion_,id](Error error) {
  std::lock_guard lock(context->mutex);
  if(auto* owner=context->owner)
   QMetaObject::invokeMethod(owner,[owner,id,error=std::move(error)]() mutable {owner->finish(id,std::move(error));},Qt::QueuedConnection);
 };
 QPointer<SetupService> guarded(this);
 reply({},{{"setupId",id}});
 if(!guarded)return;
 emit changed(id);
 if(!guarded)return;
 if((kind=="testDestination"&&providerSetup_)||QStringList{"connectAccount","inspectAccount","createFolder","storeSharePassword"}.contains(kind)) {
  if(!providerSetup_){complete({"setup_unsupported","Account setup is unavailable",{}});return;}
  const Destination destination{profile["provider"].toString(),profileId,expectedRevision,profile["settings"].toObject(),reference};
  auto finish=[context=completion_,id](Error error,QJsonObject result){
   std::lock_guard lock(context->mutex);if(auto* owner=context->owner)QMetaObject::invokeMethod(owner,[owner,id,error,result]{owner->finish(id,error,result);},Qt::QueuedConnection);
  };
  auto event=[context=completion_,id](QJsonObject value){
   std::lock_guard lock(context->mutex);if(auto* owner=context->owner)QMetaObject::invokeMethod(owner,[owner,id,value]{if(owner->jobs_.contains(id)&&owner->jobs_[id]["state"]=="running"){auto job=owner->jobs_[id];job["event"]=value;owner->jobs_[id]=job;emit owner->changed(id);}},Qt::QueuedConnection);
  };
  providerSetup_(kind,destination,input,std::move(finish),std::move(event));
 }else if(kind=="storeCredentials") {
  Credentials value{input["accessKey"].toString(),input["secretKey"].toString(),input["sessionToken"].toString()};
  credentials_.save(profileId,reference,std::move(value),complete);
 }else if(kind=="unlockCredentials")credentials_.unlock(profileId,reference,complete);
 else if(tester_)tester_(profileId,profile,complete);
 else complete({"test_unsupported","This resident has no destination test",{}});
}
QStringList SetupService::unlockSavedCredentials() {
 QStringList started;
 if(std::exchange(startupUnlockDone_,true))return started;
 const auto snapshot=settings_.snapshot();
 if(!snapshot.unlockCredentialsAtStartup||!settings_.metadata()["validation"].toObject()["ok"].toBool())return started;
 for(auto it=snapshot.profiles.cbegin();it!=snapshot.profiles.cend();++it){
  const auto reference=it.value()["credentials"].toObject();
  // Destinations whose keys live in put.gd's credential store: S3 and Backblaze B2 (#282).
  if((it.value()["provider"]!="s3"&&it.value()["provider"]!="backblaze-b2")||reference["mode"]!="keychain"||credentials_.cached(it.key(),reference))continue;
  start("unlockCredentials",it.key(),settings_.revision(),{},[&](Error error,QJsonObject result){if(!error)started.append(result["setupId"].toString());});
 }
 for(const auto& id:started)if(jobs_.value(id)["state"]=="running")startupUnlocks_.insert(id);
 // An unlock someone already started for a profile holds the gate as well.
 for(auto job=jobs_.cbegin();job!=jobs_.cend();++job)
  if((*job)["kind"]=="unlockCredentials"&&(*job)["state"]=="running"&&snapshot.profiles.contains((*job)["profileId"].toString()))startupUnlocks_.insert(job.key());
 return started;
}
void SetupService::credentialHint(const QString& profileId,const QString& expectedRevision,JsonReply reply) {
 if(expectedRevision!=settings_.revision()){reply({"config_conflict","Configuration changed; refresh this destination",{}},{});return;}
 const auto profiles=settings_.snapshot().profiles;
 if(!profiles.contains(profileId)){reply({"profile_not_found","This destination is not saved yet",{}},{});return;}
 const auto profile=profiles[profileId];const auto reference=profile["credentials"].toObject();
 // A UI hint never unlocks or reads the OS secret store. Explicit credential
 // setup already owns that consent, and only this bounded hint leaves its cache.
 const auto cached=credentials_.cached(profileId,reference);
 QJsonObject hint{{"available",bool(cached)},{"mode",reference["mode"]}};
 if(cached){hint["accessKeyHint"]=cached->accessKey.size()>4?cached->accessKey.left(4)+QString::fromUtf8("…"):QString::fromUtf8("••••");hint["secretStored"]=!cached->secretKey.isEmpty();hint["tokenStored"]=!cached->sessionToken.isEmpty();}
 reply({},hint);
}
void SetupService::revealCredentialSecret(const QString& profileId,const QString& expectedRevision,JsonReply reply) {
 const auto metadata=settings_.metadata();
 if(expectedRevision!=settings_.revision()||expectedRevision!=metadata["observedRevision"].toString()){
  reply({"config_conflict","Configuration changed; refresh this destination",{}},{});return;
 }
 if(!metadata["validation"].toObject()["ok"].toBool()){
  reply({"config_invalid","Resolve the configuration error before showing credentials",{}},{});return;
 }
 const auto profiles=settings_.snapshot().profiles;
 if(!profiles.contains(profileId)){reply({"profile_not_found","Save the destination before showing credentials",{}},{});return;}
 const auto profile=profiles[profileId];
 if(profile["provider"]!="s3"&&profile["provider"]!="backblaze-b2"){reply({"setup_unsupported","This destination has no stored secret key",{}},{});return;}
 const auto cached=credentials_.cached(profileId,profile["credentials"].toObject());
 if(!cached){reply({"credential_setup_required","Unlock saved credentials first, or enter credentials for this session",{}},{});return;}
 reply({},{{"secretKey",cached->secretKey}});
}
void SetupService::inspect(const QString& id,JsonReply reply) {
 const auto found=jobs_.constFind(id);
 if(found==jobs_.cend()) {reply({"setup_not_found","Setup job is no longer available in this resident",{}},{});return;}
 reply({},*found);
}
void SetupService::finish(const QString& id,Error error,QJsonObject result) {
 const auto found=jobs_.find(id);if(found==jobs_.end()||(*found)["state"]!="running")return;
 auto& record=*found;record.remove("event");record["result"]=result;record["state"]=error?"failed":"completed";record["error"]=error.toJson();
 active_.remove(activeKey(record["profileId"].toString(),record["kind"].toString()));
 startupUnlocks_.remove(id);
 completed_.enqueue(id);while(completed_.size()>64)jobs_.remove(completed_.dequeue());
 emit changed(id);
}
}
