#include "core/ByteSize.h"
#include "LocalServer.h"
#include "RecordingBackend.h"
#include "ScrollBackend.h"
#include "client/SessionProtocol.h"
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
 if(method=="scroll.stop")return scroll::helperStopMs+base;
 if(method.startsWith("scroll."))return scroll::helperStartupMs+scroll::helperCommandMs+base;
 if(method=="recording.stop"||method=="action.invoke")return recording::helperStopMs+base;
 // Answered once the running job has stopped.
 if(method=="text.cancel")return textjob::maximumDeadlineMs+base;
 if(QStringList{"recording.capabilities","recording.microphones","recording.prepare","recording.select","recording.start","recording.pause","recording.resume","recording.raise","recording.overlay","recording.audio","recording.preview","recording.review"}.contains(method))return recording::helperCommandMs+base;
 // Reading the logs and compressing up to 20 MiB stays well inside 45 s; an
 // upload adds the destination test and the upload itself (#123).
 if(method=="diagnostics.bundle")return diagnosticsBundleReplyMs;
 if(method=="diagnostics.uploadTarget")return diagnosticsUploadTargetReplyMs;
 if(method=="history.preview")return historyPreviewReplyMs;
 // history.qr refreshes an expired link first, as Copy link does (#1428).
 if(method=="history.copyLink"||method=="history.copyLinks"||method=="history.qr")return 630000;
 // editor.apply with redactText answers when recognition is done, not at once.
 if(method=="editor.apply")return textjob::maximumDeadlineMs+base;
 if(method=="history.unlockLinkCredentials"||method=="history.unlockCredentials")return 300000;
 // An error log step runs the session method it stands for: unlocking can wait on a keyring prompt (#1753).
 if(method=="errors.act")return 300000;
 // Checking a large folder over GVfs or KIOFuse stats every file before the reply.
 if(method=="upload.files")return uploadFilesReplyMs;
 if(method=="settings.importUploader")return uploaderImportReplyMs;
 // Setup runs gsettings and waits for the Shell, each with its own bound.
 if(method=="desktop.setup.recheck")return desktopSetupRecheckReplyMs;
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
   if(bytes.size()>maximumRequest) {socket->setProperty("bytes",QByteArray{});socket->setProperty("handled",true);socket->stopReading();deadline->stop();send({}, {"request_too_large",ByteSize::labels("Request exceeds 1 MiB"),{}},{});return;}
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
 reply=[guarded=QPointer<LocalServer>(this),reply=std::move(reply),method=request["method"].toString().left(64),params=request["params"].toObject()](Error error,QJsonObject result){if(error)logRejection(method,error);if(guarded)guarded->noteOutcome(method,params,error);reply(std::move(error),std::move(result));};
 if(!fields(request,{"schemaVersion","requestId","session","method","params"})||!string(request,"requestId",128)||!string(request,"method",64)||!request["params"].isObject()||!request["session"].isString()) {reply(invalid(),{});return;}
 if(!request["schemaVersion"].isDouble()||request["schemaVersion"].toDouble()!=1) {reply({"unsupported_version","Request schemaVersion must be 1",{}},{});return;}
 if(request["session"].toString()!=socketPath()) {reply({"wrong_session","Request targets a different resident session",{}},{});return;}
 const auto method=request["method"].toString();const auto p=request["params"].toObject();
 if(!operations_.ready()&&(method.startsWith("operation.")||method.startsWith("history.")||method.startsWith("delete.")||method=="cache.clear"||(method.startsWith("recording.")&&method!="recording.capabilities"&&method!="recording.windows"&&method!="recording.microphones"))) {reply(operations_.notReadyError(),{});return;}
 if(method=="ui.quit") {
  if(!p.isEmpty()){reply(invalid(),{});return;}
  if(!uiQuit_){reply({"ui_unavailable","This resident has no application quit handler",{}},{});return;}
  // No deletion is admitted once the quit is accepted, not only once it runs (#1602).
  history_.stopDeletions();
  const auto quit=uiQuit_;QPointer<LocalServer> guarded(this);
  reply({},{{"quitting",true}});
  if(guarded)QTimer::singleShot(0,guarded,quit);
  return;
 }
 if(method=="ui.show") {
  // updates: Settings scrolled to Updates, as the rail's Update available link opens it (#293).
  if(!fields(p,{"view"})||!QStringList{"settings","updates","history","activity","destinations","about","editFile","errors"}.contains(p["view"].toString())){reply(invalid(),{});return;}
  if(!uiActivation_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  const auto view=p["view"].toString();uiActivation_(view,[reply=std::move(reply),view](Error error){reply(error,{{"shown",view}});});return;
 }
 // Drives the open Settings window the way a user does (Move up / Move down).
 if(method=="ui.moveDestination") {
  if(!fields(p,{"direction"},{"profileId"})||!QStringList{"up","down"}.contains(p["direction"].toString())||(p.contains("profileId")&&!string(p,"profileId",64))){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("moveDestination",p,std::move(reply));return;
 }
 // #1361: drives the Custom uploader form in the open Destinations page the way a
 // user does. import reads a .sxcu file into the form, where its secret values wait,
 // masked, until store; the reply is the form's state and never carries a value.
 if(method=="ui.customUploader") {
  static const QStringList actions{"inspect","import","openImport","closeImport","store","test"};const auto action=p["action"].toString();
  if(!fields(p,{"action"},{"path","profileId"})||!actions.contains(action)||(action=="import")!=p.contains("path")||(p.contains("path")&&(!string(p,"path",4096)||!QDir::isAbsolutePath(p["path"].toString())))||(p.contains("profileId")&&!string(p,"profileId",64))){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("customUploader",p,std::move(reply));return;
 }
 // #1871: the SFTP form in the open Destinations page, edited, pasted into, tested, trusted and
 // its folder created as a user does. Settings values only (the SSH key file's path among them,
 // #1892), never a password or passphrase.
 if(method=="ui.sftpDestination") {
  static const QHash<QString,QStringList> allowed{{"inspect",{"profileId"}},{"set",{"field","value","profileId"}},{"paste",{"text","profileId"}},{"add",{"label"}},
   {"test",{"profileId"}},{"trust",{"fingerprint","profileId"}},{"createFolder",{"profileId"}},{"store",{"profileId"}},{"clear",{"field","profileId"}}};
  const auto action=p["action"].toString();
  bool valid=allowed.contains(action)&&p["action"].isString();
  for(auto it=p.begin();valid&&it!=p.end();++it)valid=it.key()=="action"||allowed.value(action).contains(it.key());
  // keyFile (#1892): the SSH key file's path, a setting like the others; empty clears it.
  if(valid&&action=="set")valid=QStringList{"server","user","folder","publicUrlBase","keyFile"}.contains(p["field"].toString())&&p["value"].isString()&&p["value"].toString().size()<=4096;
  if(valid&&action=="paste")valid=string(p,"text",4096);
  if(valid&&action=="trust")valid=string(p,"fingerprint",128);
  if(valid&&action=="clear")valid=p["field"]=="password"||p["field"]=="passphrase";
  if(valid&&action=="add")valid=string(p,"label",128);
  if(valid&&p.contains("profileId"))valid=string(p,"profileId",64);
  if(!valid){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("sftpDestination",p,std::move(reply));return;
 }
 // #1678: the Cloudflare R2 form in the open Destinations page, edited, pasted into and moved to
 // (from an S3 destination on R2) as a user does. Settings values only, never credentials.
 if(method=="ui.r2Destination") {
  // Each action takes only its own fields (#1699): add a label; set a field and value; paste text;
  // inspect, set, paste and move an optional profileId.
  static const QHash<QString,QStringList> allowed{{"inspect",{"profileId"}},{"set",{"field","value","profileId"}},{"paste",{"text","profileId"}},{"move",{"profileId"}},{"add",{"label"}}};
  static const QStringList fieldNames{"accountId","jurisdiction","bucket","objectPrefix","linkMode","linkValidityDays","publicUrlBase","connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds","advanced"};
  const auto action=p["action"].toString();
  bool valid=allowed.contains(action)&&p["action"].isString();
  for(auto it=p.begin();valid&&it!=p.end();++it)valid=it.key()=="action"||allowed.value(action).contains(it.key());
  if(valid&&action=="set")valid=p.contains("field")&&p.contains("value")&&fieldNames.contains(p["field"].toString())&&p["value"].isString()&&p["value"].toString().size()<=2048
   &&(p["field"]!="advanced"||p["value"]=="open"||p["value"]=="closed");
  if(valid&&action=="paste")valid=string(p,"text",2048);
  if(valid&&action=="add")valid=string(p,"label",128);
  if(valid&&p.contains("profileId"))valid=string(p,"profileId",64);
  if(!valid){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("r2Destination",p,std::move(reply));return;
 }
 // #1872: the Nextcloud and WebDAV forms in the open Destinations page, edited and signed in as a
 // user does. Settings values and button presses only, never a credential.
 if(method=="ui.davDestination") {
  static const QHash<QString,QStringList> allowed{{"inspect",{"profileId"}},{"set",{"field","value","profileId"}},{"add",{"label","provider"}},{"appPassword",{"value","profileId"}},
   {"signIn",{"profileId"}},{"cancelSignIn",{"profileId"}},{"openBrowser",{"profileId"}},{"signOut",{"profileId"}},{"createFolder",{"profileId"}},{"test",{"profileId"}},{"unlock",{"profileId"}}};
  static const QStringList fieldNames{"server","folder","address","linkBase","allowInsecureHttp"};
  const auto action=p["action"].toString();
  bool valid=p["action"].isString()&&allowed.contains(action);
  for(auto it=p.begin();valid&&it!=p.end();++it)valid=it.key()=="action"||allowed.value(action).contains(it.key());
  if(valid&&action=="set")valid=fieldNames.contains(p["field"].toString())&&p["value"].isString()&&p["value"].toString().size()<=2048;
  if(valid&&action=="add")valid=string(p,"label",128)&&(p["provider"]=="nextcloud"||p["provider"]=="webdav");
  if(valid&&action=="appPassword")valid=p["value"]=="show"||p["value"]=="hide";
  if(valid&&p.contains("profileId"))valid=string(p,"profileId",64);
  if(!valid){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("davDestination",p,std::move(reply));return;
 }
 if(method=="action.invoke") {
  if(!fields(p,{"action"},{"source","path","mode","delaySeconds","sensitiveText"})||!string(p,"action")||(p.contains("source")&&!string(p,"source",32))||(p.contains("path")&&!string(p,"path",4096))||(p.contains("mode")&&(p["action"]!="record"||!string(p,"mode",16)))){reply(invalid(),{});return;}
  auto options=p;options.remove("action");operations_.invoke(p["action"].toString(),reply,options);return;
 }
 // "Upload with put.gd" from a file manager (#1105): absolute paths only; the
 // client resolves URIs first. A menu entry's client names its installation and
 // the resident refuses another's; the CLI and MCP name none.
 if(method=="upload.files") {
  if(!fields(p,{"paths"},{"installation","profileId","sensitiveText"})||!p["paths"].isArray()||p["paths"].toArray().isEmpty()||p["paths"].toArray().size()>10000
     ||(p.contains("installation")&&!QStringList{"system","appimage"}.contains(p["installation"].toString()))||(p.contains("sensitiveText")&&p["sensitiveText"]!="prompt"&&p["sensitiveText"]!="report")||(p.contains("profileId")&&!string(p,"profileId",64))){reply(invalid(),{});return;}
  QStringList paths;for(const auto& value:p["paths"].toArray()){if(!value.isString()||value.toString().isEmpty()||value.toString().size()>4096||!QDir::isAbsolutePath(value.toString())){reply(invalid(),{});return;}paths.append(value.toString());}
  // #1804: put.gd's own menu entry (it names its installation) asks a person; the CLI and MCP get operation.decide.
  const auto sensitive=p["sensitiveText"].toString(p.contains("installation")?QStringLiteral("prompt"):QStringLiteral("report"));
  operations_.uploadFiles(paths,p["profileId"].toString(),p["installation"].toString(),std::move(reply),sensitive);return;
 }
 if(method=="editor.open") {
  if(!fields(p,{},{"source","path"})||(p.contains("source")&&!string(p,"source",32))||(p.contains("path")&&!string(p,"path",4096))){reply(invalid(),{});return;}
  operations_.invoke("edit",reply,p);return;
 }
 // Agent control of the one open editor; the helper validates each command.
 if(QStringList{"editor.inspect","editor.apply","editor.preview","editor.saveCopy","editor.copy","editor.fileDialog","editor.submit","editor.cancel","editor.raise"}.contains(method)) {
  if(!string(p,"id")){reply(invalid(),{});return;}
  auto params=p;params.remove("id");operations_.editorCommand(p["id"].toString(),method.mid(7),params,reply);return;
 }
 // The editor window's view and selection controls, one per call; the helper checks the values.
 if(method=="editor.view") {
  static const QStringList actions{"inspect","tool","select","selectNext","selectPrevious","selectAffectedText","removeMark","removeSuggestions","place","geometry","applyCrop","cancelCrop","zoom","fit","actualSize","pan","showSavedCopy","copySavedPath","dismissSavedCopy","discard","keepEditing","confirmDiscard","keyboardHelp","geometryDetails","saveDetails","propertiesWidth","blurStyle","stepNumber"};
  if(!fields(p,{"id"},{"action","tool","shapeId","rect","percent","delta","pixels","style","number"})||!string(p,"id")||(p.contains("action")&&(!string(p,"action",32)||!actions.contains(p["action"].toString())))
     ||(p.contains("tool")&&!string(p,"tool",16))||(p.contains("style")&&!string(p,"style",16))||(p.contains("number")&&!p["number"].isDouble())||(p.contains("shapeId")&&!p["shapeId"].isNull()&&!p["shapeId"].isDouble())
     ||(p.contains("rect")&&(!p["rect"].isArray()||p["rect"].toArray().size()!=4))||(p.contains("percent")&&!p["percent"].isDouble())||(p.contains("pixels")&&!p["pixels"].isDouble())
     ||(p.contains("delta")&&(!p["delta"].isArray()||p["delta"].toArray().size()!=2))){reply(invalid(),{});return;}
  auto params=p;params.remove("id");operations_.editorCommand(p["id"].toString(),"view",params,reply);return;
 }
 if(method.startsWith("scroll.")){operations_.scrollCommand(method.mid(7),p,reply);return;}
 // Text recognition and QR codes (#1428). Every method of the agent contract is
 // registered and its fields are checked here (schemas/text-recognition.schema.json
 // states the same shapes).
 const auto wholeNumber=[&p](const QString& key,qint64 minimum,qint64 maximum){const auto value=p[key];return value.isDouble()&&value.toDouble()==double(value.toInteger())&&value.toInteger()>=minimum&&value.toInteger()<=maximum;};
 if(method=="text.recognize") {
  const auto source=p["source"].toString();const bool screen=QStringList{"region","window","monitor","desktop"}.contains(source);QStringList languages;
  bool valid=fields(p,{"source"},{"path","historyId","languages","copy","delaySeconds"})&&p["source"].isString()&&TextService::sources().contains(source)
   &&(source=="file")==p.contains("path")&&(!p.contains("path")||(string(p,"path",4096)&&QDir::isAbsolutePath(p["path"].toString())))
   &&(source=="history")==p.contains("historyId")&&(!p.contains("historyId")||string(p,"historyId"))
   &&(!p.contains("copy")||p["copy"].isBool())&&(!p.contains("delaySeconds")||(screen&&wholeNumber("delaySeconds",0,10)&&Settings::captureDelayChoices().contains(p["delaySeconds"].toInt())))
   &&(!p.contains("languages")||p["languages"].isArray());
  if(valid&&p.contains("languages")){for(const auto& language:p["languages"].toArray()){valid=valid&&language.isString();languages.append(language.toString());}valid=valid&&textjob::validLanguages(languages);}
  if(!valid){reply(invalid(),{});return;}
  // An agent's recognition keeps its result for text.inspect; copying is its choice.
  auto params=p;params["keepResult"]=true;
  operations_.recognizeText(params,reply);return;
 }
 if(method=="text.inspect"||method=="text.cancel") {
  if(!fields(p,{"id"})||!string(p,"id")){reply(invalid(),{});return;}
  operations_.textCommand(method.mid(5),p,reply);return;
 }
 if(method=="editor.recognize") {
  if(!fields(p,{"id"})||!string(p,"id")){reply(invalid(),{});return;}
  {auto params=p;params.remove("id");operations_.editorCommand(p["id"].toString(),method.mid(7),params,reply);}return;
 }
 // `result` is the recognition result's identity from editor.inspect; one that
 // is no longer current is refused by the editor with edit_stale.
 if(method=="editor.selectText") {
  const bool range=p.contains("from")||p.contains("to");
  if(!fields(p,{"id","result"},{"from","to","all","none"})||!string(p,"id")||!wholeNumber("result",1,2147483647)||(range?1:0)+(p.contains("all")?1:0)+(p.contains("none")?1:0)!=1
     ||(range&&(!wholeNumber("from",0,textjob::maximumWords-1)||!wholeNumber("to",0,textjob::maximumWords-1)))||(p.contains("all")&&p["all"]!=true)||(p.contains("none")&&p["none"]!=true)){reply(invalid(),{});return;}
  {auto params=p;params.remove("id");operations_.editorCommand(p["id"].toString(),method.mid(7),params,reply);}return;
 }
 if(method=="editor.copyText") {
  if(!fields(p,{"id","result"},{"all"})||!string(p,"id")||!wholeNumber("result",1,2147483647)||(p.contains("all")&&!p["all"].isBool())){reply(invalid(),{});return;}
  {auto params=p;params.remove("id");operations_.editorCommand(p["id"].toString(),method.mid(7),params,reply);}return;
 }
 if(method=="editor.copyCode"||method=="editor.openCode") {
  if(!fields(p,{"id","result","index"})||!string(p,"id")||!wholeNumber("result",1,2147483647)||!wholeNumber("index",0,textjob::maximumCodes-1)){reply(invalid(),{});return;}
  {auto params=p;params.remove("id");operations_.editorCommand(p["id"].toString(),method.mid(7),params,reply);}return;
 }
 if(method=="history.copyText") {
  if(!fields(p,{"id"},{"keepResult"})||!string(p,"id")||(p.contains("keepResult")&&!p["keepResult"].isBool())){reply(invalid(),{});return;}
  // History's Copy text: the retained local copy, one clipboard write; counts only unless asked to keep the result.
  operations_.recognizeText({{"source","history"},{"historyId",p["id"]},{"copy",true},{"keepResult",p["keepResult"].toBool()}},reply);return;
 }
 // save needs an absolute path and may overwrite; copy and show take neither.
 if(method=="history.qr") {
  const auto mode=p["mode"].toString();const bool save=mode=="save";
  if(!fields(p,{"id","mode"},{"path","overwrite"})||!string(p,"id")||!p["mode"].isString()||!QStringList{"copy","save","show"}.contains(mode)||save!=p.contains("path")
     ||(save&&(!string(p,"path",4096)||!QDir::isAbsolutePath(p["path"].toString())))||(p.contains("overwrite")&&(!save||!p["overwrite"].isBool()))){reply(invalid(),{});return;}
  // show opens the code in the History window, which the user (or ui.show) opened.
  QPointer<LocalServer> guarded(this);
  const auto show=[guarded](const QJsonObject& link,JsonReply done){if(!guarded||!guarded->uiCommand_){done({"ui_unavailable","This resident has no graphical interface",{}},{});return;}guarded->uiCommand_("historyQr",link,std::move(done));};
  history_.qr(p["id"].toString(),mode,p["path"].toString(),p["overwrite"].toBool(),reply,uiCommand_?HistoryService::QrShow(show):HistoryService::QrShow{});return;
 }
 // Screen recording: one session, backend-neutral; the service validates semantics.
 if(method=="recording.capabilities") {if(!p.isEmpty())reply(invalid(),{});else operations_.recordingCommand("capabilities",{},reply);return;}
 if(method=="recording.windows") {if(!p.isEmpty())reply(invalid(),{});else operations_.recordingCommand("windows",{},reply);return;}
 if(method=="recording.microphones") {if(!fields(p,{},{"device"})||(p.contains("device")&&!string(p,"device",256))){reply(invalid(),{});return;}operations_.recordingCommand("microphones",p,reply);return;}
 if(method=="recording.prepare") {
  if(!fields(p,{"target","format"},{"codec","fps","adaptiveFps","audio","clickHighlights","maxEdge"})||!p["target"].isObject()||!string(p,"format",8)||(p.contains("codec")&&!string(p,"codec",8))||(p.contains("audio")&&!p["audio"].isObject())||(p.contains("adaptiveFps")&&!p["adaptiveFps"].isBool())||(p.contains("clickHighlights")&&!p["clickHighlights"].isBool())){reply(invalid(),{});return;}
  operations_.recordingCommand("prepare",p,reply);return;
 }
 if(QStringList{"recording.start","recording.pause","recording.resume","recording.stop","recording.cancel","recording.inspect","recording.raise"}.contains(method)) {
  if(!fields(p,{"id"})||!string(p,"id")){reply(invalid(),{});return;}
  operations_.recordingCommand(method.mid(10),p,reply);return;
 }
 if(method=="recording.select") {if(!fields(p,{"id","rect"})||!string(p,"id")||!p["rect"].isArray()||p["rect"].toArray().size()!=4){reply(invalid(),{});return;}operations_.recordingCommand("select",p,reply);return;}
 if(method=="recording.overlay") {if(!fields(p,{"id","visible"})||!string(p,"id")||!p["visible"].isBool()){reply(invalid(),{});return;}operations_.recordingCommand("overlay",p,reply);return;}
 // #1364: the ready bar's audio toggles. At least one of the two, each a boolean.
 if(method=="recording.audio") {if(!fields(p,{"id"},{"system","microphone"})||!string(p,"id")||(!p.contains("system")&&!p.contains("microphone"))||(p.contains("system")&&!p["system"].isBool())||(p.contains("microphone")&&!p["microphone"].isBool())){reply(invalid(),{});return;}operations_.recordingCommand("audio",p,reply);return;}
 if(method=="recording.edit") {if(!fields(p,{"id","revision","commands"})||!string(p,"id")||!p["revision"].isDouble()||!p["commands"].isArray()||p["commands"].toArray().size()>100){reply(invalid(),{});return;}operations_.recordingCommand("edit",p,reply);return;}
 if(method=="recording.preview") {if(!fields(p,{"id","time"},{"maxEdge"})||!string(p,"id")||!p["time"].isDouble()||(p.contains("maxEdge")&&!p["maxEdge"].isDouble())){reply(invalid(),{});return;}operations_.recordingCommand("preview",p,reply);return;}
 if(method=="recording.review") {
  // #283: one review-window control per call; the helper checks the values.
  static const QStringList actions{"inspect","seek","play","pause","speed","setIn","setOut","resetTrim","addMask","selectMask","setMaskIn","setMaskOut","strength","removeMask","format","saveCopyDialog","closeFileDialog","placeMask","useSolidCover","trimToLimit","dismissStatus","showAdvice","showInFolder","copyPath","dismissSavedCopy","retryExport","retryPreview","discard","keepReviewing","confirmDiscard",
   // #1613: the redesigned window's controls.
   "selectRange","clearSelection","cut","selectCut","adjustCut","restoreCut","playAcross","undo","redo","armRedact","armBlur","cancelPlacement","selectEdit","nextEdit","previousEdit","seekCoverStart","seekCoverEnd","setCoverEdgeToPlayhead","setCoverEdge","coverKind","timelineZoom","timelineFit","viewerZoom","viewerPan","frameStep","skim","skimEnd","panel","resize","setTrim",
   // The review window's feel controls (S, N, = / -, a cover bar's slide, the "+n" chip, [ ], Up / Down).
   "setSkimming","setSnapping","zoomIn","zoomOut","retimeCover","moreCovers","markSelection","editPoint",
   // A cut's Set to playhead and the overview track.
   "setCutEdge","scrollTimeline"};
  // The numbers are plain JSON numbers (the window checks ranges and whole values); the strings are bounded.
  const auto number=[&](const char* key){return !p.contains(key)||p[key].isDouble();};
  if(!fields(p,{"id","action"},{"time","speed","kind","rect","maskId","strength","format","codec","start","end","cutId","editId","edge","level","mode","dx","dy","delta","open","width","height","on","at"})||!string(p,"id")||!string(p,"action",32)||!actions.contains(p["action"].toString())
     ||(p.contains("time")&&!p["time"].isDouble())||(p.contains("speed")&&!p["speed"].isDouble())||(p.contains("kind")&&!string(p,"kind",8))
     ||(p.contains("rect")&&(!p["rect"].isArray()||p["rect"].toArray().size()!=4))||(p.contains("maskId")&&!p["maskId"].isNull()&&!string(p,"maskId"))
     ||(p.contains("strength")&&!p["strength"].isDouble())||(p.contains("format")&&!string(p,"format",8))||(p.contains("codec")&&!p["codec"].isString())
     ||!number("start")||!(number("end")||p["end"].isNull())||(p.contains("cutId")&&!string(p,"cutId"))||(p.contains("editId")&&!string(p,"editId"))||(p.contains("edge")&&!string(p,"edge",8))
     ||!number("level")||(p.contains("mode")&&!string(p,"mode",8))||!number("dx")||!number("dy")||!number("delta")||(p.contains("open")&&!p["open"].isBool())||(p.contains("on")&&!p["on"].isBool())||(p.contains("at")&&(!p["at"].isArray()||p["at"].toArray().size()!=2))||!number("width")||!number("height")){reply(invalid(),{});return;}
  operations_.recordingCommand("review",p,reply);return;
 }
 if(method=="recording.saveCopy") {if(!fields(p,{"id","path"},{"overwrite"})||!string(p,"id")||!string(p,"path",4096)||(p.contains("overwrite")&&!p["overwrite"].isBool())){reply(invalid(),{});return;}operations_.recordingCommand("saveCopy",p,reply);return;}
 if(method=="recording.upload") {if(!fields(p,{"id","revision"})||!string(p,"id")||!p["revision"].isDouble()){reply(invalid(),{});return;}operations_.recordingCommand("upload",p,reply);return;}
 if(method=="operation.recent") {if(!p.isEmpty())reply(invalid(),{});else operations_.recent(reply);return;}
 // #1804, #1898: Redact and upload, Open in editor, Upload anyway or Cancel for a capture waiting in sensitive_text_found.
 if(method=="operation.decide") {
  if(!fields(p,{"id","choice"},{"prompt"})||!string(p,"id")||!string(p,"choice",16)||(p.contains("prompt")&&(!p["prompt"].isDouble()||p["prompt"].toDouble()!=p["prompt"].toInt()||p["prompt"].toInt()<1))){reply(invalid(),{});return;}
  operations_.decideSensitive(p["id"].toString(),p["choice"].toString(),reply,p["prompt"].toInt());return;
 }
 if(method=="operation.cancel"&&p.contains("itemId")) {if(!fields(p,{"id","itemId"})||!string(p,"id")||!string(p,"itemId")) {reply(invalid(),{});return;}operations_.cancel(p["id"].toString(),reply,p["itemId"].toString());return;}
 if(method.startsWith("operation.")) {
  // operation.retry alone takes scope "sharing": finish wanted share links only (#1496).
  const bool scoped=method=="operation.retry"&&p.contains("scope");if(scoped&&p["scope"]!="sharing") {reply(invalid(),{});return;}
  if(!fields(p,{"id"},scoped?QStringList{"scope"}:QStringList{})||!string(p,"id")) {reply(invalid(),{});return;}const auto id=p["id"].toString();if(method=="operation.inspect")operations_.inspect(id,reply);else if(method=="operation.cancel")operations_.cancel(id,reply);else if(method=="operation.retry")operations_.retry(id,reply,scoped);else if(method=="operation.copy")operations_.copyResult(id,reply);else if(method=="operation.retryLocalSave")operations_.retryLocalSave(id,reply);else reply({"unknown_method","Unknown operation method",{}},{});return;}
 if(method=="history.share"||method=="history.revokeLink"||method=="history.inspectLink") {
  if(!fields(p,{"id"})||!string(p,"id")){reply(invalid(),{});return;}
  history_.changeSharing(p["id"].toString(),method=="history.share"?"create":method=="history.revokeLink"?"revoke":"inspect",{},std::move(reply));return;
 }
 if(method=="share.inspect") {if(!fields(p,{"id"})||!string(p,"id")){reply(invalid(),{});return;}history_.inspectSharing(p["id"].toString(),std::move(reply));return;}
 if(method=="history.list") {
  const QStringList allowed{"limit","offset","search","category","mime","profileId","uploadStatus","localStatus","remoteStatus","from","to"};if(!fields(p,{},allowed)) {reply(invalid(),{});return;}
  for(auto it=p.begin();it!=p.end();++it) {if(it.key()=="limit"||it.key()=="offset") {if(!it.value().isDouble()||std::floor(it.value().toDouble())!=it.value().toDouble()||it.value().toDouble()>INT_MAX||it.value().toDouble()<0) {reply(invalid(),{});return;}}else if(!it.value().isString()||it.value().toString().size()>256) {reply(invalid(),{});return;}}
  HistoryQuery q;q.limit=p["limit"].toInt(50);q.offset=p["offset"].toInt();q.search=p["search"].toString();q.category=p["category"].toString();q.mime=p["mime"].toString();q.profileId=p["profileId"].toString();q.uploadStatus=p["uploadStatus"].toString();q.localStatus=p["localStatus"].toString();q.remoteStatus=p["remoteStatus"].toString();q.from=p["from"].toString();q.to=p["to"].toString();history_.list(q,[this,reply](Error e,QJsonObject page){
   // History hides Retry and Finish share link while the row's operation still settles, whatever recent lists (#1578).
   auto records=page["records"].toArray();for(qsizetype i=0;i<records.size();++i){auto record=records[i].toObject();if(operations_.operationSettling(record.value("operationId").toString())){record["settling"]=true;records[i]=record;}}
   if(!e)page["records"]=records;reply(e,page);});return;
 }
 // History's counts: the same filters as history.list (no paging) and up to
 // 64 [from, to] day ranges; category totals ignore the filters.
 if(method=="history.counts") {
  const QStringList allowed{"search","category","mime","profileId","uploadStatus","localStatus","remoteStatus","from","to","days"};if(!fields(p,{},allowed)) {reply(invalid(),{});return;}
  QList<std::pair<QString,QString>> days;
  for(auto it=p.begin();it!=p.end();++it) {
   if(it.key()=="days") {
    if(!it.value().isArray()||it.value().toArray().size()>64) {reply(invalid(),{});return;}
    for(const auto& value:it.value().toArray()) {const auto day=value.toObject();if(!value.isObject()||!fields(day,{"from","to"})||!string(day,"from",64)||!string(day,"to",64)) {reply(invalid(),{});return;}days.append({day["from"].toString(),day["to"].toString()});}
   }else if(!it.value().isString()||it.value().toString().size()>256) {reply(invalid(),{});return;}
  }
  HistoryQuery q;q.search=p["search"].toString();q.category=p["category"].toString();q.mime=p["mime"].toString();q.profileId=p["profileId"].toString();q.uploadStatus=p["uploadStatus"].toString();q.localStatus=p["localStatus"].toString();q.remoteStatus=p["remoteStatus"].toString();q.from=p["from"].toString();q.to=p["to"].toString();history_.counts(q,days,reply);return;
 }
 if(method=="history.detail"||method=="history.preview"||method=="history.copyLink"||method=="history.unlockLinkCredentials"||method=="history.unlockCredentials"||method=="history.copyImage"||(method.startsWith("delete.")&&method!="delete.prepare"&&method!="delete.confirm")) {
  // history.preview alone takes an optional boolean retry (Retry preview).
  if(!fields(p,{"id"},method=="history.preview"?QStringList{"retry"}:QStringList{})||!string(p,"id")||(p.contains("retry")&&!p["retry"].isBool())) {reply(invalid(),{});return;}const auto id=p["id"].toString();if(method=="history.detail")history_.detail(id,[this,reply](Error e,QJsonObject data){if(!e&&operations_.operationSettling(data.value("operationId").toString()))data["settling"]=true;reply(e,data);});else if(method=="history.preview")history_.preview(id,reply,p["retry"].toBool());else if(method=="history.copyLink")history_.copyLink(id,reply);else if(method=="history.unlockLinkCredentials")history_.unlockLinkCredentials(id,reply);else if(method=="history.unlockCredentials")history_.unlockItemCredentials(id,reply);else if(method=="history.copyImage")history_.copyImage(id,reply);else if(method=="delete.inspect")history_.inspectDelete(id,reply);else if(method=="delete.retry")history_.retryDelete(id,reply);else if(method=="delete.cancel")history_.cancelDelete(id,reply);else reply({"unknown_method","Unknown deletion method",{}},{});return;
 }
 if(method=="history.copyLinks") {
  if(!fields(p,{"ids"})||!p["ids"].isArray()||p["ids"].toArray().isEmpty()||p["ids"].toArray().size()>1000) {reply(invalid(),{});return;}
  QStringList ids;for(const auto& id:p["ids"].toArray()) {if(!id.isString()||id.toString().isEmpty()||id.toString().size()>128||ids.contains(id.toString())) {reply(invalid(),{});return;}ids.append(id.toString());}
  history_.copyLinks(ids,reply);return;
 }
 // Remove from History only (#1759): local records, never a remote file.
 if(method=="history.forget") {
  if(!fields(p,{"ids"})||!p["ids"].isArray()||p["ids"].toArray().isEmpty()||p["ids"].toArray().size()>1000){reply(invalid(),{});return;}
  QStringList ids;for(const auto& id:p["ids"].toArray()){if(!id.isString()||id.toString().isEmpty()||id.toString().size()>138){reply(invalid(),{});return;}ids<<id.toString();}
  history_.forget(ids,std::move(reply));return;
 }
 if(method=="delete.prepare"||method=="delete.confirm") {
  if(!fields(p,method=="delete.confirm"?QStringList{"ids","acknowledgement"}:QStringList{"ids"})||!p["ids"].isArray()||p["ids"].toArray().size()>1000||(method=="delete.confirm"&&!string(p,"acknowledgement",64))) {reply(invalid(),{});return;}
  QStringList ids;for(const auto& id:p["ids"].toArray()) {if(!id.isString()||id.toString().isEmpty()||id.toString().size()>128) {reply(invalid(),{});return;}ids.append(id.toString());}
  if(method=="delete.prepare")history_.prepareDelete(ids,reply);else history_.deleteConfirmed(ids,p["acknowledgement"].toString(),reply);return;
 }
 if(QStringList{"setup.connectAccount","setup.inspectAccount","setup.createFolder","setup.storeSharePassword","setup.signOut","setup.trustHostKey"}.contains(method)) {
  const bool hasInput=method=="setup.createFolder"||method=="setup.storeSharePassword"||method=="setup.trustHostKey";
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
 if(method=="setup.inspect"||method=="setup.cancel") {
  if(!fields(p,{"id"})||!string(p,"id")) {reply(invalid(),{});return;}
  if(!setup_) {reply({"setup_unavailable","This resident has no setup service",{}},{});return;}
  if(method=="setup.cancel")setup_->cancel(p["id"].toString(),std::move(reply));else setup_->inspect(p["id"].toString(),std::move(reply));return;
 }
 // A diagnostics package, always saved locally first, then optionally
 // uploaded to a working destination (#74 §3, #123, docs/design/diagnostics.md).
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
  static const QStringList actions{"inspect","setDebugLogging","setDesktopDetails","copyDetails","showConfigFolder","refreshDesktopSupport","clearLogs","openPackage","setPackage","savePackage","showInFolder","closePackage"};
  if(!fields(p,{"action"},{"enabled","includeNames","folder","operation","upload"})||!actions.contains(p["action"].toString())||(p.contains("enabled")&&!p["enabled"].isBool())||(p.contains("includeNames")&&!p["includeNames"].isBool())||(p.contains("upload")&&!p["upload"].isBool())
     ||(p.contains("folder")&&(!string(p,"folder",4096)||!QDir::isAbsolutePath(p["folder"].toString())))||(p.contains("operation")&&!string(p,"operation",128))){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("diagnostics",p,std::move(reply));return;
 }
 // Drives the open History page: filters, selection and the compact details view.
 if(method=="ui.history") {
  static const QStringList actions{"inspect","clearFilters","retryLoading","copyDeletionDetails","select","clearSelection","openDetails","closeDetails","toggleDetails","nextPage","previousPage","goToPage","setFilter","removeFilter","showCategory","setView","copySelectedLinks","finishShareLink","deleteSelected","cancelConfirmation","expandPreview","closePreview","retryPreview","showSavedCopy","copySavedPath","dismissSavedCopy","cancelAll","confirmCancelAll","keepUploading","removeFromHistoryOnly","openErrorLog","dismissNotice","unlockDeletionCredentials"};const auto action=p["action"].toString();
  static const QStringList filterKeys{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus","exactRemoteStatus","includeCanceled"};
  // Each parameter belongs to exactly the actions that take it.
  if(!fields(p,{"action"},{"scope","ids","page","key","value","category","view"})||!actions.contains(action)||(p.contains("scope")&&(action!="clearFilters"||!QStringList{"filters","all"}.contains(p["scope"].toString())))
     ||(action=="select")!=p.contains("ids")||(p.contains("ids")&&(!p["ids"].isArray()||p["ids"].toArray().isEmpty()||p["ids"].toArray().size()>1000))
     ||(action=="goToPage")!=p.contains("page")||(p.contains("page")&&(!p["page"].isDouble()||std::floor(p["page"].toDouble())!=p["page"].toDouble()||p["page"].toDouble()<1||p["page"].toDouble()>INT_MAX))
     ||(action=="setFilter"||action=="removeFilter")!=p.contains("key")||(p.contains("key")&&!filterKeys.contains(p["key"].toString()))
     ||(action=="setFilter")!=p.contains("value")||(p.contains("value")&&(!p["value"].isString()||p["value"].toString().size()>256))
     ||(action=="showCategory")!=p.contains("category")||(p.contains("category")&&!QStringList{"all","images","videos","text+other","failed"}.contains(p["category"].toString()))
     ||(action=="setView")!=p.contains("view")||(p.contains("view")&&!QStringList{"list","gallery"}.contains(p["view"].toString()))){reply(invalid(),{});return;}
  for(const auto& id:p["ids"].toArray())if(!id.isString()||id.toString().isEmpty()||id.toString().size()>138){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("history",p,std::move(reply));return;
 }
 // The error log (#1753): read, act on and clear its entries, and drive Settings → Error log.
 if(method.startsWith("errors.")&&!errorLog_){reply({"error_log_unavailable","This resident keeps no error log",{}},{});return;}
 if(method=="errors.list") {
  bool valid=fields(p,{},{"limit","offset","kind","search","resolved","historyId","operationId"})
   &&(!p.contains("limit")||(p["limit"].isDouble()&&std::floor(p["limit"].toDouble())==p["limit"].toDouble()&&p["limit"].toDouble()>=1&&p["limit"].toDouble()<=200))
   &&(!p.contains("offset")||(p["offset"].isDouble()&&std::floor(p["offset"].toDouble())==p["offset"].toDouble()&&p["offset"].toDouble()>=0&&p["offset"].toDouble()<=INT_MAX))
   &&(!p.contains("kind")||(p["kind"].isString()&&ErrorLog::kinds().contains(p["kind"].toString())))
   &&(!p.contains("search")||(p["search"].isString()&&p["search"].toString().size()<=256))
   &&(!p.contains("resolved")||p["resolved"].isBool())
   &&(!p.contains("historyId")||string(p,"historyId",138))&&(!p.contains("operationId")||string(p,"operationId",128));
  if(!valid){reply(invalid(),{});return;}
  ErrorLogQuery query;query.limit=p.contains("limit")?p["limit"].toInt():50;query.offset=p["offset"].toInt();query.kind=p["kind"].toString();query.search=p["search"].toString();
  query.resolved=p.contains("resolved")?int(p["resolved"].toBool()):-1;query.historyId=p["historyId"].toString();query.operationId=p["operationId"].toString();
  errorLog_->list(query,std::move(reply));return;
 }
 if(method=="errors.inspect") {
  if(!fields(p,{"id"})||!string(p,"id",64)){reply(invalid(),{});return;}
  errorLog_->inspect(p["id"].toString(),std::move(reply));return;
 }
 if(method=="errors.act") {
  static const QStringList actions{"unlock_credentials","retry","save_diagnostics","remove_from_history"};
  if(!fields(p,{"id","action"})||!string(p,"id",64)||!p["action"].isString()||!actions.contains(p["action"].toString())){reply(invalid(),{});return;}
  QPointer<LocalServer> guarded(this);const auto action=p["action"].toString();
  errorLog_->inspect(p["id"].toString(),[guarded,action,reply=std::move(reply)](Error error,QJsonObject entry){
   if(error){reply(error,{});return;}
   if(!guarded){reply({"error_log_unavailable","This resident keeps no error log",{}},{});return;}
   guarded->actOnError(entry,action,std::move(reply));
  });return;
 }
 if(method=="errors.clear") {
  if(!fields(p,{},{"ids","resolvedOnly"})||(p.contains("resolvedOnly")&&!p["resolvedOnly"].isBool())||(p.contains("ids")&&(!p["ids"].isArray()||p["ids"].toArray().isEmpty()||p["ids"].toArray().size()>500))){reply(invalid(),{});return;}
  QStringList ids;for(const auto& id:p["ids"].toArray()){if(!id.isString()||id.toString().isEmpty()||id.toString().size()>64){reply(invalid(),{});return;}ids<<id.toString();}
  errorLog_->clear(ids,p["resolvedOnly"].toBool(),std::move(reply));return;
 }
 // A failure put.gd's own window found itself (a file copy, a browser that would not
 // open): recorded like any other, and answered with the entry's ID for its link.
 if(method=="errors.record") {
  static const QStringList kinds=ErrorLog::kinds();
  static const QRegularExpression codePattern("^[a-z][a-z0-9_]{0,63}$"),idPattern("^[A-Za-z0-9][A-Za-z0-9._:-]{0,137}$");
  const auto identifier=[&p](const char* key){return !p.contains(QLatin1String(key))||(p[QLatin1String(key)].isString()&&idPattern.match(p[QLatin1String(key)].toString()).hasMatch());};
  if(!fields(p,{"kind","code","message"},{"historyId","operationId","profileId","itemName"})||!p["kind"].isString()||!kinds.contains(p["kind"].toString())||!string(p,"code",64)||!codePattern.match(p["code"].toString()).hasMatch()||!string(p,"message",4096)
     ||!identifier("historyId")||!identifier("operationId")||!identifier("profileId")||(p.contains("itemName")&&!string(p,"itemName",512))){reply(invalid(),{});return;}
  ErrorLog::Entry entry;entry.kind=p["kind"].toString();entry.code=p["code"].toString();entry.message=p["message"].toString();entry.historyId=p["historyId"].toString();entry.itemId=entry.historyId;
  entry.operationId=p["operationId"].toString();entry.profileId=p["profileId"].toString();entry.itemName=p["itemName"].toString();
  entry.subject=entry.kind+":"+entry.code+":"+(entry.historyId.isEmpty()?entry.operationId:entry.historyId);
  const auto id=errorLog_->record(entry);
  if(id.isEmpty()){reply({"error_log_full","The error log can't keep up; this entry wasn't saved",{}},{});return;}
  reply({},{{"id",id}});return;
 }
 // Drives the open Settings → Error log page the way a user does.
 if(method=="ui.errorLog") {
  static const QStringList actions{"inspect","refresh","select","step","setFilter","clear","confirmClear","cancelClear","confirmRemove","cancelRemove","nextPage","previousPage"};
  static const QStringList steps{"unlock_credentials","open_destination","retry","remove_from_history","show_in_history","save_diagnostics"};
  const auto action=p["action"].toString();
  if(!fields(p,{"action"},{"id","step","kind","search","showResolved","resolvedOnly"})||!p["action"].isString()||!actions.contains(action)
     ||(action=="select")!=p.contains("id")||(p.contains("id")&&!string(p,"id",64))
     ||(action=="step")!=p.contains("step")||(p.contains("step")&&(!p["step"].isString()||!steps.contains(p["step"].toString())))
     ||(action!="setFilter"&&(p.contains("kind")||p.contains("search")||p.contains("showResolved")))
     ||(p.contains("kind")&&(!p["kind"].isString()||(!p["kind"].toString().isEmpty()&&!ErrorLog::kinds().contains(p["kind"].toString()))))
     ||(p.contains("search")&&(!p["search"].isString()||p["search"].toString().size()>256))||(p.contains("showResolved")&&!p["showResolved"].isBool())
     ||(p.contains("resolvedOnly")&&(action!="clear"||!p["resolvedOnly"].isBool()))){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("errorLog",p,std::move(reply));return;
 }
 if(method=="ui.historySaveDiagnostics") {
  if(!fields(p,{"id"})||!string(p,"id",128)){reply(invalid(),{});return;}
  if(!uiCommand_){reply({"ui_unavailable","This resident has no graphical interface",{}},{});return;}
  uiCommand_("historySaveDiagnostics",p,std::move(reply));return;
 }
 // #259: open, inspect or cancel a file chooser in the open put.gd window, as its
 // button opens it: the desktop's own through the XDG portal, or the fallback.
 if(method=="ui.fileDialog") {
  static const QStringList dialogs{"editImage","historySaveAs","outputFolder","diagnosticsFolder","sftpKey"};
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
 if(method=="settings.importUploader") {
  if(!uploaderImport_) {reply({"import_unavailable","This resident cannot import an uploader file",{}},{});return;}
  uploaderImport_(p,std::move(reply));return;
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
 if(method=="ui.snapshot"){
  QJsonObject snapshot{{"build",QJsonDocument::fromJson(buildManifestJson()).object()},{"residentEpoch",residentEpoch_},{"operationsReady",operations_.ready()},{"settings",QJsonObject{{"document",settings_.document()},{"metadata",settings_.metadata()}}},{"diagnostics",uiDiagnostics_?uiDiagnostics_():operations_.diagnostics()}};
  // Why the journal is unavailable, while it is: History shows this instead of a bare wait (#1576).
  if(!operations_.ready())snapshot["operationsError"]=operations_.notReadyError().toJson();
  // A changed generation tells an open window that a credential read or store ended (#600).
  if(setup_)snapshot["credentials"]=QJsonObject{{"generation",double(setup_->credentialGeneration())}};
  if(destinationStatus_)snapshot["destinationStatus"]=destinationStatus_->publicJson();
  // The error log's counts and revision (#1753): an open Error log page reloads when it changes.
  if(errorLog_)snapshot["errorLog"]=errorLog_->summary();
  reply({},snapshot);
 }
 else if(method=="ui.viewState"){if(uiViewState_)reply({},uiViewState_());else reply({"ui_unavailable","This resident has no interface view state",{}},{});}
 // effectiveLimits: the resolved byte limits (null for none) and the disk that bounds imports.
 else if(method=="settings.get"){auto limits=settings_.snapshot().effectiveArtifactLimits();limits["disk"]=operations_.artifactDisk();QJsonObject result{{"document",settings_.document()},{"metadata",settings_.metadata()},{"effectiveLimits",limits}};if(destinationStatus_)result["destinationStatus"]=destinationStatus_->publicJson();reply({},result);}
 else if(method=="settings.developmentTemplate")reply({},{{"document",Settings::developmentTemplate()},{"explicitImportRequired",true}});
 else if(method=="settings.reload") {QPointer<LocalServer> guarded(this);settings_.reloadAsync([guarded,reply](Result result) {if(guarded)reply(result.error,guarded->settings_.metadata());});}
 else if(method=="capabilities")reply({},operations_.capabilities());
 else if(method=="desktop.setup")reply({},operations_.desktopSetup());
 else if(method=="desktop.setup.recheck")operations_.recheckDesktopSetup([reply](QJsonObject result){reply({},result);});
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

// Which session methods are a person's actions whose errors belong in the error log, by
// the kind of action (#1753). Read-only calls (history.detail, delete.inspect, the
// inspect and list methods) and the windows' own drivers are left out.
void LocalServer::noteOutcome(const QString& method,const QJsonObject& p,Error& error) {
 if(!errorLog_)return;
 static const QHash<QString,QString> kinds{
  {"history.copyLink","copy"},{"history.copyLinks","copy"},{"history.copyImage","copy"},{"history.qr","copy"},{"operation.copy","copy"},
  {"history.copyText","text_recognition"},{"text.recognize","text_recognition"},
  {"history.share","share_link"},{"history.revokeLink","share_link"},{"history.openLink","share_link"},
  {"history.unlockLinkCredentials","credentials"},{"history.unlockCredentials","credentials"},{"setup.unlockCredentials","credentials"},{"setup.storeCredentials","credentials"},{"setup.storeSharePassword","credentials"},
  {"setup.testDestination","destination_test"},{"setup.connectAccount","destination_setup"},{"setup.createFolder","destination_setup"},{"setup.signOut","destination_setup"},{"setup.trustHostKey","destination_setup"},
  {"delete.prepare","deletion"},{"delete.confirm","deletion"},{"delete.retry","deletion"},{"delete.cancel","deletion"},
  {"operation.retry","upload"},{"operation.retryLocalSave","local_save"},{"operation.cancel","upload"},{"upload.files","upload"},
  {"recording.start","recording"},{"recording.stop","recording"},{"recording.upload","recording"},{"recording.saveCopy","recording"},
  {"diagnostics.bundle","diagnostics"},{"settings.save","settings_save"},{"settings.patch","settings_save"},{"settings.reorderProfiles","settings_save"},{"settings.importUploader","settings_save"},
  {"history.clear","history"},{"history.forget","history"},{"cache.clear","history"},{"action.invoke",""}};
 if(!kinds.contains(method))return;
 // A diagnostics preview writes nothing; its refusal is the dialog's own message.
 if(method=="diagnostics.bundle"&&p["preview"].toBool())return;
 const auto kind=method=="action.invoke"?ErrorLog::kindForAction(p["action"].toString()):kinds.value(method);
 QString key;
 if(method.startsWith("history.")||method.startsWith("operation.")||method.startsWith("recording.")||method=="delete.retry"||method=="delete.cancel")key=p["id"].toString();
 else if(method=="delete.prepare"||method=="delete.confirm"){key=error.details.value("historyId").toString();if(key.isEmpty()&&p["ids"].toArray().size()==1)key=p["ids"].toArray().first().toString();}
 else if(method.startsWith("setup."))key=p["profileId"].toString();
 // Remove from History only takes a list; an entry names its one record.
 if(method=="history.forget")key=error.details.value("historyId").toString(p["ids"].toArray().size()==1?p["ids"].toArray().first().toString():QString());
 else if(method=="action.invoke")key=p["action"].toString();
 else if(method=="text.recognize")key=p["historyId"].toString(p["source"].toString());
 else if(method=="upload.files")key=p["profileId"].toString();
 // A settings save carries the whole document, so a later save that works says nothing about the
 // field an earlier one refused: those entries are never resolved here, only kept per field.
 const bool settings=method.startsWith("settings.");
 const auto subject=settings?"settings:"+error.details.value("field").toString(error.details.value("pointer").toString(error.details.value("path").toString())):method+':'+key;
 if(!error){if(!settings)errorLog_->resolve(subject,"It worked when tried again");return;}
 if(!ErrorLog::userFacing(error))return;
 ErrorLog::Entry context;context.subject=subject;context.action=p["action"].toString();context.details={{"method",method}};
 if(method.startsWith("history.")||method=="delete.prepare"||method=="delete.confirm"||method=="history.copyText"){context.historyId=key;context.itemId=key;}
 if(method=="text.recognize"){context.historyId=p["historyId"].toString();context.itemId=context.historyId;}
 if(method.startsWith("operation.")||method.startsWith("recording.")){context.operationId=key;}
 if(method=="delete.retry"||method=="delete.cancel")context.deletionId=key;
 if(method.startsWith("setup.")||method=="upload.files")context.profileId=method.startsWith("setup.")?key:p["profileId"].toString();
 if(method=="history.unlockCredentials"||method=="history.unlockLinkCredentials")context.profileId=error.details.value("profileId").toString();
 if(const auto id=errorLog_->record(kind,error,context);!id.isEmpty())error.details["errorLogId"]=id;
}
// One of an entry's next steps, through the same session method its button stands for,
// so the step's own result (and any error it records) is what the caller gets.
void LocalServer::actOnError(const QJsonObject& entry,const QString& action,JsonReply reply) {
 const auto steps=entry["nextSteps"].toArray();
 if(!steps.contains(QJsonValue(action))){reply({"error_action_unavailable","This error log entry doesn't offer that step",{{"id",entry["id"]},{"nextSteps",steps}}},{});return;}
 const auto kind=entry["kind"].toString(),historyId=entry["historyId"].toString(),profileId=entry["profileId"].toString(),operationId=entry["operationId"].toString();
 const auto request=[session=socketPath()](const QString& method,const QJsonObject& params){return QJsonObject{{"schemaVersion",1},{"requestId","errors.act"},{"session",session},{"method",method},{"params",params}};};
 // Remove from History only (#1759): what History's deletion dialog offers for a removed destination,
 // refused as it is there: delete.prepare says whether the record may go, then history.forget takes it.
 if(action=="remove_from_history"){
  // Already gone (removed here or from History): answered without a preview that would log a new error.
  if(entry.contains("itemPresent")&&!entry["itemPresent"].toBool()){reply({"history_not_found","This item is no longer in History.",{{"historyId",historyId}}},{});return;}
  QPointer<LocalServer> guarded(this);const auto ids=QJsonArray{historyId};
  const auto answer=[id=entry["id"],reply](Error error,QJsonObject result){reply(error,{{"id",id},{"action","remove_from_history"},{"method","history.forget"},{"result",result}});};
  dispatch(request("delete.prepare",{{"ids",ids}}),[guarded,request,historyId,ids,answer](Error error,QJsonObject preview){
   if(error){answer(error,{});return;}
   const auto target=preview["targets"].toArray().first().toObject();
   if(!target["destinationRemoved"].toBool()){answer({"destination_not_removed","This file's destination is in Settings again, so put.gd can delete it there. Delete it from History instead.",{{"historyId",historyId}}},{});return;}
   const auto retainedBy=target["retainedBy"].toString();
   if(!target["forgettable"].toBool()){answer({"history_record_retained",retainedBy=="sharing"?QStringLiteral("This file can't be removed from History yet: a share link change hasn't finished. Check its link in History first."):QStringLiteral("This file can't be removed from History yet: an earlier deletion may have reached the destination. Check the file there."),{{"historyId",historyId},{"retainedBy",retainedBy}}},{});return;}
   if(!guarded){answer({"error_log_unavailable","This resident keeps no error log",{}},{});return;}
   guarded->dispatch(request("history.forget",{{"ids",ids}}),[historyId,answer](Error error,QJsonObject result){
    if(!error&&result["removed"].toInt()<1)error={"history_record_retained","This file can't be removed from History yet: a deletion or link change started meanwhile.",{{"historyId",historyId}}};
    answer(error,result);
   });
  });
  return;
 }
 QString method;QJsonObject params;
 if(action=="unlock_credentials"){
  if(!historyId.isEmpty()){method="history.unlockCredentials";params={{"id",historyId}};}
  else{method="setup.unlockCredentials";params={{"profileId",profileId},{"expectedRevision",settings_.revision()}};}
 }
 else if(action=="retry"){
  if(kind=="deletion"){method="delete.retry";params={{"id",entry["deletionId"]}};}
  else if(kind=="destination_test"){method="setup.testDestination";params={{"profileId",profileId},{"expectedRevision",settings_.revision()}};}
  else if(kind=="copy"){method="operation.copy";params={{"id",operationId}};}
  else if(kind=="local_save"){method="operation.retryLocalSave";params={{"id",operationId}};}
  else{method="operation.retry";params={{"id",operationId}};}
 }
 else{method="diagnostics.bundle";params={{"operation",operationId}};}
 dispatch(request(method,params),[method,action,id=entry["id"],reply=std::move(reply)](Error error,QJsonObject result){reply(error,{{"id",id},{"action",action},{"method",method},{"result",result}});});
}
}
