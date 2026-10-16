#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
#include <QLocalSocket>
#include <QLocalServer>
#include <QTimer>
#include <QProcess>
#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QDir>
#include <QFileInfo>
using namespace putgd;using namespace putgd::test;
class CliContract:public QObject {
 Q_OBJECT
 static QJsonObject raw(const QString& path,const QByteArray& bytes) {QLocalSocket socket;socket.connectToServer(path);if(!socket.waitForConnected(1000))return {};socket.write(bytes);QElapsedTimer timer;timer.start();QByteArray response;while(timer.elapsed()<5000&&!response.contains('\n')) {QCoreApplication::processEvents();response+=socket.readAll();QTest::qWait(1);}return QJsonDocument::fromJson(response).object();}
private slots:
 void historyImageEndpointAcceptsOnlyJournalId(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config/config.json"));Source source;OperationService operations(d.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);QTRY_VERIFY(operations.ready());
  for(const auto& params:QList<QJsonObject>{QJsonObject{{"id","missing"}},QJsonObject{{"id","missing"},{"path","/synthetic/untrusted.png"}},QJsonObject{{"id",QJsonArray{}}}}){bool done=false;Error error;server.dispatch({{"schemaVersion",1},{"requestId","image"},{"session",server.socketPath()},{"method","history.copyImage"},{"params",params}},[&](Error e,QJsonObject){error=e;done=true;});QTRY_VERIFY(done);QCOMPARE(error.code,params.size()==1&&params["id"].isString()?QString("history_not_found"):QString("invalid_request"));}QCOMPARE(source.copies,0);QCOMPARE(source.reads,0);
 }
 void credentialEntryIsRejectedBeforeConnecting() {
  QTemporaryDir d;QLocalServer server;server.setSocketOptions(QLocalServer::UserAccessOption);QVERIFY(server.listen(d.filePath("session.sock")));int connections=0;
  connect(&server,&QLocalServer::newConnection,this,[&]{++connections;auto* socket=server.nextPendingConnection();socket->setParent(&server);socket->disconnectFromServer();});
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.fullServerName(),"setup.storeCredentials","{\"accessKey\":\"synthetic-access\",\"secretKey\":\"synthetic-secret\"}"});
  QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),1);const auto output=client.readAllStandardOutput();const auto response=QJsonDocument::fromJson(output).object();
  QCOMPARE(response["error"].toObject()["code"].toString(),QString("credentials_require_settings"));QCOMPARE(connections,0);QVERIFY(!server.hasPendingConnections());QVERIFY(!output.contains("synthetic-access"));QVERIFY(!output.contains("synthetic-secret"));QVERIFY(client.readAllStandardError().isEmpty());
 }
 void quitAcknowledgesBeforeDeferredShutdownAndRejectsUnexpectedParameters(){
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);
  auto request=[&](const QJsonObject& params){return QJsonObject{{"schemaVersion",1},{"requestId","quit"},{"session",server.socketPath()},{"method","ui.quit"},{"params",params}};};
  Error error;server.dispatch(request({}),[&](Error value,QJsonObject){error=value;});QCOMPARE(error.code,QString("ui_unavailable"));
  int calls=0;bool acknowledged=false;server.setUiQuit([&]{QVERIFY(acknowledged);++calls;});
  server.dispatch(request({{"force",true}}),[&](Error value,QJsonObject){error=value;});QCOMPARE(error.code,QString("invalid_request"));QCoreApplication::processEvents();QCOMPARE(calls,0);
  server.dispatch(request({}),[&](Error value,QJsonObject result){error=value;acknowledged=true;QVERIFY(result["quitting"].toBool());QCOMPARE(calls,0);});QVERIFY(!error);QVERIFY(acknowledged);QCOMPARE(calls,0);QTRY_COMPARE(calls,1);QCoreApplication::processEvents();QCOMPARE(calls,1);
 }
 void realClientReceivesQuitAcknowledgement(){
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(d.filePath("runtime")));int calls=0;server.setUiQuit([&]{++calls;});
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"ui.quit"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);const auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QVERIFY(response["ok"].toBool());QVERIFY(response["result"].toObject()["quitting"].toBool());QCOMPARE(calls,1);QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
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
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"ui.show","{\"view\":\"history\"}"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QCOMPARE(view,QString("history"));
  view.clear();client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"ui.show","{\"view\":\"arbitrary\"}"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),1);QVERIFY(view.isEmpty());QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void backgroundLaunchDoesNotRaiseExistingResident() {
  QTemporaryDir d;QVERIFY(d.isValid());const auto runtime=d.filePath("runtime");QVERIFY(QDir().mkpath(runtime));QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(runtime+"/putgd"));
  QStringList views;server.setUiActivation([&](const QString& view,std::function<void(Error)> ack){views.append(view);ack({});});
  QProcess client;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("XDG_RUNTIME_DIR",runtime);environment.insert("QT_QPA_PLATFORM","offscreen");client.setProcessEnvironment(environment);
  client.start(PUTGD_APP_PATH,{"--background"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QVERIFY2(views.isEmpty(),"A duplicate background launch raised the resident UI");
  client.start(PUTGD_APP_PATH,{"--history"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QCOMPARE(views,QStringList{"history"});QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void secondLauncherReportsUiFailureAndCanRetry() {
  QTemporaryDir d;const auto runtime=d.filePath("runtime");QVERIFY(QDir().mkpath(runtime));QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(runtime+"/putgd"));
  int calls=0;server.setUiActivation([&](const QString& view,std::function<void(Error)> ack){QCOMPARE(view,QString("history"));++calls;ack(calls==1?Error{"ui_incompatible","synthetic-private-message",{{"private","synthetic-private-detail"}}}:Error{});});
  QProcess client;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("XDG_RUNTIME_DIR",runtime);environment.insert("QT_QPA_PLATFORM","offscreen");environment.insert("XDG_CONFIG_HOME",d.filePath("config"));environment.insert("XDG_DATA_HOME",d.filePath("data-home"));client.setProcessEnvironment(environment);
  client.start(PUTGD_APP_PATH,{"--history"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitStatus(),QProcess::NormalExit);QCOMPARE(client.exitCode(),1);QCOMPARE(calls,1);
  const auto error=client.readAllStandardError();QVERIFY(error.contains("ui_incompatible:"));QVERIFY(error.contains("Restart put.gd after pending uploads finish."));QVERIFY(!error.contains("synthetic-private"));QVERIFY(client.readAllStandardOutput().isEmpty());
  client.start(PUTGD_APP_PATH,{"--history"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QCOMPARE(calls,2);QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
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
  client.start(PUTGD_APP_PATH,{"--history"});QTRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,5000);QCOMPARE(client.exitStatus(),QProcess::NormalExit);QCOMPARE(client.exitCode(),1);QCOMPARE(requests,1);
  const auto error=client.readAllStandardError();QVERIFY2(error.contains(expectedCode+':'),error.constData());QVERIFY(!error.contains("synthetic-private"));QVERIFY(client.readAllStandardOutput().isEmpty());QVERIFY(!QFileInfo::exists(d.filePath("config/putgd/putgd/config.json")));
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
  QTRY_VERIFY(received);QTest::qWait(3100);QCOMPARE(client.state(),QProcess::Running);QTRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,3000);QCOMPARE(client.exitCode(),0);
 }
 void secondLauncherHasOneDeadlineDespitePartialTraffic() {
  QTemporaryDir d;const auto runtime=d.filePath("runtime");QVERIFY(QDir().mkpath(runtime+"/putgd"));QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  QLocalServer server;server.setSocketOptions(QLocalServer::UserAccessOption);QVERIFY(server.listen(runtime+"/putgd/session.sock"));int connections=0;
  connect(&server,&QLocalServer::newConnection,this,[&]{auto* socket=server.nextPendingConnection();socket->setParent(&server);++connections;auto* timer=new QTimer(socket);timer->setInterval(1000);connect(timer,&QTimer::timeout,socket,[socket]{socket->write(" ");});timer->start();connect(socket,&QLocalSocket::readyRead,socket,[socket]{socket->readAll();});});
  QProcess client;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("XDG_RUNTIME_DIR",runtime);environment.insert("QT_QPA_PLATFORM","offscreen");client.setProcessEnvironment(environment);QElapsedTimer elapsed;elapsed.start();client.start(PUTGD_APP_PATH,{"--history"});
  QTRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,20000);QVERIFY(elapsed.elapsed()>=14000);QVERIFY(elapsed.elapsed()<20000);QCOMPARE(client.exitCode(),1);QCOMPARE(connections,1);QVERIFY(client.readAllStandardError().contains("activation_timeout:"));
 }
 void realClientAndMalformedProtocol() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);LocalServer server(settings,ops,history);QVERIFY(server.listen(d.filePath("runtime")));QTRY_VERIFY(ops.ready());
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"--request-id","fixture-request","capabilities"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QCOMPARE(response["schemaVersion"].toInt(),1);QCOMPARE(response["requestId"].toString(),QString("fixture-request"));QVERIFY(response["ok"].toBool());QVERIFY(client.readAllStandardError().isEmpty());
  client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"action.invoke","{\"action\":\"clipboard\"}"});QTRY_COMPARE(client.state(),QProcess::NotRunning);response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QVERIFY(response["ok"].toBool());const auto id=response["result"].toObject()["operationId"].toString();QVERIFY(!id.isEmpty());QTRY_COMPARE(source.copies,1);client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"operation.inspect",QString("{\"id\":\"%1\"}").arg(id)});QTRY_COMPARE(client.state(),QProcess::NotRunning);response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QCOMPARE(response["result"].toObject()["id"].toString(),id);
  auto malformed=raw(server.socketPath(),"{broken\n");QCOMPARE(malformed["error"].toObject()["code"].toString(),QString("invalid_json"));QJsonObject request{{"schemaVersion",99},{"requestId","fixture-id"},{"session",server.socketPath()},{"method","capabilities"},{"params",QJsonObject{}}};auto invalid=raw(server.socketPath(),QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');QCOMPARE(invalid["error"].toObject()["code"].toString(),QString("unsupported_version"));request["schemaVersion"]=1;request["session"]="wrong-session";invalid=raw(server.socketPath(),QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');QCOMPARE(invalid["error"].toObject()["code"].toString(),QString("wrong_session"));invalid=raw(server.socketPath(),QByteArray(1024*1024+1,' ')+'\n');QCOMPARE(invalid["error"].toObject()["code"].toString(),QString("request_too_large"));
#ifdef Q_OS_UNIX
  const auto permissions=QFile::permissions(server.socketPath());QVERIFY(!(permissions&(QFile::ReadGroup|QFile::WriteGroup|QFile::ReadOther|QFile::WriteOther)));const auto dirPermissions=QFile::permissions(d.filePath("runtime"));QVERIFY(!(dirPermissions&(QFile::ReadGroup|QFile::WriteGroup|QFile::ReadOther|QFile::WriteOther)));
#endif
  LocalServer second(settings,ops,history);QCOMPARE(second.listen(d.filePath("runtime")).error.code,QString("session_running"));
 }
 void settingsTransportAndRedactedDiagnostics() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);LocalServer server(settings,ops,history);QVERIFY(server.listen(d.filePath("runtime")));QTRY_VERIFY(ops.ready());const auto revision=settings.revision();QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"settings.patch",QString("{\"patch\":{\"preferences\":{\"notifications\":false}},\"expectedRevision\":\"%1\"}").arg(revision)});QTRY_COMPARE(client.state(),QProcess::NotRunning);auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QVERIFY(response["ok"].toBool());QVERIFY(!settings.snapshot().notifications);client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"settings.patch",QString("{\"patch\":{},\"expectedRevision\":\"%1\"}").arg(revision)});QTRY_COMPARE(client.state(),QProcess::NotRunning);response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();QCOMPARE(response["error"].toObject()["code"].toString(),QString("config_conflict"));client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"diagnostics"});QTRY_COMPARE(client.state(),QProcess::NotRunning);const auto bytes=client.readAllStandardOutput();QVERIFY(!bytes.contains("credentials"));QVERIFY(!bytes.contains("synthetic source"));QVERIFY(!bytes.contains("config.json"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
};
QTEST_GUILESS_MAIN(CliContract)
#include "test_cli.moc"
