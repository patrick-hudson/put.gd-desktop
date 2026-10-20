#include "HistoryStore.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>
#include <QCryptographicHash>
namespace putgd {
namespace {
Error sqlError() {return {"history_io","Persistent journal could not be read or committed",{}};}
QByteArray encode(const QJsonObject& o) {return QJsonDocument(o).toJson(QJsonDocument::Compact);}
QJsonObject decode(const QVariant& v) {return QJsonDocument::fromJson(v.toByteArray()).object();}
QString publicationIdentity(const StorageReceipt& receipt,const ProviderRegistry& providers) {
 if(receipt.validate())return {};
 const auto provider=providers.find(receipt.destination.providerId);if(!provider)return {};
 const auto key=provider->serializationKey(receipt.destination,receipt.objectReference,{});if(key.isEmpty())return {};
 return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonArray{receipt.destination.providerId,key}).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
QJsonObject publicRecord(QJsonObject data) {
 if(data.contains("shareReceipt"))data["receipt"]=data.take("shareReceipt");
 // A link past its confirmed expiry no longer opens, so it is not offered for copying (#253).
 const auto expires=QDateTime::fromString(data.value("shareExpiresAt").toString(),Qt::ISODateWithMs);
 data["linkAvailable"]=OperationItem::fromJson(data).linkAvailable()&&!(expires.isValid()&&expires<=QDateTime::currentDateTimeUtc());
 return data;
}
// The journal's own layout: <root>/artifacts/<operation>/<item> and
// <root>/artifacts/../previews/<item>.png. Any other path is left as stored.
QString relocated(const QString& path,const QString& expected,const QString& tail) {
 return !path.endsWith(tail)||QDir::cleanPath(path)==QDir::cleanPath(expected)?path:expected;
}
QString artifactPath(const QString& root,const QString& operation,const QString& item) {return root+"/artifacts/"+operation+"/"+item;}
QString previewPath(const QString& root,const QString& item) {return root+"/artifacts/../previews/"+item+".png";}
QJsonObject remoteFields(const QJsonObject& data) {
 QJsonObject remote;
 for(const auto& key:QStringList{"remoteStatus","remoteError","deleteOperationId","deleteProgress","supersededBy","shareStatus","shareIntent","shareIntentOptions","shareReceipt","shareOperationId","shareProgress","shareExpiresAt"})
  if(data.contains(key))remote[key]=data[key];
 return remote;
}
void hydrateRemote(Operation& op,const QHash<QString,QJsonObject>& statuses) {
 for(auto& item:op.items)if(statuses.contains(item.artifact.id)){
  const auto state=statuses.value(item.artifact.id);item.remoteStatus=state["remoteStatus"].toString();
  if(state.contains("shareReceipt"))item.receipt=StorageReceipt::fromJson(state["shareReceipt"].toObject());
  if(state.contains("shareStatus"))item.shareStatus=state["shareStatus"].toString();
  if(state.contains("shareIntent"))item.shareIntent=state["shareIntent"].toString();
  if(state.contains("shareIntentOptions"))item.shareIntentOptions=state["shareIntentOptions"].toObject();
 }
}
Error readRemoteStates(QSqlDatabase db,const QStringList& ids,QHash<QString,QJsonObject>& statuses) {
 // Bound SQL parameters to the exact selected operations; concurrent new
 // operations cannot shift the hydration set between the two reads.
 for(qsizetype offset=0;offset<ids.size();offset+=200) {
  const auto batch=ids.mid(offset,200);QStringList marks;for(qsizetype i=0;i<batch.size();++i)marks.append("?");
  QSqlQuery q(db);q.prepare("SELECT id,remote_status,json_extract(data,'$.shareStatus'),json_extract(data,'$.shareReceipt'),json_extract(data,'$.shareIntent'),json_extract(data,'$.shareIntentOptions') FROM item_state WHERE operation_id IN ("+marks.join(',')+")");for(const auto& id:batch)q.addBindValue(id);
  if(!q.exec())return sqlError();
  while(q.next()){QJsonObject state{{"remoteStatus",q.value(1).toString()}};if(!q.value(2).isNull())state["shareStatus"]=q.value(2).toString();if(!q.value(3).isNull())state["shareReceipt"]=decode(q.value(3));if(!q.value(4).isNull())state["shareIntent"]=q.value(4).toString();if(!q.value(5).isNull())state["shareIntentOptions"]=decode(q.value(5));statuses.insert(q.value(0).toString(),state);}
 }
 return {};
}
// Called inside the caller's transaction. The durable item state outlives the
// clearable history projection, including deletion and replacement outcomes.
bool persistRemote(QSqlDatabase db,const QString& id,const QJsonObject& remote) {
 QSqlQuery q(db);q.prepare("UPDATE item_state SET remote_status=?,data=? WHERE id=?");q.addBindValue(remote["remoteStatus"].toString());q.addBindValue(encode(remote));q.addBindValue(id);if(!q.exec())return false;
 // Merge only top-level ledger fields. JSON merge-patch would remove nested
 // null settings, changing an immutable destination or credential reference.
 q.prepare("SELECT data FROM history WHERE id=?");q.addBindValue(id);if(!q.exec())return false;if(!q.next())return true;
 auto data=decode(q.value(0));for(const auto& key:QStringList{"remoteStatus","remoteError","deleteOperationId","deleteProgress","supersededBy","shareStatus","shareIntent","shareIntentOptions","shareReceipt","shareOperationId","shareProgress","shareExpiresAt"})data.remove(key);
 for(auto it=remote.begin();it!=remote.end();++it)data[it.key()]=it.value();
 q.prepare("UPDATE history SET remote_status=?,data=? WHERE id=?");q.addBindValue(remote["remoteStatus"].toString());q.addBindValue(encode(data));q.addBindValue(id);return q.exec();
}
QString createHistory(const QString& table) {return "CREATE TABLE "+table+"(id TEXT PRIMARY KEY, operation_id TEXT NOT NULL,item_id TEXT NOT NULL,created TEXT NOT NULL,name TEXT NOT NULL,mime TEXT NOT NULL,category TEXT NOT NULL,profile_id TEXT NOT NULL,upload_status TEXT NOT NULL,local_status TEXT NOT NULL,remote_status TEXT NOT NULL,data BLOB NOT NULL,UNIQUE(operation_id,item_id))";}
}
QJsonObject HistoryPage::toJson() const {return {{"records",records},{"total",double(total)}};}
HistoryStore::HistoryStore(QString path):root_(QFileInfo(path).absolutePath()) {
 QDir().mkpath(QFileInfo(path).absolutePath()); QFile::setPermissions(QFileInfo(path).absolutePath(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 connection_=QUuid::createUuid().toString();db_=QSqlDatabase::addDatabase("QSQLITE",connection_);db_.setDatabaseName(path);db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=10000");
 if(!db_.open()) {error_=sqlError();return;} QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner);error_=initialize();
}
HistoryStore::~HistoryStore() {db_.close();db_=QSqlDatabase{};QSqlDatabase::removeDatabase(connection_);}
Error HistoryStore::initialize() {
 QSqlQuery q(db_);if(!q.exec("PRAGMA foreign_keys=ON")||!q.exec("PRAGMA user_version")||!q.next()) return sqlError(); const int version=q.value(0).toInt();
 if(version>7) return {"history_version","Unsupported history schema version",{}};
 if(version==7) return {};
 if(!db_.transaction()) return sqlError();
 auto exec=[&](const QString& s) {return q.exec(s);}; bool ok=true;
 if(version<4) {
 if(version==0) ok=exec(createHistory("history"));
 else if(version==1) {
  ok=exec(createHistory("history_next"));
  // Version 1 is a populated id/data ledger. SQLite streams this migration;
  // any malformed row, duplicate or constraint error rolls back DDL and records.
  if(ok) ok=exec("INSERT INTO history_next SELECT id,json_extract(data,'$.operationId'),json_extract(data,'$.itemId'),json_extract(data,'$.created'),json_extract(data,'$.name'),json_extract(data,'$.mime'),json_extract(data,'$.category'),json_extract(data,'$.profileId'),json_extract(data,'$.uploadStatus'),json_extract(data,'$.local.status'),json_extract(data,'$.remoteStatus'),data FROM history");
  if(ok) ok=exec("DROP TABLE history")&&exec("ALTER TABLE history_next RENAME TO history");
 }
 if(ok) ok=exec("CREATE TABLE IF NOT EXISTS operations(id TEXT PRIMARY KEY,data BLOB NOT NULL)")&&exec("CREATE TABLE IF NOT EXISTS deletions(id TEXT PRIMARY KEY,data BLOB NOT NULL)")&&exec("CREATE TABLE IF NOT EXISTS deletion_targets(deletion_id TEXT NOT NULL,history_id TEXT NOT NULL,active INTEGER NOT NULL,PRIMARY KEY(deletion_id,history_id))")&&exec("CREATE INDEX IF NOT EXISTS operations_created ON operations(json_extract(data,'$.created') DESC)")&&exec("CREATE INDEX IF NOT EXISTS history_category ON history(category,created DESC)")&&exec("CREATE INDEX IF NOT EXISTS history_upload ON history(upload_status,created DESC)")&&exec("CREATE INDEX IF NOT EXISTS history_local ON history(local_status,created DESC)")&&exec("CREATE INDEX IF NOT EXISTS history_created ON history(created DESC,id)")&&exec("CREATE INDEX IF NOT EXISTS history_filters ON history(profile_id,upload_status,category,created DESC)")&&exec("CREATE INDEX IF NOT EXISTS history_name ON history(name)")&&exec("CREATE INDEX IF NOT EXISTS history_mime ON history(mime,created)")&&exec("CREATE INDEX IF NOT EXISTS history_remote ON history(remote_status,local_status)")&&exec("CREATE TABLE IF NOT EXISTS cleared_history(id TEXT PRIMARY KEY)");
 if(ok)ok=exec("ALTER TABLE history ADD COLUMN publication_identity TEXT NOT NULL DEFAULT ''")&&exec("CREATE INDEX history_publication ON history(publication_identity,upload_status)")&&exec("PRAGMA user_version=4");
 }
 if(version<5 && ok)ok=exec("CREATE TABLE item_state(id TEXT PRIMARY KEY,operation_id TEXT NOT NULL,remote_status TEXT NOT NULL,data BLOB NOT NULL,publication_identity TEXT NOT NULL DEFAULT '',upload_status TEXT NOT NULL)")
  &&exec("CREATE INDEX item_state_operation ON item_state(operation_id)")
  &&exec("CREATE INDEX item_state_publication ON item_state(publication_identity,upload_status)")
  &&exec("INSERT INTO item_state SELECT id,operation_id,remote_status,json_object('remoteStatus',remote_status,'remoteError',json_extract(data,'$.remoteError'),'deleteOperationId',json_extract(data,'$.deleteOperationId'),'deleteProgress',json_extract(data,'$.deleteProgress'),'supersededBy',json_extract(data,'$.supersededBy')),publication_identity,upload_status FROM history")
  // Expand legacy deletion documents once, then use an indexed lookup for
  // each cleared item. Do not repeatedly parse every journal during startup.
  &&exec("CREATE TEMP TABLE legacy_remote_outcome AS SELECT json_extract(target.value,'$.historyId') AS id,json_extract(target.value,'$.status') AS status,deletions.rowid AS seq FROM deletions,json_each(deletions.data,'$.items') AS target WHERE json_extract(target.value,'$.status') IN ('removed','already_missing','changed','hidden_version_retained')")
  &&exec("CREATE INDEX legacy_remote_outcome_by_id ON legacy_remote_outcome(id,seq DESC)")
  // Older clear/expiry removed the projection but retained operation and
  // deletion journals. Recover known non-copyable outcomes without remote I/O.
  &&exec("INSERT INTO item_state SELECT id,operation_id,status,json_object('remoteStatus',status),'',upload_status FROM (SELECT json_extract(item.value,'$.artifact.id') AS id,operations.id AS operation_id,json_extract(item.value,'$.uploadStatus') AS upload_status,COALESCE((SELECT status FROM legacy_remote_outcome WHERE id=json_extract(item.value,'$.artifact.id') ORDER BY seq DESC LIMIT 1),json_extract(item.value,'$.remoteStatus'),'not_requested') AS status FROM operations,json_each(operations.data,'$.items') AS item JOIN cleared_history ON cleared_history.id=json_extract(item.value,'$.artifact.id') WHERE NOT EXISTS(SELECT 1 FROM item_state WHERE item_state.id=cleared_history.id))")
  &&exec("DROP TABLE legacy_remote_outcome")
  &&exec("PRAGMA user_version=5");
 if(ok)ok=exec("CREATE TABLE IF NOT EXISTS sharing_operations(id TEXT PRIMARY KEY,history_id TEXT NOT NULL,data BLOB NOT NULL)")
  &&exec("CREATE INDEX IF NOT EXISTS sharing_history ON sharing_operations(history_id)")&&exec("CREATE INDEX IF NOT EXISTS sharing_state ON sharing_operations(json_extract(data,'$.state'))")&&exec("PRAGMA user_version=7");
 if(!ok||!db_.commit()) {db_.rollback();return {"history_migration","History migration failed; original records preserved",{}};}
 return {};
}
void HistoryStore::relocate(Operation& op) const {
 for(auto& item:op.items) {
  auto& artifact=item.artifact;
  artifact.path=relocated(artifact.path,artifactPath(root_,op.id,artifact.id),"/artifacts/"+op.id+"/"+artifact.id);
  artifact.previewPath=relocated(artifact.previewPath,previewPath(root_,artifact.id),"/previews/"+artifact.id+".png");
 }
}
QJsonObject HistoryStore::relocate(QJsonObject record) const {
 // Read through value(): operator[] would add the key to records that lack it.
 const auto item=record.value("id").toString(),preview=record.value("previewPath").toString();
 if(!preview.isEmpty())record["previewPath"]=relocated(preview,previewPath(root_,item),"/previews/"+item+".png");
 return record;
}
Error HistoryStore::saveOperation(const Operation& op,const ProviderRegistry* providers) {
 return persistOperation(op,providers,nullptr);
}
Error HistoryStore::checkpointOperation(const Operation& op,const QSet<QString>& changedItems,const ProviderRegistry* providers) {
 return persistOperation(op,providers,&changedItems);
}
Error HistoryStore::persistOperation(const Operation& op,const ProviderRegistry* providers,const QSet<QString>* changedItems) {
 if(error_) return error_;
 if(!db_.transaction()) return sqlError();
 QSqlQuery q(db_);
 q.prepare("INSERT INTO operations VALUES(?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data");q.addBindValue(op.id);q.addBindValue(encode(op.toJson()));bool ok=q.exec();
 for(const auto& item:op.items) {
  if(!ok) break;
  if(changedItems&&!changedItems->contains(item.artifact.id))continue;
  QString identity;bool newlyUploaded=true;QJsonObject remote;
  q.prepare("SELECT data,publication_identity,upload_status FROM item_state WHERE id=?");q.addBindValue(item.artifact.id);if(!q.exec()){ok=false;break;}
  if(q.next()){identity=q.value(1).toString();newlyUploaded=q.value(2).toString()!="uploaded";remote=decode(q.value(0));}
  if(providers&&item.uploadStatus=="uploaded")identity=publicationIdentity(item.receipt,*providers);
  if(!remote.contains("remoteStatus"))remote["remoteStatus"]=item.remoteStatus;
  q.prepare("INSERT INTO item_state VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET remote_status=excluded.remote_status,data=excluded.data,publication_identity=excluded.publication_identity,upload_status=excluded.upload_status");
  for(const auto& value:QVariantList{item.artifact.id,op.id,remote["remoteStatus"].toString(),encode(remote),identity.isNull()?QStringLiteral(""):identity,item.uploadStatus})q.addBindValue(value);
  if(!q.exec()){ok=false;break;}
  q.prepare("SELECT 1 FROM cleared_history WHERE id=?");q.addBindValue(item.artifact.id);if(!q.exec()){ok=false;break;}const bool cleared=q.next();
  if(!cleared) {
  auto data=item.toJson();for(auto it=remote.begin();it!=remote.end();++it)data[it.key()]=it.value();
  if(remote.contains("shareReceipt"))data["receipt"]=remote["shareReceipt"];
  auto artifact=data.take("artifact").toObject();artifact.remove("path");
  for(auto it=artifact.begin();it!=artifact.end();++it) data[it.key()]=it.value();
  data["operationId"]=op.id;data["itemId"]=item.artifact.id;data["id"]=item.artifact.id;data["created"]=op.created;data["profileId"]=op.destination.profileId;
  q.prepare("INSERT INTO history(id,operation_id,item_id,created,name,mime,category,profile_id,upload_status,local_status,remote_status,data,publication_identity) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET upload_status=excluded.upload_status,local_status=excluded.local_status,remote_status=excluded.remote_status,data=excluded.data,publication_identity=excluded.publication_identity");
  for(const auto& value:QVariantList{item.artifact.id,op.id,item.artifact.id,op.created,item.artifact.name,item.artifact.mime,item.artifact.category,op.destination.profileId,item.uploadStatus,item.local.status,remote["remoteStatus"].toString(),encode(data),identity}) q.addBindValue(value.metaType().id()==QMetaType::QString&&value.toString().isNull()?QVariant(QStringLiteral("")):value);
  ok=q.exec();
  }
  if(ok&&newlyUploaded&&item.uploadStatus=="uploaded"&&item.condition=="match"&&!identity.isEmpty()) {
   QSqlQuery older(db_);older.prepare("SELECT id,data FROM item_state WHERE publication_identity=? AND id<>? AND upload_status='uploaded' AND remote_status NOT IN ('removed','already_missing','changed','hidden_version_retained','superseded')");older.addBindValue(identity);older.addBindValue(item.artifact.id);ok=older.exec();
   while(ok&&older.next()) {auto previous=decode(older.value(1));previous["remoteStatus"]="superseded";previous["supersededBy"]=item.artifact.id;ok=persistRemote(db_,older.value(0).toString(),previous);}
  }

 }
 if(!ok||!db_.commit()) {db_.rollback();return sqlError();}return {};
}
Error HistoryStore::indexPublications(const ProviderRegistry& providers) {
 if(error_)return error_;
 // Stream bounded pages; unknown/unavailable providers remain unindexed until restored.
 QString after=QStringLiteral("");
 for(;;) {
  QSqlQuery rows(db_);rows.prepare("SELECT id,data FROM history WHERE id>? AND publication_identity='' AND upload_status='uploaded' ORDER BY id LIMIT 200");rows.addBindValue(after);if(!rows.exec())return sqlError();
  QList<QPair<QString,QString>> identities;int count=0;
  while(rows.next()) {++count;after=rows.value(0).toString();const auto receipt=StorageReceipt::fromJson(decode(rows.value(1))["receipt"].toObject());const auto identity=publicationIdentity(receipt,providers);if(!identity.isEmpty())identities.append({after,identity});}
  rows.finish();
  if(count==0)return {};
  if(!db_.transaction())return sqlError();
  bool ok=true;
  for(const auto& entry:identities) {QSqlQuery update(db_);update.prepare("UPDATE history SET publication_identity=? WHERE id=? AND publication_identity=''");update.addBindValue(entry.second);update.addBindValue(entry.first);if(!update.exec()){ok=false;break;}update.prepare("UPDATE item_state SET publication_identity=? WHERE id=? AND publication_identity=''");update.addBindValue(entry.second);update.addBindValue(entry.first);if(!update.exec()){ok=false;break;}}
  if(!ok||!db_.commit()){db_.rollback();return sqlError();}
  // The next keyset query ends naturally when all remaining IDs have been inspected.
  QSqlQuery remaining(db_);remaining.prepare("SELECT 1 FROM history WHERE id>? AND publication_identity='' AND upload_status='uploaded' LIMIT 1");remaining.addBindValue(after);if(!remaining.exec())return sqlError();if(!remaining.next())return {};
 }
}
Operation HistoryStore::operation(const QString& id,Error* error) const {
 if(error_) {if(error)*error=error_;return {};}
 QSqlQuery q(db_);q.prepare("SELECT data FROM operations WHERE id=?");q.addBindValue(id);if(!q.exec()) {if(error)*error=sqlError();return {};}
 if(!q.next()) {if(error)*error={"operation_not_found","Operation not found",{}};return {};}
 auto op=Operation::fromJson(decode(q.value(0)));relocate(op);
 QHash<QString,QJsonObject> statuses;if(auto e=readRemoteStates(db_,{id},statuses)){if(error)*error=e;return {};}
 hydrateRemote(op,statuses);

 return op;
}
QList<Operation> HistoryStore::recover(Error* error) {
 QList<Operation> result;if(error_) {if(error)*error=error_;return result;}
 QSqlQuery q(db_);if(!q.exec("SELECT data FROM operations")) {if(error)*error=sqlError();return result;}
 while(q.next()) {auto op=Operation::fromJson(decode(q.value(0)));relocate(op);bool changed=false;
  if(op.state=="accepted"||op.state=="acquiring") {op.state="interrupted";op.error={"source_interrupted","Acquisition interrupted; invoke a new action",{}};changed=true;}
  // An unsubmitted edit is never resumed or uploaded; its private source is discarded.
  else if(op.state=="editing") {op.state="interrupted";op.error={"edit_interrupted","Editing was interrupted before upload; nothing was uploaded",{}};changed=true;}
  // A recording that never reached explicit Upload is discarded with its private clip.
  else if(op.state=="recording") {op.state="interrupted";op.error={"recording_interrupted","Recording was interrupted before upload; nothing was uploaded",{}};changed=true;}
  else if(op.state=="running"||op.state=="stored"||op.state=="queued") {op.state="interrupted";changed=true;}
  for(auto& i:op.items) if(i.uploadStatus=="uploading") {i.uploadStatus="unknown";changed=true;}
  if(op.state=="interrupted"&&op.copyStatus=="pending") {op.copyStatus="no_result";changed=true;}
  if(changed){op.phase=op.state;for(auto& item:op.items)item.phase=item.uploadStatus;}
  if(changed||(op.state=="interrupted"&&op.items.isEmpty())) result.append(op);
 }
 q.finish();for(const auto& op:result) if(auto e=saveOperation(op)) {if(error)*error=e;return {};}
 // Sharing intent survives a crash; revoke/create are never automatically replayed.
 QSqlQuery shares(db_);if(!shares.exec("SELECT data FROM sharing_operations WHERE json_extract(data,'$.state') IN ('accepted','running')")){if(error)*error=sqlError();return result;}
 QList<QJsonObject> interruptedShares;
 while(shares.next()){auto doc=decode(shares.value(0));if(doc["state"]=="accepted"||doc["state"]=="running"){doc["state"]="interrupted";doc["status"]="unknown";auto receipt=doc["receipt"].toObject();receipt["directUrl"]="";doc["receipt"]=receipt;interruptedShares.append(doc);}}
 shares.finish();for(const auto& doc:interruptedShares)if(auto e=saveSharing(doc)){if(error)*error=e;return {};}
 // A driver may have sent a DELETE before the session died. Keep its exact progress.
 QSqlQuery d(db_);if(!d.exec("SELECT data FROM deletions")) {if(error)*error=sqlError();return result;}
 QList<QJsonObject> pending;while(d.next()) {auto doc=decode(d.value(0));if(doc["state"]=="accepted"||doc["state"]=="running") {doc["state"]="interrupted";auto items=doc["items"].toArray();for(int i=0;i<items.size();++i) {auto item=items[i].toObject();if(item["status"]=="deleting") item["status"]="unknown";items[i]=item;}doc["items"]=items;pending.append(doc);}}
 d.finish();for(const auto& doc:pending) if(auto e=saveDeletion(doc)) {if(error)*error=e;return {};}
 return result;
}
HistoryPage HistoryStore::list(const HistoryQuery& query) const {
 HistoryPage r;if(error_) {r.error=error_;return r;}
 if(query.limit<1||query.limit>200||query.offset<0||query.search.size()>256) {r.error={"invalid_query","History page must be between 1 and 200 records",{}};return r;}
 QString where=" WHERE 1=1";QVariantList values;
 auto filter=[&](QString column,const QString& value) {if(!value.isEmpty()) {where+=" AND "+column+"=?";values.append(value);}};
 filter("category",query.category);filter("mime",query.mime);filter("profile_id",query.profileId);filter("upload_status",query.uploadStatus);filter("local_status",query.localStatus);filter("remote_status",query.remoteStatus);
 if(!query.from.isEmpty()) {where+=" AND created>=?";values.append(query.from);}if(!query.to.isEmpty()) {where+=" AND created<=?";values.append(query.to);}
 if(!query.search.isEmpty()) {where+=" AND name LIKE ? ESCAPE '\\'";auto escaped=query.search;escaped.replace("\\","\\\\");escaped.replace("%","\\%");escaped.replace("_","\\_");values.append("%"+escaped+"%");}
 QSqlQuery q(db_);q.prepare("SELECT count(*) FROM history"+where);for(const auto& v:values) q.addBindValue(v);
 if(!q.exec()||!q.next()) {r.error=sqlError();return r;}r.total=q.value(0).toLongLong();
 q.prepare("SELECT data FROM history"+where+" ORDER BY created DESC,id LIMIT ? OFFSET ?");for(const auto& v:values) q.addBindValue(v);q.addBindValue(query.limit);q.addBindValue(query.offset);
 if(!q.exec()) {r.error=sqlError();return r;}while(q.next()) r.records.append(relocate(publicRecord(decode(q.value(0)))));return r;
}
QJsonObject HistoryStore::detail(const QString& id,Error* error) const {if(error_) {if(error)*error=error_;return {};}
 QSqlQuery q(db_);q.prepare("SELECT data FROM history WHERE id=?");q.addBindValue(id);if(!q.exec()) {if(error)*error=sqlError();return {};}if(!q.next()) {if(error)*error={"history_not_found","History record not found",{}};return {};}return relocate(publicRecord(decode(q.value(0))));}
QJsonArray HistoryStore::recent(int limit,Error* error) const {
 QJsonArray result;if(error_) {if(error)*error=error_;return result;}
 if(limit<1||limit>10000) {if(error)*error={"invalid_query","Invalid recent activity limit",{}};return result;}
 QSqlQuery q(db_);q.prepare("SELECT data FROM operations ORDER BY json_extract(data,'$.created') DESC,id LIMIT ?");q.addBindValue(limit);if(!q.exec()){if(error)*error=sqlError();return result;}
 QList<Operation> operations;QStringList ids;while(q.next()){auto op=Operation::fromJson(decode(q.value(0)));relocate(op);ids.append(op.id);operations.append(std::move(op));}q.finish();
 QHash<QString,QJsonObject> statuses;if(auto e=readRemoteStates(db_,ids,statuses)){if(error)*error=e;return {};}
 for(auto& op:operations){hydrateRemote(op,statuses);result.append(op.publicJson());}return result;
}
QList<Operation> HistoryStore::operationsSince(const QString& from,const QStringList& states,int limit,Error* error) const {
 QList<Operation> result;if(error_) {if(error)*error=error_;return result;}
 QSqlQuery q(db_);q.prepare("SELECT data FROM operations WHERE json_extract(data,'$.created')>=? ORDER BY json_extract(data,'$.created') DESC,id");q.addBindValue(from);
 if(!q.exec()){if(error)*error=sqlError();return result;}
 while(result.size()<limit&&q.next()){auto op=Operation::fromJson(decode(q.value(0)));if(!states.contains(op.state))continue;relocate(op);result.append(std::move(op));}
 return result;
}
std::optional<Operation> HistoryStore::lastUpload(const QString& profileId,Error* error) const {
 if(error_) {if(error)*error=error_;return {};}
 QSqlQuery q(db_);
 q.prepare("SELECT data FROM operations WHERE json_extract(data,'$.destination.profileId')=? AND json_extract(data,'$.state') IN ('completed','partial','failed') ORDER BY json_extract(data,'$.created') DESC,id LIMIT 1");
 q.addBindValue(profileId);
 if(!q.exec()){if(error)*error=sqlError();return {};}
 if(!q.next())return {};
 auto op=Operation::fromJson(decode(q.value(0)));relocate(op);return op;
}
QStringList HistoryStore::journaledOperations(Error* error) const {
 QStringList result;if(error_) {if(error)*error=error_;return result;}
 QSqlQuery q(db_);
 if(!q.exec("SELECT id FROM operations WHERE EXISTS(SELECT 1 FROM json_each(operations.data,'$.items') AS item WHERE json_extract(item.value,'$.uploadJournal.uploadId') IS NOT NULL)")) {if(error)*error=sqlError();return result;}
 while(q.next())result.append(q.value(0).toString());
 return result;
}
Error HistoryStore::expireBefore(const QString& timestamp) {
 if(error_)return error_;
 if(!db_.transaction())return sqlError();
 QSqlQuery q(db_);
 const auto where=QStringLiteral(" WHERE upload_status='uploaded' AND created<? AND id NOT IN (SELECT history_id FROM deletion_targets WHERE active=1) AND id NOT IN (SELECT id FROM item_state WHERE json_extract(data,'$.shareStatus') IN ('pending','unknown'))");
 q.prepare("INSERT OR IGNORE INTO cleared_history SELECT id FROM history"+where);q.addBindValue(timestamp);bool ok=q.exec();if(ok) {q.prepare("DELETE FROM history"+where);q.addBindValue(timestamp);ok=q.exec();}
 if(!ok||!db_.commit()) {db_.rollback();return sqlError();}return {};
}

Error HistoryStore::clear() {
 if(error_)return error_;
 if(!db_.transaction())return sqlError();
 QSqlQuery q(db_);
 const auto retained=QStringLiteral(" WHERE id NOT IN (SELECT history_id FROM deletion_targets WHERE active=1) AND id NOT IN (SELECT id FROM item_state WHERE json_extract(data,'$.shareStatus') IN ('pending','unknown'))");
 if(!q.exec("INSERT OR IGNORE INTO cleared_history SELECT id FROM history"+retained)||!q.exec("DELETE FROM history"+retained)||!db_.commit()) {db_.rollback();return sqlError();}return {};
}
QJsonObject HistoryStore::deletion(const QString& id,Error* error) const {if(error_) {if(error)*error=error_;return {};}
 QSqlQuery q(db_);q.prepare("SELECT data FROM deletions WHERE id=?");q.addBindValue(id);if(!q.exec()) {if(error)*error=sqlError();return {};}if(!q.next()) {if(error)*error={"delete_not_found","Deletion not found",{}};return {};}return decode(q.value(0));}
Error HistoryStore::saveDeletion(const QJsonObject& doc) {
 if(error_) return error_;
 if(!db_.transaction()) return sqlError();
 QSqlQuery q(db_);q.prepare("INSERT INTO deletions VALUES(?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data");q.addBindValue(doc["id"].toString());q.addBindValue(encode(doc));bool ok=q.exec();
 for(const auto& v:doc["items"].toArray()) {if(!ok)break;const auto item=v.toObject();const auto status=item["status"].toString();const bool active=status=="pending"||status=="deleting"||status=="unknown"||status=="failed"||status=="hidden_version_retained";
  q.prepare("INSERT INTO deletion_targets VALUES(?,?,?) ON CONFLICT(deletion_id,history_id) DO UPDATE SET active=excluded.active");q.addBindValue(doc["id"].toString());q.addBindValue(item["historyId"].toString());q.addBindValue(active?1:0);ok=q.exec();
  if(ok) {q.prepare("SELECT data FROM item_state WHERE id=?");q.addBindValue(item["historyId"].toString());ok=q.exec();if(ok&&q.next()){auto data=decode(q.value(0));data["remoteStatus"]=status;data["remoteError"]=item["error"];data["deleteOperationId"]=doc["id"];data["deleteProgress"]=item["progress"];ok=persistRemote(db_,item["historyId"].toString(),data);}}
 }
 if(!ok||!db_.commit()) {db_.rollback();return sqlError();}return {};
}
QJsonObject HistoryStore::sharing(const QString& id,Error* error) const {
 if(error_){if(error)*error=error_;return {};}
 QSqlQuery q(db_);q.prepare("SELECT data FROM sharing_operations WHERE id=?");q.addBindValue(id);
 if(!q.exec()){if(error)*error=sqlError();return {};}
 if(!q.next()){if(error)*error={"share_not_found","Sharing operation not found",{}};return {};}
 return decode(q.value(0));
}
Error HistoryStore::saveSharing(const QJsonObject& doc) {
 if(error_)return error_;
 const auto id=doc["id"].toString(),historyId=doc["historyId"].toString();
 const auto receipt=StorageReceipt::fromJson(doc["receipt"].toObject());
 if(id.isEmpty()||historyId.isEmpty()||receipt.validate())return {"share_journal_invalid","Incomplete sharing intent",{}};
 if(!db_.transaction())return sqlError();
 QSqlQuery q(db_);q.prepare("SELECT data FROM item_state WHERE id=?");q.addBindValue(historyId);
 if(!q.exec()||!q.next()){db_.rollback();return {"history_not_found","Uploaded item ledger not found",{}};}
 auto remote=decode(q.value(0));
 // Remember a create until it is confirmed shared or explicitly abandoned.
 // A read-only inspection that proves private resolves uncertainty, not intent.
 auto intent=remote["shareIntent"].toString();
 if(doc["action"]=="create"){intent="create";remote["shareIntentOptions"]=doc["options"].toObject();}
 if(doc["action"]=="revoke"||doc["status"]=="shared"||doc["status"]=="revoked"||doc["status"]=="changed")intent.clear();
 if(intent.isEmpty())remote["shareIntentOptions"]=QJsonObject{};
 // A confirmed result replaces the recorded link expiry; an uncertain one keeps the last confirmed value.
 if(QStringList{"shared","private","revoked","changed"}.contains(doc["status"].toString())){if(doc["expiresAt"].isString())remote["shareExpiresAt"]=doc["expiresAt"];else remote.remove("shareExpiresAt");}
 remote["shareIntent"]=intent;remote["shareStatus"]=doc["status"];remote["shareReceipt"]=doc["receipt"];remote["shareOperationId"]=id;remote["shareProgress"]=doc["progress"];
 q.prepare("INSERT INTO sharing_operations VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data");q.addBindValue(id);q.addBindValue(historyId);q.addBindValue(encode(doc));
 if(!q.exec()||!persistRemote(db_,historyId,remote)||!db_.commit()){db_.rollback();return sqlError();}return {};
}
Error HistoryStore::updateRemote(const QString& id,const QString& status,const Error& e) {
 Error error;auto data=detail(id,&error);if(error)return error;
 auto remote=remoteFields(data);remote["remoteStatus"]=status;remote["remoteError"]=e.toJson();
 if(!db_.transaction())return sqlError();
 if(!persistRemote(db_,id,remote)||!db_.commit()){db_.rollback();return sqlError();}return {};
}
}
