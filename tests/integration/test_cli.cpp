#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
#include "fixtures/FakeRecordingBackend.h"
#include "client/SessionClient.h"
#include <QLocalSocket>
#include <QLocalServer>
#include <QTimer>
#include <QProcess>
#include <QImage>
#include <QScopeGuard>
#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QDir>
#include <QFileInfo>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;
class CliContract:public QObject {
 Q_OBJECT
 static QJsonObject raw(const QString& path,const QByteArray& bytes) {QLocalSocket socket;socket.connectToServer(path);if(!socket.waitForConnected(1000))return {};socket.write(bytes);QElapsedTimer timer;timer.start();QByteArray response;while(timer.elapsed()<putgd::test::scaled(5000)&&!response.contains('\n')) {QCoreApplication::processEvents();response+=socket.readAll();putgd::test::settle(1);}return QJsonDocument::fromJson(response).object();}
private slots:
 void stdinParametersStayOutOfProcessArguments(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;
  OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);LocalServer server(settings,ops,history);QVERIFY(server.listen(d.filePath("runtime")));PUTGD_TRY_VERIFY(ops.ready());
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"--params-stdin","settings.patch"});QVERIFY(client.waitForStarted());
  client.write(QJsonDocument(QJsonObject{{"expectedRevision",settings.revision()},{"patch",QJsonObject{{"preferences",QJsonObject{{"notifications",false}}}}}}).toJson(QJsonDocument::Compact));client.closeWriteChannel();
  PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QVERIFY(!settings.snapshot().notifications);QVERIFY(QJsonDocument::fromJson(client.readAllStandardOutput()).object()["ok"].toBool());
  client.start(PUTGD_CLI_PATH,{"--params-stdin","capabilities","{}"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),1);
  client.start(PUTGD_CLI_PATH,{"--params-stdin","capabilities"});QVERIFY(client.waitForStarted());client.write(QByteArray(1024*1024+1,'x'));client.closeWriteChannel();PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(QJsonDocument::fromJson(client.readAllStandardOutput()).object()["error"].toObject()["code"].toString(),QString("request_too_large"));
 }

 void editorViewControlsCrossCliResidentAndHelper(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.payload.kind="image";source.payload.image=QImage(320,200,QImage::Format_RGB32);source.payload.image.fill(Qt::cyan);
  OperationService operations(d.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);const auto runtime=d.filePath("runtime");QVERIFY(QFile::encodeName(runtime+"/session.sock").size()<=107);QVERIFY(server.listen(runtime));PUTGD_TRY_VERIFY(operations.ready());
  auto request=[&](const QString& method,const QJsonObject& params){QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),method,QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))});(void)putgd::test::waitFor([&]{return client.state()==QProcess::NotRunning;});return QJsonDocument::fromJson(client.readAllStandardOutput()).object();};
  const auto invoked=request("action.invoke",{{"action","edit"}});QVERIFY2(invoked["ok"].toBool(),QJsonDocument(invoked).toJson().constData());const auto id=invoked["result"].toObject()["operationId"].toString();QVERIFY(!id.isEmpty());auto cleanup=qScopeGuard([&]{request("editor.cancel",{{"id",id}});});
  QJsonObject ready;PUTGD_TRY_VERIFY(([&]{ready=request("editor.view",{{"id",id},{"action","inspect"}});return ready["ok"].toBool();})());QVERIFY(!ready["result"].toObject()["keyboardHelp"].toObject()["open"].toBool());
  const auto opened=request("editor.view",{{"id",id},{"action","keyboardHelp"}});QVERIFY2(opened["ok"].toBool(),QJsonDocument(opened).toJson().constData());QVERIFY(opened["result"].toObject()["keyboardHelp"].toObject()["open"].toBool());QVERIFY(request("editor.view",{{"id",id},{"action","inspect"}})["result"].toObject()["keyboardHelp"].toObject()["open"].toBool());
  const auto closed=request("editor.view",{{"id",id},{"action","keyboardHelp"}});QVERIFY(closed["ok"].toBool());QVERIFY(!closed["result"].toObject()["keyboardHelp"].toObject()["open"].toBool());QCOMPARE(source.reads,1);
  const auto geometry=request("editor.view",{{"id",id},{"action","geometryDetails"}});QVERIFY2(geometry["ok"].toBool(),QJsonDocument(geometry).toJson().constData());QVERIFY(geometry["result"].toObject()["geometryDetails"].toObject()["open"].toBool());
  QVERIFY(request("editor.view",{{"id",id},{"action","geometryDetails"}})["ok"].toBool());
  // Requests retain the 200 px API floor; the reply reports the UI's bounded width.
  const auto panel=request("editor.view",{{"id",id},{"action","propertiesWidth"},{"pixels",220}});QVERIFY2(panel["ok"].toBool(),QJsonDocument(panel).toJson().constData());QCOMPARE(panel["result"].toObject()["propertiesWidth"].toInt(),240);
  const auto wider=request("editor.view",{{"id",id},{"action","propertiesWidth"},{"pixels",280}});QVERIFY2(wider["ok"].toBool(),QJsonDocument(wider).toJson().constData());const int bounded=wider["result"].toObject()["propertiesWidth"].toInt();QVERIFY(bounded>=240&&bounded<=280);
  QCOMPARE(request("editor.view",{{"id",id},{"action","inspect"}})["result"].toObject()["propertiesWidth"].toInt(),bounded);
  QVERIFY(!request("editor.view",{{"id",id},{"action","propertiesWidth"},{"pixels",199}})["ok"].toBool());
  const auto applied=request("editor.apply",{{"id",id},{"commands",QJsonArray{QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","text"},{"at",QJsonArray{310,190}},{"text","Clipped annotation"}}}}}}});QVERIFY2(applied["ok"].toBool(),QJsonDocument(applied).toJson().constData());
  const auto affected=request("editor.view",{{"id",id},{"action","selectAffectedText"}});QVERIFY2(affected["ok"].toBool(),QJsonDocument(affected).toJson().constData());QVERIFY(affected["result"].toObject()["selectedShapeId"].toDouble()>0);QVERIFY(affected["result"].toObject()["textOutsideImage"].toBool());
  const auto saved=request("editor.saveCopy",{{"id",id},{"path",d.filePath("synthetic-copy.png")}});QVERIFY2(saved["ok"].toBool(),QJsonDocument(saved).toJson().constData());
  QVERIFY(request("editor.view",{{"id",id}})["result"].toObject()["saveFeedbackVisible"].toBool());
  const auto failed=request("editor.saveCopy",{{"id",id},{"path",d.filePath("missing/denied.png")}});QVERIFY(!failed["ok"].toBool());
  auto details=request("editor.view",{{"id",id},{"action","saveDetails"}});QVERIFY2(details["ok"].toBool(),QJsonDocument(details).toJson().constData());QVERIFY(details["result"].toObject()["saveDetailsOpen"].toBool());
  details=request("editor.view",{{"id",id},{"action","saveDetails"}});QVERIFY(details["ok"].toBool());QVERIFY(!details["result"].toObject()["saveDetailsOpen"].toBool());
  const auto dismissed=request("editor.view",{{"id",id},{"action","dismissSavedCopy"}});QVERIFY2(dismissed["ok"].toBool(),QJsonDocument(dismissed).toJson().constData());QVERIFY(!dismissed["result"].toObject()["saveFeedbackVisible"].toBool());QCOMPARE(dismissed["result"].toObject()["lastSave"].toObject()["status"].toString(),QString("failed"));

 }
 void recordToggleWaitsForASlowStopThroughTheRealClient(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;
  OperationService ops(d.filePath("data"),settings,registry,source);auto owned=std::make_unique<FakeRecordingBackend>();auto* backend=owned.get();ops.setRecordingBackend(std::move(owned));
  HistoryService history(ops);LocalServer server(settings,ops,history);QVERIFY(server.listen(d.filePath("runtime")));PUTGD_TRY_VERIFY(ops.ready());
  auto request=[&](const QString& method,const QJsonObject& params){QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),method,QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))});(void)putgd::test::waitFor([&]{return client.state()==QProcess::NotRunning;});return QJsonDocument::fromJson(client.readAllStandardOutput()).object();};
  auto invoke=[&](const QString& mode){return request("action.invoke",{{"action","record"},{"mode",mode}});};
  const auto prepared=invoke("open");QVERIFY(prepared["ok"].toBool());const auto id=prepared["result"].toObject()["recordingId"].toString();QVERIFY(!id.isEmpty());
  PUTGD_TRY_COMPARE(request("recording.inspect",{{"id",id}})["result"].toObject()["state"].toString(),QString("ready"));QCOMPARE(invoke("toggle")["result"].toObject()["step"].toString(),QString("start"));
  backend->deferStop=true;QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"action.invoke","{\"action\":\"record\",\"mode\":\"toggle\"}"});PUTGD_TRY_VERIFY(backend->pendingStop);
  // Outlast action.invoke's former 15 s client budget, within the helper's 30 s.
  QTimer::singleShot(16000,Qt::PreciseTimer,&ops,[backend]{auto done=std::move(backend->pendingStop);done({},{{"durationUs",12000000.0},{"canvas",QJsonArray{1280,720}}});});
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,LocalServer::replyDeadlineMs("recording.stop")+1000);
  QCOMPARE(client.exitCode(),0);const auto stopped=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QVERIFY(stopped["ok"].toBool());
  QCOMPARE(stopped["result"].toObject()["step"].toString(),QString("stop"));QCOMPARE(stopped["result"].toObject()["stopReason"].toString(),QString("user"));
  client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"recording.inspect",QString("{\"id\":\"%1\"}").arg(id)});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);
  auto expected=stopped["result"].toObject();expected.remove("step");QCOMPARE(QJsonDocument::fromJson(client.readAllStandardOutput()).object()["result"].toObject(),expected);
  QVERIFY(request("recording.cancel",{{"id",id}})["ok"].toBool());
 }
 void historyImageEndpointAcceptsOnlyJournalId(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config/config.json"));Source source;OperationService operations(d.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);PUTGD_TRY_VERIFY(operations.ready());
  for(const auto& params:QList<QJsonObject>{QJsonObject{{"id","missing"}},QJsonObject{{"id","missing"},{"path","/synthetic/untrusted.png"}},QJsonObject{{"id",QJsonArray{}}}}){bool done=false;Error error;server.dispatch({{"schemaVersion",1},{"requestId","image"},{"session",server.socketPath()},{"method","history.copyImage"},{"params",params}},[&](Error e,QJsonObject){error=e;done=true;});PUTGD_TRY_VERIFY(done);QCOMPARE(error.code,params.size()==1&&params["id"].isString()?QString("history_not_found"):QString("invalid_request"));}QCOMPARE(source.copies,0);QCOMPARE(source.reads,0);
 }
 // #190: history.preview takes exactly one history ID and waits for the
 // decode: the CLI and History give it longer than other history methods.
 void historyPreviewEndpointAcceptsOnlyJournalId(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config/config.json"));Source source;OperationService operations(d.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);PUTGD_TRY_VERIFY(operations.ready());
  for(const auto& params:QList<QJsonObject>{QJsonObject{{"id","missing"}},QJsonObject{{"id","missing"},{"path","/synthetic/untrusted.mp4"}},QJsonObject{{"id",QJsonArray{}}},QJsonObject{}}){bool done=false;Error error;server.dispatch({{"schemaVersion",1},{"requestId","preview"},{"session",server.socketPath()},{"method","history.preview"},{"params",params}},[&](Error e,QJsonObject){error=e;done=true;});PUTGD_TRY_VERIFY(done);QCOMPARE(error.code,params.size()==1&&params["id"].isString()?QString("history_not_found"):QString("invalid_request"));}
  QCOMPARE(LocalServer::replyDeadlineMs("history.preview"),historyPreviewReplyMs);QCOMPARE(LocalServer::replyDeadlineMs("history.detail"),15000);
 }
 void credentialEntryIsRejectedBeforeConnecting() {
  QTemporaryDir d;QLocalServer server;server.setSocketOptions(QLocalServer::UserAccessOption);QVERIFY(server.listen(d.filePath("session.sock")));int connections=0;
  connect(&server,&QLocalServer::newConnection,this,[&]{++connections;auto* socket=server.nextPendingConnection();socket->setParent(&server);socket->disconnectFromServer();});
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.fullServerName(),"setup.storeCredentials","{\"accessKey\":\"synthetic-access\",\"secretKey\":\"synthetic-secret\"}"});
  PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),1);const auto output=client.readAllStandardOutput();const auto response=QJsonDocument::fromJson(output).object();
  QCOMPARE(response["error"].toObject()["code"].toString(),QString("credentials_require_settings"));QCOMPARE(connections,0);QVERIFY(!server.hasPendingConnections());QVERIFY(!output.contains("synthetic-access"));QVERIFY(!output.contains("synthetic-secret"));QVERIFY(client.readAllStandardError().isEmpty());
 }
 void credentialRevealIsRejectedBeforeConnecting() {
  QTemporaryDir dir;QLocalServer server;server.setSocketOptions(QLocalServer::UserAccessOption);QVERIFY(server.listen(dir.filePath("session.sock")));
  int connections=0;connect(&server,&QLocalServer::newConnection,this,[&]{++connections;});
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.fullServerName(),"setup.revealCredentialSecret"});
  PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),1);
  const auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();
  QCOMPARE(response["error"].toObject()["code"].toString(),QString("credentials_require_settings"));
  QCOMPARE(connections,0);QVERIFY(!server.hasPendingConnections());
  QVERIFY(client.readAllStandardError().isEmpty());
 }
 // #1105: the upload client tells a resident from before upload.files by this:
 // with no parameters, a method the resident lacks is unknown_method and one it
 // has is its own invalid_request. Builds before the feature answer the same way.
 void methodWithoutParametersSaysWhetherTheResidentKnowsIt(){
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);
  const auto code=[&](const QString& method,const QJsonObject& params){QString found;server.dispatch({{"schemaVersion",1},{"requestId","known"},{"session",server.socketPath()},{"method",method},{"params",params}},[&](Error error,QJsonObject){found=error.code;});return found;};
  QCOMPARE(code("upload.files",{}),QString("invalid_request"));
  QCOMPARE(code("upload.futureMethod",{}),QString("unknown_method"));
  QCOMPARE(code("upload.futureMethod",{{"paths",QJsonArray{"/a"}}}),QString("invalid_request"));
 }
 void quitAcknowledgesBeforeDeferredShutdownAndRejectsUnexpectedParameters(){
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);
  auto request=[&](const QJsonObject& params){return QJsonObject{{"schemaVersion",1},{"requestId","quit"},{"session",server.socketPath()},{"method","ui.quit"},{"params",params}};};
  Error error;server.dispatch(request({}),[&](Error value,QJsonObject){error=value;});QCOMPARE(error.code,QString("ui_unavailable"));
  int calls=0;bool acknowledged=false;server.setUiQuit([&]{QVERIFY(acknowledged);++calls;});
  server.dispatch(request({{"force",true}}),[&](Error value,QJsonObject){error=value;});QCOMPARE(error.code,QString("invalid_request"));QCoreApplication::processEvents();QCOMPARE(calls,0);
  server.dispatch(request({}),[&](Error value,QJsonObject result){error=value;acknowledged=true;QVERIFY(result["quitting"].toBool());QCOMPARE(calls,0);});QVERIFY(!error);QVERIFY(acknowledged);QCOMPARE(calls,0);PUTGD_TRY_COMPARE(calls,1);QCoreApplication::processEvents();QCOMPARE(calls,1);
 }
 void realClientReceivesQuitAcknowledgement(){
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(d.filePath("runtime")));int calls=0;server.setUiQuit([&]{++calls;});
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"ui.quit"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);const auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QVERIFY(response["ok"].toBool());QVERIFY(response["result"].toObject()["quitting"].toBool());QCOMPARE(calls,1);QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void uiShowWaitsForWindowAcknowledgement(){
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);
  std::function<void(Error)> acknowledge;server.setUiActivation([&](const QString& view,auto ack){QCOMPARE(view,QString("about"));acknowledge=std::move(ack);});
  bool replied=false;Error error;server.dispatch({{"schemaVersion",1},{"requestId","show"},{"session",server.socketPath()},{"method","ui.show"},{"params",QJsonObject{{"view","about"}}}},[&](Error e,QJsonObject){error=e;replied=true;});
  QVERIFY(acknowledge);QVERIFY(!replied);acknowledge({"ui_start_failed","Cannot start helper",{}});QVERIFY(replied);QCOMPARE(error.code,QString("ui_start_failed"));
 }
 void launcherActivationUsesValidatedResidentUiRequest() {
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(d.filePath("runtime")));QString view;
  server.setUiActivation([&](const QString& requested,std::function<void(Error)> ack){view=requested;ack({});});
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"ui.show","{\"view\":\"history\"}"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QCOMPARE(view,QString("history"));
  view.clear();client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"ui.show","{\"view\":\"destinations\"}"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QCOMPARE(view,QString("destinations"));
  const auto destinationReply=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QVERIFY(destinationReply["ok"].toBool());QCOMPARE(destinationReply["result"].toObject()["shown"].toString(),QString("destinations"));
  // Settings scrolled to Updates (#293).
  view.clear();client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"ui.show","{\"view\":\"updates\"}"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QCOMPARE(view,QString("updates"));
  view.clear();client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"ui.show","{\"view\":\"arbitrary\"}"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),1);QVERIFY(view.isEmpty());QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void backgroundLaunchDoesNotRaiseExistingResident() {
  QTemporaryDir d;QVERIFY(d.isValid());const auto runtime=d.filePath("runtime");QVERIFY(QDir().mkpath(runtime));QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(runtime+"/putgd"));
  QStringList views;server.setUiActivation([&](const QString& view,std::function<void(Error)> ack){views.append(view);ack({});});
  QProcess client;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("XDG_RUNTIME_DIR",runtime);environment.insert("QT_QPA_PLATFORM","offscreen");client.setProcessEnvironment(environment);
  client.start(PUTGD_APP_PATH,{"--background"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QVERIFY2(views.isEmpty(),"A duplicate background launch raised the resident UI");
  client.start(PUTGD_APP_PATH,{"--history"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QCOMPARE(views,QStringList{"history"});QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void secondLauncherReportsUiFailureAndCanRetry() {
  QTemporaryDir d;const auto runtime=d.filePath("runtime");QVERIFY(QDir().mkpath(runtime));QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(runtime+"/putgd"));
  int calls=0;server.setUiActivation([&](const QString& view,std::function<void(Error)> ack){QCOMPARE(view,QString("history"));++calls;ack(calls==1?Error{"ui_incompatible","synthetic-private-message",{{"private","synthetic-private-detail"}}}:Error{});});
  QProcess client;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("XDG_RUNTIME_DIR",runtime);environment.insert("QT_QPA_PLATFORM","offscreen");environment.insert("XDG_CONFIG_HOME",d.filePath("config"));environment.insert("XDG_DATA_HOME",d.filePath("data-home"));client.setProcessEnvironment(environment);
  client.start(PUTGD_APP_PATH,{"--history"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitStatus(),QProcess::NormalExit);QCOMPARE(client.exitCode(),1);QCOMPARE(calls,1);
  const auto error=client.readAllStandardError();QVERIFY(error.contains("ui_incompatible:"));QVERIFY(error.contains("Restart put.gd after pending uploads finish."));QVERIFY(!error.contains("synthetic-private"));QVERIFY(client.readAllStandardOutput().isEmpty());
  client.start(PUTGD_APP_PATH,{"--history"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QCOMPARE(calls,2);QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void secondLauncherRejectsInvalidResponses_data() {
  QTest::addColumn<QString>("variant");QTest::addColumn<QByteArray>("expectedCode");
  for(const auto* variant:{"json","schema","request-id","ok-type","wrong-view","result-type","error-type","success-error","empty-error","duplicate"})QTest::newRow(variant)<<QString::fromLatin1(variant)<<QByteArray("invalid_response");
  QTest::newRow("private-code")<<QString("private-code")<<QByteArray("ui_activation_failed");
  QTest::newRow("closed")<<QString("closed")<<QByteArray("transport_failed");
  QTest::newRow("partial")<<QString("partial")<<QByteArray("transport_failed");
  QTest::newRow("oversized")<<QString("oversized")<<QByteArray("response_too_large");
 }
 void secondLauncherRejectsInvalidResponses() {
  QFETCH(QString,variant);QFETCH(QByteArray,expectedCode);
  QTemporaryDir d;const auto runtime=d.filePath("runtime");QVERIFY(QDir().mkpath(runtime+"/putgd"));QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  const auto path=runtime+"/putgd/session.sock";QLocalServer server;server.setSocketOptions(QLocalServer::UserAccessOption);QVERIFY(server.listen(path));int requests=0;
  connect(&server,&QLocalServer::newConnection,this,[&]{auto* socket=server.nextPendingConnection();socket->setParent(&server);
   connect(socket,&QLocalSocket::readyRead,socket,[&,socket]{
    auto bytes=socket->property("request").toByteArray()+socket->readAll();socket->setProperty("request",bytes);if(!bytes.endsWith('\n'))return;
    QObject::disconnect(socket,&QLocalSocket::readyRead,socket,nullptr);++requests;const auto request=QJsonDocument::fromJson(bytes).object();QCOMPARE(request["session"].toString(),path);QCOMPARE(request["method"].toString(),QString("ui.show"));
    auto response=LocalServer::response(request["requestId"].toString(),{},{{"shown","history"}});
    if(variant=="schema")response["schemaVersion"]=2;
    if(variant=="request-id")response["requestId"]="another-request";
    if(variant=="ok-type")response["ok"]="true";
    if(variant=="wrong-view")response["result"]=QJsonObject{{"shown","settings"}};
    if(variant=="result-type")response["result"]=QJsonArray{};
    if(variant=="error-type")response["error"]=QJsonArray{};
    if(variant=="success-error")response["error"]=Error{"ui_incompatible","synthetic-private-message",{}}.toJson();
    if(variant=="empty-error")response["ok"]=false;
    if(variant=="private-code")response=LocalServer::response(request["requestId"].toString(),{"synthetic-private-code","synthetic-private-message",{{"private","synthetic-private-detail"}}},{});
    QByteArray reply=QJsonDocument(response).toJson(QJsonDocument::Compact)+'\n';
    if(variant=="json")reply="{synthetic-private-invalid\n";
    if(variant=="duplicate")reply+=reply;
    if(variant=="closed")reply.clear();
    if(variant=="partial")reply.chop(1);
    if(variant=="oversized")reply=QByteArray(16*1024+1,'x')+'\n';
    socket->write(reply);socket->disconnectFromServer();
   });
  });
  QProcess client;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("XDG_RUNTIME_DIR",runtime);environment.insert("QT_QPA_PLATFORM","offscreen");environment.insert("XDG_CONFIG_HOME",d.filePath("config"));environment.insert("XDG_DATA_HOME",d.filePath("data"));client.setProcessEnvironment(environment);
  client.start(PUTGD_APP_PATH,{"--history"});PUTGD_TRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,5000);QCOMPARE(client.exitStatus(),QProcess::NormalExit);QCOMPARE(client.exitCode(),1);QCOMPARE(requests,1);
  const auto error=client.readAllStandardError();QVERIFY2(error.contains(expectedCode+':'),error.constData());QVERIFY(!error.contains("synthetic-private"));QVERIFY(client.readAllStandardOutput().isEmpty());QVERIFY(!QFileInfo::exists(d.filePath("config/putgd/config.json")));QVERIFY(!QFileInfo::exists(d.filePath("data/putgd")));
 }
 void secondLauncherWaitsForCompleteDelayedResponse() {
  QTemporaryDir d;const auto runtime=d.filePath("runtime");QVERIFY(QDir().mkpath(runtime+"/putgd"));QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  QLocalServer server;server.setSocketOptions(QLocalServer::UserAccessOption);QVERIFY(server.listen(runtime+"/putgd/session.sock"));bool received=false;
  connect(&server,&QLocalServer::newConnection,this,[&]{auto* socket=server.nextPendingConnection();socket->setParent(&server);
   connect(socket,&QLocalSocket::readyRead,socket,[&,socket]{auto bytes=socket->property("request").toByteArray()+socket->readAll();socket->setProperty("request",bytes);if(!bytes.endsWith('\n'))return;QObject::disconnect(socket,&QLocalSocket::readyRead,socket,nullptr);received=true;
    const auto request=QJsonDocument::fromJson(bytes).object();const auto reply=QJsonDocument(LocalServer::response(request["requestId"].toString(),{},{{"shown","history"}})).toJson(QJsonDocument::Compact)+'\n';
    socket->write(reply.left(reply.size()/2));QTimer::singleShot(3500,socket,[socket,reply]{socket->write(reply.mid(reply.size()/2));socket->disconnectFromServer();});
   });
  });
  QProcess client;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("XDG_RUNTIME_DIR",runtime);environment.insert("QT_QPA_PLATFORM","offscreen");client.setProcessEnvironment(environment);client.start(PUTGD_APP_PATH,{"--history"});
  PUTGD_TRY_VERIFY(received);putgd::test::settle(3100);PUTGD_TRY_COMPARE(client.state(),QProcess::Running);PUTGD_TRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,3000);QCOMPARE(client.exitCode(),0);
 }
 void secondLauncherHasOneDeadlineDespitePartialTraffic() {
  QTemporaryDir d;const auto runtime=d.filePath("runtime");QVERIFY(QDir().mkpath(runtime+"/putgd"));QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  QLocalServer server;server.setSocketOptions(QLocalServer::UserAccessOption);QVERIFY(server.listen(runtime+"/putgd/session.sock"));int connections=0;
  connect(&server,&QLocalServer::newConnection,this,[&]{auto* socket=server.nextPendingConnection();socket->setParent(&server);++connections;auto* timer=new QTimer(socket);timer->setInterval(1000);connect(timer,&QTimer::timeout,socket,[socket]{socket->write(" ");});timer->start();connect(socket,&QLocalSocket::readyRead,socket,[socket]{socket->readAll();});});
  QProcess client;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("XDG_RUNTIME_DIR",runtime);environment.insert("QT_QPA_PLATFORM","offscreen");client.setProcessEnvironment(environment);QElapsedTimer elapsed;elapsed.start();client.start(PUTGD_APP_PATH,{"--history"});
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,20000);QVERIFY(elapsed.elapsed()>=14000);QVERIFY(elapsed.elapsed()<20000);QCOMPARE(client.exitCode(),1);QCOMPARE(connections,1);QVERIFY(client.readAllStandardError().contains("activation_timeout:"));
 }
 void realClientHistoryLinkCopyUpdatesPersistedOperationStatus(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));Source source;
  OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);LocalServer server(settings,ops,history);QVERIFY(server.listen(d.filePath("runtime")));PUTGD_TRY_VERIFY(ops.ready());
  Operation op;op.id="copy-recovery";op.state="completed";op.copyStatus="suppressed";OperationItem item;item.artifact.id="stored-link";item.uploadStatus="uploaded";item.receipt.directUrl="https://example.invalid/stored-link";op.items={item};HistoryStore store(ops.databasePath());QVERIFY(!store.saveOperation(op));
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"history.copyLink","{\"id\":\"stored-link\"}"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);
  QCOMPARE(client.exitCode(),0);const auto copied=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QVERIFY(copied["ok"].toBool());QVERIFY(copied["result"].toObject()["copied"].toBool());QCOMPARE(source.copied.text,item.receipt.directUrl);
  client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"operation.inspect","{\"id\":\"copy-recovery\"}"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);
  QCOMPARE(client.exitCode(),0);const auto inspected=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QVERIFY(inspected["ok"].toBool());QCOMPARE(inspected["result"].toObject()["copyStatus"].toString(),QString("copied"));
 }
 void realClientAndMalformedProtocol() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);LocalServer server(settings,ops,history);QVERIFY(server.listen(d.filePath("runtime")));PUTGD_TRY_VERIFY(ops.ready());
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"--request-id","fixture-request","capabilities"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QCOMPARE(response["schemaVersion"].toInt(),1);QCOMPARE(response["requestId"].toString(),QString("fixture-request"));QVERIFY(response["ok"].toBool());QVERIFY(client.readAllStandardError().isEmpty());
  client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"action.invoke","{\"action\":\"clipboard\"}"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QVERIFY(response["ok"].toBool());const auto id=response["result"].toObject()["operationId"].toString();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.copies,1);client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"operation.inspect",QString("{\"id\":\"%1\"}").arg(id)});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QCOMPARE(response["result"].toObject()["id"].toString(),id);
  auto malformed=raw(server.socketPath(),"{broken\n");QCOMPARE(malformed["error"].toObject()["code"].toString(),QString("invalid_json"));QJsonObject request{{"schemaVersion",99},{"requestId","fixture-id"},{"session",server.socketPath()},{"method","capabilities"},{"params",QJsonObject{}}};auto invalid=raw(server.socketPath(),QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');QCOMPARE(invalid["error"].toObject()["code"].toString(),QString("unsupported_version"));request["schemaVersion"]=1;request["session"]="wrong-session";invalid=raw(server.socketPath(),QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');QCOMPARE(invalid["error"].toObject()["code"].toString(),QString("wrong_session"));invalid=raw(server.socketPath(),QByteArray(1024*1024+1,' ')+'\n');QCOMPARE(invalid["error"].toObject()["code"].toString(),QString("request_too_large"));
#ifdef Q_OS_UNIX
  const auto permissions=QFile::permissions(server.socketPath());QVERIFY(!(permissions&(QFile::ReadGroup|QFile::WriteGroup|QFile::ReadOther|QFile::WriteOther)));const auto dirPermissions=QFile::permissions(d.filePath("runtime"));QVERIFY(!(dirPermissions&(QFile::ReadGroup|QFile::WriteGroup|QFile::ReadOther|QFile::WriteOther)));
#endif
  LocalServer second(settings,ops,history);QCOMPARE(second.listen(d.filePath("runtime")).error.code,QString("session_running"));
 }
 void recordingFrameRateTogglesThroughTheCli() {
  // #984: Settings, session settings.patch, putgd-cli and MCP share one key.
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);LocalServer server(settings,ops,history);QVERIFY(server.listen(d.filePath("runtime")));PUTGD_TRY_VERIFY(ops.ready());
  QCOMPARE(settings.snapshot().recordingFrameRate,30);
  QProcess client;const auto patch=[&](const QString& rate){client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"settings.patch",QString("{\"patch\":{\"preferences\":{\"recordingFrameRate\":%1}},\"expectedRevision\":\"%2\"}").arg(rate,settings.revision())});};
  const auto reply=[&]{return QJsonDocument::fromJson(client.readAllStandardOutput()).object();};
  patch("60");PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QVERIFY(reply()["ok"].toBool());QCOMPARE(settings.snapshot().recordingFrameRate,60);
  patch("45");PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(reply()["error"].toObject()["code"].toString(),QString("config_invalid"));QCOMPARE(settings.snapshot().recordingFrameRate,60);
  patch("30");PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QVERIFY(reply()["ok"].toBool());QCOMPARE(settings.snapshot().recordingFrameRate,30);
  QCOMPARE(source.reads,0);
 }
 void regionSelectionModeTogglesThroughTheCli() {
  // #629: Settings, session settings.patch, putgd-cli and MCP share one key.
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);LocalServer server(settings,ops,history);QVERIFY(server.listen(d.filePath("runtime")));PUTGD_TRY_VERIFY(ops.ready());
  QCOMPARE(settings.snapshot().regionSelectionMode,QString("live"));
  QProcess client;const auto patch=[&](const QString& mode){client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"settings.patch",QString("{\"patch\":{\"preferences\":{\"regionSelectionMode\":\"%1\"}},\"expectedRevision\":\"%2\"}").arg(mode,settings.revision())});};
  const auto reply=[&]{return QJsonDocument::fromJson(client.readAllStandardOutput()).object();};
  patch("screenshot_first");PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QVERIFY(reply()["ok"].toBool());QCOMPARE(settings.snapshot().regionSelectionMode,QString("screenshot_first"));
  patch("frozen");PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(reply()["error"].toObject()["code"].toString(),QString("config_invalid"));QCOMPARE(settings.snapshot().regionSelectionMode,QString("screenshot_first"));
  patch("live");PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);QVERIFY(reply()["ok"].toBool());QCOMPARE(settings.snapshot().regionSelectionMode,QString("live"));
  QCOMPARE(source.reads,0);
 }
 void settingsTransportAndRedactedDiagnostics() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);LocalServer server(settings,ops,history);QVERIFY(server.listen(d.filePath("runtime")));PUTGD_TRY_VERIFY(ops.ready());const auto revision=settings.revision();QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"settings.patch",QString("{\"patch\":{\"preferences\":{\"notifications\":false}},\"expectedRevision\":\"%1\"}").arg(revision)});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QVERIFY(response["ok"].toBool());QVERIFY(!settings.snapshot().notifications);client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"settings.patch",QString("{\"patch\":{},\"expectedRevision\":\"%1\"}").arg(revision)});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QCOMPARE(response["error"].toObject()["code"].toString(),QString("config_conflict"));client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"diagnostics"});PUTGD_TRY_COMPARE(client.state(),QProcess::NotRunning);const auto bytes=client.readAllStandardOutput();QVERIFY(!bytes.contains("credentials"));QVERIFY(!bytes.contains("synthetic source"));QVERIFY(!bytes.contains("config.json"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
};
QTEST_GUILESS_MAIN(CliContract)
#include "test_cli.moc"
