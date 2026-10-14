#include "Operation.h"
namespace putgd {
bool OperationItem::linkAvailable() const {return uploadStatus=="uploaded"&&!receipt.directUrl.isEmpty()&&!QStringList{"pending","unknown","revoked","private","failed","changed"}.contains(shareStatus)&&!QStringList{"removed","already_missing","changed","hidden_version_retained"}.contains(remoteStatus);}
namespace {Error errorFrom(const QJsonObject& o) {return {o["code"].toString(),o["message"].toString(),o["details"].toObject()};}}
QJsonObject OperationItem::toJson() const {return {{"artifact",artifact.toJson()},{"uploadStatus",uploadStatus},{"candidate",candidate},{"condition",condition},{"concurrencyToken",concurrencyToken},{"publicationId",publicationId},{"objectReference",objectReference},{"receipt",receipt.toJson()},{"local",local.toJson()},{"remoteStatus",remoteStatus},{"shareStatus",shareStatus},{"shareIntent",shareIntent},{"shareIntentOptions",shareIntentOptions},{"error",error.toJson()},{"phase",phase},{"uploadProgress",uploadProgress?QJsonValue(uploadProgress->toJson()):QJsonValue(QJsonValue::Null)}};}
OperationItem OperationItem::fromJson(const QJsonObject& o) {OperationItem i; i.artifact=Artifact::fromJson(o["artifact"].toObject());i.uploadStatus=o["uploadStatus"].toString();i.candidate=o["candidate"].toString();i.condition=o["condition"].toString();i.concurrencyToken=o["concurrencyToken"].toString();i.publicationId=o["publicationId"].toString();i.objectReference=o["objectReference"].toObject();i.receipt=StorageReceipt::fromJson(o["receipt"].toObject());i.local=LocalCopy::fromJson(o["local"].toObject());i.remoteStatus=o["remoteStatus"].toString();i.shareStatus=o["shareStatus"].toString("not_requested");i.shareIntent=o["shareIntent"].toString();i.shareIntentOptions=o["shareIntentOptions"].toObject();i.error=errorFrom(o["error"].toObject());i.phase=o["phase"].toString(i.uploadStatus);i.uploadProgress=UploadProgress::fromJson(o["uploadProgress"].toObject());return i;}
UploadRequest OperationItem::uploadRequest(const Destination& d) const {return {d,artifact,candidate,publicationId,condition,objectReference,concurrencyToken,{},{}};}
QJsonObject Operation::toJson() const {QJsonArray array; for(const auto& i:items) array.append(i.toJson()); return {{"id",id},{"action",action},{"state",state},{"phase",phase},{"settingsRevision",settingsRevision},{"settings",settings.document},{"destination",destination.toJson()},{"created",created},{"items",array},{"invocation",QString::number(invocation)},{"clipboardRevision",QString::number(clipboardRevision)},{"copyStatus",copyStatus},{"cancelled",cancelled},{"error",error.toJson()}};}
QJsonObject Operation::publicJson() const {
 auto result=toJson();
 result.remove("settings");
 auto array=result["items"].toArray();
 for(qsizetype index=0;index<array.size();++index) {
  auto item=array[index].toObject();
  auto artifact=item["artifact"].toObject();
  artifact.remove("path");
  item["artifact"]=artifact;
  array[index]=item;
 }
 result["items"]=array;
 return result;
}
Operation Operation::fromJson(const QJsonObject& o) {Operation op;op.id=o["id"].toString();op.action=o["action"].toString();op.state=o["state"].toString();op.phase=o["phase"].toString(op.state);op.settingsRevision=o["settingsRevision"].toString();op.settings=Settings::fromValidated(o["settings"].toObject());op.destination=Destination::fromJson(o["destination"].toObject());op.created=o["created"].toString();for(const auto& i:o["items"].toArray()) op.items.append(OperationItem::fromJson(i.toObject()));op.invocation=o["invocation"].toString().toULongLong();op.clipboardRevision=o["clipboardRevision"].toString().toULongLong();op.copyStatus=o["copyStatus"].toString();op.cancelled=o["cancelled"].toBool();op.error=errorFrom(o["error"].toObject());return op;}
}
