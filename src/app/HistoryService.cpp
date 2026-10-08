#include "HistoryService.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
namespace putgd {
HistoryService::HistoryService(OperationService& operations,QObject* parent):QObject(parent),operations_(operations) {
 retention_.setInterval(60*60*1000);connect(&retention_,&QTimer::timeout,this,&HistoryService::expireConfigured);connect(&operations_,&OperationService::initialized,this,&HistoryService::expireConfigured);retention_.start();if(operations_.ready())expireConfigured();
}
HistoryService::~HistoryService() {
 for(const auto& cancelled:active_)cancelled->store(true);
 operations_.shutdown();
}
void HistoryService::expireConfigured() {
 const int days=operations_.settingsSnapshot().historyRetentionDays;if(days<=0||!operations_.ready())return;
 const auto before=QDateTime::currentDateTimeUtc().addDays(-days).toString(Qt::ISODateWithMs),path=operations_.databasePath();operations_.background([before,path](Error& e) {HistoryStore store(path);e=store.expireBefore(before);return QJsonObject{};},[](Error,QJsonObject) {});
}
void HistoryService::list(const HistoryQuery& query,JsonReply reply) {const auto path=operations_.databasePath();operations_.background([path,query](Error& e) {HistoryStore store(path);auto page=store.list(query);e=page.error;return page.toJson();},reply);}
void HistoryService::detail(const QString& id,JsonReply reply) {const auto path=operations_.databasePath();operations_.background([path,id](Error& e) {HistoryStore store(path);return store.detail(id,&e);},reply);}
void HistoryService::clear(JsonReply reply) {const auto path=operations_.databasePath();operations_.background([path](Error& e) {HistoryStore store(path);e=store.clear();return QJsonObject{{"cleared",!e},{"pendingDeletionTargetsRetained",true}};},reply);}
void HistoryService::copyLink(const QString& id,JsonReply reply) {const auto ticket=operations_.invalidateCopyEligibility();detail(id,[this,reply,ticket](Error e,QJsonObject data) {if(e)reply(e,{});else operations_.copyHistoryUrl(data["receipt"].toObject()["directUrl"].toString(),reply,ticket);});}
QJsonObject HistoryService::prepare(const QStringList& ids,Error& e) const {
 if(ids.isEmpty()||ids.size()>1000) {e={"invalid_selection","Select between 1 and 1000 confirmed uploads",{}};return {};}
 HistoryStore store(operations_.databasePath());QJsonArray targets;QSet<QString> seen;
 for(const auto& id:ids) {
  if(seen.contains(id)) {e={"invalid_selection","Duplicate history ID",{}};return {};}seen.insert(id);
  auto data=store.detail(id,&e);if(e)return {};
  if(data["uploadStatus"]!="uploaded") {e={"delete_target_invalid","Only confirmed uploaded entries can be deleted",{{"historyId",id}}};return {};}
  const auto remote=data["remoteStatus"].toString();if(remote=="pending"||remote=="deleting"||remote=="unknown"||remote=="changed"||remote=="hidden_version_retained") {e={"delete_target_changed","Resolve or retry the existing deletion identity",{{"historyId",id}}};return {};}
  auto receipt=StorageReceipt::fromJson(data["receipt"].toObject());if(auto error=receipt.validate()) {e=error;return {};}
  auto provider=operations_.providers().find(receipt.destination.providerId);if(!provider) {e={"provider_unavailable","Restore access to the original destination",{{"historyId",id}}};return {};}
  auto caps=provider->capabilities(receipt.destination);if(!caps.remove||!caps.stat||(!caps.conditionalDelete&&!caps.versionAddressing)||caps.deletionEffect.isEmpty()) {e={"unsupported_capability","Provider cannot safely delete this recorded object",{{"historyId",id}}};return {};}
  targets.append(QJsonObject{{"historyId",id},{"receipt",receipt.toJson()},{"effect",caps.deletionEffect}});
 }
 const auto digest=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(targets).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
 return {{"targets",targets},{"acknowledgement",digest},{"localCopiesPreserved",true},{"cacheNotice","Origin deletion does not invalidate browser or CDN caches"}};
}
void HistoryService::prepareDelete(const QStringList& ids,JsonReply reply) {operations_.background([this,ids](Error& e) {return prepare(ids,e);},reply);}
void HistoryService::deleteConfirmed(const QStringList& ids,const QString& acknowledgement,JsonReply reply) {
 operations_.background([this,ids,acknowledgement](Error& e) {auto preview=prepare(ids,e);if(e)return QJsonObject{};if(acknowledgement.isEmpty()||preview["acknowledgement"]!=acknowledgement) {e={"delete_confirmation_changed","Acknowledge the current exact target set and effect",{}};return QJsonObject{};}
  QJsonArray items;for(auto target:preview["targets"].toArray()) {auto item=target.toObject();item["status"]="pending";item["progress"]=QJsonObject{{"schemaVersion",1}};items.append(item);}
  QJsonObject doc{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"created",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"state","accepted"},{"acknowledgement",acknowledgement},{"items",items},{"cancelled",false}};
  HistoryStore store(operations_.databasePath());e=store.saveDeletion(doc);return doc;
 },[this,reply](Error e,QJsonObject doc) {if(e) {reply(e,{});return;}const auto id=doc["id"].toString();auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(id,flag);reply({},{{"deleteOperationId",id},{"deletion",doc}});runDelete(doc,flag);});
}
void HistoryService::runDelete(QJsonObject doc,std::shared_ptr<std::atomic_bool> cancelled) {
 operations_.background([this,doc,cancelled](Error& error) mutable {
  const auto path=operations_.databasePath();HistoryStore store(path);doc["state"]="running";if((error=store.saveDeletion(doc)))return doc;
  auto items=doc["items"].toArray();
  for(int index=0;index<items.size();++index) {
   auto item=items[index].toObject();auto status=item["status"].toString();if(status=="removed"||status=="already_missing"||status=="changed")continue;
   if(cancelled->load()) {doc["cancelled"]=true;break;}
   const auto receipt=StorageReceipt::fromJson(item["receipt"].toObject());auto provider=operations_.providers().find(receipt.destination.providerId);
   Error e;QString outcome;QJsonObject progress=item["progress"].toObject();
   if(!provider) {e={"provider_unavailable","Restore access to the recorded destination",{}};outcome="failed";}
   else if(!provider->capabilities(receipt.destination).remove||!provider->capabilities(receipt.destination).stat||(!provider->capabilities(receipt.destination).conditionalDelete&&!provider->capabilities(receipt.destination).versionAddressing)) {e={"unsupported_capability","Provider cannot safely delete this recorded target",{}};outcome="failed";}
   else {
    auto lock=operations_.objectLock(receipt.destination,receipt.objectReference,{});std::unique_lock guard(*lock);
    auto stat=awaitProvider<StatResult>([&](auto cb) {provider->stat(receipt,cb);});
    if(stat.status=="missing")outcome="already_missing";
    else if(stat.status=="foreign")outcome="changed";
    else if(stat.status!="own"||stat.error) {outcome="unknown";e=stat.error?stat.error:Error{"delete_unknown","Recorded object identity could not be reconciled",{}};}
    else if(stat.receipt.publicationId!=receipt.publicationId||stat.receipt.objectReference!=receipt.objectReference||stat.receipt.concurrencyToken!=receipt.concurrencyToken) {outcome="changed";e={"delete_target_changed","The recorded target has changed",{}};}
    else {
     item["status"]="deleting";items[index]=item;doc["items"]=items;if((error=store.saveDeletion(doc)))return doc;
     // Callback may run on the provider's worker. It creates a connection on that
     // thread, commits progress, then acknowledges; no Qt SQL connection crosses threads.
     auto checkpoint=[path,&doc,&items,index](const QJsonObject& checkpoint,std::function<void(Error)> ack) {
      if(!checkpoint["schemaVersion"].isDouble()||checkpoint["schemaVersion"].toInt()<1) {ack({"delete_checkpoint_invalid","Driver progress requires a version",{}});return;}
      auto target=items[index].toObject();target["progress"]=checkpoint;items[index]=target;doc["items"]=items;HistoryStore durable(path);ack(durable.saveDeletion(doc));
     };
     DeleteRequest request{doc["id"].toString(),receipt,progress};
     auto result=awaitProvider<DeleteResult>([&](auto cb) {provider->remove(request,checkpoint,cb);});outcome=result.status=="missing"?"already_missing":result.status;progress=result.progress.isEmpty()?items[index].toObject()["progress"].toObject():result.progress;e=result.error;
     const QStringList allowed{"removed","already_missing","changed","failed","unknown","hidden_version_retained","version_retained","unsupported_safe_removal"};if(!allowed.contains(outcome)) {outcome="unknown";e={"delete_unknown","Provider returned an unrecognized deletion result",{}};}
    }
   }
   item=items[index].toObject();item["status"]=outcome;item["progress"]=progress;item["error"]=e.toJson();items[index]=item;doc["items"]=items;if((error=store.saveDeletion(doc)))return doc;
  }
  doc["state"]=cancelled->load()?"cancelled":"completed";doc["items"]=items;error=store.saveDeletion(doc);return doc;
 },[this,id=doc["id"].toString()](Error,QJsonObject) {active_.remove(id);});
}
void HistoryService::inspectDelete(const QString& id,JsonReply reply) {const auto path=operations_.databasePath();operations_.background([path,id](Error& e) {HistoryStore store(path);return store.deletion(id,&e);},reply);}
void HistoryService::retryDelete(const QString& id,JsonReply reply) {
 if(active_.contains(id)) {reply({"delete_busy","Deletion is active",{}},{});return;}
 inspectDelete(id,[this,reply](Error e,QJsonObject doc) {if(e) {reply(e,{});return;}auto id=doc["id"].toString();if(active_.contains(id)) {reply({"delete_busy","Deletion is active",{}},{});return;}auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(id,flag);reply({},{{"deleteOperationId",id},{"retryAccepted",true}});runDelete(doc,flag);});
}
void HistoryService::cancelDelete(const QString& id,JsonReply reply) {if(active_.contains(id)) {active_[id]->store(true);reply({},{{"deleteOperationId",id},{"cancellationRequested",true},{"effect","Already sent deletion cannot be rolled back"}});return;}reply({"delete_not_active","Deletion is not active",{}},{});}
}
