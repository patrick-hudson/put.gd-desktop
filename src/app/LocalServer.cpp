#include "LocalServer.h"
#include "RecordingBackend.h"
#include "client/SessionClient.h"
#include "core/BuildInfo.h"
#include "core/Log.h"
#include <QUuid>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QRegularExpression>
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
// Protocol rejections are logged with the method name only, never parameters.
void logRejection(const QString& method,const Error& error) {
 static const QStringList codes{"invalid_request","invalid_json","request_too_large","unsupported_version","wrong_session","unknown_method"};
 if(!codes.contains(error.code))return;
 static const QRegularExpression name("^[A-Za-z][A-Za-z0-9._]{0,63}$");
 logging::warning(logIpc(),"Session request rejected",{{"method",name.match(method).hasMatch()?method:QStringLiteral("<invalid>")},{"code",error.code}});
}
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
// Most methods answer at once. Recording commands that wait on the helper are
// answered only after it, so their wait outlasts its deadline; exports reply
// when they start (see RecordingService::exportTo) and need no longer wait.
int LocalServer::replyDeadlineMs(const QString& method) {
 constexpr int base=15000;
 // A record toggle can stop the helper through action.invoke as well.
 if(method=="recording.stop"||method=="action.invoke")return recording::helperStopMs+base;
 if(QStringList{"recording.capabilities","recording.prepare","recording.select","recording.start","recording.pause","recording.resume","recording.raise","recording.overlay","recording.preview","recording.review"}.contains(method))return recording::helperCommandMs+base;
 // Reading the logs and compressing up to 20 MiB stays well inside 45 s; an
 // upload adds the destination test and the upload itself (#123).
 if(method=="diagnostics.bundle")return diagnosticsBundleReplyMs;
 if(method=="diagnostics.uploadTarget")return diagnosticsUploadTargetReplyMs;
 if(method=="history.preview")return historyPreviewReplyMs;
 // The checker child has 40 s of its own (#293).
 if(method=="update.check")return UpdateService::childTimeoutMs+base;
 return base;
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
   auto send=[socket](QString id,Error e,QJsonObject result) {logRejection({},e);socket->sendAndClose(QJsonDocument(response(id,e,result)).toJson(QJsonDocument::Compact)+'\n');};
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
 reply=[reply=std::move(reply),method=request["method"].toString().left(64)](Error error,QJsonObject result){if(error)logRejection(method,error);reply(std::move(error),std::move(result));};
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
  // updates: Settings scrolled to Updates, as the rail's Update available link opens it (#293).
  if(!fields(p,{"view"})||!QStringList{"settings","updates","history","activity","about","editFile"}.contains(p["view"].toString())){reply(invalid(),{});return;}
  if(!uiActivation_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  const auto view=p["view"].toString();uiActivation_(view,[reply=std::move(reply),view](Error error){reply(error,{{"shown",view}});});return;
 }
 // Drives the open Settings window the way a user does (Move up / Move down).
 if(method=="ui.moveDestination") {
  if(!fields(p,{"direction"},{"profileId"})||!QStringList{"up","down"}.contains(p["direction"].toString())||(p.contains("profileId")&&!string(p,"profileId",64))){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("moveDestination",p,std::move(reply));return;
 }
 if(method=="action.invoke") {
  if(!fields(p,{"action"},{"source","path","mode"})||!string(p,"action")||(p.contains("source")&&!string(p,"source",32))||(p.contains("path")&&!string(p,"path",4096))||(p.contains("mode")&&(p["action"]!="record"||!string(p,"mode",16)))){reply(invalid(),{});return;}
  auto options=p;options.remove("action");operations_.invoke(p["action"].toString(),reply,options);return;
 }
 if(method=="editor.open") {
  if(!fields(p,{},{"source","path"})||(p.contains("source")&&!string(p,"source",32))||(p.contains("path")&&!string(p,"path",4096))){reply(invalid(),{});return;}
  operations_.invoke("edit",reply,p);return;
 }
 // Agent control of the one open editor; the helper validates each command.
 if(QStringList{"editor.inspect","editor.apply","editor.preview","editor.saveCopy","editor.fileDialog","editor.submit","editor.cancel","editor.raise"}.contains(method)) {
  if(!string(p,"id")){reply(invalid(),{});return;}
  auto params=p;params.remove("id");operations_.editorCommand(p["id"].toString(),method.mid(7),params,reply);return;
 }
 // The editor window's view and selection controls, one per call; the helper checks the values.
 if(method=="editor.view") {
  static const QStringList actions{"inspect","tool","select","selectNext","selectPrevious","selectAffectedText","removeMark","place","geometry","applyCrop","cancelCrop","zoom","fit","actualSize","pan","showSavedCopy","copySavedPath","dismissSavedCopy","discard","keepEditing","confirmDiscard","keyboardHelp","geometryDetails","saveDetails","propertiesWidth"};
  if(!fields(p,{"id"},{"action","tool","shapeId","rect","percent","delta","pixels"})||!string(p,"id")||(p.contains("action")&&(!string(p,"action",32)||!actions.contains(p["action"].toString())))
     ||(p.contains("tool")&&!string(p,"tool",16))||(p.contains("shapeId")&&!p["shapeId"].isNull()&&!p["shapeId"].isDouble())
     ||(p.contains("rect")&&(!p["rect"].isArray()||p["rect"].toArray().size()!=4))||(p.contains("percent")&&!p["percent"].isDouble())||(p.contains("pixels")&&!p["pixels"].isDouble())
     ||(p.contains("delta")&&(!p["delta"].isArray()||p["delta"].toArray().size()!=2))){reply(invalid(),{});return;}
  auto params=p;params.remove("id");operations_.editorCommand(p["id"].toString(),"view",params,reply);return;
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
 if(method=="recording.select") {if(!fields(p,{"id","rect"})||!string(p,"id")||!p["rect"].isArray()||p["rect"].toArray().size()!=4){reply(invalid(),{});return;}operations_.recordingCommand("select",p,reply);return;}
 if(method=="recording.overlay") {if(!fields(p,{"id","visible"})||!string(p,"id")||!p["visible"].isBool()){reply(invalid(),{});return;}operations_.recordingCommand("overlay",p,reply);return;}
 if(method=="recording.edit") {if(!fields(p,{"id","revision","commands"})||!string(p,"id")||!p["revision"].isDouble()||!p["commands"].isArray()||p["commands"].toArray().size()>100){reply(invalid(),{});return;}operations_.recordingCommand("edit",p,reply);return;}
 if(method=="recording.preview") {if(!fields(p,{"id","time"},{"maxEdge"})||!string(p,"id")||!p["time"].isDouble()||(p.contains("maxEdge")&&!p["maxEdge"].isDouble())){reply(invalid(),{});return;}operations_.recordingCommand("preview",p,reply);return;}
 if(method=="recording.review") {
  // #283: one review-window control per call; the helper checks the values.
  static const QStringList actions{"inspect","seek","play","pause","speed","setIn","setOut","resetTrim","addMask","selectMask","setMaskIn","setMaskOut","strength","removeMask","format","saveCopyDialog","closeFileDialog","placeMask","useSolidCover","trimToLimit","dismissStatus","dismissBlurTip","showAdvice","showFormatTips","showInFolder","copyPath","dismissSavedCopy","retryExport","retryPreview","discard","keepReviewing","confirmDiscard"};
  if(!fields(p,{"id","action"},{"time","speed","kind","rect","maskId","strength","format","codec"})||!string(p,"id")||!string(p,"action",32)||!actions.contains(p["action"].toString())
     ||(p.contains("time")&&!p["time"].isDouble())||(p.contains("speed")&&!p["speed"].isDouble())||(p.contains("kind")&&!string(p,"kind",8))
     ||(p.contains("rect")&&(!p["rect"].isArray()||p["rect"].toArray().size()!=4))||(p.contains("maskId")&&!p["maskId"].isNull()&&!string(p,"maskId"))
     ||(p.contains("strength")&&!p["strength"].isDouble())||(p.contains("format")&&!string(p,"format",8))||(p.contains("codec")&&!p["codec"].isString())){reply(invalid(),{});return;}
  operations_.recordingCommand("review",p,reply);return;
 }
 if(method=="recording.saveCopy") {if(!fields(p,{"id","path"},{"overwrite"})||!string(p,"id")||!string(p,"path",4096)||(p.contains("overwrite")&&!p["overwrite"].isBool())){reply(invalid(),{});return;}operations_.recordingCommand("saveCopy",p,reply);return;}
 if(method=="recording.upload") {if(!fields(p,{"id","revision"})||!string(p,"id")||!p["revision"].isDouble()){reply(invalid(),{});return;}operations_.recordingCommand("upload",p,reply);return;}
 if(method=="operation.recent") {if(!p.isEmpty())reply(invalid(),{});else operations_.recent(reply);return;}
 if(method=="operation.cancel"&&p.contains("itemId")) {if(!fields(p,{"id","itemId"})||!string(p,"id")||!string(p,"itemId")) {reply(invalid(),{});return;}operations_.cancel(p["id"].toString(),reply,p["itemId"].toString());return;}
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
 if(method=="history.detail"||method=="history.preview"||method=="history.copyLink"||method=="history.copyImage"||(method.startsWith("delete.")&&method!="delete.prepare"&&method!="delete.confirm")) {
  // history.preview alone takes an optional boolean retry (Retry preview).
  if(!fields(p,{"id"},method=="history.preview"?QStringList{"retry"}:QStringList{})||!string(p,"id")||(p.contains("retry")&&!p["retry"].isBool())) {reply(invalid(),{});return;}const auto id=p["id"].toString();if(method=="history.detail")history_.detail(id,reply);else if(method=="history.preview")history_.preview(id,reply,p["retry"].toBool());else if(method=="history.copyLink")history_.copyLink(id,reply);else if(method=="history.copyImage")history_.copyImage(id,reply);else if(method=="delete.inspect")history_.inspectDelete(id,reply);else if(method=="delete.retry")history_.retryDelete(id,reply);else if(method=="delete.cancel")history_.cancelDelete(id,reply);else reply({"unknown_method","Unknown deletion method",{}},{});return;
 }
 if(method=="history.copyLinks") {
  if(!fields(p,{"ids"})||!p["ids"].isArray()||p["ids"].toArray().isEmpty()||p["ids"].toArray().size()>1000) {reply(invalid(),{});return;}
  QStringList ids;for(const auto& id:p["ids"].toArray()) {if(!id.isString()||id.toString().isEmpty()||id.toString().size()>128||ids.contains(id.toString())) {reply(invalid(),{});return;}ids.append(id.toString());}
  history_.copyLinks(ids,reply);return;
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
 // A diagnostics package, always saved locally first, then optionally
 // uploaded to a working destination (#74 §3, #123, docs/diagnostics.md).
 // notify (#122): after saving, the resident posts "diagnostics saved" with Show in folder.
 if(method=="diagnostics.bundle") {
  auto params=p;const auto notify=params.take("notify");
  BundleRequest bundle;if(auto error=DiagnosticsBundles::parse(params,bundle)){reply(error,{});return;}
  if(!(notify.isUndefined()||notify.isBool())||(notify.toBool()&&bundle.preview)){reply(invalid(),{});return;}
  if(!bundles_){reply({"diagnostics_unavailable","This resident cannot create diagnostics packages",{}},{});return;}
  if(!notify.toBool()){bundles_->create(bundle,std::move(reply));return;}
  QPointer<LocalServer> guarded(this);
  bundles_->create(bundle,[guarded,reply=std::move(reply)](Error error,QJsonObject result){if(!error&&guarded&&guarded->diagnosticsSaved_)guarded->diagnosticsSaved_(result);reply(error,result);});return;
 }
 // A recorded link in the default browser, as the link-ready notification opens
 // it (#326); the reply says which way it went: portal, launcher.
 if(method=="history.openLink") {
  if(!fields(p,{"id"})||!string(p,"id")) {reply(invalid(),{});return;}
  if(!openLink_){reply({"ui_unavailable","This resident cannot open links",{}},{});return;}
  history_.detail(p["id"].toString(),[open=openLink_,reply](Error e,QJsonObject data){
   if(e){reply(e,{});return;}
   if(!data["linkAvailable"].toBool()){reply({"link_unavailable","This recorded link is no longer available",{}},{});return;}
   open(QUrl(data["receipt"].toObject()["directUrl"].toString()),[reply](const QJsonObject& result){
    if(result["opened"].toBool())reply({},result);else reply({"link_not_opened","The link could not be opened on this desktop",result},{});
   });
  });
  return;
 }
 // Show in folder for a saved package: the file manager selects it (#122).
 if(method=="diagnostics.reveal") {
  if(!fields(p,{"path"})||!string(p,"path",4096)){reply(invalid(),{});return;}
  if(!revealPackage_){reply({"ui_unavailable","This resident cannot open a file manager",{}},{});return;}
  const auto error=revealPackage_(p["path"].toString());reply(error,error?QJsonObject{}:QJsonObject{{"revealed",p["path"]}});return;
 }
 // Drives the open Settings window's Diagnostics group and package dialog, and the
 // History window's Save diagnostics row action, the way a user does (#122).
 if(method=="ui.diagnostics") {
  static const QStringList actions{"inspect","setDebugLogging","clearLogs","openPackage","setPackage","savePackage","showInFolder","closePackage"};
  if(!fields(p,{"action"},{"enabled","includeNames","folder","operation","upload"})||!actions.contains(p["action"].toString())||(p.contains("enabled")&&!p["enabled"].isBool())||(p.contains("includeNames")&&!p["includeNames"].isBool())||(p.contains("upload")&&!p["upload"].isBool())
     ||(p.contains("folder")&&(!string(p,"folder",4096)||!QDir::isAbsolutePath(p["folder"].toString())))||(p.contains("operation")&&!string(p,"operation",128))){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("diagnostics",p,std::move(reply));return;
 }
 // Drives the open History page: filters, selection and the compact details view.
 if(method=="ui.history") {
  static const QStringList actions{"inspect","clearFilters","retryLoading","copyDeletionDetails","select","clearSelection","openDetails","closeDetails","expandPreview","closePreview","retryPreview","showSavedCopy","copySavedPath","dismissSavedCopy","cancelAll","confirmCancelAll","keepUploading"};const auto action=p["action"].toString();
  if(!fields(p,{"action"},{"scope","ids"})||!actions.contains(action)||(p.contains("scope")&&(action!="clearFilters"||!QStringList{"filters","all"}.contains(p["scope"].toString())))
     ||(action=="select")!=p.contains("ids")||(p.contains("ids")&&(!p["ids"].isArray()||p["ids"].toArray().isEmpty()||p["ids"].toArray().size()>1000))){reply(invalid(),{});return;}
  for(const auto& id:p["ids"].toArray())if(!id.isString()||id.toString().isEmpty()||id.toString().size()>138){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("history",p,std::move(reply));return;
 }
 if(method=="ui.historySaveDiagnostics") {
  if(!fields(p,{"id"})||!string(p,"id",128)){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("historySaveDiagnostics",p,std::move(reply));return;
 }
 // #259: open, inspect or cancel a file chooser in the open put.gd window, as its
 // button opens it: the desktop's own through the XDG portal, or the fallback.
 if(method=="ui.fileDialog") {
  static const QStringList dialogs{"editImage","historySaveAs","outputFolder","diagnosticsFolder"};
  static const QStringList actions{"open","inspect","cancel"};
  static const QStringList folderFields{"root","images","videos","text","other"};
  const auto dialog=p["dialog"].toString();
  if(!fields(p,{"dialog","action"},{"field","id"})||!string(p,"dialog",32)||!dialogs.contains(dialog)||!string(p,"action",16)||!actions.contains(p["action"].toString())
     ||(p.contains("field")&&(dialog!="outputFolder"||!string(p,"field",16)||!folderFields.contains(p["field"].toString())))
     ||(p.contains("id")&&(dialog!="historySaveAs"||!string(p,"id",128)))){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("fileDialog",p,std::move(reply));return;
 }
 // Where a package would upload and whether that destination works now.
 if(method=="diagnostics.uploadTarget") {
  if(!fields(p,{},{"profileId"})||(p.contains("profileId")&&!string(p,"profileId",64))){reply(invalid(),{});return;}
  if(!bundles_){reply({"diagnostics_unavailable","This resident cannot create diagnostics packages",{}},{});return;}
  bundles_->uploadTarget(p["profileId"].toString(),std::move(reply));return;
 }
 if(method=="settings.save") {
  if(!fields(p,{"document","expectedRevision"})||!p["document"].isObject()||!p["expectedRevision"].isString()) {reply(invalid(),{});return;}
  QPointer<LocalServer> guarded(this);settings_.saveAsync(p["document"].toObject(),p["expectedRevision"].toString(),[guarded,reply](Result result) {if(guarded)reply(result.error,{{"document",guarded->settings_.document()},{"metadata",guarded->settings_.metadata()}});});return;
 }
 if(method=="settings.patch") {if(!fields(p,{"patch","expectedRevision"})||!p["patch"].isObject()||!p["expectedRevision"].isString()) {reply(invalid(),{});return;}QPointer<LocalServer> guarded(this);settings_.patchAsync(p["patch"].toObject(),p["expectedRevision"].toString(),[guarded,reply](Result result) {if(guarded)reply(result.error,guarded->settings_.metadata());});return;}
 // Display order only: routing, exceptions and history refer to profiles by ID.
 if(method=="settings.reorderProfiles") {
  if(!fields(p,{"order","expectedRevision"})||!p["order"].isArray()||p["order"].toArray().size()>64||!p["expectedRevision"].isString()) {reply(invalid(),{});return;}
  // Reorder only the accepted document it was read from; the save rechecks the file under its lock.
  if(p["expectedRevision"].toString()!=settings_.revision()) {reply({"config_conflict","Configuration changed; reload or rebase before saving.",{{"observedRevision",settings_.metadata()["observedRevision"]}}},{});return;}
  QJsonObject document;if(auto error=Settings::reorderProfiles(settings_.document(),p["order"].toArray(),document)) {reply(error,{});return;}
  if(document==settings_.document()) {reply({},{{"changed",false},{"metadata",settings_.metadata()}});return;}
  QPointer<LocalServer> guarded(this);settings_.saveAsync(document,p["expectedRevision"].toString(),[guarded,reply](Result result) {if(guarded)reply(result.error,{{"changed",true},{"metadata",guarded->settings_.metadata()}});});return;
 }
 if(method=="settings.importDevelopment") {
  if(!fields(p,{"expectedRevision"})||!p["expectedRevision"].isString()) {reply(invalid(),{});return;}
  QPointer<LocalServer> guarded(this);settings_.saveAsync(Settings::developmentTemplate(),p["expectedRevision"].toString(),[guarded,reply](Result result) {if(guarded)reply(result.error,guarded->settings_.metadata());});return;
 }
 if(method=="settings.validate") {if(!fields(p,{"document"})||!p["document"].isObject()) {reply(invalid(),{});return;}auto error=Settings::validate(p["document"].toObject(),[this](const auto& id,const auto& s) {return operations_.providers().validate(id,s);},[this](const auto& id,const auto& reference){return operations_.providers().validateCredentials(id,reference);});reply(error,{{"valid",!error}});return;}
 // AppImage installs (#293 phase 3): only with confirm: true.
 if(method=="update.install"||method=="update.restart") {
  if(!updates_){reply({"update_check_unavailable","This resident does not check for updates",{}},{});return;}
  if(method=="update.install")updates_->install(p,reply);else updates_->restart(p,reply);
  return;
 }
 if(!p.isEmpty()) {reply(invalid(),{});return;}
 if(method=="ui.snapshot")reply({},{{"build",QJsonDocument::fromJson(buildManifestJson()).object()},{"residentEpoch",residentEpoch_},{"operationsReady",operations_.ready()},{"settings",QJsonObject{{"document",settings_.document()},{"metadata",settings_.metadata()}}},{"diagnostics",uiDiagnostics_?uiDiagnostics_():operations_.diagnostics()}});
 else if(method=="ui.viewState"){if(uiViewState_)reply({},uiViewState_());else reply({"ui_unavailable","This resident has no interface view state",{}},{});}
 // effectiveLimits: the resolved byte limits (null for none) and the disk that bounds imports.
 else if(method=="settings.get"){auto limits=settings_.snapshot().effectiveArtifactLimits();limits["disk"]=operations_.artifactDisk();reply({},{{"document",settings_.document()},{"metadata",settings_.metadata()},{"effectiveLimits",limits}});}
 else if(method=="settings.developmentTemplate")reply({},{{"document",Settings::developmentTemplate()},{"explicitImportRequired",true}});
 else if(method=="settings.reload") {QPointer<LocalServer> guarded(this);settings_.reloadAsync([guarded,reply](Result result) {if(guarded)reply(result.error,guarded->settings_.metadata());});}
 else if(method=="capabilities")reply({},operations_.capabilities());
 else if(method=="diagnostics")reply({},operations_.diagnostics());
 // The daily update check (#293): its state, and a check now.
 else if(method=="update.status"){if(updates_)reply({},updates_->status());else reply({"update_check_unavailable","This resident does not check for updates",{}},{});}
 else if(method=="update.check"){if(updates_)updates_->check(reply);else reply({"update_check_unavailable","This resident does not check for updates",{}},{});}
 // Repository installs (#293 phase 2): the desktop's software updater.
 else if(method=="update.openSoftwareUpdates"){if(updates_)updates_->openSoftwareUpdates(reply);else reply({"update_check_unavailable","This resident does not check for updates",{}},{});}
 else if(method=="history.clear")history_.clear(reply);
 else if(method=="cache.clear")operations_.cleanup(reply);
 // Removes every persistent log file and starts a new one (#74).
 else if(method=="logs.clear")operations_.background([](Error& error){return logging::clear(error);},reply);
 else reply({"unknown_method","Unknown session method",{}},{});
}
}
