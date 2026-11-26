#include "HistoryService.h"
#include "core/Log.h"
#include "core/QrImage.h"
#include <QFileInfo>
#include <chrono>
#include <QSaveFile>
#include <QTemporaryFile>
#include <unistd.h>
#include <cerrno>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QUrl>
#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <QPointer>
#include <QUuid>
namespace putgd {
namespace {
// What a sharing step records only for its provider (#1872) stays out of what clients see.
QJsonObject publicProgress(QJsonObject progress){progress.remove("shareId");progress.remove("tokenDigest");return progress;}
QJsonObject publicSharing(QJsonObject doc){if(doc.contains("progress"))doc["progress"]=publicProgress(doc["progress"].toObject());return doc;}
}
QJsonObject HistoryService::recordedShare(const StorageReceipt& receipt,const QJsonObject& previous,const QString& action){
 QJsonObject recorded;
 // Only Nextcloud reads these; every other provider's request progress stays as it was.
 if(receipt.destination.providerId!=QLatin1String("nextcloud"))return recorded;
 if(const auto shareId=previous.value("shareId");shareId.isString())recorded["shareId"]=shareId;
 static const QRegularExpression token(QStringLiteral("/s/([^/?#]+)/?$"));
 if(action=="revoke")if(const auto match=token.match(QUrl(receipt.directUrl).path());match.hasMatch())
  recorded["tokenDigest"]=QString::fromLatin1(QCryptographicHash::hash(match.captured(1).toUtf8(),QCryptographicHash::Sha256).toHex());
 return recorded;
}
HistoryService::HistoryService(OperationService& operations,QObject* parent):QObject(parent),operations_(operations) {
 // Workers read the destinations through currentProfiles(); the accepted settings change here (#1759).
 const auto keepProfiles=[this]{std::lock_guard lock(profilesMutex_);profiles_=operations_.settingsSnapshot().document["profiles"].toArray();};
 keepProfiles();connect(&operations_,&OperationService::settingsAccepted,this,keepProfiles);
 connect(&operations_,&OperationService::sharingStopped,this,&HistoryService::recordStoppedSharing);
 notification_.setSingleShot(true);notification_.setInterval(25);connect(&notification_,&QTimer::timeout,this,&HistoryService::changed);
 retention_.setInterval(60*60*1000);connect(&retention_,&QTimer::timeout,this,&HistoryService::expireConfigured);connect(&operations_,&OperationService::initialized,this,&HistoryService::expireConfigured);retention_.start();if(operations_.ready())expireConfigured();
}
HistoryService::~HistoryService() {
 for(const auto& cancelled:std::as_const(active_))cancelled->store(true);
 operations_.shutdown();
}
void HistoryService::notifyChanged() {if(!notification_.isActive())notification_.start();}
void HistoryService::expireConfigured() {
 const int days=operations_.settingsSnapshot().historyRetentionDays;if(days<=0||!operations_.ready())return;
 const auto before=QDateTime::currentDateTimeUtc().addDays(-days).toString(Qt::ISODateWithMs),path=operations_.databasePath();operations_.background([before,path](Error& e) {HistoryStore store(path);e=store.expireBefore(before);return QJsonObject{};},[this](Error e,QJsonObject) {if(!e)notifyChanged();});
}
void HistoryService::list(const HistoryQuery& query,JsonReply reply) {if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}const auto path=operations_.databasePath();const auto profiles=operations_.settingsSnapshot().document["profiles"].toArray();operations_.background([path,query,profiles](Error& e) {HistoryStore store(path);auto page=store.list(query);e=page.error;auto result=page.toJson();auto records=result["records"].toArray();for(qsizetype i=0;i<records.size();++i){auto record=records[i].toObject();if(record.contains("shareProgress"))record["shareProgress"]=publicProgress(record["shareProgress"].toObject());markRemovedDestinations(record,profiles);records[i]=record;}result["records"]=records;return result;},std::move(reply));}
void HistoryService::counts(const HistoryQuery& query,const QList<std::pair<QString,QString>>& days,JsonReply reply) {if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}const auto path=operations_.databasePath();operations_.background([path,query,days](Error& e) {HistoryStore store(path);return store.counts(query,days,&e);},std::move(reply));}
void HistoryService::detail(const QString& id,JsonReply reply) {if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}const auto path=operations_.databasePath();const auto profiles=operations_.settingsSnapshot().document["profiles"].toArray();operations_.background([this,path,id,profiles](Error& e) {HistoryStore store(path);auto data=store.detail(id,&e);if(!e){data["shareProgress"]=publicProgress(data["shareProgress"].toObject());markRemovedDestinations(data,profiles);auto receipt=StorageReceipt::fromJson(data["receipt"].toObject());auto provider=operations_.providers().find(receipt.destination.providerId);if(provider)data["capabilities"]=provider->receiptCapabilities(receipt).toJson();
  // No receipt yet (a failed upload, or one whose outcome is unknown): the operation's destination says what its provider can do.
  else{Error ignored;const auto operation=store.operation(data["operationId"].toString(),&ignored);if(!ignored)if(auto planned=operations_.providers().find(operation.destination.providerId))data["capabilities"]=planned->capabilities(operation.destination).toJson();}}return data;},reply);}
void HistoryService::preview(const QString& id,JsonReply reply,bool retry) {operations_.makePreview(id,std::move(reply),retry);}
void HistoryService::clear(JsonReply reply) {if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}const auto path=operations_.databasePath();operations_.background([path](Error& e) {HistoryStore store(path);e=store.clear();return QJsonObject{{"cleared",!e},{"pendingDeletionTargetsRetained",true}};},[this,reply=std::move(reply)](Error e,QJsonObject result){if(!e)notifyChanged();reply(e,result);});}
// Remove from History only (#1759): the local records go; the files stay where they were uploaded.
void HistoryService::forget(const QStringList& ids,JsonReply reply) {
 if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}
 const auto path=operations_.databasePath();const auto running=QSet<QString>(active_.keyBegin(),active_.keyEnd());
 operations_.background([path,ids,running](Error& e){HistoryStore store(path);
  int removed=0;QStringList taken;e=store.forget(ids,running,&removed,&taken);return QJsonObject{{"removed",removed},{"removedIds",QJsonArray::fromStringList(taken)}};},[this,reply=std::move(reply)](Error e,QJsonObject result){
  if(!e){notifyChanged();QStringList taken;for(const auto& id:result.take("removedIds").toArray())taken<<id.toString();if(!taken.isEmpty())emit recordsForgotten(taken);}
  reply(e,result);});
}
void HistoryService::copyImage(const QString& id,JsonReply reply) {if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}operations_.copyHistoryImage(id,std::move(reply));}
QJsonObject HistoryService::copyableLink(const QString& id,Error& e) {

  HistoryStore store(operations_.databasePath());auto data=store.detail(id,&e);if(e)return QJsonObject{};
  if(data["linkRefreshable"].toBool()) {
   auto receipt=StorageReceipt::fromJson(data["receipt"].toObject());auto provider=operations_.providers().find(receipt.destination.providerId);
   if(!provider){e={"provider_unavailable","Destination is unavailable. Restore access to it in Destinations.",{}};return QJsonObject{};}
   auto lock=operations_.objectLock(receipt.destination,receipt.objectReference,{});std::unique_lock guard(*lock);
   data=store.detail(id,&e);if(e)return QJsonObject{};
   if(data["linkRefreshable"].toBool()) {
    receipt=StorageReceipt::fromJson(data["receipt"].toObject());
    // Quit kills a renewal helper waiting on a stalled server; a link not renewed stays as recorded (#1602).
    auto result=awaitProvider<StatResult>([&](auto cb){provider->refreshLinkForCopy(receipt,quitting_,cb);});
    if(result.error||result.status!="own"){e=result.error?result.error:Error{"link_refresh_failed","Couldn't confirm the original uploaded file, so its link wasn't refreshed.",{}};return QJsonObject{};}
    if((e=store.refreshLink(id,receipt,result.receipt)))return QJsonObject{};
    data=store.detail(id,&e);
   }
  }
  return data;

}
void HistoryService::unlockLinkCredentials(const QString& id,JsonReply reply) {
 detail(id,[this,reply](Error e,QJsonObject data){
  if(e){reply(e,{});return;}
  const auto receipt=StorageReceipt::fromJson(data["receipt"].toObject());
  auto provider=operations_.providers().find(receipt.destination.providerId);
  if(!data["linkRefreshable"].toBool()||!provider||receipt.destination.credentialReference["mode"]!="keychain"){reply({"credential_setup_required","Credentials for this destination are unavailable. Restore them in Destinations.",{}},{});return;}
  provider->unlockLinkCredentials(receipt,[reply](Error e){reply(e,{{"unlocked",!e}});});
 });
}
// The saved keychain credentials of the destination a stored item went to, as its
// receipt names them: what a deletion or link refresh of that item needs (#1753).
void HistoryService::unlockItemCredentials(const QString& id,JsonReply reply) {
 detail(id,[this,reply](Error e,QJsonObject data){
  if(e){reply(e,{});return;}
  const auto receipt=StorageReceipt::fromJson(data["receipt"].toObject());
  auto provider=operations_.providers().find(receipt.destination.providerId);
  if(receipt.validate()||!provider){reply({"provider_unavailable","Destination is unavailable. Restore access to it in Destinations.",{}},{});return;}
  if(receipt.destination.credentialReference["mode"]!="keychain"){reply({"credential_setup_required","This file's destination used credentials for one session only. Enter them again in Destinations.",{{"profileId",receipt.destination.profileId}}},{});return;}
  provider->unlockLinkCredentials(receipt,[reply,profileId=receipt.destination.profileId](Error e){reply(e,{{"unlocked",!e},{"profileId",profileId}});});
 });
}
void HistoryService::copyLink(const QString& id,JsonReply reply) {
 if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}
 const auto ticket=operations_.invalidateCopyEligibility();
 operations_.background([this,id](Error& e) {return copyableLink(id,e); },[this,id,reply,ticket](Error e,QJsonObject data) {
  if(e){reply(e,{});return;}
  if(!data["linkAvailable"].toBool()){reply({"link_unavailable","This link is no longer available.",{}},{});return;}
  notifyChanged();
  operations_.copyHistoryUrl(data["link"].toString(),[this,id,reply](Error e,QJsonObject result) {
   if(e||!result["copied"].toBool()){reply(e,result);return;}
   const auto path=operations_.databasePath();
   operations_.background([path,id](Error& e){HistoryStore store(path);QString operationId;e=store.recordCopiedLink(id,operationId);return QJsonObject{{"operationId",operationId}};},
    [this,reply,result](Error e,QJsonObject recorded){
     if(!e&&!recorded["operationId"].toString().isEmpty())emit operations_.changed(recorded["operationId"].toString());
     reply(e,result);
    });
  },ticket);
 });
}
void HistoryService::forgetSealKey(const QString& id,JsonReply reply) {
 if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}
 const auto path=operations_.databasePath();
 operations_.background([path,id](Error& e){HistoryStore store(path);const auto data=store.detail(id,&e);return QJsonObject{{"operationId",data["operationId"]}};},[this,path,id,reply](Error e,QJsonObject found){
  if(e){reply(e,{});return;}
  const auto operationId=found["operationId"].toString();
  if(!operations_.holdOperation(operationId)){reply({"history_busy","This upload is running, or History is already changing it. Wait a moment, then select Forget key again.",{}},{});return;}
  operations_.background([path,id](Error& e){HistoryStore store(path);e=store.forgetSealKey(id);return QJsonObject{{"historyId",id},{"keyForgotten",!e}};},[this,id,operationId,reply](Error e,QJsonObject result){
   operations_.releaseOperation(operationId);
   if(!e){notifyChanged();logging::info(logHistory(),"Sealed link key forgotten",{{"historyId",id}});}
   reply(e,result);
  });
 });
}
void HistoryService::copyLinks(const QStringList& ids,JsonReply reply) {
 if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}
 const auto ticket=operations_.invalidateCopyEligibility();
 operations_.background([this,ids](Error& e) {
  QStringList urls;
  for(const auto& id:ids){const auto data=copyableLink(id,e);if(e)return QJsonObject{};if(!data["linkAvailable"].toBool()){e={"link_unavailable","This link is no longer available.",{{"historyId",id}}};return QJsonObject{};}urls.append(data["link"].toString());}
  return QJsonObject{{"text",urls.join('\n')}};
 },[this,ids,reply,ticket](Error e,QJsonObject links) {
  if(e){reply(e,{});return;}
  operations_.copyHistoryUrl(links["text"].toString(),[this,ids,reply](Error e,QJsonObject result) {
   if(e||!result["copied"].toBool()){reply(e,result);return;}
   result["links"]=int(ids.size());const auto path=operations_.databasePath();
   operations_.background([path,ids](Error& e){HistoryStore store(path);QJsonArray changed;for(const auto& id:ids){QString operationId;if((e=store.recordCopiedLink(id,operationId)))break;if(!operationId.isEmpty())changed.append(operationId);}return QJsonObject{{"operations",changed}};},
    [this,reply,result](Error e,QJsonObject recorded){for(const auto& id:recorded["operations"].toArray())emit operations_.changed(id.toString());reply(e,result);});
  },ticket);
 });
}
void HistoryService::qr(const QString& id,const QString& mode,const QString& path,bool overwrite,JsonReply reply,QrShow show) {
 if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}
 if(mode=="save"&&!path.endsWith(".png",Qt::CaseInsensitive)){reply({"invalid_request","Save a QR code to an absolute .png path",{}},{});return;}
 if(mode=="show"&&!show){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
 // Copy image is an explicit copy from the moment it is asked for, as Copy link is.
 const auto ticket=mode=="copy"?operations_.invalidateCopyEligibility():0;
 const auto revision=operations_.clipboardRevision();
 operations_.background([this,id,mode,path,overwrite](Error& e) {
  const auto data=copyableLink(id,e);if(e)return QJsonObject{};
  if(!data["linkAvailable"].toBool()){e=qr::unavailable(data);return QJsonObject{};}
  const auto url=data["link"].toString();
  qr::Matrix matrix;if((e=qr::encode(url.toUtf8(),matrix)))return QJsonObject{};
  QJsonObject result{{"historyId",id},{"modules",matrix.size}};
  if(mode=="show"){result["url"]=url;return result;}
  const auto png=qr::png(matrix);if(png.isEmpty()){e={"save_failed","The QR code could not be encoded",{}};return QJsonObject{};}const auto scale=qr::exportScale(matrix);
  result["width"]=matrix.edge()*scale;result["height"]=matrix.edge()*scale;result["bytes"]=double(png.size());
  result["sha256"]=QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex());
  if(mode=="copy"){result["png"]=QString::fromLatin1(png.toBase64());return result;}
  // Save never replaces a file without overwrite, nor anything but a regular file.
  const QFileInfo target(path);
  if((target.exists()||target.isSymLink())&&(!overwrite||!target.isFile())){e={"file_exists","The file exists; pass overwrite to replace it",{{"path",path}}};return QJsonObject{};}
  bool saved=false;
  if(overwrite){
   QSaveFile file(path);file.setDirectWriteFallback(false);
   saved=file.open(QIODevice::WriteOnly)&&file.write(png)==png.size()&&file.commit();
  }else{
   // link publishes a complete private file only if the destination is absent.
   QTemporaryFile file(target.absolutePath()+"/.putgd-qr-XXXXXX");
   if(file.open()&&file.write(png)==png.size()&&file.flush()){
    saved=::link(QFile::encodeName(file.fileName()).constData(),QFile::encodeName(path).constData())==0;
    if(!saved&&errno==EEXIST){e={"file_exists","The file exists; pass overwrite to replace it",{{"path",path}}};return QJsonObject{};}
   }
  }
  if(!saved){e={"save_failed","The QR code could not be saved",{{"path",path}}};return QJsonObject{};}
  result["saved"]=true;result["path"]=path;return result;
 },[this,mode,reply,ticket,revision,show](Error e,QJsonObject result) {
  if(e){reply(e,{});return;}
  notifyChanged();
  if(mode=="show"){show(result,reply);return;}
  if(mode=="save"){reply({},result);return;}
  const auto png=QByteArray::fromBase64(result.take("png").toString().toLatin1());
  operations_.copyHistoryQr(png,[reply,result](Error e,QJsonObject copied){auto answer=result;answer["copied"]=copied["copied"].toBool();answer["copyStatus"]=e.code=="copy_suppressed"?"suppressed":e?"failed":"copied";reply(e.code=="copy_suppressed"?Error{}:e,answer);},ticket,revision);
 });
}
namespace {
// A destination_readded refusal (#1760) after the read-only check through the destination added back
// (#1772): `found` is what that check answered, own or unknown, or empty when it couldn't be asked. A
// deletion that ended unsupported_safe_removal or version_retained can also be removed from History only
// after a warning (#1763); any other may still be reaching the file.
QString readdedMessage(const QString& status,const QString& found,bool credentials,const Error& checkError) {
 const bool wayOut=status=="unsupported_safe_removal"||status=="version_retained";
 const QString why=credentials?QStringLiteral("This deletion started with different credentials, so it won't continue with the current ones. "):QStringLiteral("This deletion started before its destination was removed, so it won't continue through the re-added destination. ");
 const QString restore=credentials?QStringLiteral(" Restore the original credentials in Destinations to finish it."):QString();
 const QString forget=wayOut?QStringLiteral(" You can also select Remove from History only."):QString();
 if(found=="own")return why+QStringLiteral("The file is still at the destination. Open Advanced file details for its exact name and version, delete it there yourself, then select Retry deletion to confirm it is gone.")+restore+forget;
 if(found=="unknown"){auto reason=checkError.message.trimmed();while(reason.endsWith('.'))reason.chop(1);
  return why+QStringLiteral("Couldn't check whether the file is still there")+(reason.isEmpty()?QString():QStringLiteral(" (")+reason+QStringLiteral(")"))+QStringLiteral(". Select Retry deletion to check again.")+restore+forget;}
 return why+QStringLiteral("Check the file at the destination")+(wayOut?QStringLiteral(", or select Remove from History only."):QStringLiteral(". Remove from History only isn't available while the file may still be there."))+restore;
}
Error destinationRemovedError(const QString& historyId) {
 return {"destination_removed","This file's destination was removed, so put.gd can't delete it there. The file stays at the destination. Add a destination for the same place to delete it, or select Remove from History only.",{{"historyId",historyId}}};
}
// An uploaded file whose recorded destination is gone from Settings, with nothing else at the same place (#1759).
bool destinationGone(const QJsonObject& record,const QJsonArray& profiles) {
 if(record["uploadStatus"]!="uploaded"||QStringList{"removed","already_missing"}.contains(record["remoteStatus"].toString()))return false;
 const auto destination=record["receipt"].toObject()["destination"].toObject();
 return !destination["providerId"].toString().isEmpty()&&Settings::profileForDestination(destination,profiles).isEmpty();
}
}
void HistoryService::markRemovedDestinations(QJsonObject& data,const QJsonArray& profiles) {
 if(destinationGone(data,profiles))data["destinationRemoved"]=true;
 if(data.contains("records")){auto records=data["records"].toArray();for(qsizetype i=0;i<records.size();++i){auto record=records[i].toObject();if(record.contains("shareProgress"))record["shareProgress"]=publicProgress(record["shareProgress"].toObject());markRemovedDestinations(record,profiles);records[i]=record;}data["records"]=records;}
}
QJsonObject HistoryService::prepare(const QStringList& ids,const QJsonArray& profiles,const QSet<QString>& running,Error& e) const {
 if(ids.isEmpty()||ids.size()>1000) {e={"invalid_selection","Select between 1 and 1000 confirmed uploads",{}};return {};}
 HistoryStore store(operations_.databasePath());QJsonArray targets;QSet<QString> seen;
 for(const auto& id:ids) {
  if(seen.contains(id)) {e={"invalid_selection","Duplicate history ID",{}};return {};}seen.insert(id);
  auto data=store.detail(id,&e);if(e)return {};
  if(data["uploadStatus"]!="uploaded") {e={"delete_target_invalid","Only completed uploads can be deleted from their destination.",{{"historyId",id}}};return {};}
  auto receipt=StorageReceipt::fromJson(data["receipt"].toObject());if(auto error=receipt.validate()) {e=error;return {};}
  const auto remote=data["remoteStatus"].toString();
  // #1759: a removed destination is said before anything else, even after an earlier attempt
  // that failed or ended unknown; this preview offers Remove from History only, never a deletion.
  const auto current=Settings::profileForDestination(receipt.destination.toJson(),profiles);
  if(current.isEmpty()&&remote!="removed"&&remote!="already_missing"){const auto blocker=store.forgetBlocker(id,running);targets.append(QJsonObject{{"historyId",id},{"receipt",receipt.toJson()},{"destinationRemoved",true},{"forgettable",blocker.isEmpty()},{"retainedBy",blocker},{"effect",QStringLiteral("This file's destination was removed. The file stays at the destination.")}});continue;}
  if(remote=="pending"||remote=="deleting"||remote=="unknown"||remote=="changed"||remote=="hidden_version_retained") {e={"delete_target_changed","A deletion of this file is already running or didn't finish. Check its status in History.",{{"historyId",id}}};return {};}
  // An earlier deletion that may have sent a request keeps its checkpoint for Retry (#1763): a new one would start without it.
  if(data["deleteProgress"].toObject().contains("phase")&&!QStringList{"removed","already_missing","changed"}.contains(remote)) {e={"delete_target_changed","An earlier deletion of this file saved its progress. Select Retry deletion in History instead.",{{"historyId",id},{"deleteOperationId",data["deleteOperationId"]}}};return {};}
  auto provider=operations_.providers().find(receipt.destination.providerId);if(!provider) {e={"provider_unavailable","Destination is unavailable. Restore access to it in Destinations.",{{"historyId",id}}};return {};}
  // A destination added back for the same place under another ID deletes it with its own
  // credentials when the deletion runs (runDelete); the receipt stays as recorded.
  QJsonObject target{{"historyId",id}};
  if(current["id"].toString()!=receipt.destination.profileId)target["currentProfileId"]=current["id"];
  auto caps=provider->receiptCapabilities(receipt);if(!caps.safeRemoval()) {e={"unsupported_capability",caps.removeUnavailable.isEmpty()?QStringLiteral("This destination can't safely delete this file."):caps.removeUnavailable,{{"historyId",id}}};return {};}
  target["receipt"]=receipt.toJson();target["effect"]=caps.deletionEffect;
  // #2079: a split sealed link's data file goes too, once its page is gone.
  if(const auto part=data["seal"].toObject()["data"].toObject();part["uploadStatus"]=="uploaded"&&!part["receipt"].toObject().isEmpty()) {
   target["sealData"]=part["receipt"];
   // A data file this destination can't delete safely (a WebDAV file over 256 MB whose identity
   // couldn't be confirmed, for one) stays; the page goes, and the confirmation says so.
   const auto partCaps=provider->receiptCapabilities(StorageReceipt::fromJson(part["receipt"].toObject()));
   if(!partCaps.safeRemoval()) {
    target["sealDataKept"]=true;
    target["effect"]=caps.deletionEffect+" The sealed link's data file beside it stays at the destination, because put.gd can't delete it safely there."+(partCaps.removeUnavailable.isEmpty()?QString():" "+partCaps.removeUnavailable)+" It holds only encrypted data. Someone who saved the page and has the full link (and its passphrase, if it has one) can still open it until you delete the file there yourself.";
   }
  }
  targets.append(target);
 }
 const auto digest=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(targets).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
 return {{"targets",targets},{"acknowledgement",digest},{"localCopiesPreserved",true},{"cacheNotice","Copies already cached by browsers or a CDN can stay visible for a while"}};
}
void HistoryService::prepareDelete(const QStringList& ids,JsonReply reply) {if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}const auto running=QSet<QString>(active_.keyBegin(),active_.keyEnd());operations_.background([this,ids,running](Error& e) {return prepare(ids,currentProfiles(),running,e);},reply);}
namespace {Error quittingError() {return {"cancelled","put.gd is quitting. Try again after it restarts.",{}};}}
void HistoryService::stopDeletions() {quitting_->store(true);}
void HistoryService::deleteConfirmed(const QStringList& ids,const QString& acknowledgement,JsonReply reply) {
 if(quitting_->load()) {reply(quittingError(),{});return;}
  const auto running=QSet<QString>(active_.keyBegin(),active_.keyEnd());
 operations_.background([this,ids,acknowledgement,running](Error& e) {auto preview=prepare(ids,currentProfiles(),running,e);if(e)return QJsonObject{};
  // Nothing is deleted while any target's destination is gone (#1759): the preview offered Remove from History only.
  for(const auto& target:preview["targets"].toArray())if(target.toObject()["destinationRemoved"].toBool()){e=destinationRemovedError(target.toObject()["historyId"].toString());return QJsonObject{};}if(acknowledgement.isEmpty()||preview["acknowledgement"]!=acknowledgement) {e={"delete_confirmation_changed","The files to delete or their status changed. Review the confirmation and try again.",{}};return QJsonObject{};}
  // Each item keeps the file's name from the journal, so its outcome names it wherever it is shown (#1753).
  HistoryStore names(operations_.databasePath());
  QJsonArray items;for(auto target:preview["targets"].toArray()) {auto item=target.toObject();item["status"]="pending";item["progress"]=QJsonObject{{"schemaVersion",1}};Error ignored;const auto name=names.detail(item["historyId"].toString(),&ignored)["name"].toString();if(!name.isEmpty())item["name"]=name;items.append(item);}
  QJsonObject doc{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"created",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"state","accepted"},{"acknowledgement",acknowledgement},{"items",items},{"cancelled",false}};
  HistoryStore store(operations_.databasePath());e=store.saveDeletion(doc,true);return doc;
 },[this,reply](Error e,QJsonObject doc) {if(e) {reply(e,{});return;}notifyChanged();
  // Accepted but not started: the next start recovers it as interrupted.
  if(quitting_->load()) {reply(quittingError(),{});return;}
  const auto id=doc["id"].toString();auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(id,flag);reply({},{{"deleteOperationId",id},{"deletion",doc}});runDelete(doc,flag);});
}
void HistoryService::inspectSharing(const QString& id,JsonReply reply){if(stoppedSharing_.contains(id)){reply({},publicSharing(stoppedSharing_.value(id)));return;}const auto path=operations_.databasePath();operations_.background([path,id](Error& e){HistoryStore store(path);return publicSharing(store.sharing(id,&e));},std::move(reply));}
void HistoryService::changeSharing(const QString& historyId,const QString& action,const QJsonObject& options,JsonReply reply){
 if(!operations_.ready()){reply(operations_.notReadyError(),{});return;}
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
  if(receipt.validate()||!provider||!provider->capabilities(receipt.destination).sharing){e={"sharing_unsupported","This destination doesn't support managing share links.",{}};return QJsonObject{};}
  auto lock=operations_.objectLock(receipt.destination,receipt.objectReference,{});std::unique_lock guard(*lock);
  item=store.detail(historyId,&e);if(e)return QJsonObject{};
  const auto previousId=item["shareOperationId"].toString();
  if(stopped.contains(previousId)&&action=="inspect"){
   // A failed terminal write left a durable pending intent. Commit an unknown,
   // URL-cleared outcome before permitting a new read-only reconciliation.
   e=store.saveSharing(stopped.value(previousId));if(e)return QJsonObject{};
   item=store.detail(historyId,&e);if(e)return QJsonObject{};
  }
  // A new link needs a file still stored as uploaded; revoke and inspect also reach older states (#1496).
  if(item["uploadStatus"]!="uploaded"||QStringList{"pending","deleting","unknown","removed","already_missing","changed"}.contains(item["remoteStatus"].toString())||(action=="create"&&!OperationItem::shareableRemote(item["remoteStatus"].toString()))){e={"share_unavailable","This item isn't uploaded or is no longer available at the destination.",{}};return QJsonObject{};}
  if(item["shareStatus"]=="pending"||(action!="inspect"&&item["shareStatus"]=="unknown")){e={"share_result_unknown","A previous share link change is still pending or unconfirmed. Wait for it or inspect the link before changing it again.",{}};return QJsonObject{};}
  receipt=StorageReceipt::fromJson(item["receipt"].toObject());
  // Nextcloud (#1872): what identifies the link already made goes with the request: the share ID
  // its step recorded and, for a revoke, a digest of the recorded link's token (never the link,
  // which the receipt itself doesn't keep while the outcome is pending).
  const auto recorded=recordedShare(receipt,item.value("shareProgress").toObject(),action);
  if(action=="revoke")receipt.directUrl.clear();
  // A create without options keeps the ones the upload asked for, such as a diagnostics link's expiry (#1496).
  const auto chosen=action=="create"&&options.isEmpty()&&item["shareIntent"]=="create"?item["shareIntentOptions"].toObject():options;
  QJsonObject doc{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"historyId",historyId},{"action",action},{"options",chosen},{"state","accepted"},{"status","pending"},{"receipt",receipt.toJson()},{"progress",recorded}};
  e=store.saveSharing(doc,true);return doc;
 },[this,reply,stopped](Error e,QJsonObject doc){
  if(e){reply(e,{});return;}for(auto it=stopped.begin();it!=stopped.end();++it)stoppedSharing_.remove(it.key());const auto id=doc["id"].toString();auto cancelled=std::make_shared<std::atomic_bool>(false);active_.insert(id,cancelled);notifyChanged();
  reply({},{{"shareOperationId",id},{"sharing",publicSharing(doc)}});runSharing(doc,cancelled);
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
  if(!provider){e={"provider_unavailable","Destination is unavailable. Restore access to it in Destinations.",{}};return doc;}
  auto lock=operations_.objectLock(receipt.destination,receipt.objectReference,{});std::unique_lock guard(*lock);
  doc["state"]="running";if((e=store.saveSharing(doc)))return doc;
  auto checkpoint=[path,&doc](const QJsonObject& progress,std::function<void(Error)> ack){auto proposed=doc;proposed["progress"]=progress;HistoryStore durable(path);const auto error=durable.saveSharing(proposed);if(!error)doc=std::move(proposed);ack(error);};
  ShareRequest request{doc["id"].toString(),doc["action"].toString(),receipt,doc["options"].toObject(),doc["progress"].toObject(),cancelled};
  auto result=awaitProvider<ShareResult>([&](auto cb){provider->share(request,checkpoint,cb);});
  if(!shareResultMatchesReceipt(result,receipt)){
   result.status="unknown";result.receipt=receipt;result.expiresAt.clear();result.error={"share_identity_changed","Couldn't confirm that the share link belongs to this uploaded file.",{}};
  }
  if(!result.expiresAt.isEmpty())doc["expiresAt"]=result.expiresAt;
  if(result.status!="shared")result.receipt.directUrl.clear();
  doc["state"]="completed";doc["status"]=result.status;doc["receipt"]=result.receipt.toJson();// An unknown result without progress (a helper stopped by shutdown) keeps the last durable checkpoint.
  if(!(result.status=="unknown"&&result.progress.isEmpty()))doc["progress"]=result.progress;doc["error"]=result.error.toJson();e=store.saveSharing(doc);return doc;
 },[this,id](Error e,QJsonObject doc){
  if(e)recordStoppedSharing(doc,e);
  else emit sharingFinished(doc);
  active_.remove(id);notifyChanged();
 });
}
void HistoryService::runDelete(QJsonObject doc,std::shared_ptr<std::atomic_bool> cancelled) {
 // Provider checkpoints can run on a different worker. Queue a coalesced
 // metadata invalidation only after the durable transaction commits.
 const auto committed=[owner=QPointer<HistoryService>(this)] {
  if(owner)QMetaObject::invokeMethod(owner,[owner]{if(owner)owner->notifyChanged();},Qt::QueuedConnection);
 };
 const auto quitting=quitting_;
 operations_.background([this,doc,cancelled,committed,quitting](Error& error) mutable {
  // A run starts atomically with Remove from History only (#1759): records removed from History
  // only are left out of it (listed in "forgotten"), and every other unfinished target is retained.
  const auto path=operations_.databasePath();HistoryStore store(path);doc["state"]="running";if((error=store.startDeletion(doc)))return doc;committed();
  auto items=doc["items"].toArray();
  for(int index=0;index<items.size();++index) {
   // An item a newer deletion of the same file took over (#1773) is that deletion's to finish.
   auto item=items[index].toObject();auto status=item["status"].toString();if(status=="removed"||status=="already_missing"||status=="changed"||status=="taken_over")continue;
   if(cancelled->load()) {doc["cancelled"]=true;break;}
   // Quit: the rest is left as journaled; the next start recovers it as interrupted.
   if(quitting->load())return doc;
   auto receipt=StorageReceipt::fromJson(item["receipt"].toObject());auto provider=operations_.providers().find(receipt.destination.providerId);
   Error e;QString outcome;QJsonObject progress=item["progress"].toObject();
   // #1759: the destination serving this receipt now, read for each item, since Settings can change mid-batch.
   const auto current=Settings::profileForDestination(receipt.destination.toJson(),currentProfiles());
   if(!provider) {e={"provider_unavailable","Destination is unavailable. Restore access to it in Destinations.",{}};outcome="failed";}
   // Removed from Settings (#1759): said plainly, not as credentials not loaded. An earlier outcome that still
   // needs reconciling (unknown, a version left) and its checkpoint are kept; only a pending item becomes failed.
   else if(current.isEmpty()) {e=destinationRemovedError(item["historyId"].toString());outcome=status=="pending"?QStringLiteral("failed"):status;}
   else if(!provider->receiptCapabilities(receipt).safeRemoval()) {e={"unsupported_capability","This destination can't safely delete this file.",{}};outcome="failed";}
   else {
    // Another job on the same object (a link refresh, a sharing change) may hold it on
    // a stalled request: Quit stops this wait and leaves the item as it was.
    auto lock=operations_.objectLock(receipt.destination,receipt.objectReference,{});std::unique_lock guard(*lock,std::defer_lock);
    while(!guard.try_lock_for(std::chrono::milliseconds(100)))if(quitting->load())return doc;
    // No driver sends a destructive request before a "phase" checkpoint; a "target" alone is
    // recorded before anything is sent.
    const bool checkpointed=progress.contains("phase");
    // The destination an attempt went through: the recorded one, or one added back for the same place.
    const auto bound=item["deletingProfileId"].toString(receipt.destination.profileId);
    // Settings may have changed during that wait: the destination is read again now, that one first (#1759).
    // A destination added back serves only while it still points at the recorded place: edited to
    // point elsewhere, its credentials never go to this receipt's address.
    const auto profiles=currentProfiles();QJsonObject serving;
    if(bound!=receipt.destination.profileId){
     auto place=receipt.destination.toJson();place["profileId"]=QString();
     for(const auto& entry:profiles)if(entry.toObject()["id"].toString()==bound&&!Settings::profileForDestination(place,QJsonArray{entry}).isEmpty())serving=entry.toObject();
    }
    if(serving.isEmpty())serving=Settings::profileForDestination(receipt.destination.toJson(),profiles);
    if(serving.isEmpty()){e=destinationRemovedError(item["historyId"].toString());outcome=status=="pending"?QStringLiteral("failed"):status;}
    // A worker's checkpoint is bound to the destination it went through, and records the credentials it used
    // (drivers treat a different target as a changed object): another destination added back, or other
    // credentials on it, can't finish that attempt. Retry through the same one goes on.
    else if(checkpointed&&(serving["id"].toString()!=bound||(serving["id"].toString()!=receipt.destination.profileId&&progress["target"].toObject()["destination"].toObject()["credentials"].toObject()!=serving["credentials"].toObject()))){
     // The recorded object is checked read-only through the destination serving it now (#1772): gone, the
     // deletion ends; there or not known, it is left as it was with its checkpoint. Only a destination that
     // points at the recorded place is asked, so its credentials never go to another address (a profile can
     // serve by its ID after being edited). A version can stay behind a missing current object, so neither
     // an outcome that says so nor a checkpoint naming a delete marker (S3's markerVersionId, which a Quit
     // can leave on an unknown item) is asked.
     const bool credentials=serving["id"].toString()==bound;
     auto place=receipt.destination.toJson();place["profileId"]=QString();
     StatResult found{"",{},{}};
     if(!Settings::profileForDestination(place,QJsonArray{serving}).isEmpty()&&status!="hidden_version_retained"&&status!="version_retained"&&!progress.contains("markerVersionId")){
      auto check=receipt;check.destination.profileId=serving["id"].toString();check.destination.credentialReference=serving["credentials"].toObject();
      found=awaitProvider<StatResult>([&](auto cb){provider->statForRemoval(check,quitting,cb);});
      if(found.status=="unknown"&&quitting->load())return doc;
     }
     if(found.status=="missing")outcome="already_missing";
     else if(found.status=="foreign"){outcome="changed";e={"delete_target_changed","The file at the destination changed, so it wasn't deleted.",{}};}
     else {
      const auto answer=found.status.isEmpty()?QString():found.status=="own"?QStringLiteral("own"):QStringLiteral("unknown");
      QJsonObject details{{"historyId",item["historyId"]}};if(!answer.isEmpty())details["found"]=answer;if(found.error)details["checkError"]=found.error.toJson();
      e={"destination_readded",readdedMessage(status,answer,credentials,found.error),details};outcome=status;
     }
    }
    else {
    // A destination added back for the same place under another ID deletes with its own credentials;
    // the journal keeps the recorded receipt and names the destination used (deletingProfileId).
    if(serving["id"].toString()!=receipt.destination.profileId){
     receipt.destination.profileId=serving["id"].toString();receipt.destination.credentialReference=serving["credentials"].toObject();
    }
    // A "target" recorded for another binding sent nothing: this attempt records its own.
    if(!checkpointed&&progress.contains("target")&&progress["target"].toObject()!=receipt.toJson())progress.remove("target");
    const bool driverReconciles=provider->reconcilesDeleteProgress() && progress.contains("phase");
    // A reference the upload returned addresses exactly that upload: there is nothing to stat (#1361).
    const bool byReference=provider->receiptCapabilities(receipt).referenceDelete;
    auto stat=driverReconciles||byReference ? StatResult{"own",receipt,{}} : awaitProvider<StatResult>([&](auto cb) {provider->statForRemoval(receipt,quitting,cb);});
    // A lookup killed at quit sent nothing that deletes: the item stays as it was.
    if(stat.status=="unknown"&&quitting->load())return doc;
    if(stat.status=="missing")outcome="already_missing";
    else if(stat.status=="foreign")outcome="changed";
    else if(stat.status!="own"||stat.error) {outcome="unknown";e=stat.error?stat.error:Error{"delete_unknown","Couldn't confirm whether the file at the destination is still the one put.gd uploaded.",{}};}
    else if(stat.receipt.publicationId!=receipt.publicationId||stat.receipt.objectReference!=receipt.objectReference||stat.receipt.concurrencyToken!=receipt.concurrencyToken||stat.receipt.destination.toJson()!=receipt.destination.toJson()) {outcome="changed";e={"delete_target_changed","The file at the destination changed, so it wasn't deleted.",{}};}
    else {
     const auto before=items[index].toObject();
     item["status"]="deleting";item["deletingProfileId"]=receipt.destination.profileId;items[index]=item;doc["items"]=items;if((error=store.saveDeletion(doc)))return doc;committed();
     // Marking it deleting is where this deletion holds the file against a newer one (#1773): one recorded
     // since took it over, and nothing is sent; once it is deleting, a newer one is refused.
     items=doc["items"].toArray();if(items[index].toObject().value("status")=="taken_over")continue;
     // Whether this attempt's driver got a checkpoint acknowledged: none send a
     // destructive request before one (see DurableCheckpoint).
     auto acknowledged=std::make_shared<std::atomic_bool>(false);
     // Callback may run on the provider's worker. It creates a connection on that
     // thread, commits progress, then acknowledges; no Qt SQL connection crosses threads.
     auto checkpoint=[path,&doc,&items,index,committed,acknowledged,quitting](const QJsonObject& checkpoint,std::function<void(Error)> ack) {
      if(!checkpoint["schemaVersion"].isDouble()||checkpoint["schemaVersion"].toInt()<1) {ack({"delete_checkpoint_invalid","Driver progress requires a version",{}});return;}
      auto proposedItems=items;
      auto target=proposedItems[index].toObject();target["progress"]=checkpoint;proposedItems[index]=target;
      auto proposedDoc=doc;proposedDoc["items"]=proposedItems;
      HistoryStore durable(path);const auto e=durable.saveDeletion(proposedDoc);
      // Quit: the acknowledgement is withheld, so the helper takes no further step
      // (#1602). An intent (a driver's "..._intent" phase) only asks permission for the
      // next request, so the journal goes back to the checkpoint before it. Any other
      // checkpoint records what the server already did (S3's marker_done and its marker
      // ID): it is kept, for Retry. An acknowledgement sent just before Quit may have
      // been delivered: that attempt counts as acknowledged.
      if(!e&&quitting->load()) {
       if(checkpoint["phase"].toString().endsWith("intent")) {if(!durable.saveDeletion(doc))committed();}
       else {items=std::move(proposedItems);doc=std::move(proposedDoc);committed();acknowledged->store(true);}
       ack({"cancelled","put.gd is quitting",{}});return;
      }
      // Empty provider results may reuse this state, so only retain acknowledged progress.
      if(!e) {items=std::move(proposedItems);doc=std::move(proposedDoc);committed();acknowledged->store(true);}
      ack(e);
     };
     DeleteRequest request{doc["id"].toString(),receipt,progress,cancelled,quitting};
     auto result=awaitProvider<DeleteResult>([&](auto cb) {provider->remove(request,checkpoint,cb);});outcome=result.status=="missing"?"already_missing":result.status;progress=result.progress.isEmpty()?items[index].toObject()["progress"].toObject():result.progress;e=result.error;
     const QStringList allowed{"removed","already_missing","changed","failed","unknown","hidden_version_retained","version_retained","unsupported_safe_removal"};if(!allowed.contains(outcome)) {outcome="unknown";e={"delete_unknown","Couldn't confirm whether the file was deleted.",{}};}
     // Killed at quit before any checkpoint of this attempt: nothing was sent that
     // deletes, so the item goes back to what it recorded before (pending, or an
     // earlier attempt's unknown with its intent).
     if(quitting->load()&&!acknowledged->load()&&(outcome=="unknown"||outcome=="failed")) {
      items[index]=before;doc["items"]=items;if((error=store.saveDeletion(doc)))return doc;committed();return doc;
     }
     // A result read after Quit began cannot show that the server did nothing (its
     // helper was killed or stopped): failed is recorded unknown, with the last
     // acknowledged intent. A result that shows the outcome (gone, changed, a version
     // retained) stands.
     if(quitting->load()&&outcome=="failed") {
      outcome="unknown";progress=items[index].toObject()["progress"].toObject();
      e={"delete_unknown","put.gd quit before confirming whether the file was deleted. Select Retry deletion to check again.",{{"reportedError",e.toJson()}}};
     }
    }
    }
   }
   // #2079: the sealed page is gone, so its data file beside it goes too. It is checked and
   // deleted like the page, with its own durable checkpoint; one left behind holds only
   // ciphertext nobody has a page for, and the deletion says so.
   // Quit before the data file's turn: the item stays unfinished, so Retry deletion still takes it.
   // A data file the confirmation said stays is left as said.
   if((outcome=="removed"||outcome=="already_missing")&&items[index].toObject().contains("sealData")&&items[index].toObject()["sealDataKept"].toBool()){auto target=items[index].toObject();target["sealDataStatus"]="kept";items[index]=target;doc["items"]=items;}
   else if((outcome=="removed"||outcome=="already_missing")&&items[index].toObject().contains("sealData")&&items[index].toObject()["sealDataStatus"]!="removed"&&quitting->load()){outcome="unknown";e={"seal_data_left","The sealed page was deleted, but put.gd quit before deleting its data file. Select Retry deletion.",{}};}
   else if((outcome=="removed"||outcome=="already_missing")&&items[index].toObject().contains("sealData")&&!quitting->load()) {
    auto part=StorageReceipt::fromJson(items[index].toObject()["sealData"].toObject());
    part.destination.profileId=receipt.destination.profileId;part.destination.credentialReference=receipt.destination.credentialReference;
    QString partOutcome="unknown";Error partError;
    if(!part.validate()) {
     auto found=awaitProvider<StatResult>([&](auto cb){provider->statForRemoval(part,quitting,cb);});
     if(found.status=="missing")partOutcome="already_missing";
     else if(found.status=="foreign")partOutcome="changed";
     else if(found.status=="own"&&found.receipt.publicationId==part.publicationId) {
      // As the page's checkpoint: once Quit began, an intent isn't acknowledged (the journal goes
      // back to before it), so the provider sends no further delete during shutdown.
      const auto partAcknowledged=std::make_shared<std::atomic_bool>(false);
      auto partCheckpoint=[path,&doc,&items,index,committed,quitting,partAcknowledged](const QJsonObject& checkpoint,std::function<void(Error)> ack){
       auto proposed=items;auto target=proposed[index].toObject();target["sealDataProgress"]=checkpoint;proposed[index]=target;
       auto proposedDoc=doc;proposedDoc["items"]=proposed;HistoryStore durable(path);const auto saved=durable.saveDeletion(proposedDoc);
       if(!saved&&quitting->load()) {
        if(checkpoint["phase"].toString().endsWith("intent")){if(!durable.saveDeletion(doc))committed();}
        else {items=std::move(proposed);doc=std::move(proposedDoc);committed();partAcknowledged->store(true);}
        ack({"cancelled","put.gd is quitting",{}});return;
       }
       if(!saved){items=std::move(proposed);doc=std::move(proposedDoc);committed();partAcknowledged->store(true);}
       ack(saved);
      };
      DeleteRequest request{doc["id"].toString()+"-data",part,items[index].toObject()["sealDataProgress"].toObject(),cancelled,quitting};
      auto removed=awaitProvider<DeleteResult>([&](auto cb){provider->remove(request,partCheckpoint,cb);});
      partOutcome=removed.status=="missing"?QStringLiteral("already_missing"):removed.status;partError=removed.error;
      // As for the page: a failed result read after Quit began can't show the server did nothing.
      // With an acknowledged checkpoint the delete may have gone out, so it is unknown; without one
      // nothing that deletes was sent this time, and the data file keeps what it recorded before.
      if(quitting->load()&&(partOutcome=="failed"||partOutcome=="unknown")) {
       if(partAcknowledged->load()){partOutcome=QStringLiteral("unknown");partError={"delete_unknown","put.gd quit before confirming whether the data file was deleted. Select Retry deletion to check again.",{{"reportedError",partError.toJson()}}};}
       else partOutcome=items[index].toObject()["sealDataStatus"]=="unknown"?QStringLiteral("unknown"):QStringLiteral("failed");
      }
     } else partError=found.error;
    }
    auto target=items[index].toObject();target["sealDataStatus"]=partOutcome;items[index]=target;doc["items"]=items;
    // Not finished until the data file goes too: Retry deletion finds the page gone and tries it again.
    // A data file changed at the destination isn't put.gd's to delete; it is said, not retried.
    if(partOutcome=="changed")e={"seal_data_left","The sealed page was deleted. The file beside it changed at the destination, so put.gd left it.",{{"dataStatus",partOutcome}}};
    else if(partOutcome!="removed"&&partOutcome!="already_missing"){
     outcome=partOutcome=="unknown"?QStringLiteral("unknown"):QStringLiteral("failed");
     // Unknown isn't "not deleted": put.gd only doesn't know yet.
     const auto message=partOutcome=="unknown"
      ?(quitting->load()?QStringLiteral("The sealed page was deleted, but put.gd quit before confirming whether its data file was deleted. Select Retry deletion to check again.")
        :QStringLiteral("The sealed page was deleted, but put.gd couldn't confirm whether its data file was deleted. Select Retry deletion to check again."))
      :QStringLiteral("The sealed page was deleted, but its data file beside it wasn't. It holds only encrypted data. Select Retry deletion.");
     e={"seal_data_left",message,{{"dataStatus",partOutcome},{"dataError",partError.toJson()}}};
    }
   }
   item=items[index].toObject();item["status"]=outcome;item["progress"]=progress;item["error"]=e.toJson();items[index]=item;doc["items"]=items;if((error=store.saveDeletion(doc)))return doc;committed();
   items=doc["items"].toArray();
  }
  // Quit: the deletion stays running in its journal; the next start marks it interrupted.
  if(quitting->load())return doc;
  doc["state"]=cancelled->load()?"cancelled":"completed";doc["items"]=items;error=store.saveDeletion(doc);if(!error)committed();return doc;
 },[this,id=doc["id"].toString()](Error,QJsonObject finished) {active_.remove(id);if(finished["state"]=="completed"||finished["state"]=="cancelled")emit deletionFinished(finished);});
}
void HistoryService::inspectDelete(const QString& id,JsonReply reply) {const auto path=operations_.databasePath();operations_.background([path,id](Error& e) {HistoryStore store(path);return store.deletion(id,&e);},reply);}
void HistoryService::retryDelete(const QString& id,JsonReply reply) {
 if(quitting_->load()) {reply(quittingError(),{});return;}
 if(active_.contains(id)) {reply({"delete_busy","This deletion is already running.",{}},{});return;}
 inspectDelete(id,[this,reply](Error e,QJsonObject doc) {if(e) {reply(e,{});return;}if(quitting_->load()) {reply(quittingError(),{});return;}auto id=doc["id"].toString();if(active_.contains(id)) {reply({"delete_busy","This deletion is already running.",{}},{});return;}auto flag=std::make_shared<std::atomic_bool>(false);active_.insert(id,flag);reply({},{{"deleteOperationId",id},{"retryAccepted",true}});runDelete(doc,flag);});
}
void HistoryService::cancelDelete(const QString& id,JsonReply reply) {if(active_.contains(id)) {active_[id]->store(true);reply({},{{"deleteOperationId",id},{"cancellationRequested",true},{"effect","Already sent deletion can't be rolled back"}});return;}reply({"delete_not_active","This deletion isn't running.",{}},{});}
}
