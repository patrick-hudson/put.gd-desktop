#include "ErrorLog.h"
#include "core/Log.h"
#include <QJsonDocument>
#include <QPointer>
#include <QUuid>
namespace putgd {
namespace {
QString iso(const QDateTime& time) {return time.toUTC().toString(Qt::ISODateWithMs);}
bool meaningful(const QJsonObject& error) {
 const auto code=error["code"].toString();
 return !code.isEmpty()&&code!="copy_suppressed"&&code!="cancelled";
}
Error errorFrom(const QJsonObject& error) {return {error["code"].toString(),error["message"].toString(),error["details"].toObject()};}
// The first line, and its first sentence when that is shorter: the table's one-line message.
QString oneLine(const QString& message) {
 auto line=message.section('\n',0,0).trimmed();
 const auto stop=line.indexOf(QLatin1String(". "));
 if(stop>20)line=line.left(stop+1);
 if(line.size()>200)line=line.left(199)+QChar(0x2026);
 return line;
}
bool credentialCode(const QString& code) {
 return code=="credential_setup_required"||code.startsWith("credential")||code.startsWith("credentials_")||code.contains("access_denied")||code.contains("permission_denied")||code.contains("unauthorized");
}
QString providerName(const QString& provider) {
 if(provider=="s3")return "Amazon S3";
 if(provider=="backblaze-b2")return "Backblaze B2";
 if(provider=="cloudflare-r2")return "Cloudflare R2";
 if(provider=="proton-drive")return "Proton Drive";
 if(provider=="custom-http")return "Custom uploader";
 return provider;
}
}
ErrorLog::ErrorLog(OperationService& operations,QObject* parent):QObject(parent),operations_(operations) {
 // Every operation's terminal result, a local save that failed on retry, and Copy text.
 connect(&operations_,&OperationService::resultReady,this,[this](const QString& id){
  operations_.inspect(id,[guard=QPointer<ErrorLog>(this)](Error error,QJsonObject operation){if(guard&&!error)guard->operationFinished(operation);});
 });
 connect(&operations_,&OperationService::operationError,this,[this](const QString& id,QJsonObject operation){operation["id"]=id;operationFinished(operation);});
 connect(&operations_,&OperationService::textFinished,this,[this](const QString& id,const QJsonObject& record,const QString&){
  if(record["state"]!="failed")return;
  Entry context;context.action=Settings::textActionName();context.operationId=id;context.subject="text:"+id;
  this->record("text_recognition",errorFrom(record["error"].toObject()),context);
 });
 // The table is read once the journal is recovered; entries queued before then wait.
 connect(&operations_,&OperationService::initialized,this,[this]{load();pump();});
 if(operations_.ready())load();
}
const QStringList& ErrorLog::kinds() {
 static const QStringList list{"upload","capture","recording","local_save","copy","deletion","share_link","destination_test","destination_setup","credentials","settings_save","text_recognition","diagnostics","history","desktop_setup","other"};
 return list;
}
QString ErrorLog::kindForAction(const QString& action) {
 if(action==Settings::recordingActionName())return "recording";
 if(action==Settings::textActionName())return "text_recognition";
 if(action==OperationService::fileManagerAction)return "upload";
 if(action==OperationService::diagnosticsAction)return "diagnostics";
 return "capture";
}
bool ErrorLog::userFacing(const Error& error) {
 static const QStringList protocol{"invalid_request","invalid_json","request_too_large","unsupported_version","wrong_session","unknown_method","initializing","operation_settling","credentials_require_settings","error_not_found","not_found"};
 const auto& code=error.code;
 if(code.isEmpty()||protocol.contains(code)||code.endsWith("_busy")||code.startsWith("ui_")||code=="cancelled"||code=="copy_suppressed")return false;
 return true;
}
QString ErrorLog::destinationLabel(const QString& profileId,const QString& provider) const {
 if(!profileId.isEmpty()){const auto profile=operations_.settingsSnapshot().profiles.value(profileId);if(!profile["label"].toString().isEmpty())return profile["label"].toString();}
 return providerName(provider);
}
namespace {
// What the journal knows that the caller may not have: the item's name and the
// destination its receipt names. Then the destination's name now, and next steps.
void completeEntry(QJsonObject& entry,const QJsonObject& item,const QHash<QString,QJsonObject>& profiles) {
 if(entry["itemName"].toString().isEmpty()&&!item["name"].toString().isEmpty())entry["itemName"]=logging::redact(item["name"].toString()).left(512);
 if(entry["profileId"].toString().isEmpty()&&!item["profileId"].toString().isEmpty())entry["profileId"]=item["profileId"];
 if(entry["provider"].toString().isEmpty()&&!item["providerId"].toString().isEmpty())entry["provider"]=item["providerId"];
 const auto profileId=entry["profileId"].toString(),provider=entry["provider"].toString();
 if(entry["destination"].toString().isEmpty()&&(!profileId.isEmpty()||!provider.isEmpty())){
  const auto label=profiles.value(profileId)["label"].toString();
  entry["destination"]=label.isEmpty()?providerName(provider):label;
 }
 // What the Error log offers as buttons, in the order it shows them.
 QStringList steps;const auto add=[&steps](const QString& step){if(!steps.contains(step))steps<<step;};
 const auto kind=entry["kind"].toString(),code=entry["code"].toString();
 const auto has=[&entry](const char* key){return !entry[QLatin1String(key)].toString().isEmpty();};
 if(credentialCode(code)&&(has("historyId")||has("profileId")))add("unlock_credentials");
 if(has("profileId")&&kind!="settings_save")add("open_destination");
 if((QStringList{"upload","local_save","copy"}.contains(kind)&&has("operationId"))||(kind=="deletion"&&has("deletionId"))||(kind=="destination_test"&&has("profileId")))add("retry");
 for(const auto& step:entry["nextSteps"].toArray())add(step.toString());
 if(has("historyId"))add("show_in_history");
 if(has("operationId"))add("save_diagnostics");
 entry["nextSteps"]=QJsonArray::fromStringList(steps);
}
}
QJsonObject ErrorLog::entryJson(const Entry& e,const QString& id,const QDateTime& now,int occurrences,const QString& first) const {
 const auto message=logging::redact(e.message.isEmpty()?QStringLiteral("put.gd couldn't finish this."):e.message).left(4096);
 auto details=logging::redact(QJsonValue(e.details)).toObject();
 if(QJsonDocument(details).toJson(QJsonDocument::Compact).size()>maximumDetailBytes)details=QJsonObject{{"truncated",true}};
 auto provider=e.provider;
 if(provider.isEmpty()&&!e.profileId.isEmpty())provider=operations_.settingsSnapshot().profiles.value(e.profileId)["provider"].toString();
 QJsonObject entry{{"id",id},{"time",iso(now)},{"firstTime",first},{"occurrences",occurrences},{"kind",kinds().contains(e.kind)?e.kind:QStringLiteral("other")},
  {"code",e.code.left(128)},{"title",oneLine(message)},{"message",message},{"details",details},{"subject",e.subject},{"resolved",false},{"nextSteps",QJsonArray::fromStringList(e.steps)}};
 const auto put=[&entry](const char* key,const QString& value){if(!value.isEmpty())entry[QLatin1String(key)]=value.left(512);};
 put("action",e.action);put("operationId",e.operationId);put("itemId",e.itemId);put("historyId",e.historyId);put("deletionId",e.deletionId);put("sharingId",e.sharingId);
 put("setupId",e.setupId);put("profileId",e.profileId);put("provider",provider);put("itemName",logging::redact(e.itemName));
 put("destination",e.destination.isEmpty()&&!e.profileId.isEmpty()?destinationLabel(e.profileId,provider):e.destination);
 return entry;
}
QString ErrorLog::record(Entry e) {
 const auto now=QDateTime::currentDateTimeUtc();
 const auto key=e.kind+'|'+(e.subject.isEmpty()?e.message:e.subject)+'|'+e.code;
 QString id,first=iso(now);int occurrences=1;
 auto recent=recent_.find(key);
 if(recent!=recent_.end()&&recent->last.secsTo(now)<coalesceSeconds){id=recent->id;first=recent->first;occurrences=++recent->occurrences;recent->last=now;}
 else {
  id=QUuid::createUuid().toString(QUuid::WithoutBraces);
  if(recent_.size()>=256)recent_.clear();
  recent_.insert(key,{id,first,now,1});
 }
 auto entry=entryJson(e,id,now,occurrences,first);
 if(!e.subject.isEmpty())open_.insert(e.subject);
 logging::info(logHistory(),"Error log entry",{{"errorId",id},{"kind",entry["kind"]},{"code",entry["code"]},{"occurrences",occurrences},{"operationId",e.operationId},{"itemId",e.historyId}});
 const auto notBefore=iso(now.addDays(-keepDays));const auto profiles=operations_.settingsSnapshot().profiles;
 enqueue([entry,notBefore,profiles](HistoryStore& store,Error& error) mutable {
  completeEntry(entry,store.errorItem(entry["historyId"].toString(),entry["operationId"].toString()),profiles);
  QJsonObject saved;error=store.saveError(entry,keep,notBefore,&saved);return saved;
 },[id](Error error,QJsonObject){if(error)logging::warning(logHistory(),"Error log entry not saved",{{"errorId",id},{"code",error.code}});});
 return id;
}
QString ErrorLog::record(const QString& kind,const Error& error,Entry context) {
 context.kind=kind;context.code=error.code;context.message=error.message;
 auto details=error.details;details.remove("errorLogId");
 for(auto it=context.details.begin();it!=context.details.end();++it)details[it.key()]=it.value();
 context.details=details;
 return record(std::move(context));
}
void ErrorLog::resolve(const QString& subject,const QString& resolution) {
 if(subject.isEmpty()||(loaded_&&!open_.contains(subject)))return;
 open_.remove(subject);
 for(auto it=recent_.begin();it!=recent_.end();)it=it.key().section('|',1,1)==subject?recent_.erase(it):std::next(it);
 const auto at=iso(QDateTime::currentDateTimeUtc());
 enqueue([subject,at,resolution](HistoryStore& store,Error& error){QStringList ids;error=store.resolveErrors(subject,at,resolution,&ids);return QJsonObject{{"resolved",QJsonArray::fromStringList(ids)}};});
}
void ErrorLog::operationFinished(const QJsonObject& op) {
 const auto state=op["state"].toString();
 if(!QStringList{"completed","partial","failed","interrupted"}.contains(state))return;
 const auto id=op["id"].toString(),action=op["action"].toString();
 const auto destination=op["destination"].toObject();
 const auto profileId=destination["profileId"].toString(),provider=destination["providerId"].toString();
 const auto uploadKind=action==OperationService::diagnosticsAction?QStringLiteral("diagnostics"):QStringLiteral("upload");
 bool itemErrors=false;
 for(const auto& value:op["items"].toArray()){
  const auto item=value.toObject();const auto artifact=item["artifact"].toObject();
  const auto historyId=artifact["id"].toString(),name=artifact["name"].toString(),status=item["uploadStatus"].toString();
  Entry context;context.action=action;context.operationId=id;context.itemId=historyId;context.historyId=historyId;context.itemName=name;context.profileId=profileId;context.provider=provider;
  if(status=="uploaded")resolve("item:"+historyId,"Uploaded");
  else if(meaningful(item["error"].toObject())){itemErrors=true;context.subject="item:"+historyId;record(uploadKind,errorFrom(item["error"].toObject()),context);}
  const auto local=item["local"].toObject();
  if(meaningful(local["error"].toObject())){itemErrors=true;context.subject="local:"+historyId;context.profileId.clear();record("local_save",errorFrom(local["error"].toObject()),context);}
  else if(local["status"]=="saved")resolve("local:"+historyId,"Saved");
 }
 Entry context;context.action=action;context.operationId=id;context.profileId=profileId;context.provider=provider;context.subject="operation:"+id;
 if(meaningful(op["error"].toObject())&&!itemErrors)record(kindForAction(action),errorFrom(op["error"].toObject()),context);
 else if(state=="completed")resolve("operation:"+id,"Finished");
 // An upload's own link copy (#473): failed or partial is an error; suppressed is by design.
 const auto copy=op["copyStatus"].toString();
 if(copy=="failed"||copy=="partial"){context.subject="copy:"+id;context.profileId.clear();record("copy",{"copy_failed",copy=="partial"?QStringLiteral("Only some of the links were copied to the clipboard."):QStringLiteral("The link couldn't be copied to the clipboard."),{{"copyStatus",copy}}},context);}
 else if(copy=="copied")resolve("copy:"+id,"Copied");
}
void ErrorLog::deletionFinished(const QJsonObject& doc) {
 static const QHash<QString,QString> outcomes{
  {"failed","The deletion failed."},{"unknown","put.gd couldn't tell whether the file was deleted."},
  {"changed","The file at the destination changed after it was uploaded, so put.gd left it in place."},
  {"hidden_version_retained","The file was deleted, but an older hidden version remains at the destination."},
  {"version_retained","The file was deleted, but an earlier version remains at the destination."},
  {"unsupported_safe_removal","This destination can't safely delete the recorded file."}};
 for(const auto& value:doc["items"].toArray()){
  const auto item=value.toObject();const auto status=item["status"].toString(),historyId=item["historyId"].toString();
  if(status=="removed"||status=="already_missing"){resolve("deletion:"+historyId,"Deleted from the destination");continue;}
  if(!outcomes.contains(status))continue;
  const auto destination=item["receipt"].toObject()["destination"].toObject();
  Entry context;context.deletionId=doc["id"].toString();context.historyId=historyId;context.itemId=historyId;context.itemName=item["name"].toString();
  context.profileId=destination["profileId"].toString();context.provider=destination["providerId"].toString();context.subject="deletion:"+historyId;
  context.details={{"deletionStatus",status}};
  auto error=errorFrom(item["error"].toObject());if(error.code.isEmpty())error={"delete_"+status,outcomes.value(status),{}};
  record("deletion",error,context);
 }
}
void ErrorLog::sharingFinished(const QJsonObject& doc) {
 const auto historyId=doc["historyId"].toString(),status=doc["status"].toString();
 const auto error=errorFrom(doc["error"].toObject());
 if(!meaningful(doc["error"].toObject())){if(QStringList{"shared","private","revoked"}.contains(status))resolve("share:"+historyId,status=="shared"?QStringLiteral("Link created"):QStringLiteral("Link changed"));return;}
 const auto destination=doc["receipt"].toObject()["destination"].toObject();
 Entry context;context.sharingId=doc["id"].toString();context.historyId=historyId;context.itemId=historyId;context.action=doc["action"].toString();
 context.profileId=destination["profileId"].toString();context.provider=destination["providerId"].toString();context.subject="share:"+historyId;context.details={{"shareStatus",status}};
 record("share_link",error,context);
}
void ErrorLog::setupFinished(const QJsonObject& job) {
 const auto kind=job["kind"].toString(),profileId=job["profileId"].toString(),state=job["state"].toString();
 const bool credential=QStringList{"storeCredentials","unlockCredentials","storeSharePassword"}.contains(kind);
 const auto subject=(kind=="testDestination"?QStringLiteral("test:"):credential?QStringLiteral("credentials:"):QStringLiteral("setup:"))+profileId;
 if(state=="completed"){resolve(subject,kind=="testDestination"?QStringLiteral("Test connection passed"):credential?QStringLiteral("Credentials loaded"):QStringLiteral("Finished"));return;}
 const auto error=errorFrom(job["error"].toObject());
 if(state!="failed"||!userFacing(error))return;
 Entry context;context.setupId=job["setupId"].toString();context.profileId=profileId;context.action=kind;context.subject=subject;
 record(kind=="testDestination"?QStringLiteral("destination_test"):credential?QStringLiteral("credentials"):QStringLiteral("destination_setup"),error,context);
}
void ErrorLog::list(const ErrorLogQuery& query,JsonReply reply) {
 enqueue([query](HistoryStore& store,Error& error){return store.listErrors(query,&error);},[this,reply](Error error,QJsonObject page){if(!error)page["revision"]=QString::number(revision_);reply(error,page);});
}
void ErrorLog::inspect(const QString& id,JsonReply reply) {
 enqueue([id](HistoryStore& store,Error& error){return store.errorEntry(id,&error);},std::move(reply));
}
void ErrorLog::clear(const QStringList& ids,bool resolvedOnly,JsonReply reply) {
 enqueue([ids,resolvedOnly](HistoryStore& store,Error& error){int removed=0;error=store.clearErrors(ids,resolvedOnly,&removed);return QJsonObject{{"removed",removed}};},
  [this,reply](Error error,QJsonObject result){if(!error&&result["removed"].toInt()>0){recent_.clear();load();}reply(error,result);});
}
QJsonObject ErrorLog::summary() const {return {{"revision",QString::number(revision_)},{"total",double(total_)},{"unresolved",double(unresolved_)}};}
void ErrorLog::load() {
 enqueue([](HistoryStore& store,Error& error){return store.errorSummary(&error);},[this](Error error,QJsonObject summary){
  if(error)return;
  open_.clear();for(const auto& subject:summary["subjects"].toArray())open_.insert(subject.toString());
  loaded_=true;
 });
}
void ErrorLog::enqueue(Work work,JsonReply done) {queue_.push_back({std::move(work),std::move(done)});pump();}
void ErrorLog::pump() {
 if(running_||queue_.empty()||!operations_.ready())return;
 running_=true;auto job=std::move(queue_.front());queue_.pop_front();
 const auto path=operations_.databasePath();
 // Every job reads the counts after its own work, so the snapshot's summary follows the table.
 operations_.background([path,work=std::move(job.work)](Error& error){
  HistoryStore store(path);if((error=store.error()))return QJsonObject{};
  auto result=work(store,error);
  Error ignored;const auto summary=store.errorSummary(&ignored);
  result["__summary"]=summary;return result;
 },[guard=QPointer<ErrorLog>(this),done=std::move(job.done)](Error error,QJsonObject result){
  if(!guard)return;
  const auto summary=result.take("__summary").toObject();
  if(!summary.isEmpty()){
   const auto total=qint64(summary["total"].toDouble()),unresolved=qint64(summary["unresolved"].toDouble());
   if(total!=guard->total_||unresolved!=guard->unresolved_||result.contains("id")||!result["resolved"].toArray().isEmpty()||result["removed"].toInt()>0){
    guard->total_=total;guard->unresolved_=unresolved;++guard->revision_;emit guard->changed();
   }
  }
  if(done)done(error,result);
  guard->running_=false;guard->pump();
 });
}
}
