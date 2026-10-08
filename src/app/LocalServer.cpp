#include "LocalServer.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <cmath>
#include <climits>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace putgd {
namespace {
constexpr qsizetype maximumRequest=1024*1024;
Error invalid() {return {"invalid_request","Invalid request fields or parameter types",{}};}
bool fields(const QJsonObject& object,const QStringList& required,const QStringList& optional={}) {for(auto it=object.begin();it!=object.end();++it)if(!required.contains(it.key())&&!optional.contains(it.key()))return false;for(const auto& key:required)if(!object.contains(key))return false;return true;}
bool string(const QJsonObject& object,const QString& key,int max=256) {return object[key].isString()&&!object[key].toString().isEmpty()&&object[key].toString().size()<=max;}
}
LocalServer::LocalServer(SettingsService& settings,OperationService& operations,HistoryService& history,QObject* parent):QObject(parent),settings_(settings),operations_(operations),history_(history) {connect(&server_,&LocalIpcServer::newConnection,this,&LocalServer::accept);}
QString LocalServer::defaultRuntimeDirectory() {const auto runtime=QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);return runtime.isEmpty()?QString{}:QDir(runtime).filePath("putgd");}
QString LocalServer::defaultSocketPath() {return QDir(defaultRuntimeDirectory()).filePath("session.sock");}
Result LocalServer::listen(const QString& supplied) {
 const auto runtime=supplied.isEmpty()?defaultRuntimeDirectory():supplied;
 if(runtime.isEmpty()||!QDir::isAbsolutePath(runtime))return {{"session_unavailable","No user runtime directory",{}},false};
 QFileInfo fi(runtime);if(fi.isSymLink())return {{"session_unavailable","Runtime directory must not be a symbolic link",{}},false};
 if(!QDir().mkpath(runtime))return {{"session_unavailable","Cannot create runtime directory",{}},false};
#ifdef Q_OS_UNIX
 struct stat status{};const auto bytes=QFile::encodeName(runtime);if(::lstat(bytes.constData(),&status)!=0||status.st_uid!=::getuid()||!S_ISDIR(status.st_mode))return {{"session_unavailable","Runtime directory has a different owner",{}},false};
#endif
 if(!QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner))return {{"session_unavailable","Cannot restrict runtime permissions",{}},false};
 const auto path=QDir(runtime).filePath("session.sock");
 // Probe an existing endpoint. Never unlink a live resident session.
 if(QFileInfo::exists(path)) {const auto probe=probeLocalSession(path,200);if(probe==LocalSessionState::Connected)return {{"session_running","A resident process already owns this session",{}},false};
#ifdef Q_OS_UNIX
  struct stat socketStatus{};const auto encoded=QFile::encodeName(path);if(::lstat(encoded.constData(),&socketStatus)!=0||socketStatus.st_uid!=::getuid()||!S_ISSOCK(socketStatus.st_mode))return {{"session_unavailable","Existing endpoint is not an owned session socket",{}},false};
#endif
  if(probe!=LocalSessionState::Absent)return {{"session_unavailable","Cannot establish whether the existing session is stale",{}},false};
  if(!LocalIpcServer::removeServer(path))return {{"session_unavailable","Cannot remove stale session socket",{}},false};
 }
 if(!server_.listen(path))return {{"session_unavailable","Cannot listen on user session socket",{}},false};
 return {{},true};
}
QJsonObject LocalServer::response(const QString& requestId,const Error& error,const QJsonObject& result) {return {{"schemaVersion",1},{"requestId",requestId},{"ok",!error},{"result",error?QJsonObject{}:result},{"error",error.toJson()}};}
void LocalServer::accept() {
 while(auto* socket=server_.nextPendingConnection()) {
  if(clients_.size()>=64) {socket->disconnectFromServer();socket->deleteLater();continue;}
  clients_.insert(socket);socket->setReadBufferSize(maximumRequest+1);socket->setProperty("bytes",QByteArray{});socket->setProperty("handled",false);
  auto* deadline=new QTimer(socket);deadline->setSingleShot(true);deadline->start(10000);connect(deadline,&QTimer::timeout,socket,[socket] {socket->disconnectFromServer();});
  connect(socket,&LocalIpcConnection::disconnected,this,[this,socket] {clients_.remove(socket);socket->deleteLater();});
  connect(socket,&LocalIpcConnection::readyRead,this,[this,socket,deadline] {
   if(socket->property("handled").toBool())return;
   auto bytes=socket->property("bytes").toByteArray();bytes+=socket->readAll();socket->setProperty("bytes",bytes);
   auto send=[socket](QString id,Error e,QJsonObject result) {socket->sendAndClose(QJsonDocument(response(id,e,result)).toJson(QJsonDocument::Compact)+'\n');};
   if(bytes.size()>maximumRequest) {socket->setProperty("handled",true);send({}, {"request_too_large","Request exceeds 1 MiB",{}},{});return;}
   if(!bytes.contains('\n'))return;
   socket->setProperty("handled",true);socket->stopReading();deadline->stop();
   if(bytes.indexOf('\n')!=bytes.size()-1) {send({},invalid(),{});return;}
   QJsonParseError parseError;auto doc=QJsonDocument::fromJson(bytes,&parseError);if(parseError.error!=QJsonParseError::NoError||!doc.isObject()) {send({}, {"invalid_json","Request must be a JSON object",{}},{});return;}
   auto request=doc.object();const auto id=request["requestId"].toString();QPointer<LocalIpcConnection> guarded(socket);
   dispatch(request,[guarded,id](Error e,QJsonObject result) {if(guarded) {guarded->sendAndClose(QJsonDocument(response(id,e,result)).toJson(QJsonDocument::Compact)+'\n');}});
  });
 }
}
void LocalServer::dispatch(const QJsonObject& request,JsonReply reply) {
 if(!fields(request,{"schemaVersion","requestId","session","method","params"})||!string(request,"requestId",128)||!string(request,"method",64)||!request["params"].isObject()||!request["session"].isString()) {reply(invalid(),{});return;}
 if(!request["schemaVersion"].isDouble()||request["schemaVersion"].toDouble()!=1) {reply({"unsupported_version","Request schemaVersion must be 1",{}},{});return;}
 if(request["session"].toString()!=socketPath()) {reply({"wrong_session","Request targets a different resident session",{}},{});return;}
 const auto method=request["method"].toString();const auto p=request["params"].toObject();
 if(!operations_.ready()&&(method.startsWith("operation.")||method.startsWith("history.")||method.startsWith("delete.")||method=="cache.clear")) {reply({"initializing","Wait for resident journal recovery",{}},{});return;}
 if(method=="ui.show") {
  if(!fields(p,{"view"})||!QStringList{"settings","history","activity"}.contains(p["view"].toString())){reply(invalid(),{});return;}
  if(!uiActivation_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiActivation_(p["view"].toString());reply({},{{"shown",p["view"]}});return;
 }
 if(method=="action.invoke") {if(!fields(p,{"action"})||!string(p,"action"))reply(invalid(),{});else operations_.invoke(p["action"].toString(),reply);return;}
 if(method=="operation.recent") {if(!p.isEmpty())reply(invalid(),{});else operations_.recent(reply);return;}
 if(method.startsWith("operation.")) {if(!fields(p,{"id"})||!string(p,"id")) {reply(invalid(),{});return;}const auto id=p["id"].toString();if(method=="operation.inspect")operations_.inspect(id,reply);else if(method=="operation.cancel")operations_.cancel(id,reply);else if(method=="operation.retry")operations_.retry(id,reply);else if(method=="operation.copy")operations_.copyResult(id,reply);else if(method=="operation.retryLocalSave")operations_.retryLocalSave(id,reply);else reply({"unknown_method","Unknown operation method",{}},{});return;}
 if(method=="history.list") {
  const QStringList allowed{"limit","offset","search","category","mime","profileId","uploadStatus","localStatus","remoteStatus","from","to"};if(!fields(p,{},allowed)) {reply(invalid(),{});return;}
  for(auto it=p.begin();it!=p.end();++it) {if(it.key()=="limit"||it.key()=="offset") {if(!it.value().isDouble()||std::floor(it.value().toDouble())!=it.value().toDouble()||it.value().toDouble()>INT_MAX||it.value().toDouble()<0) {reply(invalid(),{});return;}}else if(!it.value().isString()||it.value().toString().size()>256) {reply(invalid(),{});return;}}
  HistoryQuery q;q.limit=p["limit"].toInt(50);q.offset=p["offset"].toInt();q.search=p["search"].toString();q.category=p["category"].toString();q.mime=p["mime"].toString();q.profileId=p["profileId"].toString();q.uploadStatus=p["uploadStatus"].toString();q.localStatus=p["localStatus"].toString();q.remoteStatus=p["remoteStatus"].toString();q.from=p["from"].toString();q.to=p["to"].toString();history_.list(q,reply);return;
 }
 if(method=="history.detail"||method=="history.copyLink"||(method.startsWith("delete.")&&method!="delete.prepare"&&method!="delete.confirm")) {
  if(!fields(p,{"id"})||!string(p,"id")) {reply(invalid(),{});return;}const auto id=p["id"].toString();if(method=="history.detail")history_.detail(id,reply);else if(method=="history.copyLink")history_.copyLink(id,reply);else if(method=="delete.inspect")history_.inspectDelete(id,reply);else if(method=="delete.retry")history_.retryDelete(id,reply);else if(method=="delete.cancel")history_.cancelDelete(id,reply);else reply({"unknown_method","Unknown deletion method",{}},{});return;
 }
 if(method=="delete.prepare"||method=="delete.confirm") {
  if(!fields(p,method=="delete.confirm"?QStringList{"ids","acknowledgement"}:QStringList{"ids"})||!p["ids"].isArray()||p["ids"].toArray().size()>1000||(method=="delete.confirm"&&!string(p,"acknowledgement",64))) {reply(invalid(),{});return;}
  QStringList ids;for(const auto& id:p["ids"].toArray()) {if(!id.isString()||id.toString().isEmpty()||id.toString().size()>128) {reply(invalid(),{});return;}ids.append(id.toString());}
  if(method=="delete.prepare")history_.prepareDelete(ids,reply);else history_.deleteConfirmed(ids,p["acknowledgement"].toString(),reply);return;
 }
 if(method=="settings.patch") {if(!fields(p,{"patch","expectedRevision"})||!p["patch"].isObject()||!p["expectedRevision"].isString()) {reply(invalid(),{});return;}QPointer<LocalServer> guarded(this);settings_.patchAsync(p["patch"].toObject(),p["expectedRevision"].toString(),[guarded,reply](Result result) {if(guarded)reply(result.error,guarded->settings_.metadata());});return;}
 if(method=="settings.importDevelopment") {
  if(!fields(p,{"expectedRevision"})||!p["expectedRevision"].isString()) {reply(invalid(),{});return;}
  QPointer<LocalServer> guarded(this);settings_.saveAsync(Settings::developmentTemplate(),p["expectedRevision"].toString(),[guarded,reply](Result result) {if(guarded)reply(result.error,guarded->settings_.metadata());});return;
 }
 if(method=="settings.validate") {if(!fields(p,{"document"})||!p["document"].isObject()) {reply(invalid(),{});return;}auto error=Settings::validate(p["document"].toObject(),[this](const auto& id,const auto& s) {return operations_.providers().validate(id,s);});reply(error,{{"valid",!error}});return;}
 if(!p.isEmpty()) {reply(invalid(),{});return;}
 if(method=="settings.get")reply({},{{"document",settings_.document()},{"metadata",settings_.metadata()}});
 else if(method=="settings.developmentTemplate")reply({},{{"document",Settings::developmentTemplate()},{"explicitImportRequired",true}});
 else if(method=="settings.reload") {QPointer<LocalServer> guarded(this);settings_.reloadAsync([guarded,reply](Result result) {if(guarded)reply(result.error,guarded->settings_.metadata());});}
 else if(method=="capabilities")reply({},operations_.capabilities());
 else if(method=="diagnostics")reply({},operations_.diagnostics());
 else if(method=="history.clear")history_.clear(reply);
 else if(method=="cache.clear")operations_.cleanup(reply);
 else reply({"unknown_method","Unknown session method",{}},{});
}
}
