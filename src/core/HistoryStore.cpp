#include "HistoryStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>
namespace putgd {
namespace {
Error sqlError() {return {"history_io","Persistent journal could not be read or committed",{}};}
QByteArray encode(const QJsonObject& o) {return QJsonDocument(o).toJson(QJsonDocument::Compact);}
QJsonObject decode(const QVariant& v) {return QJsonDocument::fromJson(v.toByteArray()).object();}
QString createHistory(const QString& table) {return "CREATE TABLE "+table+"(id TEXT PRIMARY KEY, operation_id TEXT NOT NULL,item_id TEXT NOT NULL,created TEXT NOT NULL,name TEXT NOT NULL,mime TEXT NOT NULL,category TEXT NOT NULL,profile_id TEXT NOT NULL,upload_status TEXT NOT NULL,local_status TEXT NOT NULL,remote_status TEXT NOT NULL,data BLOB NOT NULL,UNIQUE(operation_id,item_id))";}
}
QJsonObject HistoryPage::toJson() const {return {{"records",records},{"total",double(total)}};}
HistoryStore::HistoryStore(QString path) {
 QDir().mkpath(QFileInfo(path).absolutePath()); QFile::setPermissions(QFileInfo(path).absolutePath(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 connection_=QUuid::createUuid().toString();db_=QSqlDatabase::addDatabase("QSQLITE",connection_);db_.setDatabaseName(path);db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=10000");
 if(!db_.open()) {error_=sqlError();return;} QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner);error_=initialize();
}
HistoryStore::~HistoryStore() {db_.close();db_=QSqlDatabase{};QSqlDatabase::removeDatabase(connection_);}
Error HistoryStore::initialize() {
 QSqlQuery q(db_);if(!q.exec("PRAGMA foreign_keys=ON")||!q.exec("PRAGMA user_version")||!q.next()) return sqlError(); const int version=q.value(0).toInt();
 if(version>3) return {"history_version","Unsupported history schema version",{}};
 if(version==3) return {};
 if(!db_.transaction()) return sqlError();
 auto exec=[&](const QString& s) {return q.exec(s);}; bool ok=true;
 if(version==0) ok=exec(createHistory("history"));
 else if(version==1) {
  ok=exec(createHistory("history_next"));
  // Version 1 is a populated id/data ledger. SQLite streams this migration;
  // any malformed row, duplicate or constraint error rolls back DDL and records.
  if(ok) ok=exec("INSERT INTO history_next SELECT id,json_extract(data,'$.operationId'),json_extract(data,'$.itemId'),json_extract(data,'$.created'),json_extract(data,'$.name'),json_extract(data,'$.mime'),json_extract(data,'$.category'),json_extract(data,'$.profileId'),json_extract(data,'$.uploadStatus'),json_extract(data,'$.local.status'),json_extract(data,'$.remoteStatus'),data FROM history");
  if(ok) ok=exec("DROP TABLE history")&&exec("ALTER TABLE history_next RENAME TO history");
 }
 if(ok) ok=exec("CREATE TABLE IF NOT EXISTS operations(id TEXT PRIMARY KEY,data BLOB NOT NULL)")&&exec("CREATE TABLE IF NOT EXISTS deletions(id TEXT PRIMARY KEY,data BLOB NOT NULL)")&&exec("CREATE TABLE IF NOT EXISTS deletion_targets(deletion_id TEXT NOT NULL,history_id TEXT NOT NULL,active INTEGER NOT NULL,PRIMARY KEY(deletion_id,history_id))")&&exec("CREATE INDEX IF NOT EXISTS operations_created ON operations(json_extract(data,'$.created') DESC)")&&exec("CREATE INDEX IF NOT EXISTS history_category ON history(category,created DESC)")&&exec("CREATE INDEX IF NOT EXISTS history_upload ON history(upload_status,created DESC)")&&exec("CREATE INDEX IF NOT EXISTS history_local ON history(local_status,created DESC)")&&exec("CREATE INDEX IF NOT EXISTS history_created ON history(created DESC,id)")&&exec("CREATE INDEX IF NOT EXISTS history_filters ON history(profile_id,upload_status,category,created DESC)")&&exec("CREATE INDEX IF NOT EXISTS history_name ON history(name)")&&exec("CREATE INDEX IF NOT EXISTS history_mime ON history(mime,created)")&&exec("CREATE INDEX IF NOT EXISTS history_remote ON history(remote_status,local_status)")&&exec("CREATE TABLE IF NOT EXISTS cleared_history(id TEXT PRIMARY KEY)")&&exec("PRAGMA user_version=3");
 if(!ok||!db_.commit()) {db_.rollback();return {"history_migration","History migration failed; original records preserved",{}};}
 return {};
}
Error HistoryStore::saveOperation(const Operation& op) {
 if(error_) return error_;
 if(!db_.transaction()) return sqlError();
 QSqlQuery q(db_);
 q.prepare("INSERT INTO operations VALUES(?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data");q.addBindValue(op.id);q.addBindValue(encode(op.toJson()));bool ok=q.exec();
 for(const auto& item:op.items) {
  if(!ok) break;
  q.prepare("SELECT 1 FROM cleared_history WHERE id=?");q.addBindValue(item.artifact.id);if(!q.exec()) {ok=false;break;}if(q.next())continue;
  QJsonObject remote; q.prepare("SELECT data FROM history WHERE id=?");q.addBindValue(item.artifact.id);if(!q.exec()) {ok=false;break;}if(q.next()) {const auto retained=decode(q.value(0));for(const auto& key:QStringList{"remoteStatus","remoteError","deleteOperationId","deleteProgress"})if(retained.contains(key))remote[key]=retained[key];}
  auto data=item.toJson();for(auto it=remote.begin();it!=remote.end();++it)data[it.key()]=it.value();auto artifact=data.take("artifact").toObject();artifact.remove("path");
  for(auto it=artifact.begin();it!=artifact.end();++it) data[it.key()]=it.value();
  data["operationId"]=op.id;data["itemId"]=item.artifact.id;data["id"]=item.artifact.id;data["created"]=op.created;data["profileId"]=op.destination.profileId;
  q.prepare("INSERT INTO history VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET upload_status=excluded.upload_status,local_status=excluded.local_status,data=excluded.data");
  for(const auto& value:QVariantList{item.artifact.id,op.id,item.artifact.id,op.created,item.artifact.name,item.artifact.mime,item.artifact.category,op.destination.profileId,item.uploadStatus,item.local.status,item.remoteStatus,encode(data)}) q.addBindValue(value.metaType().id()==QMetaType::QString&&value.toString().isNull()?QVariant(QStringLiteral("")):value);
  ok=q.exec();
  // Remote management is independent: operation retry must never overwrite a delete result.
  if(ok) {q.prepare("SELECT remote_status,data FROM history WHERE id=?");q.addBindValue(item.artifact.id);ok=q.exec()&&q.next();if(ok) {auto retained=decode(q.value(1));retained["remoteStatus"]=q.value(0).toString();q.prepare("UPDATE history SET data=? WHERE id=?");q.addBindValue(encode(retained));q.addBindValue(item.artifact.id);ok=q.exec();}}
 }
 if(!ok||!db_.commit()) {db_.rollback();return sqlError();}return {};
}
Operation HistoryStore::operation(const QString& id,Error* error) const {
 if(error_) {if(error)*error=error_;return {};}
 QSqlQuery q(db_);q.prepare("SELECT data FROM operations WHERE id=?");q.addBindValue(id);if(!q.exec()) {if(error)*error=sqlError();return {};}
 if(!q.next()) {if(error)*error={"operation_not_found","Operation not found",{}};return {};}
 return Operation::fromJson(decode(q.value(0)));
}
QList<Operation> HistoryStore::recover(Error* error) {
 QList<Operation> result;if(error_) {if(error)*error=error_;return result;}
 QSqlQuery q(db_);if(!q.exec("SELECT data FROM operations")) {if(error)*error=sqlError();return result;}
 while(q.next()) {auto op=Operation::fromJson(decode(q.value(0)));bool changed=false;
  if(op.state=="accepted"||op.state=="acquiring") {op.state="interrupted";op.error={"source_interrupted","Acquisition interrupted; invoke a new action",{}};changed=true;}
  else if(op.state=="running"||op.state=="stored"||op.state=="queued") {op.state="interrupted";changed=true;}
  for(auto& i:op.items) if(i.uploadStatus=="uploading") {i.uploadStatus="unknown";changed=true;}
  if(changed||(op.state=="interrupted"&&op.items.isEmpty())) result.append(op);
 }
 q.finish();for(const auto& op:result) if(auto e=saveOperation(op)) {if(error)*error=e;return {};}
 // A driver may have sent a DELETE before the session died. Keep its exact progress.
 QSqlQuery d(db_);if(!d.exec("SELECT data FROM deletions")) {if(error)*error=sqlError();return result;}
 QList<QJsonObject> pending;while(d.next()) {auto doc=decode(d.value(0));if(doc["state"]=="running") {doc["state"]="interrupted";auto items=doc["items"].toArray();for(int i=0;i<items.size();++i) {auto item=items[i].toObject();if(item["status"]=="deleting") item["status"]="unknown";items[i]=item;}doc["items"]=items;pending.append(doc);}}
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
 if(!q.exec()) {r.error=sqlError();return r;}while(q.next()) r.records.append(decode(q.value(0)));return r;
}
QJsonObject HistoryStore::detail(const QString& id,Error* error) const {if(error_) {if(error)*error=error_;return {};}
 QSqlQuery q(db_);q.prepare("SELECT data FROM history WHERE id=?");q.addBindValue(id);if(!q.exec()) {if(error)*error=sqlError();return {};}if(!q.next()) {if(error)*error={"history_not_found","History record not found",{}};return {};}return decode(q.value(0));}
QJsonArray HistoryStore::recent(int limit,Error* error) const {
 QJsonArray result;if(error_) {if(error)*error=error_;return result;}
 if(limit<1||limit>10000) {if(error)*error={"invalid_query","Invalid recent activity limit",{}};return result;}
 QSqlQuery q(db_);q.prepare("SELECT data FROM operations ORDER BY json_extract(data,'$.created') DESC,id LIMIT ?");q.addBindValue(limit);if(!q.exec()) {if(error)*error=sqlError();return result;}while(q.next())result.append(Operation::fromJson(decode(q.value(0))).publicJson());return result;
}
Error HistoryStore::expireBefore(const QString& timestamp) {
 if(error_)return error_;
 if(!db_.transaction())return sqlError();
 QSqlQuery q(db_);
 const auto where=QStringLiteral(" WHERE upload_status='uploaded' AND created<? AND id NOT IN (SELECT history_id FROM deletion_targets WHERE active=1)");
 q.prepare("INSERT OR IGNORE INTO cleared_history SELECT id FROM history"+where);q.addBindValue(timestamp);bool ok=q.exec();if(ok) {q.prepare("DELETE FROM history"+where);q.addBindValue(timestamp);ok=q.exec();}
 if(!ok||!db_.commit()) {db_.rollback();return sqlError();}return {};
}

Error HistoryStore::clear() {
 if(error_)return error_;
 if(!db_.transaction())return sqlError();
 QSqlQuery q(db_);
 const auto retained=QStringLiteral(" WHERE id NOT IN (SELECT history_id FROM deletion_targets WHERE active=1)");
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
  if(ok) {q.prepare("SELECT data FROM history WHERE id=?");q.addBindValue(item["historyId"].toString());ok=q.exec();if(ok&&q.next()) {auto data=decode(q.value(0));data["remoteStatus"]=status;data["remoteError"]=item["error"];data["deleteOperationId"]=doc["id"];data["deleteProgress"]=item["progress"];q.prepare("UPDATE history SET remote_status=?,data=? WHERE id=?");q.addBindValue(status);q.addBindValue(encode(data));q.addBindValue(item["historyId"].toString());ok=q.exec();}}
 }
 if(!ok||!db_.commit()) {db_.rollback();return sqlError();}return {};
}
Error HistoryStore::updateRemote(const QString& id,const QString& status,const Error& e) {Error error;auto data=detail(id,&error);if(error)return error;data["remoteStatus"]=status;data["remoteError"]=e.toJson();QSqlQuery q(db_);q.prepare("UPDATE history SET remote_status=?,data=? WHERE id=?");q.addBindValue(status);q.addBindValue(encode(data));q.addBindValue(id);return q.exec()?Error{}:sqlError();}
}
