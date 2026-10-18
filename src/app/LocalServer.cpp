#include "LocalServer.h"
#include "core/BuildInfo.h"
#include <QUuid>
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
LocalServer::LocalServer(SettingsService& settings,OperationService& operations,HistoryService& history,QObject* parent):QObject(parent),residentEpoch_(QUuid::createUuid().toString(QUuid::WithoutBraces)),settings_(settings),operations_(operations),history_(history) {connect(&server_,&LocalIpcServer::newConnection,this,&LocalServer::accept);}
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
   auto bytes=socket->property("bytes").toByteArray();bytes+=socket->readAll();
   auto send=[socket](QString id,Error e,QJsonObject result) {socket->sendAndClose(QJsonDocument(response(id,e,result)).toJson(QJsonDocument::Compact)+'\n');};
   if(bytes.size()>maximumRequest) {socket->setProperty("bytes",QByteArray{});socket->setProperty("handled",true);socket->stopReading();deadline->stop();send({}, {"request_too_large","Request exceeds 1 MiB",{}},{});return;}
   if(!bytes.contains('\n')) {socket->setProperty("bytes",bytes);return;}
   socket->setProperty("bytes",QByteArray{});
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
 if(!operations_.ready()&&(method.startsWith("operation.")||method.startsWith("history.")||method.startsWith("delete.")||method=="cache.clear"||(method.startsWith("recording.")&&method!="recording.capabilities"))) {reply({"initializing","Wait for resident journal recovery",{}},{});return;}
 if(method=="ui.quit") {
  if(!p.isEmpty()){reply(invalid(),{});return;}
  if(!uiQuit_){reply({"ui_unavailable","This resident has no application quit handler",{}},{});return;}
  const auto quit=uiQuit_;QPointer<LocalServer> guarded(this);
  reply({},{{"quitting",true}});
  if(guarded)QTimer::singleShot(0,guarded,quit);
  return;
 }
 if(method=="ui.show") {
  if(!fields(p,{"view"})||!QStringList{"settings","history","activity","about","editFile"}.contains(p["view"].toString())){reply(invalid(),{});return;}
  if(!uiActivation_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  const auto view=p["view"].toString();uiActivation_(view,[reply=std::move(reply),view](Error error){reply(error,{{"shown",view}});});return;
 }
 if(method=="action.invoke") {
  if(!fields(p,{"action"},{"source","path"})||!string(p,"action")||(p.contains("source")&&!string(p,"source",32))||(p.contains("path")&&!string(p,"path",4096))){reply(invalid(),{});return;}
  auto options=p;options.remove("action");operations_.invoke(p["action"].toString(),reply,options);return;
 }
 if(method=="editor.open") {
  if(!fields(p,{},{"source","path"})||(p.contains("source")&&!string(p,"source",32))||(p.contains("path")&&!string(p,"path",4096))){reply(invalid(),{});return;}
  operations_.invoke("edit",reply,p);return;
 }
 // Agent control of the one open editor; the helper validates each command.
 if(QStringList{"editor.inspect","editor.apply","editor.preview","editor.saveCopy","editor.submit","editor.cancel","editor.raise"}.contains(method)) {
  if(!string(p,"id")){reply(invalid(),{});return;}
  auto params=p;params.remove("id");operations_.editorCommand(p["id"].toString(),method.mid(7),params,reply);return;
 }
 // Screen recording: one session, backend-neutral; the service validates semantics.
 if(method=="recording.capabilities") {if(!p.isEmpty())reply(invalid(),{});else operations_.recordingCommand("capabilities",{},reply);return;}
 if(method=="recording.prepare") {
  if(!fields(p,{"target","format"},{"codec","fps","audio","maxEdge"})||!p["target"].isObject()||!string(p,"format",8)||(p.contains("codec")&&!string(p,"codec",8))||(p.contains("audio")&&!p["audio"].isObject())){reply(invalid(),{});return;}
  operations_.recordingCommand("prepare",p,reply);return;
 }
 if(QStringList{"recording.start","recording.pause","recording.resume","recording.stop","recording.cancel","recording.inspect","recording.raise"}.contains(method)) {
  if(!fields(p,{"id"})||!string(p,"id")){reply(invalid(),{});return;}
  operations_.recordingCommand(method.mid(10),p,reply);return;
 }
 if(method=="recording.overlay") {if(!fields(p,{"id","visible"})||!string(p,"id")||!p["visible"].isBool()){reply(invalid(),{});return;}operations_.recordingCommand("overlay",p,reply);return;}
 if(method=="recording.edit") {if(!fields(p,{"id","revision","commands"})||!string(p,"id")||!p["revision"].isDouble()||!p["commands"].isArray()||p["commands"].toArray().size()>100){reply(invalid(),{});return;}operations_.recordingCommand("edit",p,reply);return;}
 if(method=="recording.preview") {if(!fields(p,{"id","time"},{"maxEdge"})||!string(p,"id")||!p["time"].isDouble()||(p.contains("maxEdge")&&!p["maxEdge"].isDouble())){reply(invalid(),{});return;}operations_.recordingCommand("preview",p,reply);return;}
 if(method=="recording.saveCopy") {if(!fields(p,{"id","path"},{"overwrite"})||!string(p,"id")||!string(p,"path",4096)||(p.contains("overwrite")&&!p["overwrite"].isBool())){reply(invalid(),{});return;}operations_.recordingCommand("saveCopy",p,reply);return;}
 if(method=="recording.upload") {if(!fields(p,{"id","revision"})||!string(p,"id")||!p["revision"].isDouble()){reply(invalid(),{});return;}operations_.recordingCommand("upload",p,reply);return;}
 if(method=="operation.recent") {if(!p.isEmpty())reply(invalid(),{});else operations_.recent(reply);return;}
 if(method.startsWith("operation.")) {if(!fields(p,{"id"})||!string(p,"id")) {reply(invalid(),{});return;}const auto id=p["id"].toString();if(method=="operation.inspect")operations_.inspect(id,reply);else if(method=="operation.cancel")operations_.cancel(id,reply);else if(method=="operation.retry")operations_.retry(id,reply);else if(method=="operation.copy")operations_.copyResult(id,reply);else if(method=="operation.retryLocalSave")operations_.retryLocalSave(id,reply);else reply({"unknown_method","Unknown operation method",{}},{});return;}
 if(method=="history.share"||method=="history.revokeLink"||method=="history.inspectLink") {
  if(!fields(p,{"id"})||!string(p,"id")){reply(invalid(),{});return;}
  history_.changeSharing(p["id"].toString(),method=="history.share"?"create":method=="history.revokeLink"?"revoke":"inspect",{},std::move(reply));return;
 }
 if(method=="share.inspect") {if(!fields(p,{"id"})||!string(p,"id")){reply(invalid(),{});return;}history_.inspectSharing(p["id"].toString(),std::move(reply));return;}
 if(method=="history.list") {
  const QStringList allowed{"limit","offset","search","category","mime","profileId","uploadStatus","localStatus","remoteStatus","from","to"};if(!fields(p,{},allowed)) {reply(invalid(),{});return;}
  for(auto it=p.begin();it!=p.end();++it) {if(it.key()=="limit"||it.key()=="offset") {if(!it.value().isDouble()||std::floor(it.value().toDouble())!=it.value().toDouble()||it.value().toDouble()>INT_MAX||it.value().toDouble()<0) {reply(invalid(),{});return;}}else if(!it.value().isString()||it.value().toString().size()>256) {reply(invalid(),{});return;}}
  HistoryQuery q;q.limit=p["limit"].toInt(50);q.offset=p["offset"].toInt();q.search=p["search"].toString();q.category=p["category"].toString();q.mime=p["mime"].toString();q.profileId=p["profileId"].toString();q.uploadStatus=p["uploadStatus"].toString();q.localStatus=p["localStatus"].toString();q.remoteStatus=p["remoteStatus"].toString();q.from=p["from"].toString();q.to=p["to"].toString();history_.list(q,reply);return;
 }
 if(method=="history.detail"||method=="history.copyLink"||method=="history.copyImage"||(method.startsWith("delete.")&&method!="delete.prepare"&&method!="delete.confirm")) {
  if(!fields(p,{"id"})||!string(p,"id")) {reply(invalid(),{});return;}const auto id=p["id"].toString();if(method=="history.detail")history_.detail(id,reply);else if(method=="history.copyLink")history_.copyLink(id,reply);else if(method=="history.copyImage")history_.copyImage(id,reply);else if(method=="delete.inspect")history_.inspectDelete(id,reply);else if(method=="delete.retry")history_.retryDelete(id,reply);else if(method=="delete.cancel")history_.cancelDelete(id,reply);else reply({"unknown_method","Unknown deletion method",{}},{});return;
 }
 if(method=="delete.prepare"||method=="delete.confirm") {
  if(!fields(p,method=="delete.confirm"?QStringList{"ids","acknowledgement"}:QStringList{"ids"})||!p["ids"].isArray()||p["ids"].toArray().size()>1000||(method=="delete.confirm"&&!string(p,"acknowledgement",64))) {reply(invalid(),{});return;}
  QStringList ids;for(const auto& id:p["ids"].toArray()) {if(!id.isString()||id.toString().isEmpty()||id.toString().size()>128) {reply(invalid(),{});return;}ids.append(id.toString());}
  if(method=="delete.prepare")history_.prepareDelete(ids,reply);else history_.deleteConfirmed(ids,p["acknowledgement"].toString(),reply);return;
 }
 if(QStringList{"setup.connectAccount","setup.inspectAccount","setup.createFolder","setup.storeSharePassword"}.contains(method)) {
  const bool hasInput=method=="setup.createFolder"||method=="setup.storeSharePassword";
  if(!fields(p,hasInput?QStringList{"profileId","expectedRevision","input"}:QStringList{"profileId","expectedRevision"})||!string(p,"profileId")||!p["expectedRevision"].isString()||(hasInput&&!p["input"].isObject())){reply(invalid(),{});return;}
  if(!setup_){reply({"setup_unavailable","This resident has no setup service",{}},{});return;}
  setup_->start(method.mid(6),p["profileId"].toString(),p["expectedRevision"].toString(),p["input"].toObject(),std::move(reply));return;
 }
 if(method=="setup.storeCredentials"||method=="setup.unlockCredentials"||method=="setup.testDestination") {
  const bool store=method=="setup.storeCredentials";
  if(!fields(p,store?QStringList{"profileId","expectedRevision","credentials"}:QStringList{"profileId","expectedRevision"})||!string(p,"profileId")||!p["expectedRevision"].isString()||(store&&!p["credentials"].isObject())) {reply(invalid(),{});return;}
  if(!setup_) {reply({"setup_unavailable","This resident has no setup service",{}},{});return;}
  setup_->start(method.mid(6),p["profileId"].toString(),p["expectedRevision"].toString(),p["credentials"].toObject(),std::move(reply));return;
 }
 if(method=="setup.credentialHint"||method=="setup.revealCredentialSecret") {
  if(!fields(p,{"profileId","expectedRevision"})||!string(p,"profileId")||!p["expectedRevision"].isString()){reply(invalid(),{});return;}
  if(!setup_){reply({"setup_unavailable","This resident has no setup service",{}},{});return;}
  if(method=="setup.credentialHint")setup_->credentialHint(p["profileId"].toString(),p["expectedRevision"].toString(),std::move(reply));
  else setup_->revealCredentialSecret(p["profileId"].toString(),p["expectedRevision"].toString(),std::move(reply));
  return;
 }
 if(method=="setup.inspect") {
  if(!fields(p,{"id"})||!string(p,"id")) {reply(invalid(),{});return;}
  if(!setup_) {reply({"setup_unavailable","This resident has no setup service",{}},{});return;}
  setup_->inspect(p["id"].toString(),std::move(reply));return;
 }
 if(method=="settings.save") {
  if(!fields(p,{"document","expectedRevision"})||!p["document"].isObject()||!p["expectedRevision"].isString()) {reply(invalid(),{});return;}
  QPointer<LocalServer> guarded(this);settings_.saveAsync(p["document"].toObject(),p["expectedRevision"].toString(),[guarded,reply](Result result) {if(guarded)reply(result.error,{{"document",guarded->settings_.document()},{"metadata",guarded->settings_.metadata()}});});return;
 }
 if(method=="settings.patch") {if(!fields(p,{"patch","expectedRevision"})||!p["patch"].isObject()||!p["expectedRevision"].isString()) {reply(invalid(),{});return;}QPointer<LocalServer> guarded(this);settings_.patchAsync(p["patch"].toObject(),p["expectedRevision"].toString(),[guarded,reply](Result result) {if(guarded)reply(result.error,guarded->settings_.metadata());});return;}
 if(method=="settings.importDevelopment") {
  if(!fields(p,{"expectedRevision"})||!p["expectedRevision"].isString()) {reply(invalid(),{});return;}
  QPointer<LocalServer> guarded(this);settings_.saveAsync(Settings::developmentTemplate(),p["expectedRevision"].toString(),[guarded,reply](Result result) {if(guarded)reply(result.error,guarded->settings_.metadata());});return;
 }
 if(method=="settings.validate") {if(!fields(p,{"document"})||!p["document"].isObject()) {reply(invalid(),{});return;}auto error=Settings::validate(p["document"].toObject(),[this](const auto& id,const auto& s) {return operations_.providers().validate(id,s);},[this](const auto& id,const auto& reference){return operations_.providers().validateCredentials(id,reference);});reply(error,{{"valid",!error}});return;}
 if(!p.isEmpty()) {reply(invalid(),{});return;}
 if(method=="ui.snapshot")reply({},{{"build",QJsonDocument::fromJson(buildManifestJson()).object()},{"residentEpoch",residentEpoch_},{"operationsReady",operations_.ready()},{"settings",QJsonObject{{"document",settings_.document()},{"metadata",settings_.metadata()}}},{"diagnostics",uiDiagnostics_?uiDiagnostics_():operations_.diagnostics()}});
 else if(method=="settings.get")reply({},{{"document",settings_.document()},{"metadata",settings_.metadata()}});
 else if(method=="settings.developmentTemplate")reply({},{{"document",Settings::developmentTemplate()},{"explicitImportRequired",true}});
 else if(method=="settings.reload") {QPointer<LocalServer> guarded(this);settings_.reloadAsync([guarded,reply](Result result) {if(guarded)reply(result.error,guarded->settings_.metadata());});}
 else if(method=="capabilities")reply({},operations_.capabilities());
 else if(method=="diagnostics")reply({},operations_.diagnostics());
 else if(method=="history.clear")history_.clear(reply);
 else if(method=="cache.clear")operations_.cleanup(reply);
 else reply({"unknown_method","Unknown session method",{}},{});
}
}
