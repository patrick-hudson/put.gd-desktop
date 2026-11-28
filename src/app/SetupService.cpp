#include "SetupService.h"
#include "core/DestinationCheck.h"
#include "core/EncryptJob.h"
#include <QJsonArray>
#include <QPointer>
#include <QUuid>
#include <optional>

namespace putgd {
namespace {
Error invalid() {return {"invalid_request","Invalid setup fields or parameter types",{}};}
bool validInput(const QString& kind,const QJsonObject& input) {
 // An SFTP folder path may be up to 4096 bytes (#1871); Proton's folder name keeps its own 255 below.
 if(kind=="createFolder"){
  // SFTP also names the path its test found missing (#1871); Proton names the folder only.
  if(input.contains("path")&&(input.size()!=2||!input["path"].isString()||input["path"].toString().isEmpty()||input["path"].toString().toUtf8().size()>4096))return false;
  return input.size()==(input.contains("path")?2:1)&&input["name"].isString()&&!input["name"].toString().isEmpty()&&input["name"].toString().toUtf8().size()<=4096;
 }
 // SFTP (#1871): the host key fingerprint the user (or agent) was shown, exactly.
 if(kind=="trustHostKey")return input.size()==1&&input["fingerprint"].isString()&&!input["fingerprint"].toString().isEmpty()&&input["fingerprint"].toString().size()<=128;
 if(kind=="storeSharePassword")return input.size()==2&&input["password"].isString()&&input["password"].toString().size()<=4096&&input["reference"].isString()&&!input["reference"].toString().isEmpty()&&input["reference"].toString().size()<=256;
 // #2079: a destination's Encrypted links passphrase, one line of 1 to 1024 bytes.
 if(kind=="storeEncryptPassphrase")return input.size()==1&&input["passphrase"].isString()&&!input["passphrase"].toString().isEmpty()&&input["passphrase"].toString().normalized(QString::NormalizationForm_C).toUtf8().size()<=1024&&!input["passphrase"].toString().contains('\n')&&!input["passphrase"].toString().contains('\r');
 if(kind!="storeCredentials")return input.isEmpty();
 // Named values of a custom HTTP uploader: {"values":{key:value}}; the provider checks them.
 if(input.size()==1&&input.contains("values")) {
  const auto values=input["values"].toObject();
  if(!input["values"].isObject()||values.size()>maximumNamedValues)return false;
  for(auto it=values.begin();it!=values.end();++it)if(it.key().isEmpty()||it.key().size()>300||!it.value().isString()||it.value().toString().size()>65536)return false;
  return true;
 }
 if(input.size()<2||input.size()>3)return false;
 for(auto it=input.begin();it!=input.end();++it)
  if(it.key()!="accessKey"&&it.key()!="secretKey"&&it.key()!="sessionToken")return false;
 for(const auto& key:{"accessKey","secretKey"})
  if(!input[key].isString()||input[key].toString().isEmpty()||input[key].toString().size()>4096)return false;
 return !input.contains("sessionToken")||(input["sessionToken"].isString()&&input["sessionToken"].toString().size()<=16384);
}
// Signing in, signing out, storing credentials, the account inspection that binds them and a Test
// (which binds a hand-stored Nextcloud app password first) use the same credential: one at a time
// per destination (#1872).
QString activeKey(const QString& profileId,const QString& kind) {
 // #2079: storing and unlocking one Encrypted links passphrase run one at a time, as for a keychain entry.
 if(kind=="storeEncryptPassphrase"||kind=="unlockEncryptPassphrase")return profileId+"/encryptPassphrase";
 return profileId+"/"+(QStringList{"connectAccount","signOut","storeCredentials","inspectAccount","testDestination"}.contains(kind)?QStringLiteral("account"):kind);
}
}
// Destinations whose credentials live in put.gd's credential store: S3, Backblaze B2
// (#282), Cloudflare R2 (#1678), Nextcloud and WebDAV (#1872), and a provider with named values
// when its settings need any (#1361).
bool SetupService::storesKeys(const QJsonObject& profile) {
 const auto provider=profile.value("provider").toString();
 return provider=="s3"||provider=="backblaze-b2"||provider=="cloudflare-r2"||provider=="nextcloud"||provider=="webdav";
}
bool SetupService::usesCredentialStore(const QJsonObject& profile) const {
 if(storesKeys(profile))return true;
 QStringList names;
 return namedCredentials_&&!namedCredentials_(profile,{},&names)&&!names.isEmpty();
}
SetupService::SetupService(SettingsService& settings,CredentialStore& credentials,DestinationTester tester,QObject* parent)
 :QObject(parent),settings_(settings),credentials_(credentials),tester_(std::move(tester)),completion_(std::make_shared<CompletionContext>()) {
 completion_->owner=this;
}
SetupService::~SetupService() {
 std::lock_guard lock(completion_->mutex);completion_->owner=nullptr;
}
void SetupService::start(const QString& kind,const QString& profileId,const QString& expectedRevision,const QJsonObject& input,JsonReply reply) {
 if(!QStringList{"storeCredentials","unlockCredentials","testDestination","connectAccount","inspectAccount","createFolder","storeSharePassword","signOut","trustHostKey","storeEncryptPassphrase","unlockEncryptPassphrase"}.contains(kind)||!validInput(kind,input)) {reply(invalid(),{});return;}
 const auto metadata=settings_.metadata();
 if(expectedRevision!=settings_.revision()||expectedRevision!=metadata["observedRevision"].toString()) {
  reply({"config_conflict","Settings changed elsewhere. Save or reload before continuing.",{}},{});return;
 }
 if(!metadata["validation"].toObject()["ok"].toBool()) {
  reply({"config_invalid","Fix the settings error before continuing.",{}},{});return;
 }
 const auto profiles=settings_.snapshot().profiles;
 if(!profiles.contains(profileId)) {reply({"profile_not_found","Save the destination first.",{}},{});return;}
 const auto profile=profiles[profileId];const auto reference=profile["credentials"].toObject();
 if(kind=="createFolder"&&profile["provider"]!="sftp"&&input["name"].toString().size()>255){reply(invalid(),{});return;}
 const auto key=activeKey(profileId,kind);
 // Destinations can share one keychain entry: a store (or Clear) reads, merges and writes it
 // whole, so only one at a time per entry, from the read through the write (#1890).
 // An unlock holds it too, so a read begun before a store can't put older values back after it.
 const auto encryptReference=profile["encrypt"].toObject()["passphraseCredentials"].toObject();
 const bool encryptKind=kind=="storeEncryptPassphrase"||kind=="unlockEncryptPassphrase";
 // #2079: a Encrypted links passphrase's keychain entry takes the same one-at-a-time slot as any entry.
 const auto entry=encryptKind?(encryptReference["mode"]=="keychain"&&!encryptReference["reference"].toString().isEmpty()?"keychain:"+encryptReference["reference"].toString():QString())
  :(kind=="storeCredentials"||kind=="unlockCredentials")&&reference["mode"]=="keychain"&&!reference["reference"].toString().isEmpty()?"keychain:"+reference["reference"].toString():QString();
 if(active_.contains(key)||(!entry.isEmpty()&&active_.contains(entry))||(!startupUnlocking_&&active_.size()-entrySlots_.size()>=32)) {reply({"setup_busy","Another setup step is already running. Wait for it to finish.",{}},{});return;}
 // Whether put.gd ever saved to this keychain entry: only such a save marks it (#1890).
 const bool storedBefore=kind=="storeCredentials"&&!entry.isEmpty()&&status_&&status_->keychainSaved(reference["reference"].toString());
 const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
 jobs_.insert(id,{{"setupId",id},{"kind",kind},{"profileId",profileId},{"state","running"},{"error",Error{}.toJson()}});
 active_.insert(key);
 if(!entry.isEmpty()){active_.insert(entry);entrySlots_.insert(id,entry);}
 if(kind=="testDestination"&&status_)testStarts_.insert(id,{destinationcheck::fingerprint(profile),status_->credentialRevision(profileId)});
 cancellations_.insert(id,std::make_shared<std::atomic_bool>(false));
 // The keys change from here on, so an earlier check never vouches for the new ones, even
 // when the resident stops before this job ends.
 if(status_&&(kind=="storeCredentials"||kind=="connectAccount"||kind=="signOut"))status_->credentialsChanged(profileId);
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
 if((kind=="testDestination"&&providerSetup_)||QStringList{"connectAccount","inspectAccount","createFolder","storeSharePassword","signOut","trustHostKey"}.contains(kind)) {
  if(!providerSetup_){complete({"setup_unsupported","Account setup is unavailable",{}});return;}
  const Destination destination{profile["provider"].toString(),profileId,expectedRevision,profile["settings"].toObject(),reference};
  auto finish=[context=completion_,id](Error error,QJsonObject result){
   std::lock_guard lock(context->mutex);if(auto* owner=context->owner)QMetaObject::invokeMethod(owner,[owner,id,error,result]{owner->finish(id,error,result);},Qt::QueuedConnection);
  };
  auto event=[context=completion_,id](QJsonObject value){
   std::lock_guard lock(context->mutex);if(auto* owner=context->owner)QMetaObject::invokeMethod(owner,[owner,id,value]{if(owner->jobs_.contains(id)&&owner->jobs_[id]["state"]=="running"){auto job=owner->jobs_[id];job["event"]=value;owner->jobs_[id]=job;emit owner->changed(id);}},Qt::QueuedConnection);
  };
  providerSetup_(kind,destination,input,std::move(finish),std::move(event));
 }else if(kind=="storeEncryptPassphrase"||kind=="unlockEncryptPassphrase") {
  // #2079: kept like any other secret, in its own entry beside the destination's keys.
  const auto passphraseReference=profile["encrypt"].toObject()["passphraseCredentials"].toObject();
  if(passphraseReference.isEmpty()){complete({"encrypt_passphrase_unset","Choose On with a passphrase for Encrypted links on this destination first.",{}});return;}
  if(kind=="storeEncryptPassphrase"){Credentials value;value.values.insert("passphrase",input["passphrase"].toString());credentials_.save(encryptPassphraseId(profileId),passphraseReference,std::move(value),complete);}
  else credentials_.unlock(encryptPassphraseId(profileId),passphraseReference,complete);
 }else if(kind=="storeCredentials") {
  QStringList names;
  const bool named=namedCredentials_&&!namedCredentials_(profile,{},&names);
  if(named!=input.contains("values")) {complete({"credential_invalid",named?"This destination stores named values, not an access key":"This destination stores an access key and secret key",{}});return;}
  Credentials value;
  if(named) {
   QMap<QString,QString> typed;const auto values=input["values"].toObject();
   for(auto it=values.begin();it!=values.end();++it)typed.insert(it.key(),it.value().toString());
   if(auto error=namedCredentials_(profile,typed,&names)) {complete(error);return;}
   // Values typed now replace stored ones; the rest are kept from the loaded set.
   const bool sftp=profile["provider"]=="sftp";
   const auto merge=[names,typed,sftp](const std::optional<Credentials>& loaded,Credentials& merged){
    QStringList missing;
    for(const auto& name:std::as_const(names)) {
     if(typed.contains(name))merged.values.insert(name,typed.value(name));
     else if(loaded&&loaded->values.contains(name))merged.values.insert(name,loaded->values.value(name));
     // An SFTP destination's password and key passphrase are each optional (#1871).
     else if(sftp)merged.values.insert(name,QString());
     else missing.append(name);
    }
    return missing;
   };
   const auto cached=credentials_.cached(profileId,reference);
   QStringList untyped;for(const auto& name:std::as_const(names))if(!typed.contains(name))untyped.append(name);
   // Both SFTP secrets live in one keychain entry (#1890): one stored alone keeps the other, so a
   // saved entry that isn't loaded is read first, and one that can't be read is never overwritten.
   if(sftp&&reference["mode"]=="keychain"&&!cached&&!untyped.isEmpty()) {
    credentials_.unlock(profileId,reference,[guarded,profileId,reference,merge,untyped,complete,storedBefore,cancelled=cancellations_.value(id)](Error error){
     if(!guarded)return;
     // setup.cancel while the entry was being read: nothing is written.
     if(cancelled&&cancelled->load()){complete({"cancelled","Cancelled. Credentials weren't changed.",{}});return;}
     // No saved entry yet: nothing to keep. A keychain that still holds the entry locked (after a
     // dismissed prompt) can also answer "not found", so that counts only while put.gd never
     // stored credentials for this destination.
     if(error&&(!error.details["missing"].toBool()||storedBefore)) {
      complete({"credential_incomplete","Select Unlock saved credentials first, or enter both the password and the key passphrase so storing one doesn't erase the other. Credentials weren't changed.",{{"missing",QJsonArray::fromStringList(untyped)}}});
      return;
     }
     Credentials merged;merge(error?std::nullopt:guarded->credentials_.cached(profileId,reference),merged);
     guarded->credentials_.save(profileId,reference,std::move(merged),complete);
    });
    return;
   }
   if(const auto missing=merge(cached,value);!missing.isEmpty()) {complete({"credential_incomplete","Enter all required secrets for this destination, or select Unlock saved credentials first.",{{"missing",QJsonArray::fromStringList(missing)}}});return;}
   if(value.values.isEmpty()) {complete({"credential_invalid","This destination has no secret values to store",{}});return;}
  }else value={input["accessKey"].toString(),input["secretKey"].toString(),input["sessionToken"].toString()};
  // A provider that binds a key pair to where it was given keeps that binding beside it.
  if(!named&&credentialBinding_)if(const auto bound=credentialBinding_(profile);!bound.isEmpty())value.sessionToken=bound;
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
 startupUnlocking_=true;
 for(auto it=snapshot.profiles.cbegin();it!=snapshot.profiles.cend();++it){
  // #2079: a Encrypted links passphrase saved to the keychain is read once at startup too.
  const auto encrypt=it.value()["encrypt"].toObject();const auto encryptReference=encrypt["passphraseCredentials"].toObject();
  if(encrypt["mode"]=="passphrase"&&encryptReference["mode"]=="keychain"&&!credentials_.cached(encryptPassphraseId(it.key()),encryptReference))
   start("unlockEncryptPassphrase",it.key(),settings_.revision(),{},[&](Error error,QJsonObject result){if(!error)started.append(result["setupId"].toString());});
  const auto reference=it.value()["credentials"].toObject();
  if(!usesCredentialStore(it.value())||reference["mode"]!="keychain"||credentials_.cached(it.key(),reference))continue;
  start("unlockCredentials",it.key(),settings_.revision(),{},[&](Error error,QJsonObject result){if(!error)started.append(result["setupId"].toString());});
 }
 startupUnlocking_=false;
 for(const auto& id:std::as_const(started))if(jobs_.value(id)["state"]=="running")startupUnlocks_.insert(id);
 // An unlock someone already started for a profile holds the gate as well.
 for(auto job=jobs_.cbegin();job!=jobs_.cend();++job)
  if((*job)["kind"]=="unlockCredentials"&&(*job)["state"]=="running"&&snapshot.profiles.contains((*job)["profileId"].toString()))startupUnlocks_.insert(job.key());
 return started;
}
void SetupService::credentialHint(const QString& profileId,const QString& expectedRevision,JsonReply reply) {
 if(expectedRevision!=settings_.revision()){reply({"config_conflict","Settings changed elsewhere. Reload before continuing.",{}},{});return;}
 const auto profiles=settings_.snapshot().profiles;
 if(!profiles.contains(profileId)){reply({"profile_not_found","This destination is not saved yet",{}},{});return;}
 const auto profile=profiles[profileId];const auto reference=profile["credentials"].toObject();
 // A UI hint never unlocks or reads the OS secret store. Explicit credential
 // setup already owns that consent, and only this bounded hint leaves its cache.
 const auto cached=credentials_.cached(profileId,reference);
 QJsonObject hint{{"available",bool(cached)},{"mode",reference["mode"]}};
 // #2079: whether the Encrypted links passphrase is loaded, never the passphrase.
 if(const auto encryptReference=profile["encrypt"].toObject()["passphraseCredentials"].toObject();!encryptReference.isEmpty())hint["encryptPassphraseLoaded"]=bool(credentials_.cached(encryptPassphraseId(profileId),encryptReference));
 if(cached){hint["accessKeyHint"]=cached->accessKey.size()>4?cached->accessKey.left(4)+QString::fromUtf8("…"):QString::fromUtf8("••••");hint["secretStored"]=!cached->secretKey.isEmpty();hint["tokenStored"]=!cached->sessionToken.isEmpty();}
 // Named values: which of the names the settings need are loaded, never a value.
 QStringList names;
 if(namedCredentials_&&!namedCredentials_(profile,{},&names)) {
  QJsonArray stored,missing;
  // An SFTP destination stores its unused secret as empty: that one is not stored (#1871).
  for(const auto& name:std::as_const(names))(cached&&cached->values.contains(name)&&(profile["provider"]!="sftp"||!cached->values.value(name).isEmpty())?stored:missing).append(name);
  hint["storedValues"]=stored;hint["missingValues"]=missing;hint.remove("accessKeyHint");hint.remove("secretStored");hint.remove("tokenStored");
 }
 reply({},hint);
}
void SetupService::revealCredentialSecret(const QString& profileId,const QString& expectedRevision,JsonReply reply) {
 const auto metadata=settings_.metadata();
 if(expectedRevision!=settings_.revision()||expectedRevision!=metadata["observedRevision"].toString()){
  reply({"config_conflict","Settings changed elsewhere. Reload before continuing.",{}},{});return;
 }
 if(!metadata["validation"].toObject()["ok"].toBool()){
  reply({"config_invalid","Fix the settings error before viewing credentials.",{}},{});return;
 }
 const auto profiles=settings_.snapshot().profiles;
 if(!profiles.contains(profileId)){reply({"profile_not_found","Save the destination before showing credentials",{}},{});return;}
 const auto profile=profiles[profileId];
 if(profile["provider"]!="s3"&&profile["provider"]!="backblaze-b2"&&profile["provider"]!="cloudflare-r2"){reply({"setup_unsupported","This destination has no stored secret key",{}},{});return;}
 const auto cached=credentials_.cached(profileId,profile["credentials"].toObject());
 if(!cached){reply({"credential_setup_required","Select Unlock saved credentials first, or enter credentials and select Use for this session.",{}},{});return;}
 reply({},{{"secretKey",cached->secretKey}});
}
void SetupService::inspect(const QString& id,JsonReply reply) {
 const auto found=jobs_.constFind(id);
 if(found==jobs_.cend()) {reply({"setup_not_found","Setup job is no longer available in this resident",{}},{});return;}
 reply({},*found);
}
void SetupService::cancel(const QString& id,JsonReply reply) {
 const auto found=jobs_.constFind(id);
 if(found==jobs_.cend()){reply({"setup_not_found","Setup job is no longer available in this resident",{}},{});return;}
 const auto flag=cancellations_.value(id);
 if((*found)["state"]!="running"||!flag){reply({},{{"cancelRequested",false}});return;}
 flag->store(true);reply({},{{"cancelRequested",true}});
}
std::shared_ptr<std::atomic_bool> SetupService::cancellation(const QString& profileId,const QString& kind) const {
 for(auto job=jobs_.cbegin();job!=jobs_.cend();++job)
  if((*job)["profileId"]==profileId&&(*job)["kind"]==kind&&(*job)["state"]=="running"&&cancellations_.contains(job.key()))return cancellations_.value(job.key());
 return std::make_shared<std::atomic_bool>(false);
}
void SetupService::finish(const QString& id,Error error,QJsonObject result) {
 const auto found=jobs_.find(id);if(found==jobs_.end()||(*found)["state"]!="running")return;
 // #2079: a passed Test goes on to the encrypted-page check before it reports, unless cancelled.
 if((*found)["kind"]=="testDestination"&&!error&&encryptChecker_&&!encryptChecking_.contains(id)&&!(cancellations_.value(id)&&cancellations_.value(id)->load())) {
  const auto profileId=(*found)["profileId"].toString();const auto profiles=settings_.snapshot().profiles;
  // The check runs only on the connection this Test checked: one saved since it began is untested.
  const auto started=testStarts_.constFind(id);
  if(status_&&profiles.contains(profileId)&&started!=testStarts_.cend()&&(destinationcheck::fingerprint(profiles.value(profileId))!=started->fingerprint||status_->credentialRevision(profileId)!=started->credentialRevision))
   result["encrypt"]=QJsonObject{{"state","untested"},{"reason",QStringLiteral("The destination changed during the test. Test again.")}};
  else if(profiles.contains(profileId)) {
   encryptChecking_.insert(id);
   auto event=*found;event["event"]=QJsonObject{{"step","encrypt"},{"status","running"}};*found=event;emit changed(id);
   QPointer<SetupService> guarded(this);
   const auto flag=cancellations_.value(id);
   encryptChecker_(profileId,profiles.value(profileId),flag,[guarded,id,result,flag](QJsonObject encrypt) mutable {
    if(!guarded)return;
    // A Cancel the check saw, or one that came while it cleaned up, ends the Test cancelled; the
    // connection it already checked stays passed.
    const bool cancelled=encrypt.value("state")=="cancelled"||(flag&&flag->load());
    if(cancelled)encrypt["state"]=QStringLiteral("cancelled");
    if(!encrypt.isEmpty())result["encrypt"]=encrypt;
    guarded->finish(id,cancelled?Error{"cancelled","The connection test passed, but the encrypted-link check was cancelled. Test again to check encrypted links.",{{"connectionPassed",true}}}:Error{},result);
    guarded->encryptChecking_.remove(id);
   });
   return;
  }
 }
 cancellations_.remove(id);
 auto& record=*found;record.remove("event");record["result"]=result;record["state"]=error?"failed":"completed";record["error"]=error.toJson();
 active_.remove(activeKey(record["profileId"].toString(),record["kind"].toString()));
 if(const auto entry=entrySlots_.take(id);!entry.isEmpty()) {
  active_.remove(entry);
  if(!error&&record["kind"]=="storeCredentials"&&status_)status_->keychainWritten(entry.mid(QStringLiteral("keychain:").size()));
 }
 startupUnlocks_.remove(id);
 if(record["kind"]=="storeCredentials"||record["kind"]=="unlockCredentials"||record["kind"]=="storeEncryptPassphrase"||record["kind"]=="unlockEncryptPassphrase")++credentialGeneration_;
 // The record of the destination's last check changes before anyone hears the job ended.
 const auto started=testStarts_.take(id);
 if(status_) {
  const auto profileId=record["profileId"].toString();
  if(record["kind"]=="testDestination")status_->testFinished(profileId,started.fingerprint,started.credentialRevision,error.details.value("connectionPassed").toBool()?Error{}:error);
  // New keys, or a Proton sign-in, are another connection than the one checked before.
  else if(!error&&(record["kind"]=="storeCredentials"||record["kind"]=="connectAccount"||record["kind"]=="signOut"))status_->credentialsChanged(profileId);
 }
 completed_.enqueue(id);while(completed_.size()>64)jobs_.remove(completed_.dequeue());
 emit changed(id);
}
}
