#include "Operation.h"
#include "Log.h"
#include <utility>
namespace putgd {
namespace {
using S=OperationState;
constexpr std::pair<S,const char*> stateNames[]{{S::Accepted,"accepted"},{S::Acquiring,"acquiring"},{S::Editing,"editing"},{S::Recording,"recording"},{S::Recognizing,"recognizing"},
 {S::Stored,"stored"},{S::Running,"running"},{S::Completed,"completed"},{S::Partial,"partial"},{S::Failed,"failed"},{S::Cancelled,"cancelled"},{S::Interrupted,"interrupted"}};
// Every step some code takes today. A journal write that fails after the
// final state was chosen ends the operation failed; Retry starts any finished
// row again, and one a failed journal write left stored or running.
constexpr std::pair<S,S> steps[]{
 {S::Accepted,S::Acquiring},{S::Accepted,S::Recording},{S::Accepted,S::Cancelled},{S::Accepted,S::Failed},{S::Accepted,S::Interrupted},
 {S::Acquiring,S::Editing},{S::Acquiring,S::Recognizing},{S::Acquiring,S::Stored},{S::Acquiring,S::Cancelled},{S::Acquiring,S::Failed},{S::Acquiring,S::Interrupted},
 {S::Editing,S::Acquiring},{S::Editing,S::Cancelled},{S::Editing,S::Failed},{S::Editing,S::Interrupted},
 {S::Recording,S::Acquiring},{S::Recording,S::Cancelled},{S::Recording,S::Failed},{S::Recording,S::Interrupted},
 {S::Recognizing,S::Completed},{S::Recognizing,S::Cancelled},{S::Recognizing,S::Failed},{S::Recognizing,S::Interrupted},
 {S::Stored,S::Running},{S::Stored,S::Completed},{S::Stored,S::Cancelled},{S::Stored,S::Failed},{S::Stored,S::Interrupted},
 {S::Running,S::Completed},{S::Running,S::Partial},{S::Running,S::Cancelled},{S::Running,S::Failed},{S::Running,S::Interrupted},{S::Running,S::Stored},
 {S::Completed,S::Failed},{S::Partial,S::Failed},{S::Cancelled,S::Failed},
 {S::Completed,S::Stored},{S::Partial,S::Stored},{S::Failed,S::Stored},{S::Cancelled,S::Stored},{S::Interrupted,S::Stored},{S::Unknown,S::Stored}};
}
QString operationStateName(OperationState state) {for(const auto& [value,name]:stateNames)if(value==state)return QString::fromLatin1(name);return {};}
OperationState operationState(const QString& name) {for(const auto& [value,text]:stateNames)if(name==QLatin1String(text))return value;return S::Unknown;}
namespace {bool activeValue(S s) {return s==S::Accepted||s==S::Acquiring||s==S::Editing||s==S::Recording||s==S::Recognizing||s==S::Stored||s==S::Running;}}
bool operationStep(OperationState from,OperationState to) {for(const auto& [a,b]:steps)if(a==from&&b==to)return true;return false;}
bool Operation::activeState(const QString& state) {return activeValue(operationState(state));}
bool Operation::active() const {return activeValue(state_);}
QString Operation::stateName() const {return state_==S::Unknown?unknownState_:operationStateName(state_);}
bool Operation::advance(OperationState to) {
 if(to==state_)return true;
 if(to==S::Unknown||!operationStep(state_,to)) {logging::warning(logOperation(),"Operation state step refused",{{"operationId",id},{"from",state_==S::Unknown?QStringLiteral("unknown"):operationStateName(state_)},{"to",operationStateName(to)}});return false;}
 state_=to;unknownState_.clear();return true;
}
bool OperationItem::linkAvailable() const {return uploadStatus=="uploaded"&&!receipt.directUrl.isEmpty()&&!QStringList{"pending","unknown","revoked","private","failed","changed"}.contains(shareStatus)&&!QStringList{"removed","already_missing","changed","hidden_version_retained"}.contains(remoteStatus);}
namespace {Error errorFrom(const QJsonObject& o) {return {o["code"].toString(),o["message"].toString(),o["details"].toObject()};}}
QJsonObject OperationItem::toJson() const {return {{"artifact",artifact.toJson()},{"uploadStatus",uploadStatus},{"candidate",candidate},{"condition",condition},{"concurrencyToken",concurrencyToken},{"publicationId",publicationId},{"objectReference",objectReference},{"receipt",receipt.toJson()},{"local",local.toJson()},{"remoteStatus",remoteStatus},{"shareStatus",shareStatus},{"shareIntent",shareIntent},{"shareIntentOptions",shareIntentOptions},{"error",error.toJson()},{"phase",phase},{"uploadProgress",uploadProgress?QJsonValue(uploadProgress->toJson()):QJsonValue(QJsonValue::Null)},{"uploadJournal",uploadJournal}};}
OperationItem OperationItem::fromJson(const QJsonObject& o) {OperationItem i; i.artifact=Artifact::fromJson(o["artifact"].toObject());i.uploadStatus=o["uploadStatus"].toString();i.candidate=o["candidate"].toString();i.condition=o["condition"].toString();i.concurrencyToken=o["concurrencyToken"].toString();i.publicationId=o["publicationId"].toString();i.objectReference=o["objectReference"].toObject();i.receipt=StorageReceipt::fromJson(o["receipt"].toObject());i.local=LocalCopy::fromJson(o["local"].toObject());i.remoteStatus=o["remoteStatus"].toString();i.shareStatus=o["shareStatus"].toString("not_requested");i.shareIntent=o["shareIntent"].toString();i.shareIntentOptions=o["shareIntentOptions"].toObject();i.error=errorFrom(o["error"].toObject());i.phase=o["phase"].toString(i.uploadStatus);i.uploadProgress=UploadProgress::fromJson(o["uploadProgress"].toObject());i.uploadJournal=o["uploadJournal"].toObject();return i;}
UploadRequest OperationItem::uploadRequest(const Destination& d,std::optional<int> timeout) const {return {d,artifact,candidate,publicationId,condition,objectReference,concurrencyToken,{},{},timeout,uploadJournal};}
QJsonObject Operation::toJson() const {QJsonArray array; for(const auto& i:items) array.append(i.toJson()); return {{"note",note},{"id",id},{"action",action},{"state",stateName()},{"phase",phase},{"settingsRevision",settingsRevision},{"settings",settings.document},{"destination",destination.toJson()},{"requestTimeoutMs",requestTimeoutMs?QJsonValue(*requestTimeoutMs):QJsonValue(QJsonValue::Null)},{"created",created},{"items",array},{"invocation",QString::number(invocation)},{"clipboardRevision",QString::number(clipboardRevision)},{"copyStatus",copyStatus},{"cancelled",cancelled},{"error",error.toJson()}};}
QJsonObject Operation::publicJson() const {
 auto result=toJson();
 result.remove("settings");
 auto array=result["items"].toArray();
 for(qsizetype index=0;index<array.size();++index) {
  auto item=array[index].toObject();
  auto artifact=item["artifact"].toObject();
  artifact.remove("path");
  item["artifact"]=artifact;
  item.remove("uploadJournal");
  array[index]=item;
 }
 result["items"]=array;
 return result;
}
Operation Operation::fromJson(const QJsonObject& o) {Operation op;op.note=o["note"].toString();op.id=o["id"].toString();op.action=o["action"].toString();const auto state=o["state"].toString();op.state_=operationState(state);if(op.state_==S::Unknown)op.unknownState_=state;op.phase=o["phase"].toString(state);op.settingsRevision=o["settingsRevision"].toString();op.settings=Settings::fromValidated(o["settings"].toObject());op.destination=Destination::fromJson(o["destination"].toObject());const auto timeout=o["requestTimeoutMs"].toInt();if(timeout>=1000)op.requestTimeoutMs=timeout;op.created=o["created"].toString();for(const auto& i:o["items"].toArray()) op.items.append(OperationItem::fromJson(i.toObject()));op.invocation=o["invocation"].toString().toULongLong();op.clipboardRevision=o["clipboardRevision"].toString().toULongLong();op.copyStatus=o["copyStatus"].toString();op.cancelled=o["cancelled"].toBool();op.error=errorFrom(o["error"].toObject());return op;}
}
