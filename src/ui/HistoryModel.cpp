#include "HistoryModel.h"
#include "core/ByteSize.h"
#include "core/Operation.h"
#include "PreviewReader.h"
#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QLocale>
#include <QPointer>
#include <algorithm>
#include <utility>
#include <tuple>
namespace putgd {
namespace {
QDateTime created(const QJsonObject& r){auto d=QDateTime::fromString(r["created"].toString(),Qt::ISODateWithMs);if(!d.isValid())d=QDateTime::fromString(r["created"].toString(),Qt::ISODate);return d.toLocalTime();}
// A day section's heading, in sentence case: the year only when it is not this one.
QString day(const QDateTime& created){const auto date=created.date();const auto today=QDate::currentDate();if(date==today)return QStringLiteral("Today");if(date==today.addDays(-1))return QStringLiteral("Yesterday");return date.isValid()?QLocale().toString(date,date.year()==today.year()?QStringLiteral("dddd, MMMM d"):QStringLiteral("dddd, MMMM d, yyyy")):QStringLiteral("Earlier");}
QString itemCount(qint64 count){return QStringLiteral("%1 %2").arg(QLocale().toString(count),count==1?QStringLiteral("item"):QStringLiteral("items"));}
struct AggregateProgress {
 qint64 sent=0,total=0;bool known=false;
 int percent()const{return total>0?qBound(0,int(100.0*sent/total),100):-1;}
};
AggregateProgress aggregateProgress(const QJsonObject& operation){
 AggregateProgress result;
 for(const auto& value:operation["items"].toArray()){
  const auto progress=UploadProgress::fromJson(value.toObject()["uploadProgress"].toObject());
  if(progress&&progress->bytesTransferred){result.sent+=*progress->bytesTransferred;result.known=true;}
  if(progress&&progress->bytesTotal)result.total+=*progress->bytesTotal;
 }
 return result;
}
bool localSaveFailed(const QJsonObject& r){
 if(r["local"].toObject()["status"]=="failed")return true;
 for(const auto& value:r["records"].toArray())if(value.toObject()["local"].toObject()["status"]=="failed")return true;
 return false;
}
bool active(const QJsonObject& r){return Operation::activeState(r["operation"].toObject()["state"].toString());}
}
// The locale's short time of day, never with seconds (the C locale's short form has them).
static QString shortTime(const QTime& time){auto format=QLocale().timeFormat(QLocale::ShortFormat);format.remove(QStringLiteral(":ss"));format.remove(QStringLiteral(".ss"));return QLocale().toString(time,format);}
QString HistoryModel::longTimestamp(const QDateTime& when){if(!when.isValid())return {};const auto local=when.toLocalTime();return QLocale().toString(local.date(),QStringLiteral("MMMM d, yyyy"))+QStringLiteral(", ")+shortTime(local.time());}
// The time column: relative within the last hour, else the locale's short
// time; with the date (and a year that is not this one) where there are no
// day headings to carry it.
QString HistoryModel::timeColumn(const QDateTime& when,bool withDate){
 if(!when.isValid())return {};const auto local=when.toLocalTime();const auto seconds=local.secsTo(QDateTime::currentDateTime());
 if(seconds<60)return QStringLiteral("Just now");if(seconds<3600)return QStringLiteral("%1 min ago").arg(seconds/60);
 const auto time=shortTime(local.time());if(!withDate)return time;
 return QLocale().toString(local.date(),local.date().year()==QDate::currentDate().year()?QStringLiteral("MMM d"):QStringLiteral("MMM d, yyyy"))+QStringLiteral(", ")+time;
}
QString HistoryModel::dayLabel(int row)const{
 // Today and Yesterday move at midnight; the labels follow the date.
 const auto today=QDate::currentDate();
 if(dayLabelsFor_!=today){dayLabels_.clear();for(const auto& created:createdAt_)dayLabels_.append(day(created));dayLabelsFor_=today;}
 return dayLabels_.at(row);
}
// Work in progress sorts first under its own heading; a page that starts in
// the middle of a day says so.
QString HistoryModel::section(int row)const{
 if(facts_.at(row).active)return QStringLiteral("In progress");
 auto label=dayLabel(row);if(continued_.isValid()&&createdAt_.at(row).date()==continued_)label+=QStringLiteral(" · continued");return label;
}
QJsonArray HistoryModel::dayCounts()const{
 QJsonArray result;QList<QDate> seen;
 for(int row=0;row<rows_.size();++row){if(facts_.at(row).active)continue;const auto date=createdAt_.at(row).date();if(seen.contains(date))continue;seen.append(date);
  result.append(QJsonObject{{"day",date.toString(Qt::ISODate)},{"heading",section(row)},{"files",dayCounts_.contains(date)?QJsonValue(double(dayCounts_.value(date))):QJsonValue()}});}
 return result;
}
// Where a row's result went or was going: this device for a local-only
// result, else the destination's current name, or its provider when the
// destination was since removed.
QString HistoryModel::destination(int row)const{
 const auto r=record(row);const auto op=r["operation"].toObject();
 if(r["uploadStatus"]=="local_only")return QStringLiteral("This device");
 const auto profileId=r["profileId"].toString(op["destination"].toObject()["profileId"].toString());
 for(const auto& value:history_.document()["profiles"].toArray())if(value.toObject()["id"]==profileId&&!profileId.isEmpty())return value.toObject()["label"].toString();
 auto provider=r["receipt"].toObject()["destination"].toObject()["providerId"].toString();if(provider.isEmpty())provider=op["destination"].toObject()["providerId"].toString();
 if(provider=="proton-drive")return QStringLiteral("Proton Drive");if(provider=="backblaze-b2")return QStringLiteral("Backblaze B2");if(provider=="custom-http")return QStringLiteral("Custom uploader");if(provider=="s3")return QStringLiteral("S3 destination");
 return r["local"].toObject()["status"]=="saved"?QStringLiteral("This device"):QString{};
}
HistoryModel::RowFacts HistoryModel::rowFacts(const QJsonObject& r){
 RowFacts facts;facts.active=active(r);facts.failed=failed(r);
 if(facts.active)return facts;
 if(r["remoteStatus"]=="failed"&&!r["deleteOperationId"].toString().isEmpty())facts.primary="Retry deletion";else if(copyLocalResult(r))facts.primary="Copy result";else if(retryUpload(r))facts.primary="Retry upload";else if(finishShareLink(r))facts.primary=finishShareLabel(r);else if(retryLocalSave(r))facts.primary="Retry local save";else if(r["linkAvailable"].toBool()||r["linkRefreshable"].toBool())facts.primary="Copy link";
 return facts;
}
QString HistoryModel::fileSize(qint64 bytes){return ByteSize::fileSummary(bytes);}
QString HistoryModel::duration(qint64 ms){if(ms<=0)return {};const qint64 s=(ms+999)/1000;return s>=3600?QString("%1:%2:%3").arg(s/3600).arg(s/60%60,2,10,QChar('0')).arg(s%60,2,10,QChar('0')):QString("%1:%2").arg(s/60).arg(s%60,2,10,QChar('0'));}
HistoryModel::HistoryModel(UiSession& history,QObject* parent):QAbstractListModel(parent),history_(history){}
int HistoryModel::rowCount(const QModelIndex& parent)const{return parent.isValid()?0:rows_.size();}
QJsonObject HistoryModel::record(int row)const{return row>=0&&row<rows_.size()?rows_.at(row):QJsonObject{};}
QJsonArray HistoryModel::activeOperations()const{QJsonArray result;for(const auto& v:operations_)if(Operation::activeState(v.toObject()["state"].toString()))result.append(v);return result;}
int HistoryModel::operationProgress(const QJsonObject& operation){return aggregateProgress(operation).percent();}
bool HistoryModel::linkAvailable(int row)const{return record(row)["linkAvailable"].toBool();}
bool HistoryModel::scanDestinations()const{
 QSet<QString> destinations;
 for(const auto& row:rows_){auto files=row["records"].toArray();if(files.isEmpty())files.append(row);for(const auto& value:std::as_const(files)){const auto file=value.toObject(),destination=file["receipt"].toObject()["destination"].toObject();auto id=file["profileId"].toString();if(id.isEmpty())id=destination["profileId"].toString();if(id.isEmpty())id=row["operation"].toObject()["destination"].toObject()["profileId"].toString();id=file["uploadStatus"]=="local_only"?QString("local:"):!id.isEmpty()?"profile:"+id:"receipt:"+QString::fromUtf8(QJsonDocument(destination).toJson(QJsonDocument::Compact));destinations.insert(id);if(destinations.size()>1)return true;}}
 return false;
}
QString HistoryModel::friendlyAction(const QString& action){if(action=="region")return "Region";if(action=="window")return "Window";if(action=="desktop")return "Full desktop";if(action=="monitor")return "Monitor";if(action=="clipboard")return "Clipboard";if(action=="files")return "File manager";if(action=="edit")return "Region & edit";if(action=="scroll")return "Scrolling region";if(action=="record")return "Recording";if(action=="images"||action=="image")return "Image";if(action=="text")return "Text";if(action=="videos"||action=="video")return "Video";return "File";}
bool HistoryModel::storedNothing(const QJsonObject& r){return r["records"].toArray().isEmpty()&&r["operation"].toObject()["items"].toArray().isEmpty();}
QString HistoryModel::failureLabel(const QJsonObject& r){
 if(localSaveFailed(r)&&r["uploadStatus"]!="failed")return "Local save failed";
 if(!storedNothing(r)||r["operation"].toObject().isEmpty())return "Upload failed";
 const auto action=r["operation"].toObject()["action"].toString();
 return action=="edit"?"Edit failed":action=="text"?"Text recognition failed":action=="clipboard"||action=="files"?"Upload failed":"Capture failed";
}
bool HistoryModel::failed(const QJsonObject& r){if(r["uploadStatus"]=="local_only")return localSaveFailed(r);const auto state=r["operation"].toObject().value("state");return localSaveFailed(r)||r["uploadStatus"]=="failed"||state=="failed"||state=="partial";}
bool HistoryModel::retryUpload(const QJsonObject& r){return (r["uploadStatus"]=="failed"||r["operation"].toObject()["state"]=="failed"||r["operation"].toObject()["state"]=="partial")&&!active(r)&&r["uploadStatus"]!="local_only"&&r["uploadStatus"]!="uploaded"&&!r["operationId"].toString().isEmpty()&&!storedNothing(r);}
namespace {
bool terminalUpload(const QString& s){return s=="uploaded"||s=="skipped"||s=="conflict"||s=="local_only";}
// The operation's own items when History has them (a filter or page can hide some files), else the row's files.
QJsonArray operationFiles(const QJsonObject& r){auto items=r["operation"].toObject()["items"].toArray();if(!items.isEmpty())return items;auto files=r["records"].toArray();if(files.isEmpty())files.append(r);return files;}
QJsonArray rowFiles(const QJsonObject& r){auto files=r["records"].toArray();if(files.isEmpty())files.append(r);return files;}
}
// The resident refuses the link-only retry when any file of the operation still needs
// its upload, so a row this rule misjudges never uploads anything.
bool HistoryModel::finishShareLink(const QJsonObject& r){
 if(active(r)||r["operationId"].toString().isEmpty()||retryUpload(r))return false;
 for(const auto& v:operationFiles(r))if(!terminalUpload(v.toObject()["uploadStatus"].toString()))return false;
 bool finishable=false;for(const auto& v:rowFiles(r)){const auto f=v.toObject();if(!terminalUpload(f["uploadStatus"].toString()))return false;if(f["shareStatus"]=="pending")return false;finishable|=f["linkFinishable"].toBool();}
 return finishable;
}
QString HistoryModel::finishShareLabel(const QJsonObject& r){
 if(!finishShareLink(r))return {};
 // Count every link the retry may make: in share mode it also makes an uploaded file's
 // missing or failed link, so err towards the plural.
 int links=0;for(const auto& v:operationFiles(r)){const auto f=v.toObject();const auto share=f["shareStatus"].toString();
  if(f["uploadStatus"]=="uploaded"&&f["receipt"].toObject()["directUrl"].toString().isEmpty()&&(f["linkFinishable"].toBool()||(f["shareIntent"]=="create"&&QStringList{"unknown","private","failed"}.contains(share))||share=="not_requested"))++links;}
 return links>1?QStringLiteral("Finish share links"):QStringLiteral("Finish share link");
}
// Any failed local copy can be saved again from the retained artifact,
// including one whose upload succeeded.
bool HistoryModel::retryLocalSave(const QJsonObject& r){return localSaveFailed(r)&&!active(r)&&!r["operationId"].toString().isEmpty();}
bool HistoryModel::copyLocalResult(const QJsonObject& r){return r["uploadStatus"]=="local_only"&&!localSaveFailed(r)&&r["local"].toObject()["status"]=="saved"&&!active(r)&&!r["operationId"].toString().isEmpty();}
// delete.prepare accepts confirmed uploads whose remote outcome is settled;
// a target already gone has nothing left to delete.
bool HistoryModel::remoteDeletable(const QJsonObject& f){return f["uploadStatus"]=="uploaded"&&!QStringList{"pending","deleting","unknown","changed","hidden_version_retained","removed","already_missing"}.contains(f["remoteStatus"].toString());}
QString HistoryModel::remoteStateText(const QJsonObject& r){
 // Current outcomes outrank an earlier clipboard success, including in a batch.
 auto files=r["records"].toArray();files.prepend(r);
 static const QList<std::tuple<QString,QString,QString>> outcomes{
  {"remoteStatus","failed","Deletion failed"},{"remoteStatus","unknown","Remote outcome needs inspection"},{"remoteStatus","changed","Remote file changed"},
  {"remoteStatus","deleting","Deleting from destination"},{"remoteStatus","pending","Waiting for deletion"},
  {"remoteStatus","removed","Deleted from destination"},{"remoteStatus","already_missing","Deleted from destination"},
  {"shareStatus","failed","Sharing failed"},{"shareStatus","unfinished","Share link not finished"},{"shareStatus","unknown","Sharing outcome needs inspection"},{"shareStatus","changed","Share link changed"},
  {"shareStatus","pending","Creating share link"},{"shareStatus","revoked","Link revoked"},
  {"remoteStatus","hidden_version_retained","Hidden; original version remains"},{"remoteStatus","version_retained","Original version retained"},
  {"remoteStatus","unsupported_safe_removal","Safe removal unavailable"},{"remoteStatus","superseded","Replaced by a newer upload"}};
 QList<std::pair<QString,QString>> states;for(const auto& value:std::as_const(files)){const auto file=value.toObject();
  // A wanted link not confirmed or not yet made reads the same before and after Inspect link (#1496).
  states.append({file["remoteStatus"].toString(),file["linkFinishable"].toBool()?QStringLiteral("unfinished"):file["shareStatus"].toString()});}
 for(const auto& [key,state,label]:outcomes)
  for(const auto& [remote,share]:states)if((key=="remoteStatus"?remote:share)==state)return label;
 return {};
}
bool HistoryModel::localCopyUnavailable(const QJsonObject& r){return (r["local"].toObject()["status"]=="saved"||r["uploadStatus"]=="local_only")&&!QFileInfo(r["local"].toObject()["path"].toString()).isFile();}
QString HistoryModel::statusText(const QJsonObject& r){
 const auto op=r["operation"].toObject();if(!op["note"].toString().isEmpty()){
  auto plain=r;auto operation=op;operation.remove("note");plain["operation"]=operation;
  if(active(r)||op["state"]=="cancelled"||op["cancelled"].toBool()||r["uploadStatus"]=="cancelled"||r["uploadStatus"]=="failed"||op["state"]=="failed"||op["state"]=="partial"||localSaveFailed(r))return statusText(plain)+" · "+op["note"].toString();
  return op["note"].toString();
 }const auto state=op["state"].toString();const auto phase=op["phase"].toString();
 if(active(r)){
  if(op["action"]=="record"){
   if(phase=="preparing")return "Setting up the recording…";
   if(phase=="ready")return "Ready to record";
   if(phase=="recording")return "Recording";
   if(phase=="paused")return "Recording paused";
   if(phase=="stopping")return "Stopping the recording…";
   if(phase=="reviewing")return "Review recording";
   if(phase=="exporting")return "Saving a recording copy…";
   if(phase=="encoding")return "Encoding recording…";
  }
  // A retry with every upload done only finishes links (#1496).
  {const auto items=op["items"].toArray();bool linksOnly=!items.isEmpty();for(const auto& v:items)linksOnly&=terminalUpload(v.toObject()["uploadStatus"].toString());
   if(linksOnly&&QStringList{"queued","preflight","sharing"}.contains(phase))return "Creating share link…";}
  if(phase=="queued")return "Waiting to upload…";
  if(state=="editing"||phase=="editing")return "Editing · Upload or cancel in the editor window";
  if(op["action"]=="edit"&&(state=="acquiring"||phase=="acquiring"))return "Choose what to edit… · Press Esc to cancel";
  if(state=="acquiring"||phase=="acquiring")return op["action"]=="region"?"Selecting a region… · Press Esc to cancel":op["action"]=="clipboard"?"Reading the clipboard…":op["action"]=="files"?"Copying the files…":"Selecting a capture…";
  if(phase=="uploading"){
   // Batch files upload side by side (#28): say how many run, wait, finished or stopped.
   QString files;const auto items=op["items"].toArray();
   if(items.size()>1){int running=0,waiting=0,done=0,failed=0,cancelled=0;
    for(const auto& value:items){const auto item=value.toObject();const auto status=item["uploadStatus"].toString(),itemPhase=item["phase"].toString();
     if(status=="cancelled")++cancelled;else if(status=="failed"||status=="unknown")++failed;else if(QStringList{"uploaded","skipped","conflict"}.contains(status))++done;else if(itemPhase=="queued")++waiting;else if(status=="uploading")++running;}
    QStringList parts;if(running)parts<<QString("%1 uploading").arg(running);if(waiting)parts<<QString("%1 waiting").arg(waiting);if(done)parts<<QString("%1 done").arg(done);if(failed)parts<<QString("%1 failed").arg(failed);if(cancelled)parts<<QString("%1 canceled").arg(cancelled);
    if(!parts.isEmpty())files=" · "+parts.join(", ");}
   const auto progress=aggregateProgress(op);if(progress.total>0)return QString("Uploading · %1%").arg(progress.percent())+files;if(progress.known)return "Uploading · "+ByteSize::summary(progress.sent)+" transferred"+files;return "Uploading…"+files;}
  if(phase=="preflight")return "Checking the destination…";
  if(phase=="saving_local")return "Saving locally…";
  return "Preparing capture…";
 }
 auto message=r["error"].toObject()["message"].toString();if(message.isEmpty())message=op["error"].toObject()["message"].toString();
 const auto remote=remoteStateText(r);if(!remote.isEmpty())return remote;
 if(r["uploadStatus"]=="local_only"&&localCopyUnavailable(r)&&!localSaveFailed(r))return "Saved previously · Local file unavailable";
 if(r["uploadStatus"]=="local_only")return localSaveFailed(r)?QString("Local save failed"):op["copyStatus"]=="copied"?QString("Saved locally · Copied to clipboard"):op["copyStatus"]=="suppressed"?QString("Saved locally · Not copied (clipboard changed). Copy it again."):QString("Saved locally");
 if(localSaveFailed(r)&&r["uploadStatus"]!="failed")return "Local save failed";
 if(r["uploadStatus"]=="failed"||state=="failed"||state=="partial"){
  message.replace(" Try again.","").replace(" Try again","");message=message.trimmed();while(!message.isEmpty()&&QStringLiteral(".!?…").contains(message.back()))message.chop(1);
  auto text=failureLabel(r);if(!message.isEmpty()&&message!=text)text+=" · "+message;if(r["local"].toObject()["status"]=="saved")text+=localCopyUnavailable(r)?" · Saved previously · Local file unavailable":" · Saved locally";return text;
 }
 if(state=="cancelled"||op["cancelled"].toBool()||r["uploadStatus"]=="cancelled")return "Canceled";
 if(r["uploadStatus"]=="uploaded"&&op["copyStatus"]=="failed")return "Uploaded · Couldn't copy link. Copy it again.";
 // The automatic copy was refused because the clipboard moved on (#473); Copy link is the way back.
 if(r["uploadStatus"]=="uploaded"&&op["copyStatus"]=="suppressed")return "Uploaded · Link not copied (clipboard changed). Copy it again.";
 if(r["uploadStatus"]=="uploaded")return op["copyStatus"]=="copied"?"Uploaded · Shared link":"Uploaded";
 if(r["local"].toObject()["status"]=="saved"||r["uploadStatus"]=="local_only")return op["copyStatus"]=="copied"?"Saved locally · Copied to clipboard":op["copyStatus"]=="suppressed"?"Saved locally · Not copied (clipboard changed). Copy it again.":"Saved locally";
 return "Capture complete";
}
QVariant HistoryModel::data(const QModelIndex& i,int role)const{
 if(!i.isValid()||i.row()<0||i.row()>=rows_.size())return {};
 const auto r=record(i.row());const auto op=r["operation"].toObject();
 if(role==RecordRole)return r;
 if(role==Qt::DisplayRole){const auto name=r["name"].toString();return r["mime"].toString().startsWith("image/")?QFileInfo(name).completeBaseName():name;}
 if(role==Qt::AccessibleTextRole)return r["name"].toString()+". "+statusText(r);
 if(role==Qt::ToolTipRole)return r["name"].toString()+"\n"+statusText(r);
 if(role==DayRole){if(!daySections())return QString{};const auto current=section(i.row());return i.row()==0||current!=section(i.row()-1)?current:QString{};}
 if(role==SectionRole)return daySections()?section(i.row()):QString{};
 if(role==DayCountRole){if(!daySections()||facts_.at(i.row()).active)return QString{};const auto date=createdAt_.at(i.row()).date();return dayCounts_.contains(date)?itemCount(dayCounts_.value(date)):QString{};}
 if(role==CreatedTipRole)return longTimestamp(createdAt_.at(i.row()));
 if(role==ActiveRole)return facts_.at(i.row()).active;
 if(role==FailedRole)return facts_.at(i.row()).failed;
 if(role==PrimaryActionRole)return facts_.at(i.row()).primary;
 if(role==ProgressRole)return aggregateProgress(op).percent();
 if(role==CreatedRole)return timeColumn(createdAt_.at(i.row()),!daySections());
 if(role==OutputRole)return destination(i.row());
 // The detail line: kind · size · destination · state. A failure names where
 // the upload was going, then its stored error text; live work keeps its
 // phase words. The time is a column of its own (CreatedRole).
 if(role==SubtitleRole){
  const auto files=r["records"].toArray();const auto where=destination(i.row());
  if(facts_.at(i.row()).failed){auto text=statusText(r);const auto label=failureLabel(r);if(label=="Upload failed"&&!where.isEmpty()&&where!="This device"&&text.startsWith(label))text.replace(0,label.size(),"Upload to "+where+" failed");return text;}
  if(files.isEmpty())return statusText(r);
  QStringList parts{friendlyAction(op["action"].toString(r["category"].toString())),fileSize(r["bytes"].toInteger())};if(files.size()>1)parts.append(QString("%1 files").arg(files.size()));if(!where.isEmpty())parts.append(where);
  if(active(r)||op["state"]=="cancelled"||(r["uploadStatus"]=="local_only"&&localCopyUnavailable(r))){parts.append(statusText(r));return parts.join(" · ");}
  if(op["action"]=="scroll"&&!op["note"].toString().isEmpty())parts.append(op["note"].toString());
  const auto remote=remoteStateText(r);if(!remote.isEmpty())parts.append(remote);
  else if(op["copyStatus"]=="copied")parts.append(r["uploadStatus"]=="local_only"?"Copied to clipboard":"Shared link");else if(op["copyStatus"]=="failed")parts.append(r["uploadStatus"]=="local_only"?"Couldn't copy result":"Couldn't copy link");else if(op["copyStatus"]=="suppressed")parts.append(r["uploadStatus"]=="local_only"?"Result not copied":"Link not copied");
  return parts.join(" · ");
 }
 if(role==ThumbnailRole){const auto path=r["previewPath"].toString();if(path.isEmpty())return {};if(auto* img=thumbnails_.object(path))return *img;loadThumbnail(path);}
 return {};
}
void HistoryModel::setQuery(const UiHistoryQuery& query){query_=query;query_.limit=qBound(1,query.limit,200);query_.offset=qMax(0,query.offset);++request_;if(!queryBusy_){queryBusy_=true;emit busyChanged(true);}beginResetModel();rows_.clear();createdAt_.clear();facts_.clear();dayLabelsFor_={};multipleDestinations_=false;records_={};total_=0;dayCounts_.clear();continued_={};countsKey_.clear();++countsRequest_;endResetModel();refresh();}
void HistoryModel::setShowCancelled(bool show){if(showCancelled_==show)return;showCancelled_=show;if(!queryBusy_)rebuild();}
void HistoryModel::refresh(){
 if(loadInFlight_){loadPending_=true;return;}const auto seq=request_;loadInFlight_=true;
 // Read the operation snapshot first: its ledger rows must already exist when
 // the following history snapshot runs. Reading in the opposite order could
 // briefly erase an acquiring row as its new operation became terminal.
 history_.request("operation.recent",{},this,[g=QPointer<HistoryModel>(this),seq](Error operationError,QJsonObject recent){
  if(!g)return;
  if(!operationError){g->operations_=recent["operations"].toArray();emit g->activityChanged();}
  const auto query=g->query_;
  g->history_.request("history.list",{{"limit",query.limit},{"offset",query.offset},{"search",query.search},{"category",query.category},{"mime",query.mime},{"profileId",query.profileId},{"uploadStatus",query.uploadStatus},{"localStatus",query.localStatus},{"remoteStatus",query.remoteStatus},{"from",query.from},{"to",query.to}},g,[g,seq,operationError](Error e,QJsonObject data){
   if(!g)return;
   g->loadInFlight_=false;const bool again=std::exchange(g->loadPending_,false);
   if(g->request_==seq){if(g->queryBusy_){g->queryBusy_=false;emit g->busyChanged(false);}if(e)emit g->error(e.message);else{g->records_=data["records"].toArray();g->total_=data["total"].toInteger();g->rebuild();if(operationError)emit g->error("Recent captures unavailable: "+operationError.message);}}
   if(again)g->refresh();
  });
 });
}
void HistoryModel::rebuild(){
 QList<QJsonObject> groups;QHash<QString,int> grouped;
 for(const auto& v:std::as_const(records_)){const auto r=v.toObject();const auto key=r["operationId"].toString(r["id"].toString());if(grouped.contains(key)){auto& group=groups[grouped[key]];auto children=group["records"].toArray();children.append(r);group["records"]=children;group["bytes"]=double(group["bytes"].toInteger()+r["bytes"].toInteger());if(r["uploadStatus"]=="failed")group["error"]=r["error"],group["uploadStatus"]="failed";}else{auto group=r;group["records"]=QJsonArray{r};grouped[key]=groups.size();groups.append(group);}}
 const bool specific=!query_.category.isEmpty()||!query_.mime.isEmpty()||!query_.profileId.isEmpty()||!query_.localStatus.isEmpty()||!query_.remoteStatus.isEmpty()||!query_.from.isEmpty()||!query_.to.isEmpty();
 for(const auto& v:operations_){const auto op=v.toObject();const auto id=op["id"].toString();if(grouped.contains(id)){groups[grouped[id]]["operation"]=op;continue;}
  // A cleared history stays clear. Only a live or no-file failed/cancelled capture
  // needs an activity-only row; completed captures come from the durable ledger.
  const bool uploading=query_.uploadStatus=="uploading"&&Operation::activeState(op["state"].toString())&&op["phase"]=="uploading";
  if(query_.offset||specific||(!query_.uploadStatus.isEmpty()&&!uploading)||(!Operation::activeState(op["state"].toString())&&!op["items"].toArray().isEmpty()))continue;
  if(!Operation::activeState(op["state"].toString())&&!QStringList{"failed","cancelled"}.contains(op["state"].toString()))continue;
  QJsonObject group{{"id","operation:"+id},{"operationId",id},{"created",op["created"]},{"operation",op},{"name",friendlyAction(op["action"].toString())+(op["action"]=="files"?" upload":" capture")},{"records",QJsonArray{}}};
  if(!query_.search.isEmpty()&&!group["name"].toString().contains(query_.search,Qt::CaseInsensitive))continue;
  groups.append(group);
 }
 std::stable_sort(groups.begin(),groups.end(),[](const auto& a,const auto& b){if(active(a)!=active(b))return active(a);return a["created"].toString()>b["created"].toString();});
 QList<QJsonObject> rows;QList<QDateTime> createdAt;QList<RowFacts> facts;
 for(auto group:std::as_const(groups)){const auto op=group["operation"].toObject();const bool cancelled=op["state"]=="cancelled"||op["cancelled"].toBool()||group["uploadStatus"]=="cancelled";if(cancelled&&!showCancelled_)continue;rows.append(group);createdAt.append(created(group));facts.append(rowFacts(group));}
 // A live operation republishes up to four times a second, usually without
 // moving anything a row is laid out from. Update such rows where they are: a
 // reset repaints every visible tile and drops a press, for one row's facts.
 QList<int> changed;bool keep=rows.size()==rows_.size();
 for(int row=0;keep&&row<rows.size();++row)if(rows[row]!=rows_[row]){const auto key=[](const QJsonObject& r){return r["operationId"].toString(r["id"].toString());};keep=key(rows[row])==key(rows_[row])&&rows[row]["name"]==rows_[row]["name"]&&createdAt[row]==createdAt_[row]&&facts[row]==facts_[row];changed.append(row);}
 if(keep)for(int row:changed){const auto before=data(index(row),SubtitleRole);rows_[row]=rows[row];keep=keep&&before==data(index(row),SubtitleRole);}
 const bool destinations=keep?scanDestinations():false;
 const qint64 minute=QDateTime::currentSecsSinceEpoch()/60;const bool timesMoved=std::exchange(timesMinute_,minute)!=minute;
 if(keep&&destinations==multipleDestinations_&&(!dayLabelsFor_.isValid()||dayLabelsFor_==QDate::currentDate())){
  for(int row:changed)emit dataChanged(index(row),index(row),{RecordRole,SubtitleRole,ThumbnailRole,ProgressRole,OutputRole,Qt::ToolTipRole,Qt::AccessibleTextRole});
  // Nothing else repaints the rows that didn't change, so their ages would stop.
  if(timesMoved&&!rows_.isEmpty())emit dataChanged(index(0),index(rows_.size()-1),{CreatedRole});
  emit loaded();requestCounts();return;
 }
 // Paths queued for the old rows mustn't crowd out the ones the new rows ask for.
 for(const auto& path:std::as_const(thumbnailQueue_))thumbnailRequested_.remove(path);thumbnailQueue_.clear();
 thumbnails_.setMaxCost(thumbnailCacheCost(rows.size()));
 beginResetModel();rows_=rows;createdAt_=createdAt;facts_=facts;dayLabelsFor_={};
 multipleDestinations_=scanDestinations();endResetModel();emit loaded();requestCounts();
}
void HistoryModel::requestCounts(){
 // The page's files decide every count; live progress never does.
 QStringList key{query_.search,query_.category,query_.mime,query_.profileId,query_.uploadStatus,query_.localStatus,query_.remoteStatus,query_.from,query_.to,QString::number(query_.offset),QString::number(query_.limit),QString::number(total_),QDate::currentDate().toString(Qt::ISODate)};
 for(const auto& value:std::as_const(records_)){const auto file=value.toObject();key<<file["id"].toString()+'/'+file["uploadStatus"].toString();}
 // Totals also move when something outside this page changes (an upload into
 // another category, expiry): an operation changing state is part of the key,
 // and a count older than half a minute is asked for again on the next refresh.
 for(const auto& value:std::as_const(operations_)){const auto operation=value.toObject();key<<operation["id"].toString()+'/'+operation["state"].toString();}
 const auto joined=key.join(QChar(0x1f));if(joined==countsKey_&&countsAge_.isValid()&&countsAge_.elapsed()<30000)return;countsKey_=joined;countsAge_.start();
 QList<QDate> dates;QDateTime first;
 for(int row=0;row<rows_.size();++row){if(facts_.at(row).active)continue;const auto date=createdAt_.at(row).date();if(!first.isValid())first=createdAt_.at(row);if(date.isValid()&&!dates.contains(date))dates.append(date);}
 QJsonArray days;if(daySections())for(const auto& date:std::as_const(dates).mid(0,64))days.append(QJsonObject{{"from",QDateTime(date,QTime(0,0)).toUTC().toString(Qt::ISODateWithMs)},{"to",QDateTime(date,QTime(23,59,59,999)).toUTC().toString(Qt::ISODateWithMs)}});
 const auto query=query_;const auto seq=++countsRequest_;
 QJsonObject params{{"search",query.search},{"category",query.category},{"mime",query.mime},{"profileId",query.profileId},{"uploadStatus",query.uploadStatus},{"localStatus",query.localStatus},{"remoteStatus",query.remoteStatus},{"from",query.from},{"to",query.to},{"days",days}};
 history_.request("history.counts",params,this,[g=QPointer<HistoryModel>(this),seq,dates,query,first](Error e,QJsonObject data){
  if(!g||g->countsRequest_!=seq)return;
  if(e){g->countsKey_.clear();g->categoryTotals_={};g->dayCounts_.clear();g->continued_={};g->dayLabelsFor_={};emit g->countsChanged();return;}
  g->categoryTotals_=data["categories"].toObject();g->dayCounts_.clear();const auto counts=data["days"].toArray();for(int i=0;i<counts.size()&&i<dates.size();++i)g->dayCounts_.insert(dates[i],counts[i].toInteger());
  auto finish=[g](const QDate& continued){g->continued_=continued;g->dayLabelsFor_={};if(g->rowCount())emit g->dataChanged(g->index(0),g->index(g->rowCount()-1),{DayRole,SectionRole,DayCountRole});emit g->countsChanged();};
  // Whether the first day began on an earlier page: the stored file just before this page.
  if(query.offset<=0||!first.isValid()||!g->daySections()){finish({});return;}
  g->history_.request("history.list",{{"limit",1},{"offset",query.offset-1},{"search",query.search},{"category",query.category},{"mime",query.mime},{"profileId",query.profileId},{"uploadStatus",query.uploadStatus},{"localStatus",query.localStatus},{"remoteStatus",query.remoteStatus},{"from",query.from},{"to",query.to}},g,[g,seq,first,finish](Error e,QJsonObject page){
   if(!g||g->countsRequest_!=seq)return;const auto before=page["records"].toArray();finish(!e&&!before.isEmpty()&&created(before.first().toObject()).date()==first.date()?first.date():QDate{});
  });
 });
}
void HistoryModel::loadThumbnail(const QString& path)const{if(thumbnailRequested_.contains(path)||thumbnailQueue_.size()>=qMax(32,rowCount()))return;thumbnailRequested_.insert(path);thumbnailQueue_.append(path);nextThumbnail();}
void HistoryModel::nextThumbnail()const{
 if(thumbnailLoading_||thumbnailQueue_.isEmpty())return;
 thumbnailLoading_=true;const auto path=thumbnailQueue_.takeFirst();const auto generation=thumbnailGeneration_;auto image=std::make_shared<QImage>();
 history_.background([path,image](Error&){QFileInfo file(path);if(!file.isFile()||file.size()>8*1024*1024)return QJsonObject{};QImageReader reader(path);preparePreviewReader(reader,path);const auto s=reader.size();if(!s.isValid()||qint64(s.width())*s.height()>16000000)return QJsonObject{};reader.setScaledSize(s.scaled(360,220,Qt::KeepAspectRatio));const auto length=reader.text(durationKey);*image=reader.read();if(image->depth()>32)*image=image->convertToFormat(QImage::Format_ARGB32);if(!length.isEmpty())image->setText(durationKey,length);return QJsonObject{};},const_cast<HistoryModel*>(this),[g=QPointer<HistoryModel>(const_cast<HistoryModel*>(this)),path,generation,image](Error,QJsonObject){if(!g)return;g->thumbnailLoading_=false;if(generation==g->thumbnailGeneration_){g->thumbnailRequested_.remove(path);g->thumbnails_.insert(path,new QImage(*image),qMax(1,int(image->sizeInBytes()/1024)));for(int row=0;row<g->rowCount();++row)if(g->record(row)["previewPath"]==path)emit g->dataChanged(g->index(row),g->index(row),{ThumbnailRole});}g->nextThumbnail();});
}
void HistoryModel::reloadThumbnail(const QString& path){if(path.isEmpty())return;thumbnails_.remove(path);thumbnailRequested_.remove(path);for(int row=0;row<rowCount();++row)if(record(row)["previewPath"]==path)emit dataChanged(index(row),index(row),{ThumbnailRole});}
void HistoryModel::releaseThumbnails(){++thumbnailGeneration_;thumbnailQueue_.clear();thumbnailRequested_.clear();thumbnails_.clear();}
}
