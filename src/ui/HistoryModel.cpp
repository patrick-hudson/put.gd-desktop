#include "HistoryModel.h"
#include "ByteSize.h"
#include "core/Operation.h"
#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QPointer>
#include <algorithm>
#include <utility>
namespace putgd {
namespace {
QDateTime created(const QJsonObject& r){auto d=QDateTime::fromString(r["created"].toString(),Qt::ISODateWithMs);if(!d.isValid())d=QDateTime::fromString(r["created"].toString(),Qt::ISODate);return d.toLocalTime();}
QString relativeTime(const QJsonObject& r){const auto d=created(r);if(!d.isValid())return {};const auto s=d.secsTo(QDateTime::currentDateTime());if(s<60)return "Just now";if(s<3600)return QString("%1 min ago").arg(s/60);if(d.date()==QDate::currentDate())return d.toString("HH:mm");if(d.date()==QDate::currentDate().addDays(-1))return "Yesterday";return d.toString("MMM d, yyyy");}
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
QString HistoryModel::fileSize(qint64 bytes){if(bytes<1024)return QString("%1 B").arg(bytes);if(bytes<1024*1024)return QString("%1 KiB").arg(QString::number(double(bytes)/1024,'f',bytes<10*1024?1:0));return ByteSize::summary(bytes);}
QString HistoryModel::duration(qint64 ms){if(ms<=0)return {};const qint64 s=(ms+999)/1000;return s>=3600?QString("%1:%2:%3").arg(s/3600).arg(s/60%60,2,10,QChar('0')).arg(s%60,2,10,QChar('0')):QString("%1:%2").arg(s/60).arg(s%60,2,10,QChar('0'));}
HistoryModel::HistoryModel(UiSession& history,QObject* parent):QAbstractListModel(parent),history_(history){}
int HistoryModel::rowCount(const QModelIndex& parent)const{return parent.isValid()?0:rows_.size();}
QJsonObject HistoryModel::record(int row)const{return row>=0&&row<rows_.size()?rows_.at(row):QJsonObject{};}
bool HistoryModel::linkAvailable(int row)const{return record(row)["linkAvailable"].toBool();}
QString HistoryModel::friendlyAction(const QString& action){if(action=="region")return "Region";if(action=="window")return "Window";if(action=="desktop")return "Full desktop";if(action=="clipboard")return "Clipboard";if(action=="edit")return "Region & edit";if(action=="record")return "Recording";if(action=="images"||action=="image")return "Image";if(action=="text")return "Text";if(action=="videos"||action=="video")return "Video";return "File";}
bool HistoryModel::storedNothing(const QJsonObject& r){return r["records"].toArray().isEmpty()&&r["operation"].toObject()["items"].toArray().isEmpty();}
QString HistoryModel::failureLabel(const QJsonObject& r){
 if(r["uploadStatus"]=="local_only")return "Local save failed";
 if(!storedNothing(r))return "Upload failed";
 const auto action=r["operation"].toObject()["action"].toString();
 return action=="edit"?"Edit failed":action=="clipboard"?"Import failed":"Capture failed";
}
QString HistoryModel::statusText(const QJsonObject& r){
 const auto op=r["operation"].toObject();const auto state=op["state"].toString();const auto phase=op["phase"].toString();
 if(active(r)){
  if(state=="editing"||phase=="editing")return "Editing · Upload or cancel in the editor window";
  if(op["action"]=="edit"&&(state=="acquiring"||phase=="acquiring"))return "Choose what to edit… · Press Esc to cancel";
  if(state=="acquiring"||phase=="acquiring")return op["action"]=="region"?"Selecting a region… · Press Esc to cancel":op["action"]=="clipboard"?"Reading the clipboard…":"Selecting a capture…";
  if(phase=="uploading"){
   // Batch files upload side by side (#28): say how many run, wait, finished or stopped.
   QString files;const auto items=op["items"].toArray();
   if(items.size()>1){int running=0,waiting=0,done=0,failed=0,cancelled=0;
    for(const auto& value:items){const auto item=value.toObject();const auto status=item["uploadStatus"].toString(),itemPhase=item["phase"].toString();
     if(status=="cancelled")++cancelled;else if(status=="failed"||status=="unknown")++failed;else if(QStringList{"uploaded","skipped","conflict"}.contains(status))++done;else if(itemPhase=="queued")++waiting;else if(status=="uploading")++running;}
    QStringList parts;if(running)parts<<QString("%1 uploading").arg(running);if(waiting)parts<<QString("%1 waiting").arg(waiting);if(done)parts<<QString("%1 done").arg(done);if(failed)parts<<QString("%1 failed").arg(failed);if(cancelled)parts<<QString("%1 cancelled").arg(cancelled);
    if(!parts.isEmpty())files=" · "+parts.join(", ");}
   const auto progress=aggregateProgress(op);if(progress.total>0)return QString("Uploading · %1%").arg(progress.percent())+files;if(progress.known)return "Uploading · "+ByteSize::summary(progress.sent)+" transferred"+files;return "Uploading…"+files;}
  if(phase=="saving_local")return "Saving locally…";
  if(phase=="queued")return "Waiting to upload…";
  return "Preparing capture…";
 }
 auto message=r["error"].toObject()["message"].toString();if(message.isEmpty())message=op["error"].toObject()["message"].toString();
 if(r["uploadStatus"]=="local_only")return localSaveFailed(r)?QString("Local save failed"):op["copyStatus"]=="copied"?QString("Saved locally · Copied to clipboard"):QString("Saved locally");
 if(r["uploadStatus"]=="uploaded"&&QStringList{"failed","unknown","changed"}.contains(r["shareStatus"].toString()))return message.isEmpty()?QString("Share link needs attention"):message;
 if(localSaveFailed(r)&&r["uploadStatus"]!="failed")return "Local save failed";
 if(r["uploadStatus"]=="failed"||state=="failed"||state=="partial")return (message.isEmpty()?failureLabel(r):message)+(r["local"].toObject()["status"]=="saved"?" · Saved locally":QString{});
 if(state=="cancelled"||op["cancelled"].toBool()||r["uploadStatus"]=="cancelled")return "Cancelled";
 if(r["remoteStatus"]=="removed"||r["remoteStatus"]=="already_missing")return "Deleted from destination";
 if(r["remoteStatus"]=="changed"||r["remoteStatus"]=="unknown")return "Remote outcome needs inspection";
 if(r["shareStatus"]=="revoked")return "Share link revoked";
 if(r["uploadStatus"]=="uploaded"&&op["copyStatus"]=="failed")return "Uploaded · Could not copy link. Copy it again.";
 if(r["uploadStatus"]=="uploaded")return op["copyStatus"]=="copied"?"Uploaded · Link copied":"Uploaded";
 if(r["local"].toObject()["status"]=="saved"||r["uploadStatus"]=="local_only")return op["copyStatus"]=="copied"?"Saved locally · Copied to clipboard":"Saved locally";
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
 if(role==FailedRole){if(r["uploadStatus"]=="local_only")return localSaveFailed(r);return r["uploadStatus"]=="failed"||op["state"]=="failed"||op["state"]=="partial";}
 if(role==PrimaryActionRole){if(active(r))return "Cancel";if(r["uploadStatus"]=="local_only"){if(localSaveFailed(r))return "Retry local save";if(r["local"].toObject()["status"]=="saved")return "Copy result";}if(data(i,FailedRole).toBool()&&storedNothing(r))return QString{};if(data(i,FailedRole).toBool()&&!r["operationId"].toString().isEmpty())return "Retry upload";if(linkAvailable(i.row()))return "Copy link";return QString{};}
 if(role==ProgressRole)return aggregateProgress(op).percent();
 if(role==SubtitleRole){if(data(i,FailedRole).toBool()||active(r)||op["state"]=="cancelled")return statusText(r);auto text=friendlyAction(op["action"].toString(r["category"].toString()))+" · "+fileSize(r["bytes"].toInteger());const auto time=relativeTime(r);if(!time.isEmpty())text+=" · "+time;if(op["copyStatus"]=="copied")text+=r["uploadStatus"]=="local_only"?" · Copied to clipboard":" · Link copied";else if(op["copyStatus"]=="failed")text+=r["uploadStatus"]=="local_only"?" · Could not copy result":" · Could not copy link";if(r["records"].toArray().size()>1)text+=QString(" · %1 files").arg(r["records"].toArray().size());return text;}
 if(role==ThumbnailRole){const auto path=r["previewPath"].toString();if(path.isEmpty())return {};if(auto* img=thumbnails_.object(path))return *img;loadThumbnail(path);}
 return {};
}
void HistoryModel::setQuery(const UiHistoryQuery& query){query_=query;query_.limit=qBound(1,query.limit,200);query_.offset=qMax(0,query.offset);++request_;refresh();}
void HistoryModel::setShowCancelled(bool show){if(showCancelled_==show)return;showCancelled_=show;rebuild();}
void HistoryModel::refresh(){
 if(loadInFlight_){loadPending_=true;return;}const auto seq=request_;loadInFlight_=true;
 // Read the operation snapshot first: its ledger rows must already exist when
 // the following history snapshot runs. Reading in the opposite order could
 // briefly erase an acquiring row as its new operation became terminal.
 history_.request("operation.recent",{},this,[g=QPointer<HistoryModel>(this),seq](Error operationError,QJsonObject recent){
  if(!g)return;
  const auto query=g->query_;
  g->history_.request("history.list",{{"limit",query.limit},{"offset",query.offset},{"search",query.search},{"category",query.category},{"mime",query.mime},{"profileId",query.profileId},{"uploadStatus",query.uploadStatus},{"localStatus",query.localStatus},{"remoteStatus",query.remoteStatus},{"from",query.from},{"to",query.to}},g,[g,seq,recent,operationError](Error e,QJsonObject data){
   if(!g)return;
   g->loadInFlight_=false;const bool again=std::exchange(g->loadPending_,false);
   if(g->request_==seq){if(e)emit g->error(e.message);else{g->records_=data["records"].toArray();g->total_=data["total"].toInteger();g->operations_=recent["operations"].toArray();g->rebuild();if(operationError)emit g->error("Recent captures unavailable: "+operationError.message);}}
   if(again)g->refresh();
  });
 });
}
void HistoryModel::rebuild(){
 QList<QJsonObject> groups;QHash<QString,int> grouped;
 for(const auto& v:records_){const auto r=v.toObject();const auto key=r["operationId"].toString(r["id"].toString());if(grouped.contains(key)){auto& group=groups[grouped[key]];auto children=group["records"].toArray();children.append(r);group["records"]=children;group["bytes"]=double(group["bytes"].toInteger()+r["bytes"].toInteger());if(r["uploadStatus"]=="failed")group["error"]=r["error"],group["uploadStatus"]="failed";}else{auto group=r;group["records"]=QJsonArray{r};grouped[key]=groups.size();groups.append(group);}}
 const bool specific=!query_.category.isEmpty()||!query_.mime.isEmpty()||!query_.profileId.isEmpty()||!query_.uploadStatus.isEmpty()||!query_.localStatus.isEmpty()||!query_.remoteStatus.isEmpty()||!query_.from.isEmpty()||!query_.to.isEmpty();
 for(const auto& v:operations_){const auto op=v.toObject();const auto id=op["id"].toString();if(grouped.contains(id)){groups[grouped[id]]["operation"]=op;continue;}
  // A cleared history stays clear. Only a live or no-file failed/cancelled capture
  // needs an activity-only row; completed captures come from the durable ledger.
  if(query_.offset||specific||(!Operation::activeState(op["state"].toString())&&!op["items"].toArray().isEmpty()))continue;
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
void HistoryModel::releaseThumbnails(){++thumbnailGeneration_;thumbnailQueue_.clear();thumbnailRequested_.clear();thumbnails_.clear();}
}
