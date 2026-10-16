#include "HistoryService.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <QPointer>
#include <QUuid>
namespace putgd {
HistoryService::HistoryService(OperationService& operations,QObject* parent):QObject(parent),operations_(operations) {
 connect(&operations_,&OperationService::sharingStopped,this,&HistoryService::recordStoppedSharing);
 notification_.setSingleShot(true);notification_.setInterval(25);connect(&notification_,&QTimer::timeout,this,&HistoryService::changed);
 retention_.setInterval(60*60*1000);connect(&retention_,&QTimer::timeout,this,&HistoryService::expireConfigured);connect(&operations_,&OperationService::initialized,this,&HistoryService::expireConfigured);retention_.start();if(operations_.ready())expireConfigured();
}
HistoryService::~HistoryService() {
 for(const auto& cancelled:active_)cancelled->store(true);
 operations_.shutdown();
}
void HistoryService::notifyChanged() {if(!notification_.isActive())notification_.start();}
void HistoryService::expireConfigured() {
 const int days=operations_.settingsSnapshot().historyRetentionDays;if(days<=0||!operations_.ready())return;
 const auto before=QDateTime::currentDateTimeUtc().addDays(-days).toString(Qt::ISODateWithMs),path=operations_.databasePath();operations_.background([before,path](Error& e) {HistoryStore store(path);e=store.expireBefore(before);return QJsonObject{};},[this](Error e,QJsonObject) {if(!e)notifyChanged();});
}
void HistoryService::list(const HistoryQuery& query,JsonReply reply) {if(!operations_.ready()){reply({"initializing","Wait for resident journal recovery",{}},{});return;}const auto path=operations_.databasePath();operations_.background([path,query](Error& e) {HistoryStore store(path);auto page=store.list(query);e=page.error;return page.toJson();},reply);}
void HistoryService::detail(const QString& id,JsonReply reply) {if(!operations_.ready()){reply({"initializing","Wait for resident journal recovery",{}},{});return;}const auto path=operations_.databasePath();operations_.background([this,path,id](Error& e) {HistoryStore store(path);auto data=store.detail(id,&e);if(!e){auto receipt=StorageReceipt::fromJson(data["receipt"].toObject());auto provider=operations_.providers().find(receipt.destination.providerId);if(provider)data["capabilities"]=provider->capabilities(receipt.destination).toJson();}return data;},reply);}
void HistoryService::clear(JsonReply reply) {if(!operations_.ready()){reply({"initializing","Wait for resident journal recovery",{}},{});return;}const auto path=operations_.databasePath();operations_.background([path](Error& e) {HistoryStore store(path);e=store.clear();return QJsonObject{{"cleared",!e},{"pendingDeletionTargetsRetained",true}};},[this,reply=std::move(reply)](Error e,QJsonObject result){if(!e)notifyChanged();reply(e,result);});}
void HistoryService::copyImage(const QString& id,JsonReply reply) {if(!operations_.ready()){reply({"initializing","Wait for resident journal recovery",{}},{});return;}operations_.copyHistoryImage(id,std::move(reply));}
void HistoryService::copyLink(const QString& id,JsonReply reply) {const auto ticket=operations_.invalidateCopyEligibility();detail(id,[this,reply,ticket](Error e,QJsonObject data) {if(e)reply(e,{});else if(!data["linkAvailable"].toBool())reply({"link_unavailable","This recorded link is no longer available",{}},{});else operations_.copyHistoryUrl(data["receipt"].toObject()["directUrl"].toString(),reply,ticket);});}
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
void HistoryService::prepareDelete(const QStringList& ids,JsonReply reply) {if(!operations_.ready()){reply({"initializing","Wait for resident journal recovery",{}},{});return;}operations_.background([this,ids](Error& e) {return prepare(ids,e);},reply);}
void HistoryService::deleteConfirmed(const QStringList& ids,const QString& acknowledgement,JsonReply reply) {
 operations_.background([this,ids,acknowledgement](Error& e) {auto preview=prepare(ids,e);if(e)return QJsonObject{};if(acknowledgement.isEmpty()||preview["acknowledgement"]!=acknowledgement) {e={"delete_confirmation_changed","Acknowledge the current exact target set and effect",{}};return QJsonObject{};}
  QJsonArray items;for(auto target:preview["targets"].toArray()) {auto item=target.toObject();item["status"]="pending";item["progress"]=QJsonObject{{"schemaVersion",1}};items.append(item);}
  QJsonObject doc{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"created",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"state","accepted"},{"acknowledgement",acknowledgement},{"items",items},{"cancelled",false}};
  HistoryStore store(operations_.databasePath());e=store.saveDeletion(doc);return doc;
 },[this,reply](Error e,QJsonObject doc) {if(e) {reply(e,{});return;}notifyChanged();const auto id=doc["id"].toString();auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(id,flag);reply({},{{"deleteOperationId",id},{"deletion",doc}});runDelete(doc,flag);});
}
void HistoryService::inspectSharing(const QString& id,JsonReply reply){if(stoppedSharing_.contains(id)){reply({},stoppedSharing_.value(id));return;}const auto path=operations_.databasePath();operations_.background([path,id](Error& e){HistoryStore store(path);return store.sharing(id,&e);},std::move(reply));}
void HistoryService::changeSharing(const QString& historyId,const QString& action,const QJsonObject& options,JsonReply reply){
 if(!operations_.ready()){reply({"initializing","Wait for resident journal recovery",{}},{});return;}
 if(!QStringList{"create","revoke","inspect"}.contains(action)){reply({"invalid_request","Invalid sharing action",{}},{});return;}
 for(auto i=options.begin();i!=options.end();++i)if(i.key()!="expirationDays"&&i.key()!="passwordReference"){reply({"invalid_request","Sharing options contain unsupported fields",{}},{});return;}
 if(options.contains("expirationDays")){const auto n=options["expirationDays"].toDouble(-1);if(n<0||n>365||n!=int(n)){reply({"invalid_request","Invalid link expiration",{}},{});return;}}
 if(options.contains("passwordReference")&&!options["passwordReference"].isNull()&&(!options["passwordReference"].isString()||options["passwordReference"].toString().isEmpty()||options["passwordReference"].toString().size()>256)){reply({"invalid_request","Use a keychain password reference",{}},{});return;}
 if(action!="inspect")operations_.invalidateCopyEligibility();
 const auto path=operations_.databasePath();
 QHash<QString,QJsonObject> stopped;for(auto it=stoppedSharing_.begin();it!=stoppedSharing_.end();++it)if(it.value()["historyId"]==historyId)stopped.insert(it.key(),it.value());
 operations_.background([this,path,historyId,action,options,stopped](Error& e){
  HistoryStore store(path);auto item=store.detail(historyId,&e);if(e)return QJsonObject{};
  auto receipt=StorageReceipt::fromJson(item["receipt"].toObject());auto provider=operations_.providers().find(receipt.destination.providerId);
  if(receipt.validate()||!provider||!provider->capabilities(receipt.destination).sharing){e={"sharing_unsupported","This provider cannot manage share links",{}};return QJsonObject{};}
  auto lock=operations_.objectLock(receipt.destination,receipt.objectReference,{});std::unique_lock guard(*lock);
  item=store.detail(historyId,&e);if(e)return QJsonObject{};
  const auto previousId=item["shareOperationId"].toString();
  if(stopped.contains(previousId)&&action=="inspect"){
   // A failed terminal write left a durable pending intent. Commit an unknown,
   // URL-cleared outcome before permitting a new read-only reconciliation.
   e=store.saveSharing(stopped.value(previousId));if(e)return QJsonObject{};
   item=store.detail(historyId,&e);if(e)return QJsonObject{};
  }
  if(item["uploadStatus"]!="uploaded"||QStringList{"pending","deleting","unknown","removed","already_missing","changed"}.contains(item["remoteStatus"].toString())){e={"share_unavailable","This item has no available confirmed upload",{}};return QJsonObject{};}
  if(item["shareStatus"]=="pending"||(action!="inspect"&&item["shareStatus"]=="unknown")){e={"share_result_unknown","Wait for or inspect the previous sharing result before another mutation",{}};return QJsonObject{};}
  receipt=StorageReceipt::fromJson(item["receipt"].toObject());if(action=="revoke")receipt.directUrl.clear();
  QJsonObject doc{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"historyId",historyId},{"action",action},{"options",options},{"state","accepted"},{"status","pending"},{"receipt",receipt.toJson()},{"progress",QJsonObject{}}};
  e=store.saveSharing(doc);return doc;
 },[this,reply,stopped](Error e,QJsonObject doc){
  if(e){reply(e,{});return;}for(auto it=stopped.begin();it!=stopped.end();++it)stoppedSharing_.remove(it.key());const auto id=doc["id"].toString();auto cancelled=std::make_shared<std::atomic_bool>(false);active_.insert(id,cancelled);notifyChanged();
  reply({},{{"shareOperationId",id},{"sharing",doc}});runSharing(doc,cancelled);
 });
}
void HistoryService::recordStoppedSharing(QJsonObject doc,const Error& error){
 doc["state"]="interrupted";doc["status"]="unknown";doc["error"]=error.toJson();
 auto receipt=doc["receipt"].toObject();receipt["directUrl"]="";doc["receipt"]=receipt;
 stoppedSharing_.insert(doc["id"].toString(),doc);notifyChanged();
}
void HistoryService::runSharing(QJsonObject doc,std::shared_ptr<std::atomic_bool> cancelled){
 const auto path=operations_.databasePath(),id=doc["id"].toString();
 operations_.background([this,path,doc,cancelled](Error& e) mutable {
  HistoryStore store(path);auto receipt=StorageReceipt::fromJson(doc["receipt"].toObject());auto provider=operations_.providers().find(receipt.destination.providerId);
  if(!provider){e={"provider_unavailable","Restore access to the original destination",{}};return doc;}
  auto lock=operations_.objectLock(receipt.destination,receipt.objectReference,{});std::unique_lock guard(*lock);
  doc["state"]="running";if((e=store.saveSharing(doc)))return doc;
  auto checkpoint=[path,&doc](const QJsonObject& progress,std::function<void(Error)> ack){auto proposed=doc;proposed["progress"]=progress;HistoryStore durable(path);const auto error=durable.saveSharing(proposed);if(!error)doc=std::move(proposed);ack(error);};
  ShareRequest request{doc["id"].toString(),doc["action"].toString(),receipt,doc["options"].toObject(),{},cancelled};
  auto result=awaitProvider<ShareResult>([&](auto cb){provider->share(request,checkpoint,cb);});
  if(!shareResultMatchesReceipt(result,receipt)){
   result.status="unknown";result.receipt=receipt;result.error={"share_identity_changed","Sharing did not confirm the recorded publication identity",{}};
  }
  if(result.status!="shared")result.receipt.directUrl.clear();
  doc["state"]="completed";doc["status"]=result.status;doc["receipt"]=result.receipt.toJson();doc["progress"]=result.progress;doc["error"]=result.error.toJson();e=store.saveSharing(doc);return doc;
 },[this,id](Error e,QJsonObject doc){
  if(e)recordStoppedSharing(doc,e);
  active_.remove(id);notifyChanged();
 });
}
void HistoryService::runDelete(QJsonObject doc,std::shared_ptr<std::atomic_bool> cancelled) {
 // Provider checkpoints can run on a different worker. Queue a coalesced
 // metadata invalidation only after the durable transaction commits.
 const auto committed=[owner=QPointer<HistoryService>(this)] {
  if(owner)QMetaObject::invokeMethod(owner,[owner]{if(owner)owner->notifyChanged();},Qt::QueuedConnection);
 };
 operations_.background([this,doc,cancelled,committed](Error& error) mutable {
  const auto path=operations_.databasePath();HistoryStore store(path);doc["state"]="running";if((error=store.saveDeletion(doc)))return doc;committed();
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
    const bool driverReconciles=provider->reconcilesDeleteProgress() && progress.contains("phase");
    auto stat=driverReconciles ? StatResult{"own",receipt,{}} : awaitProvider<StatResult>([&](auto cb) {provider->stat(receipt,cb);});
    if(stat.status=="missing")outcome="already_missing";
    else if(stat.status=="foreign")outcome="changed";
    else if(stat.status!="own"||stat.error) {outcome="unknown";e=stat.error?stat.error:Error{"delete_unknown","Recorded object identity could not be reconciled",{}};}
    else if(stat.receipt.publicationId!=receipt.publicationId||stat.receipt.objectReference!=receipt.objectReference||stat.receipt.concurrencyToken!=receipt.concurrencyToken||stat.receipt.destination.toJson()!=receipt.destination.toJson()) {outcome="changed";e={"delete_target_changed","The recorded target has changed",{}};}
    else {
     item["status"]="deleting";items[index]=item;doc["items"]=items;if((error=store.saveDeletion(doc)))return doc;committed();
     // Callback may run on the provider's worker. It creates a connection on that
     // thread, commits progress, then acknowledges; no Qt SQL connection crosses threads.
     auto checkpoint=[path,&doc,&items,index,committed](const QJsonObject& checkpoint,std::function<void(Error)> ack) {
      if(!checkpoint["schemaVersion"].isDouble()||checkpoint["schemaVersion"].toInt()<1) {ack({"delete_checkpoint_invalid","Driver progress requires a version",{}});return;}
      auto proposedItems=items;
      auto target=proposedItems[index].toObject();target["progress"]=checkpoint;proposedItems[index]=target;
      auto proposedDoc=doc;proposedDoc["items"]=proposedItems;
      HistoryStore durable(path);const auto e=durable.saveDeletion(proposedDoc);
      // Empty provider results may reuse this state, so only retain acknowledged progress.
      if(!e) {items=std::move(proposedItems);doc=std::move(proposedDoc);committed();}
      ack(e);
     };
     DeleteRequest request{doc["id"].toString(),receipt,progress,cancelled};
     auto result=awaitProvider<DeleteResult>([&](auto cb) {provider->remove(request,checkpoint,cb);});outcome=result.status=="missing"?"already_missing":result.status;progress=result.progress.isEmpty()?items[index].toObject()["progress"].toObject():result.progress;e=result.error;
     const QStringList allowed{"removed","already_missing","changed","failed","unknown","hidden_version_retained","version_retained","unsupported_safe_removal"};if(!allowed.contains(outcome)) {outcome="unknown";e={"delete_unknown","Provider returned an unrecognized deletion result",{}};}
    }
   }
   item=items[index].toObject();item["status"]=outcome;item["progress"]=progress;item["error"]=e.toJson();items[index]=item;doc["items"]=items;if((error=store.saveDeletion(doc)))return doc;committed();
  }
  doc["state"]=cancelled->load()?"cancelled":"completed";doc["items"]=items;error=store.saveDeletion(doc);if(!error)committed();return doc;
 },[this,id=doc["id"].toString()](Error,QJsonObject) {active_.remove(id);});
}
void HistoryService::inspectDelete(const QString& id,JsonReply reply) {const auto path=operations_.databasePath();operations_.background([path,id](Error& e) {HistoryStore store(path);return store.deletion(id,&e);},reply);}
void HistoryService::retryDelete(const QString& id,JsonReply reply) {
 if(active_.contains(id)) {reply({"delete_busy","Deletion is active",{}},{});return;}
 inspectDelete(id,[this,reply](Error e,QJsonObject doc) {if(e) {reply(e,{});return;}auto id=doc["id"].toString();if(active_.contains(id)) {reply({"delete_busy","Deletion is active",{}},{});return;}auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(id,flag);reply({},{{"deleteOperationId",id},{"retryAccepted",true}});runDelete(doc,flag);});
}
void HistoryService::cancelDelete(const QString& id,JsonReply reply) {if(active_.contains(id)) {active_[id]->store(true);reply({},{{"deleteOperationId",id},{"cancellationRequested",true},{"effect","Already sent deletion cannot be rolled back"}});return;}reply({"delete_not_active","Deletion is not active",{}},{});}
}
