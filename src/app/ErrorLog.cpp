#include "ErrorLog.h"
#include "core/Log.h"
#include <QCryptographicHash>
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
 // Every operation's committed terminal snapshot (also after Retry local save), and Copy text.
 connect(&operations_,&OperationService::outcome,this,&ErrorLog::operationFinished);
 connect(&operations_,&OperationService::copyFinished,this,[this](const QJsonObject& op,const QJsonObject& copy){
  const auto id=op["id"].toString(),status=op["copyStatus"].toString();
  if(status=="copied"){resolve("copy:"+id,"Copied");return;}
  if(status!="failed"&&status!="partial")return;
  Entry context;context.action=op["action"].toString();context.operationId=id;context.subject="copy:"+id;
  const auto own=errorFrom(copy["error"].toObject());
  record("copy",meaningful(copy["error"].toObject())?own:Error{"copy_failed",status=="partial"?QStringLiteral("Only some of the links were copied to the clipboard."):QStringLiteral("The link couldn't be copied to the clipboard."),{{"copyStatus",status}}},context);
 });
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
// A subject is matched by its digest: stable across restarts, distinct for distinct subjects,
// and never the caller's text, which may hold anything an ID can.
QString ErrorLog::subjectKey(const QString& subject) {
 if(subject.isEmpty())return {};
 return QString::fromLatin1(QCryptographicHash::hash(subject.toUtf8(),QCryptographicHash::Sha256).toHex());
}
QString ErrorLog::destinationLabel(const QString& profileId,const QString& provider) const {
 if(!profileId.isEmpty()){const auto profile=operations_.settingsSnapshot().profiles.value(profileId);if(!profile["label"].toString().isEmpty())return profile["label"].toString();}
 return providerName(provider);
}
namespace {
// What the journal knows that the caller may not have: the item's name and the
// destination its receipt names. Then the destination's name now, and next steps.
void completeEntry(QJsonObject& entry,const QJsonObject& item,const QHash<QString,QJsonObject>& profiles) {
 if(entry.value("itemName").toString().isEmpty()&&!item["name"].toString().isEmpty())entry["itemName"]=logging::redact(item["name"].toString()).left(512);
 if(entry.value("profileId").toString().isEmpty()&&!item["profileId"].toString().isEmpty())entry["profileId"]=logging::redact(item["profileId"].toString()).left(512);
 if(entry.value("provider").toString().isEmpty()&&!item["providerId"].toString().isEmpty())entry["provider"]=logging::redact(item["providerId"].toString()).left(512);
 const auto profileId=entry.value("profileId").toString(),provider=entry.value("provider").toString();
 if(entry.value("destination").toString().isEmpty()&&(!profileId.isEmpty()||!provider.isEmpty())){
  const auto label=profiles.value(profileId)["label"].toString();
  entry["destination"]=logging::redact(label.isEmpty()?providerName(provider):label).left(512);
 }
 // What the Error log offers as buttons, in the order it shows them.
 QStringList steps;const auto add=[&steps](const QString& step){if(!steps.contains(step))steps<<step;};
 const auto kind=entry.value("kind").toString(),code=entry.value("code").toString();
 const auto has=[&entry](const char* key){return !entry.value(QLatin1String(key)).toString().isEmpty();};
 if(credentialCode(code)&&(has("historyId")||has("profileId")))add("unlock_credentials");
 if(has("profileId")&&kind!="settings_save")add("open_destination");
 // Retry repeats a failed outcome. A refused session call (its details name the method) offers no
 // Retry: repeating a refused cancel or a share-only retry as an upload retry would change its effect.
 const bool outcome=!entry.value("details").toObject().contains("method");
 if(outcome&&((QStringList{"upload","local_save","copy"}.contains(kind)&&has("operationId"))||(kind=="deletion"&&has("deletionId"))||(kind=="destination_test"&&has("profileId"))))add("retry");
 for(const auto& step:entry.value("nextSteps").toArray())add(step.toString());
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
  {"code",logging::redact(e.code).left(128)},{"title",oneLine(message)},{"message",message},{"details",details},{"subject",e.subject},{"resolved",false},{"nextSteps",QJsonArray::fromStringList(e.steps)}};
 // Every stored string goes through the log's redaction, IDs included: a caller's ID is never trusted.
 const auto put=[&entry](const char* key,const QString& value){if(!value.isEmpty())entry[QLatin1String(key)]=logging::redact(value).left(512);};
 put("action",e.action);put("operationId",e.operationId);put("itemId",e.itemId);put("historyId",e.historyId);put("deletionId",e.deletionId);put("sharingId",e.sharingId);
 put("setupId",e.setupId);put("profileId",e.profileId);put("provider",provider);put("itemName",logging::redact(e.itemName));
 put("destination",logging::redact(e.destination.isEmpty()&&!e.profileId.isEmpty()?destinationLabel(e.profileId,provider):e.destination));
 return entry;
}
QString ErrorLog::record(Entry e) {
 const auto now=QDateTime::currentDateTimeUtc();
 e.subject=subjectKey(e.subject);
 const auto key=e.kind+'|'+(e.subject.isEmpty()?e.message:e.subject)+'|'+e.code;
 QString id,first=iso(now);int occurrences=1;
 const auto recent=recent_.constFind(key);
 const bool repeat=recent!=recent_.constEnd()&&recent->last.secsTo(now)<coalesceSeconds;
 if(repeat){id=recent->id;first=recent->first;occurrences=recent->occurrences+1;}
 else id=QUuid::createUuid().toString(QUuid::WithoutBraces);
 auto entry=entryJson(e,id,now,occurrences,first);
 const auto notBefore=iso(now.addDays(-keepDays));const auto profiles=operations_.settingsSnapshot().profiles;
 const bool accepted=enqueue([entry,notBefore,profiles](HistoryStore& store,Error& error) mutable {
  completeEntry(entry,store.errorItem(entry.value("historyId").toString(),entry.value("operationId").toString()),profiles);
  QJsonObject saved;error=store.saveError(entry,keep,notBefore,&saved);return saved;
 },[id](Error error,QJsonObject){if(error)logging::warning(logHistory(),"Error log entry not saved",{{"errorId",id},{"code",error.code}});},true);
 // Only an accepted write gets an ID, counts as a repeat and opens its subject.
 if(!accepted)return {};
 if(recent_.size()>=256&&!repeat)recent_.clear();
 recent_.insert(key,{id,first,now,occurrences});
 if(!e.subject.isEmpty())open_.insert(e.subject);
 logging::info(logHistory(),"Error log entry",{{"errorId",id},{"kind",entry.value("kind")},{"code",entry.value("code")},{"occurrences",occurrences},{"operationId",e.operationId},{"itemId",e.historyId}});
 // Bounded: past this many open subjects the set is read again from the table, which keeps at most
 // `keep` rows; until then every success asks the table.
 if(open_.size()>=4*keep){open_.clear();loaded_=false;load();}
 return id;
}
QString ErrorLog::record(const QString& kind,const Error& error,Entry context) {
 context.kind=kind;context.code=error.code;context.message=error.message;
 auto details=error.details;details.remove("errorLogId");
 for(auto it=context.details.begin();it!=context.details.end();++it)details[it.key()]=it.value();
 context.details=details;
 return record(std::move(context));
}
void ErrorLog::resolve(const QString& raw,const QString& resolution) {
 // The same key the entry was stored under (redacted), so a subject always matches itself.
 const auto subject=subjectKey(raw);
 if(subject.isEmpty()||(loaded_&&!open_.contains(subject)))return;
 open_.remove(subject);
 for(auto it=recent_.begin();it!=recent_.end();)it=it.key().section('|',1,1)==subject?recent_.erase(it):std::next(it);
 const auto at=iso(QDateTime::currentDateTimeUtc());
 enqueue([subject,at,resolution](HistoryStore& store,Error& error){QStringList ids;error=store.resolveErrors(subject,at,resolution,&ids);return QJsonObject{{"resolved",QJsonArray::fromStringList(ids)}};},{},true);
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
  const bool failed=meaningful(item["error"].toObject());
  if(status=="uploaded"){
   resolve("item:"+historyId,"Uploaded");
   // Uploaded, but its share link wasn't made: a link error of its own (#1496's Finish share link fixes it).
   // A link confirmed shared wins over an older error the snapshot still carries.
   if(item["shareStatus"]=="shared")resolve("share:"+historyId,"Link created");
   else if(failed){itemErrors=true;context.subject="share:"+historyId;record("share_link",errorFrom(item["error"].toObject()),context);}
  }
  else if(failed){itemErrors=true;context.subject="item:"+historyId;record(uploadKind,errorFrom(item["error"].toObject()),context);}
  const auto local=item["local"].toObject();
  if(meaningful(local["error"].toObject())){itemErrors=true;context.subject="local:"+historyId;context.profileId.clear();record("local_save",errorFrom(local["error"].toObject()),context);}
  else if(local["status"]=="saved")resolve("local:"+historyId,"Saved");
 }
 Entry context;context.action=action;context.operationId=id;context.profileId=profileId;context.provider=provider;context.subject="operation:"+id;
 // An upload's own link copy (#473): failed or partial is an error; suppressed is by design. When the
 // operation's error is the copy's own, it is that one row, not two.
 const auto copy=op["copyStatus"].toString();const auto operationError=errorFrom(op["error"].toObject());
 const bool copyError=operationError.code.startsWith("copy")||operationError.code.startsWith("clipboard");
 if(meaningful(op["error"].toObject())&&!itemErrors&&!copyError)record(kindForAction(action),operationError,context);
 else if(state=="completed"&&!meaningful(op["error"].toObject()))resolve("operation:"+id,"Finished");
 if(copy=="failed"||copy=="partial"){context.subject="copy:"+id;context.profileId.clear();
  record("copy",copyError?operationError:Error{"copy_failed",copy=="partial"?QStringLiteral("Only some of the links were copied to the clipboard."):QStringLiteral("The link couldn't be copied to the clipboard."),{{"copyStatus",copy}}},context);}
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
  [this,reply](Error error,QJsonObject result){if(!error&&result["removed"].toInt()>0){recent_.clear();load();}reply(error,result);},true);
}
QJsonObject ErrorLog::summary() const {return {{"revision",QString::number(revision_)},{"total",double(total_)},{"unresolved",double(unresolved_)}};}
void ErrorLog::load() {
 // Only the newest reload marks the set loaded; an older one still queued only adds its subjects.
 const auto generation=++loads_;
 enqueue([](HistoryStore& store,Error& error){return store.errorSummary(&error);},[this,generation](Error error,QJsonObject summary){
  if(error)return;
  // Merged, never replaced: a subject recorded while this read ran stays open. A stale subject
  // only costs one resolve job that finds nothing.
  for(const auto& subject:summary["subjects"].toArray())open_.insert(subject.toString());
  if(generation==loads_)loaded_=true;
 });
}
bool ErrorLog::enqueue(Work work,JsonReply done,bool write) {
 // Reads answer at once while the journal recovers; writes wait, up to a bound (#1753).
 if(!write&&!operations_.ready()){if(done)done(operations_.notReadyError(),{});return false;}
 if(write&&queue_.size()>=maximumQueue){++dropped_;logging::warning(logHistory(),"Error log write dropped",{{"queued",qint64(queue_.size())},{"dropped",qint64(dropped_)}});if(done)done({"error_log_full","The error log can't keep up; this entry wasn't saved",{}},{});return false;}
 queue_.push_back({std::move(work),std::move(done),write});pump();
 return true;
}
// Quit: the writes still waiting are saved here, on this thread, after the workers stopped.
void ErrorLog::flush() {
 if(queue_.empty()||!operations_.ready())return;
 HistoryStore store(operations_.databasePath());if(store.error())return;
 while(!queue_.empty()){auto job=std::move(queue_.front());queue_.pop_front();if(!job.write)continue;Error error;job.work(store,error);}
}
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
