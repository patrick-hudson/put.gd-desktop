#include "SetupService.h"
#include <QPointer>
#include <QUuid>

namespace putgd {
namespace {
Error invalid() {return {"invalid_request","Invalid setup fields or parameter types",{}};}
bool validInput(const QString& kind,const QJsonObject& input) {
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
 if(!QStringList{"storeCredentials","unlockCredentials","testDestination"}.contains(kind)||!validInput(kind,input)) {reply(invalid(),{});return;}
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
 if(kind=="storeCredentials") {
  Credentials value{input["accessKey"].toString(),input["secretKey"].toString(),input["sessionToken"].toString()};
  credentials_.save(profileId,reference,std::move(value),complete);
 }else if(kind=="unlockCredentials")credentials_.unlock(profileId,reference,complete);
 else if(tester_)tester_(profileId,profile,complete);
 else complete({"test_unsupported","This resident has no destination test",{}});
}
void SetupService::inspect(const QString& id,JsonReply reply) {
 const auto found=jobs_.constFind(id);
 if(found==jobs_.cend()) {reply({"setup_not_found","Setup job is no longer available in this resident",{}},{});return;}
 reply({},*found);
}
void SetupService::finish(const QString& id,Error error) {
 const auto found=jobs_.find(id);if(found==jobs_.end()||(*found)["state"]!="running")return;
 auto& record=*found;record["state"]=error?"failed":"completed";record["error"]=error.toJson();
 active_.remove(activeKey(record["profileId"].toString(),record["kind"].toString()));
 completed_.enqueue(id);while(completed_.size()>64)jobs_.remove(completed_.dequeue());
 emit changed(id);
}
}
