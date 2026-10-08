#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
#include <QLocalSocket>
#include <QProcess>
#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QDir>
using namespace putgd;using namespace putgd::test;
class CliContract:public QObject {
 Q_OBJECT
 static QJsonObject raw(const QString& path,const QByteArray& bytes) {QLocalSocket socket;socket.connectToServer(path);if(!socket.waitForConnected(1000))return {};socket.write(bytes);QElapsedTimer timer;timer.start();QByteArray response;while(timer.elapsed()<5000&&!response.contains('\n')) {QCoreApplication::processEvents();response+=socket.readAll();QTest::qWait(1);}return QJsonDocument::fromJson(response).object();}
private slots:
 void launcherActivationUsesValidatedResidentUiRequest() {
  QTemporaryDir d;ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(d.filePath("runtime")));QString view;
  server.setUiActivation([&](const QString& requested){view=requested;});
  QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"ui.show","{\"view\":\"history\"}"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QCOMPARE(view,QString("history"));
  view.clear();client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),"ui.show","{\"view\":\"arbitrary\"}"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),1);QVERIFY(view.isEmpty());QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void backgroundLaunchDoesNotRaiseExistingResident() {
  QTemporaryDir d;QVERIFY(d.isValid());const auto runtime=d.filePath("runtime");QVERIFY(QDir().mkpath(runtime));QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  ProviderRegistry providers;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,providers,source);HistoryService history(operations);LocalServer server(settings,operations,history);QVERIFY(server.listen(runtime+"/putgd"));
  QStringList views;server.setUiActivation([&](const QString& view){views.append(view);});
  QProcess client;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("XDG_RUNTIME_DIR",runtime);environment.insert("QT_QPA_PLATFORM","offscreen");client.setProcessEnvironment(environment);
  client.start(PUTGD_APP_PATH,{"--background"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QVERIFY2(views.isEmpty(),"A duplicate background launch raised the resident UI");
  client.start(PUTGD_APP_PATH,{"--history"});QTRY_COMPARE(client.state(),QProcess::NotRunning);QCOMPARE(client.exitCode(),0);QCOMPARE(views,QStringList{"history"});QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
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
