#include "HistoryModel.h"
#include "ByteSize.h"
#include "core/Operation.h"
#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QPointer>
#include <algorithm>
#include <utility>
#include <tuple>
namespace putgd {
namespace {
QDateTime created(const QJsonObject& r){auto d=QDateTime::fromString(r["created"].toString(),Qt::ISODateWithMs);if(!d.isValid())d=QDateTime::fromString(r["created"].toString(),Qt::ISODate);return d.toLocalTime();}
QString relative(const QJsonObject& r){const auto d=created(r);if(!d.isValid())return {};const auto s=d.secsTo(QDateTime::currentDateTime());if(s<60)return "Just now";if(s<3600)return QString("%1 min ago").arg(s/60);if(d.date()==QDate::currentDate())return d.toString("HH:mm");if(d.date()==QDate::currentDate().addDays(-1))return "Yesterday";return d.toString("MMM d, yyyy");}
QString day(const QJsonObject& r){const auto date=created(r).date();if(date==QDate::currentDate())return "TODAY";if(date==QDate::currentDate().addDays(-1))return "YESTERDAY";return date.isValid()?date.toString("MMMM d, yyyy").toUpper():QString("EARLIER");}
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
QString HistoryModel::relativeTime(const QJsonObject& r){return relative(r);}
QString HistoryModel::fileSize(qint64 bytes){if(bytes<1024)return QString("%1 B").arg(bytes);if(bytes<1024*1024)return QString("%1 KiB").arg(QString::number(double(bytes)/1024,'f',bytes<10*1024?1:0));return ByteSize::summary(bytes);}
QString HistoryModel::duration(qint64 ms){if(ms<=0)return {};const qint64 s=(ms+999)/1000;return s>=3600?QString("%1:%2:%3").arg(s/3600).arg(s/60%60,2,10,QChar('0')).arg(s%60,2,10,QChar('0')):QString("%1:%2").arg(s/60).arg(s%60,2,10,QChar('0'));}
HistoryModel::HistoryModel(UiSession& history,QObject* parent):QAbstractListModel(parent),history_(history){}
int HistoryModel::rowCount(const QModelIndex& parent)const{return parent.isValid()?0:rows_.size();}
QJsonObject HistoryModel::record(int row)const{return row>=0&&row<rows_.size()?rows_.at(row):QJsonObject{};}
QJsonArray HistoryModel::activeOperations()const{QJsonArray result;for(const auto& v:operations_)if(Operation::activeState(v.toObject()["state"].toString()))result.append(v);return result;}
int HistoryModel::operationProgress(const QJsonObject& operation){return aggregateProgress(operation).percent();}
bool HistoryModel::linkAvailable(int row)const{return record(row)["linkAvailable"].toBool();}
bool HistoryModel::multipleDestinations()const{
 QSet<QString> destinations;
 for(const auto& row:rows_){auto files=row["records"].toArray();if(files.isEmpty())files.append(row);for(const auto& value:files){const auto file=value.toObject(),destination=file["receipt"].toObject()["destination"].toObject();auto id=file["profileId"].toString();if(id.isEmpty())id=destination["profileId"].toString();if(id.isEmpty())id=row["operation"].toObject()["destination"].toObject()["profileId"].toString();id=file["uploadStatus"]=="local_only"?QString("local:"):!id.isEmpty()?"profile:"+id:"receipt:"+QString::fromUtf8(QJsonDocument(destination).toJson(QJsonDocument::Compact));destinations.insert(id);if(destinations.size()>1)return true;}}
 return false;
}
QString HistoryModel::commonDestination()const{return rows_.isEmpty()||multipleDestinations()?QString{}:data(index(0),OutputRole).toString();}
QString HistoryModel::friendlyAction(const QString& action){if(action=="region")return "Region";if(action=="window")return "Window";if(action=="desktop")return "Full desktop";if(action=="clipboard")return "Clipboard";if(action=="edit")return "Region & edit";if(action=="record")return "Recording";if(action=="images"||action=="image")return "Image";if(action=="text")return "Text";if(action=="videos"||action=="video")return "Video";return "File";}
bool HistoryModel::storedNothing(const QJsonObject& r){return r["records"].toArray().isEmpty()&&r["operation"].toObject()["items"].toArray().isEmpty();}
QString HistoryModel::failureLabel(const QJsonObject& r){
 if(localSaveFailed(r)&&r["uploadStatus"]!="failed")return "Local save failed";
 if(!storedNothing(r)||r["operation"].toObject().isEmpty())return "Upload failed";
 const auto action=r["operation"].toObject()["action"].toString();
 return action=="edit"?"Edit failed":action=="clipboard"?"Upload failed":"Capture failed";
}
bool HistoryModel::failed(const QJsonObject& r){if(r["uploadStatus"]=="local_only")return localSaveFailed(r);const auto state=r["operation"].toObject().value("state");return localSaveFailed(r)||r["uploadStatus"]=="failed"||state=="failed"||state=="partial";}
bool HistoryModel::retryUpload(const QJsonObject& r){return (r["uploadStatus"]=="failed"||r["operation"].toObject()["state"]=="failed"||r["operation"].toObject()["state"]=="partial")&&!active(r)&&r["uploadStatus"]!="local_only"&&r["uploadStatus"]!="uploaded"&&!r["operationId"].toString().isEmpty()&&!storedNothing(r);}
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
 for(const auto& [key,state,label]:QList<std::tuple<QString,QString,QString>>{
  {"remoteStatus","failed","Deletion failed"},{"remoteStatus","unknown","Remote outcome needs inspection"},{"remoteStatus","changed","Remote file changed"},
  {"remoteStatus","deleting","Deleting from destination"},{"remoteStatus","pending","Waiting for deletion"},
  {"remoteStatus","removed","Deleted from destination"},{"remoteStatus","already_missing","Deleted from destination"},
  {"shareStatus","failed","Sharing failed"},{"shareStatus","unknown","Sharing outcome needs inspection"},{"shareStatus","changed","Share link changed"},
  {"shareStatus","pending","Creating share link"},{"shareStatus","revoked","Link revoked"},
  {"remoteStatus","hidden_version_retained","Hidden; original version remains"},{"remoteStatus","version_retained","Original version retained"},
  {"remoteStatus","unsupported_safe_removal","Safe removal unavailable"},{"remoteStatus","superseded","Replaced by a newer upload"}})
  for(const auto& file:files)if(file.toObject()[key]==state)return label;
 return {};
}
bool HistoryModel::localCopyUnavailable(const QJsonObject& r){return (r["local"].toObject()["status"]=="saved"||r["uploadStatus"]=="local_only")&&!QFileInfo(r["local"].toObject()["path"].toString()).isFile();}
QString HistoryModel::statusText(const QJsonObject& r){
 const auto op=r["operation"].toObject();const auto state=op["state"].toString();const auto phase=op["phase"].toString();
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
  if(phase=="queued")return "Waiting to upload…";
  if(state=="editing"||phase=="editing")return "Editing · Upload or cancel in the editor window";
  if(op["action"]=="edit"&&(state=="acquiring"||phase=="acquiring"))return "Choose what to edit… · Press Esc to cancel";
  if(state=="acquiring"||phase=="acquiring")return op["action"]=="region"?"Selecting a region… · Press Esc to cancel":op["action"]=="clipboard"?"Reading the clipboard…":"Selecting a capture…";
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
 if(role==DayRole){const auto currentDay=day(r);return i.row()==0||currentDay!=day(record(i.row()-1))?currentDay:QString{};}
 if(role==ActiveRole)return active(r);
 if(role==FailedRole)return failed(r);
 if(role==PrimaryActionRole){if(active(r))return QString{};if(r["remoteStatus"]=="failed"&&!r["deleteOperationId"].toString().isEmpty())return "Retry deletion";if(copyLocalResult(r))return "Copy result";if(retryUpload(r))return "Retry upload";if(retryLocalSave(r))return "Retry local save";if(linkAvailable(i.row()))return "Copy link";return QString{};}
 if(role==ProgressRole)return aggregateProgress(op).percent();
 if(role==CreatedRole)return relative(r);
 if(role==OutputRole){
  // Where the result went: this device for a local-only result, else the
  // destination's current name (or its provider when it was since removed).
  if(r["uploadStatus"]=="local_only")return QStringLiteral("This device");
  const auto profileId=r["profileId"].toString(op["destination"].toObject()["profileId"].toString());
  for(const auto& value:history_.document()["profiles"].toArray())if(value.toObject()["id"]==profileId&&!profileId.isEmpty())return value.toObject()["label"].toString();
  const auto provider=r["receipt"].toObject()["destination"].toObject()["providerId"].toString();
  if(provider=="proton-drive")return QStringLiteral("Proton Drive");if(provider=="backblaze-b2")return QStringLiteral("Backblaze B2");if(provider=="s3")return QStringLiteral("S3 destination");
  return r["local"].toObject()["status"]=="saved"?QStringLiteral("This device"):QString{};
 }
 if(role==SubtitleRole){if(data(i,FailedRole).toBool()||active(r)||op["state"]=="cancelled"||(r["uploadStatus"]=="local_only"&&localCopyUnavailable(r)))return statusText(r);const auto remote=remoteStateText(r);auto text=(remote.isEmpty()?QString{}:remote+" · ")+friendlyAction(op["action"].toString(r["category"].toString()))+" · "+fileSize(r["bytes"].toInteger());const auto time=relative(r);if(!time.isEmpty())text+=" · "+time;if(remote.isEmpty()){if(op["copyStatus"]=="copied")text+=r["uploadStatus"]=="local_only"?" · Copied to clipboard":" · Shared link";else if(op["copyStatus"]=="failed")text+=r["uploadStatus"]=="local_only"?" · Couldn't copy result":" · Couldn't copy link";else if(op["copyStatus"]=="suppressed")text+=r["uploadStatus"]=="local_only"?" · Result not copied":" · Link not copied";}if(r["records"].toArray().size()>1)text+=QString(" · %1 files").arg(r["records"].toArray().size());return text;}
 if(role==ThumbnailRole){const auto path=r["previewPath"].toString();if(path.isEmpty())return {};if(auto* img=thumbnails_.object(path))return *img;loadThumbnail(path);}
 return {};
}
void HistoryModel::setQuery(const UiHistoryQuery& query){query_=query;query_.limit=qBound(1,query.limit,200);query_.offset=qMax(0,query.offset);++request_;if(!queryBusy_){queryBusy_=true;emit busyChanged(true);}beginResetModel();rows_.clear();records_={};total_=0;endResetModel();refresh();}
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
 for(const auto& v:records_){const auto r=v.toObject();const auto key=r["operationId"].toString(r["id"].toString());if(grouped.contains(key)){auto& group=groups[grouped[key]];auto children=group["records"].toArray();children.append(r);group["records"]=children;group["bytes"]=double(group["bytes"].toInteger()+r["bytes"].toInteger());if(r["uploadStatus"]=="failed")group["error"]=r["error"],group["uploadStatus"]="failed";}else{auto group=r;group["records"]=QJsonArray{r};grouped[key]=groups.size();groups.append(group);}}
 const bool specific=!query_.category.isEmpty()||!query_.mime.isEmpty()||!query_.profileId.isEmpty()||!query_.localStatus.isEmpty()||!query_.remoteStatus.isEmpty()||!query_.from.isEmpty()||!query_.to.isEmpty();
 for(const auto& v:operations_){const auto op=v.toObject();const auto id=op["id"].toString();if(grouped.contains(id)){groups[grouped[id]]["operation"]=op;continue;}
  // A cleared history stays clear. Only a live or no-file failed/cancelled capture
  // needs an activity-only row; completed captures come from the durable ledger.
  const bool uploading=query_.uploadStatus=="uploading"&&Operation::activeState(op["state"].toString())&&op["phase"]=="uploading";
  if(query_.offset||specific||(!query_.uploadStatus.isEmpty()&&!uploading)||(!Operation::activeState(op["state"].toString())&&!op["items"].toArray().isEmpty()))continue;
  if(!Operation::activeState(op["state"].toString())&&!QStringList{"failed","cancelled"}.contains(op["state"].toString()))continue;
  QJsonObject group{{"id","operation:"+id},{"operationId",id},{"created",op["created"]},{"operation",op},{"name",friendlyAction(op["action"].toString())+" capture"},{"records",QJsonArray{}}};
  if(!query_.search.isEmpty()&&!group["name"].toString().contains(query_.search,Qt::CaseInsensitive))continue;
  groups.append(group);
 }
 std::stable_sort(groups.begin(),groups.end(),[](const auto& a,const auto& b){if(active(a)!=active(b))return active(a);return a["created"].toString()>b["created"].toString();});
 beginResetModel();rows_.clear();hiddenCancelled_=0;
 for(auto group:groups){const auto op=group["operation"].toObject();const bool cancelled=op["state"]=="cancelled"||op["cancelled"].toBool()||group["uploadStatus"]=="cancelled";if(cancelled&&!showCancelled_){++hiddenCancelled_;continue;}rows_.append(group);}
 endResetModel();emit loaded();
}
void HistoryModel::loadThumbnail(const QString& path)const{if(thumbnailRequested_.contains(path)||thumbnailQueue_.size()>=32)return;thumbnailRequested_.insert(path);thumbnailQueue_.append(path);nextThumbnail();}
void HistoryModel::nextThumbnail()const{
 if(thumbnailLoading_||thumbnailQueue_.isEmpty())return;
 thumbnailLoading_=true;const auto path=thumbnailQueue_.takeFirst();const auto generation=thumbnailGeneration_;auto image=std::make_shared<QImage>();
 history_.background([path,image](Error&){QFileInfo file(path);if(!file.isFile()||file.size()>8*1024*1024)return QJsonObject{};QImageReader reader(path);const auto s=reader.size();if(!s.isValid()||qint64(s.width())*s.height()>16000000)return QJsonObject{};reader.setScaledSize(s.scaled(360,220,Qt::KeepAspectRatio));const auto length=reader.text(durationKey);*image=reader.read();if(!length.isEmpty())image->setText(durationKey,length);return QJsonObject{};},const_cast<HistoryModel*>(this),[g=QPointer<HistoryModel>(const_cast<HistoryModel*>(this)),path,generation,image](Error,QJsonObject){if(!g)return;g->thumbnailLoading_=false;if(generation==g->thumbnailGeneration_){g->thumbnailRequested_.remove(path);g->thumbnails_.insert(path,new QImage(*image),qMax(1,int(image->sizeInBytes()/1024)));for(int row=0;row<g->rowCount();++row)if(g->record(row)["previewPath"]==path)emit g->dataChanged(g->index(row),g->index(row),{ThumbnailRole});}g->nextThumbnail();});
}
void HistoryModel::reloadThumbnail(const QString& path){if(path.isEmpty())return;thumbnails_.remove(path);thumbnailRequested_.remove(path);for(int row=0;row<rowCount();++row)if(record(row)["previewPath"]==path)emit dataChanged(index(row),index(row),{ThumbnailRole});}
void HistoryModel::releaseThumbnails(){++thumbnailGeneration_;thumbnailQueue_.clear();thumbnailRequested_.clear();thumbnails_.clear();}
}
