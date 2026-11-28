#include "DestinationStatus.h"
#include "OperationService.h"
#include "core/DestinationCheck.h"
#include "core/Log.h"
#include "storage/dav/DavCommon.h"
#include "storage/s3/R2Settings.h"
#include "storage/sftp/SftpCommon.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>
namespace putgd {
bool DestinationStatus::storesKeys(const QJsonObject& profile) {
 const auto provider=profile.value("provider").toString();
 return provider=="s3"||provider=="backblaze-b2"||provider==r2::providerId||dav::isDav(provider);
}
namespace {
// The keychain entries configured destinations own, in any credentials mode: the reference each
// names, and putgd/<id> (what Settings writes when it switches to the keychain) and
// putgd/sftp/<id> for each one, so keychain → session → keychain keeps an entry's mark.
QSet<QString> ownedBy(const QString& id,const QJsonObject& profile) {
 QSet<QString> owned{"putgd/"+id};
 if(const auto reference=profile["credentials"].toObject().value("reference");reference.isString()&&!reference.toString().isEmpty())owned.insert(reference.toString());
 if(profile["provider"]=="sftp")owned.insert("putgd/sftp/"+id);
 return owned;
}
QSet<QString> ownedEntries(const QHash<QString,QJsonObject>& profiles) {
 QSet<QString> owned;
 for(auto it=profiles.cbegin();it!=profiles.cend();++it)owned.unite(ownedBy(it.key(),*it));
 return owned;
}
constexpr qint64 maximumFileBytes=1024*1024;
// A JSON number that is an exact integer from 0 to 2^53: a revision this file can hold.
bool revisionValue(const QJsonValue& value,quint64* out) {
 const auto number=value.toDouble(-1);
 if(!value.isDouble()||number<0||number>9007199254740992.0||number!=std::floor(number))return false;
 *out=quint64(number);return true;
}
bool validRecord(const QJsonObject& record,quint64* revision) {
 const auto state=record["state"].toString(),source=record["source"].toString(),code=record["errorCode"].toString();
 if(!((state=="passed"||state=="failed")&&(source=="test"||source=="upload")&&record["fingerprint"].toString().size()==64&&revisionValue(record["credentialRevision"],revision)))return false;
 // Only a refusal the upload rules recognize names why a destination failed.
 return !record.contains("errorCode")||destinationcheck::rejectsDestination(Error{code,{},{}});
}
}
DestinationStatus::DestinationStatus(QString path,SettingsService& settings,QObject* parent)
 :QObject(parent),path_(std::move(path)),settings_(settings) {}
void DestinationStatus::load() {
 records_.clear();encryptRecords_.clear();credentialRevisions_.clear();keychainEntries_.clear();
 QFile file(path_);
 // A file from before #1890 has no record of saved keychain entries: the entries of a destination
 // that stored credentials (a revision above 0 in that file) then count as saved, so a store of
 // one secret never overwrites one. Without revisions to go by (no file, or an unreadable one),
 // only the entries keychain-mode destinations name count. An unreadable file is left as it is:
 // the entries count as saved for this run only.
 const auto upgrade=[this](bool persist,bool revisions){
  const auto profiles=settings_.snapshot().profiles;
  for(auto it=profiles.cbegin();it!=profiles.cend();++it) {
   const auto credentials=(*it)["credentials"].toObject();
   if(revisions){if(credentialRevisions_.value(it.key())>0)keychainEntries_.unite(ownedBy(it.key(),*it));}
   else if(credentials["mode"]=="keychain"&&!credentials["reference"].toString().isEmpty())keychainEntries_.insert(credentials["reference"].toString());
  }
  if(persist)save();
 };
 if(!file.exists()){upgrade(true,false);return;}
 if(!file.open(QIODevice::ReadOnly)||file.size()>maximumFileBytes){logging::warning(logSettings(),"Destination check records unreadable; starting without them",{});upgrade(false,false);return;}
 QJsonParseError parse;const auto document=QJsonDocument::fromJson(file.readAll(),&parse);
 if(parse.error!=QJsonParseError::NoError||!document.isObject()||document.object()["schemaVersion"]!=1){logging::warning(logSettings(),"Destination check records invalid; starting without them",{});upgrade(false,false);return;}
 const auto root=document.object();
 const auto revisions=root["credentialRevisions"].toObject();
 for(auto it=revisions.begin();it!=revisions.end();++it)if(quint64 revision=0;revisionValue(it.value(),&revision))credentialRevisions_.insert(it.key(),revision);
 for(const auto& entry:root["keychainEntries"].toArray())if(entry.isString()&&!entry.toString().isEmpty()&&entry.toString().size()<=256)keychainEntries_.insert(entry.toString());
 const auto destinations=root["destinations"].toObject();
 for(auto it=destinations.begin();it!=destinations.end();++it) {
  const auto value=it.value().toObject();quint64 revision=0;if(!validRecord(value,&revision))continue;
  records_.insert(it.key(),{value["state"].toString(),value["source"].toString(),value["errorCode"].toString(),value["fingerprint"].toString(),revision});
 }
 const auto encrypts=root["encryptChecks"].toObject();
 for(auto it=encrypts.begin();it!=encrypts.end();++it){const auto value=it.value().toObject();
  // #2079: a check counts only for the keys it ran with; one from another revision is dropped.
  quint64 revision=0;if(!revisionValue(value["credentialRevision"],&revision)||revision!=credentialRevisions_.value(it.key()))continue;
  if(value["passed"].isBool()&&value["fingerprint"].isString()&&value["fingerprint"].toString().size()==64)encryptRecords_.insert(it.key(),{value["passed"].toBool(),value["reason"].toString().left(300),value["fingerprint"].toString(),revision});}
 // After the records are read, so the upgrade's save keeps them.
 if(!root.contains("keychainEntries"))upgrade(true,true);
}
void DestinationStatus::follow(OperationService& operations) {
 connect(&settings_,&SettingsService::accepted,this,[this]{prune();});
 connect(&operations,&OperationService::uploadStarting,this,&DestinationStatus::uploadStarting);
 connect(&operations,&OperationService::uploadSettled,this,&DestinationStatus::uploadSettled);
}
void DestinationStatus::save() {
 QJsonObject destinations,revisions;
 // Keys kept only for this session make a check that ends with the session.
 const auto profiles=settings_.snapshot().profiles;
 const auto sessionOnly=[&](const QString& id){const auto found=profiles.constFind(id);if(found==profiles.cend()||(*found)["credentials"].toObject()["mode"]!="session")return false;
  // #2118: SFTP signs in with the SSH agent or a key file unless a password or key passphrase was
  // stored for this session; only then are its checks the session's.
  if((*found)["provider"]==sftp::providerId)return credentialRevisions_.value(id)>0;
  return !usesCredentials_||usesCredentials_(*found);};
 for(auto it=records_.cbegin();it!=records_.cend();++it) {
  if(sessionOnly(it.key()))continue;
  QJsonObject record{{"state",it->state},{"source",it->source},{"fingerprint",it->fingerprint},{"credentialRevision",double(it->credentialRevision)}};
  if(!it->errorCode.isEmpty())record["errorCode"]=it->errorCode;
  destinations.insert(it.key(),record);
 }
 for(auto it=credentialRevisions_.cbegin();it!=credentialRevisions_.cend();++it)if(!sessionOnly(it.key()))revisions.insert(it.key(),double(it.value()));
 // Always written, even empty: a file without it is one from before #1890.
 auto entries=QStringList(keychainEntries_.cbegin(),keychainEntries_.cend());entries.sort();
 // A check made with session-only keys ends with the session, as the destination's own does.
 QJsonObject encrypts;for(auto it=encryptRecords_.cbegin();it!=encryptRecords_.cend();++it)if(!sessionOnly(it.key()))encrypts.insert(it.key(),QJsonObject{{"passed",it->passed},{"reason",it->reason},{"fingerprint",it->fingerprint},{"credentialRevision",double(it->credentialRevision)}});
 QJsonObject root{{"schemaVersion",1},{"destinations",destinations},{"credentialRevisions",revisions},{"keychainEntries",QJsonArray::fromStringList(entries)},{"encryptChecks",encrypts}};
 const auto bytes=QJsonDocument(root).toJson(QJsonDocument::Compact);
 QDir().mkpath(QFileInfo(path_).absolutePath());
 QSaveFile file(path_);file.setDirectWriteFallback(false);
 // The record stays in memory when the write fails; the next change writes it again.
 if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||file.write(bytes)!=bytes.size()||!file.commit())
  logging::warning(logSettings(),"Cannot save destination check records",{});
}
bool DestinationStatus::current(const QString& profileId,const QString& fingerprint,quint64 revision) const {
 const auto profiles=settings_.snapshot().profiles;
 const auto found=profiles.constFind(profileId);
 return found!=profiles.cend()&&revision==credentialRevision(profileId)&&destinationcheck::fingerprint(*found)==fingerprint;
}
void DestinationStatus::put(const QString& profileId,Record record) {
 const auto old=records_.constFind(profileId);
 if(old!=records_.cend()&&old->state==record.state&&old->source==record.source&&old->errorCode==record.errorCode&&old->fingerprint==record.fingerprint&&old->credentialRevision==record.credentialRevision)return;
 // An upload that passes keeps a pass as it was; a Test's result always replaces it.
 if(old!=records_.cend()&&record.source=="upload"&&record.state=="passed"&&old->state=="passed"&&old->fingerprint==record.fingerprint&&old->credentialRevision==record.credentialRevision)return;
 records_.insert(profileId,std::move(record));save();emit changed();
}
void DestinationStatus::credentialsChanged(const QString& profileId) {
 // Two destinations can name one keychain entry; session keys are each destination's own.
 const auto profiles=settings_.snapshot().profiles;
 const auto credentials=profiles.value(profileId)["credentials"].toObject();
 credentialRevisions_[profileId]+=1;
 QStringList affected{profileId};
 if(credentials["mode"]=="keychain")for(auto it=profiles.cbegin();it!=profiles.cend();++it)if(it.key()!=profileId&&(*it)["credentials"].toObject()==credentials){credentialRevisions_[it.key()]+=1;affected.append(it.key());}
 // #2079: new keys are another connection: a passed encrypted-link check no longer vouches for them.
 {std::lock_guard lock(encryptMutex_);for(const auto& id:std::as_const(affected))encryptRecords_.remove(id);}
 save();emit changed();
}
void DestinationStatus::keychainWritten(const QString& reference) {
 if(reference.isEmpty()||reference.size()>256||keychainEntries_.contains(reference))return;
 keychainEntries_.insert(reference);save();
}
void DestinationStatus::testFinished(const QString& profileId,const QString& fingerprint,quint64 revision,const Error& error) {
 // A test held back before it reached the provider, such as during journal recovery, proves nothing.
 if(error&&error.details.value("notAttempted").toBool())return;
 if(!current(profileId,fingerprint,revision))return;
 put(profileId,{error?QStringLiteral("failed"):QStringLiteral("passed"),QStringLiteral("test"),error.code,fingerprint,revision});
}
void DestinationStatus::uploadStarting(const Operation& op) {
 if(op.destination.profileId.isEmpty()||op.destination.providerId.isEmpty())return;
 Attempt attempt{credentialRevision(op.destination.profileId),{},{}};
 for(const auto& item:op.items){if(item.uploadStatus=="uploaded")attempt.uploaded.insert(item.artifact.id);if(item.error)attempt.errors.insert(item.artifact.id,item.error.code);}
 attempts_.insert(op.id,std::move(attempt));
}
void DestinationStatus::uploadSettled(const Operation& op) {
 const auto found=attempts_.constFind(op.id);if(found==attempts_.cend())return;
 const auto attempt=*found;attempts_.erase(found);
 bool uploaded=false;QString refusal;
 for(const auto& item:op.items) {
  if(item.uploadStatus=="uploaded"&&!attempt.uploaded.contains(item.artifact.id))uploaded=true;
  // A retry carries History's item errors; only one this run met, link refusals included, counts.
  if(refusal.isEmpty()&&destinationcheck::rejectsDestination(item.error)&&attempt.errors.value(item.artifact.id)!=item.error.code)refusal=item.error.code;
 }
 if(refusal.isEmpty()&&destinationcheck::rejectsDestination(op.error))refusal=op.error.code;
 // A file that reached the destination in this run shows the connection works.
 if(!uploaded&&refusal.isEmpty())return;
 const auto fingerprint=destinationcheck::fingerprint(op.destination.providerId,op.destination.settings,op.destination.credentialReference);
 if(!current(op.destination.profileId,fingerprint,attempt.credentialRevision))return;
 put(op.destination.profileId,{uploaded?QStringLiteral("passed"):QStringLiteral("failed"),QStringLiteral("upload"),uploaded?QString():refusal,fingerprint,attempt.credentialRevision});
}
void DestinationStatus::prune() {
 // An invalid configuration file is not a list of removed destinations.
 if(!settings_.metadata()["validation"].toObject()["ok"].toBool())return;
 const auto profiles=settings_.snapshot().profiles;
 bool removed=false;
 for(auto it=records_.begin();it!=records_.end();)if(!profiles.contains(it.key())){it=records_.erase(it);removed=true;}else ++it;
 {std::lock_guard lock(encryptMutex_);for(auto it=encryptRecords_.begin();it!=encryptRecords_.end();)if(!profiles.contains(it.key())){it=encryptRecords_.erase(it);removed=true;}else ++it;}
 for(auto it=credentialRevisions_.begin();it!=credentialRevisions_.end();)if(!profiles.contains(it.key())){it=credentialRevisions_.erase(it);removed=true;}else ++it;
 const auto owned=ownedEntries(profiles);
 for(auto it=keychainEntries_.begin();it!=keychainEntries_.end();)if(!owned.contains(*it)){it=keychainEntries_.erase(it);removed=true;}else ++it;
 if(removed){save();emit changed();}
}
QJsonObject DestinationStatus::publicJson() const {
 QJsonObject out;
 const auto profiles=settings_.snapshot().profiles;
 for(auto it=profiles.cbegin();it!=profiles.cend();++it) {
  const auto record=records_.constFind(it.key());
  if(record==records_.cend()||record->credentialRevision!=credentialRevision(it.key())||record->fingerprint!=destinationcheck::fingerprint(*it)){out.insert(it.key(),QJsonObject{{"state","untested"},{"encrypt",encryptCheck(it.key())}});continue;}
  QJsonObject value{{"state",record->state},{"source",record->source},{"fingerprint",record->fingerprint},{"encrypt",encryptCheck(it.key())}};
  if(!record->errorCode.isEmpty())value["errorCode"]=record->errorCode;
  out.insert(it.key(),value);
 }
 return out;
}
void DestinationStatus::encryptChecked(const QString& profileId,const QString& fingerprint,bool passed,const QString& reason) {
 const auto profiles=settings_.snapshot().profiles;const auto found=profiles.constFind(profileId);
 if(found==profiles.cend()||destinationcheck::fingerprint(*found)!=fingerprint)return;
 {std::lock_guard lock(encryptMutex_);encryptRecords_.insert(profileId,{passed,passed?QString():reason,fingerprint,credentialRevision(profileId)});}save();emit changed();
}
QJsonObject DestinationStatus::encryptCheckFor(const QString& profileId,const QString& fingerprint) const {
 std::lock_guard lock(encryptMutex_);const auto record=encryptRecords_.constFind(profileId);
 if(record==encryptRecords_.cend())return {{"state","untested"}};
 // #2118: a check of an earlier connection is said as such, so the call-out can say test again.
 if(record->fingerprint!=fingerprint)return {{"state","changed"}};
 QJsonObject out{{"state",record->passed?"passed":"failed"}};if(!record->reason.isEmpty())out["reason"]=record->reason;return out;
}
QJsonObject DestinationStatus::encryptCheck(const QString& profileId) const {
 const auto profiles=settings_.snapshot().profiles;const auto found=profiles.constFind(profileId);const auto record=encryptRecords_.constFind(profileId);
 if(found==profiles.cend()||record==encryptRecords_.cend())return {{"state","untested"}};
 if(record->fingerprint!=destinationcheck::fingerprint(*found))return {{"state","changed"}};
 // The connection it checked, so a window editing the destination can tell it no longer applies.
 QJsonObject out{{"state",record->passed?"passed":"failed"},{"fingerprint",record->fingerprint}};if(!record->reason.isEmpty())out["reason"]=record->reason;return out;
}
}
