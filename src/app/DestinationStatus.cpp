#include "DestinationStatus.h"
#include "OperationService.h"
#include "core/DestinationCheck.h"
#include "core/Log.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>
namespace putgd {
namespace {
constexpr qint64 maximumFileBytes=1024*1024;
bool validRecord(const QJsonObject& record) {
 const auto state=record["state"].toString(),source=record["source"].toString();
 return (state=="passed"||state=="failed")&&(source=="test"||source=="upload")&&record["fingerprint"].toString().size()==64&&record["credentialRevision"].isDouble()&&record["credentialRevision"].toDouble()>=0;
}
}
DestinationStatus::DestinationStatus(QString path,SettingsService& settings,QObject* parent)
 :QObject(parent),path_(std::move(path)),settings_(settings) {}
void DestinationStatus::load() {
 records_.clear();credentialRevisions_.clear();
 QFile file(path_);
 if(!file.exists())return;
 if(!file.open(QIODevice::ReadOnly)||file.size()>maximumFileBytes){logging::warning(logSettings(),"Destination check records unreadable; starting without them",{});return;}
 QJsonParseError parse;const auto document=QJsonDocument::fromJson(file.readAll(),&parse);
 if(parse.error!=QJsonParseError::NoError||!document.isObject()||document.object()["schemaVersion"]!=1){logging::warning(logSettings(),"Destination check records invalid; starting without them",{});return;}
 const auto root=document.object();
 const auto revisions=root["credentialRevisions"].toObject();
 for(auto it=revisions.begin();it!=revisions.end();++it)if(it.value().isDouble()&&it.value().toDouble()>=0)credentialRevisions_.insert(it.key(),quint64(it.value().toDouble()));
 const auto destinations=root["destinations"].toObject();
 for(auto it=destinations.begin();it!=destinations.end();++it) {
  const auto value=it.value().toObject();if(!validRecord(value))continue;
  records_.insert(it.key(),{value["state"].toString(),value["source"].toString(),value["errorCode"].toString(),value["fingerprint"].toString(),quint64(value["credentialRevision"].toDouble())});
 }
}
void DestinationStatus::follow(OperationService& operations) {
 connect(&settings_,&SettingsService::accepted,this,[this]{prune();});
 connect(&operations,&OperationService::uploadStarting,this,&DestinationStatus::uploadStarting);
 connect(&operations,&OperationService::uploadSettled,this,&DestinationStatus::uploadSettled);
}
void DestinationStatus::save() {
 QJsonObject destinations,revisions;
 for(auto it=records_.cbegin();it!=records_.cend();++it) {
  QJsonObject record{{"state",it->state},{"source",it->source},{"fingerprint",it->fingerprint},{"credentialRevision",double(it->credentialRevision)}};
  if(!it->errorCode.isEmpty())record["errorCode"]=it->errorCode;
  destinations.insert(it.key(),record);
 }
 for(auto it=credentialRevisions_.cbegin();it!=credentialRevisions_.cend();++it)revisions.insert(it.key(),double(it.value()));
 const auto bytes=QJsonDocument(QJsonObject{{"schemaVersion",1},{"destinations",destinations},{"credentialRevisions",revisions}}).toJson(QJsonDocument::Compact);
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
 if(old!=records_.cend()&&old->state==record.state&&old->errorCode==record.errorCode&&old->fingerprint==record.fingerprint&&old->credentialRevision==record.credentialRevision)return;
 // An upload that passes again keeps a test's pass as it was.
 if(old!=records_.cend()&&record.state=="passed"&&old->state=="passed"&&old->fingerprint==record.fingerprint&&old->credentialRevision==record.credentialRevision)return;
 records_.insert(profileId,std::move(record));save();emit changed();
}
void DestinationStatus::credentialsChanged(const QString& profileId) {
 credentialRevisions_[profileId]+=1;
 save();emit changed();
}
void DestinationStatus::testFinished(const QString& profileId,const QString& fingerprint,quint64 revision,const Error& error) {
 if(!current(profileId,fingerprint,revision))return;
 put(profileId,{error?QStringLiteral("failed"):QStringLiteral("passed"),QStringLiteral("test"),error.code,fingerprint,revision});
}
void DestinationStatus::uploadStarting(const Operation& op) {
 if(op.destination.profileId.isEmpty()||op.destination.providerId.isEmpty())return;
 Attempt attempt{credentialRevision(op.destination.profileId),{}};
 for(const auto& item:op.items)if(item.uploadStatus=="uploaded")attempt.uploaded.insert(item.artifact.id);
 attempts_.insert(op.id,std::move(attempt));
}
void DestinationStatus::uploadSettled(const Operation& op) {
 const auto found=attempts_.constFind(op.id);if(found==attempts_.cend())return;
 const auto attempt=*found;attempts_.erase(found);
 bool uploaded=false;QString refusal;
 for(const auto& item:op.items) {
  if(item.uploadStatus=="uploaded"&&!attempt.uploaded.contains(item.artifact.id))uploaded=true;
  if(refusal.isEmpty()&&destinationcheck::rejectsDestination(item.error))refusal=item.error.code;
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
 for(auto it=credentialRevisions_.begin();it!=credentialRevisions_.end();)if(!profiles.contains(it.key())){it=credentialRevisions_.erase(it);removed=true;}else ++it;
 if(removed){save();emit changed();}
}
QJsonObject DestinationStatus::publicJson() const {
 QJsonObject out;
 const auto profiles=settings_.snapshot().profiles;
 for(auto it=profiles.cbegin();it!=profiles.cend();++it) {
  const auto record=records_.constFind(it.key());
  if(record==records_.cend()||record->credentialRevision!=credentialRevision(it.key())||record->fingerprint!=destinationcheck::fingerprint(*it)){out.insert(it.key(),QJsonObject{{"state","untested"}});continue;}
  QJsonObject value{{"state",record->state},{"source",record->source},{"fingerprint",record->fingerprint}};
  if(!record->errorCode.isEmpty())value["errorCode"]=record->errorCode;
  out.insert(it.key(),value);
 }
 return out;
}
}
