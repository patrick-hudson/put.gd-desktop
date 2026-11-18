#include "app/ResidentSession.h"
#include "core/Settings.h"
#include "core/SettingsService.h"
#include "fixtures/Waits.h"
#include "fixtures/ChildProcesses.h"
#include "fixtures/HttpControl.h"
#include "storage/http/HttpCommon.h"
#include "storage/http/HttpProvider.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <unistd.h>
using namespace putgd;
// #490: a resident ends with its session, and a launcher replaces one left
// from an ended session by its exact process.
class ResidentLifecycle:public QObject {
 Q_OBJECT
 static bool startFixture(QProcess& fixture,const QString& socket,const QString& mode) {
  fixture.start(PUTGD_STALE_RESIDENT_FIXTURE,{socket,mode});
  if(!fixture.waitForStarted()||!putgd::test::waitFor([&]{return fixture.canReadLine();}))return false;
  return fixture.readLine()=="ready\n";
 }
 // The resident's process as a launcher's activation sees it, refused so that nothing is sent.
 static LocalPeer peerOn(const QString& socket) {
  LocalPeer found;
  const auto activation=activateLocalSession(socket,{},[&](const LocalPeer& peer){found=peer;return false;});
  return activation.refused?found:LocalPeer{};
 }
 struct Stop {QProcess& process;~Stop(){if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(3000);}}};
private slots:
 void retireEndsTheExactResident_data() {
  QTest::addColumn<QString>("mode");
  QTest::addColumn<QString>("how");
  QTest::newRow("answers-ui-quit") << QString("quit") << QString("quit");
  QTest::newRow("ignores-ui-quit") << QString("stay") << QString("terminated");
  QTest::newRow("ignores-sigterm") << QString("stubborn") << QString("killed");
 }
 void retireEndsTheExactResident() {
  QFETCH(QString,mode);QFETCH(QString,how);
  QTemporaryDir directory;QVERIFY(directory.isValid());
  const auto socket=directory.filePath("s.sock");
  QProcess fixture;Stop stop{fixture};
  QVERIFY(startFixture(fixture,socket,mode));
  const auto peer=peerOn(socket);
  QVERIFY(peer.connected);
  QCOMPARE(peer.pid,fixture.processId());
  QCOMPARE(peer.uid,uint(::getuid()));
  QVERIFY(peer.start>0);QCOMPARE(peer.start,processStartTime(peer.pid));
  const auto retired=retireResident(socket,peer,1000,1000);
  QVERIFY(retired.done);
  QCOMPARE(retired.how,how);
  PUTGD_TRY_COMPARE(fixture.state(),QProcess::NotRunning);
  QCOMPARE(fixture.exitStatus(),mode=="quit"?QProcess::NormalExit:QProcess::CrashExit);
 }
 void retireRefusesAnyOtherProcess() {
  QTemporaryDir directory;QVERIFY(directory.isValid());
  const auto socket=directory.filePath("s.sock");
  QProcess fixture;Stop stop{fixture};
  QVERIFY(startFixture(fixture,socket,"stay"));
  const auto peer=peerOn(socket);QVERIFY(peer.pid>0);
  // A later process given the same PID has another start time.
  auto reused=peer;++reused.start;
  QVERIFY(!retireResident(socket,reused,200,200).done);
  auto self=peer;self.pid=QCoreApplication::applicationPid();self.start=processStartTime(self.pid);
  QVERIFY(!retireResident(socket,self,200,200).done);
  auto foreign=peer;foreign.uid=::getuid()+1;
  QVERIFY(!retireResident(socket,foreign,200,200).done);
  QVERIFY(!retireResident(socket,LocalPeer{},200,200).done);
  PUTGD_COMPARE_STEADY(fixture.state(),QProcess::Running,300);
 }
 // The Cinnamon route's resident shortcuts and captures through its own X
 // connection; when the session's X server goes, the resident goes too.
 // #1602. Quit while a custom-uploader deletion waits on its server: the resident ends
 // within a few seconds, the deletion's helper is killed with it, and the next resident
 // shows the deletion interrupted and unknown, which Retry then resolves.
 void quitDuringAStalledDeletionEndsResident() {
  if(qEnvironmentVariable("PUTGD_ISOLATED_RESIDENT_TEST")!="1")QSKIP("Requires its dedicated private-bus invocation");
  const auto number=qEnvironmentVariable("PUTGD_TEST_XVFB_DISPLAY");QVERIFY(!number.isEmpty());
  QTemporaryDir directory;QVERIFY(directory.isValid());
  const auto runtime=directory.filePath("r"),socket=runtime+"/putgd/session.sock";
  QVERIFY2(socket.toUtf8().size()<=107,"Resolved test socket exceeds Linux's limit");
  QVERIFY(QDir().mkpath(runtime));
  QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  // The receiver answers a deletion without deleting; it asks for no credential.
  QProcess receiver;Stop stopReceiver{receiver};
  receiver.start("python3",{PUTGD_HTTP_FIXTURE_PATH});
  QVERIFY(receiver.waitForStarted()&&receiver.waitForReadyRead(5000));
  const auto base="http://127.0.0.1:"+QString::fromUtf8(receiver.readLine()).trimmed();
  // Control requests fail the test instead of ending it, so the processes it owns are stopped.
  QVERIFY(test::tryHttpControl(base,"/__reset",{{"auth","none"}},true));
  const auto rules=[&](const QJsonArray& list){return test::tryHttpControl(base,"/__rules",{{"rules",list}},true).has_value();};
  const auto release=[&](const QString& name){return test::tryHttpControl(base,"/__release",{{"barrier",name}},true).has_value();};
  const auto events=[&](const QString& call){const auto state=test::tryHttpControl(base,"/__state");if(!state)return -1;int count=0;for(const auto& e:(*state)["events"].toArray())if(e.toObject()["call"]==call)++count;return count;};
  auto receiverSettings=http::defaults();
  receiverSettings["requestUrl"]=base+"/upload/pretend";receiverSettings["responseUrl"]="{json:files[0].url}";receiverSettings["deletionUrl"]="{json:files[0].delete}";
  receiverSettings["requestTimeoutMs"]=30000;receiverSettings["connectTimeoutMs"]=2000;
  auto config=Settings::defaults();
  config["profiles"]=QJsonArray{QJsonObject{{"id","receiver"},{"label","Scripted receiver"},{"provider","custom-http"},{"settings",receiverSettings},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}}};
  CredentialStore credentials;ProviderRegistry registry;registry.add("custom-http",std::make_shared<HttpUploaderProvider>(credentials));
  SettingsService settings(directory.filePath("c/putgd/config.json"),[&](auto id,auto value){return registry.validate(id,value);});
  const auto saved=settings.save(config,{});
  QVERIFY2(saved,qPrintable(saved.error.code+": "+saved.error.message));
  const auto file=directory.filePath("note.txt");
  {QFile out(file);QVERIFY(out.open(QIODevice::WriteOnly));out.write("synthetic note");}
  QProcess xvfb;Stop stopX{xvfb};
  xvfb.setProcessChannelMode(QProcess::ForwardedErrorChannel);
  xvfb.start(PUTGD_XVFB_PATH,{":"+number,"-displayfd","1","-screen","0","1280x800x24","-nolisten","tcp","-noreset"});
  QVERIFY(xvfb.waitForStarted());
  PUTGD_TRY_VERIFY(xvfb.canReadLine());
  QCOMPARE(xvfb.readLine().trimmed(),number.toLatin1());
  auto environment=QProcessEnvironment::systemEnvironment();
  for(const auto& [key,value]:{std::pair{"XDG_CONFIG_HOME","c"},{"XDG_DATA_HOME","d"},{"XDG_CACHE_HOME","k"},{"XDG_STATE_HOME","s"}})
   environment.insert(key,directory.filePath(value));
  environment.insert("XDG_RUNTIME_DIR",runtime);
  environment.insert("XDG_CURRENT_DESKTOP","X-Cinnamon");
  environment.insert("XDG_SESSION_TYPE","x11");
  environment.insert("DISPLAY",":"+number);
  environment.remove("WAYLAND_DISPLAY");environment.remove("QT_QPA_PLATFORM");
  const auto call=[&](const QString& method,const QJsonObject& params={}){
   QProcess cli;cli.start(PUTGD_PRODUCT_CLI_PATH,{"--socket",socket,method,QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))});
   if(!cli.waitForFinished(10000))return QJsonObject{};
   return QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
  };
  const auto start=[&](QProcess& resident){
   resident.setProcessEnvironment(environment);resident.setProcessChannelMode(QProcess::ForwardedChannels);
   resident.start(PUTGD_PRODUCT_APP_PATH,{"--background"});
   return resident.waitForStarted()&&test::waitFor([&]{return call("ui.snapshot")["result"].toObject()["operationsReady"].toBool();},10000);
  };
  QString deletion;
  {
   QProcess resident;Stop stopResident{resident};
   QVERIFY(start(resident));
   // Accepted once the resident has read its saved credentials.
   QJsonObject upload;
   // Asked again only while the resident answers "initializing": any other answer was
   // a submission, and a second would upload the file twice.
   QElapsedTimer accepting;accepting.start();
   while(true) {
    upload=call("upload.files",{{"paths",QJsonArray{file}},{"profileId","receiver"}});
    if(upload["error"].toObject()["code"].toString()!="initializing"||accepting.elapsed()>10000)break;
    test::settle(100);
   }
   const auto operation=upload["result"].toObject()["operationId"].toString();
   QVERIFY2(!operation.isEmpty(),QJsonDocument(upload).toJson().constData());
   QJsonObject inspected;
   PUTGD_TRY_VERIFY_WITH_TIMEOUT((inspected=call("operation.inspect",{{"id",operation}})["result"].toObject())["state"].toString()=="completed",15000);
   const auto historyId=inspected["items"].toArray()[0].toObject()["artifact"].toObject()["id"].toString();
   const auto prepared=call("delete.prepare",{{"ids",QJsonArray{historyId}}})["result"].toObject();
   QVERIFY(!prepared["acknowledgement"].toString().isEmpty());
   // The look at the link after the deletion is held until the test releases it.
   const auto seen=events("file");
   QVERIFY(rules(QJsonArray{QJsonObject{{"call","file"}},QJsonObject{{"call","file"},{"barrier","quit"}}}));
   deletion=call("delete.confirm",{{"ids",QJsonArray{historyId}},{"acknowledgement",prepared["acknowledgement"]}})["result"].toObject()["deleteOperationId"].toString();
   QVERIFY(!deletion.isEmpty());
   PUTGD_TRY_VERIFY_WITH_TIMEOUT(events("file")==seen+2,15000);
   const auto helpers=test::childProcesses("putgd-http-worker",resident.processId());
   QCOMPARE(helpers.size(),1);
   QElapsedTimer quit;quit.start();
   QVERIFY(call("ui.quit")["ok"].toBool());
   PUTGD_TRY_COMPARE_WITH_TIMEOUT(resident.state(),QProcess::NotRunning,10000);
   QVERIFY2(quit.elapsed()<3000,qPrintable(QString::number(quit.elapsed())));
   QCOMPARE(resident.exitStatus(),QProcess::NormalExit);
   QCOMPARE(resident.exitCode(),0);
   QVERIFY(!test::processAlive(helpers[0]));
   QVERIFY(release("quit"));
  }
  QVERIFY(rules({}));
  {
   QProcess resident;Stop stopResident{resident};
   QVERIFY(start(resident));
   auto journal=call("delete.inspect",{{"id",deletion}})["result"].toObject();
   QCOMPARE(journal["state"].toString(),QString("interrupted"));
   QCOMPARE(journal["items"].toArray()[0].toObject()["status"].toString(),QString("unknown"));
   QCOMPARE(journal["items"].toArray()[0].toObject()["progress"].toObject()["phase"].toString(),QString("delete_intent"));
   QVERIFY(call("delete.retry",{{"id",deletion}})["ok"].toBool());
   PUTGD_TRY_VERIFY_WITH_TIMEOUT((journal=call("delete.inspect",{{"id",deletion}})["result"].toObject())["state"].toString()=="completed",15000);
   // The server still serves the file: Retry finds nothing deleted, and says so as unknown.
   QCOMPARE(journal["items"].toArray()[0].toObject()["status"].toString(),QString("unknown"));
   QCOMPARE(journal["items"].toArray()[0].toObject()["error"].toObject()["code"].toString(),QString("http_delete_unconfirmed"));
   QVERIFY(call("ui.quit")["ok"].toBool());
   PUTGD_TRY_COMPARE_WITH_TIMEOUT(resident.state(),QProcess::NotRunning,10000);
  }
 }
 void displayLossEndsResident() {
  if(qEnvironmentVariable("PUTGD_ISOLATED_RESIDENT_TEST")!="1")QSKIP("Requires its dedicated private-bus invocation");
  const auto number=qEnvironmentVariable("PUTGD_TEST_XVFB_DISPLAY");QVERIFY(!number.isEmpty());
  QTemporaryDir directory;QVERIFY(directory.isValid());
  const auto runtime=directory.filePath("r"),socket=runtime+"/putgd/session.sock";
  QVERIFY2(socket.toUtf8().size()<=107,"Resolved test socket exceeds Linux's limit");
  QVERIFY(QDir().mkpath(runtime));
  QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  SettingsService settings(directory.filePath("c/putgd/config.json"));
  QVERIFY(settings.save(Settings::defaults(),{}));
  QProcess xvfb;Stop stopX{xvfb};
  // With -displayfd the server names its display once it accepts connections.
  xvfb.setProcessChannelMode(QProcess::ForwardedErrorChannel);
  xvfb.start(PUTGD_XVFB_PATH,{":"+number,"-displayfd","1","-screen","0","1280x800x24","-nolisten","tcp","-noreset"});
  QVERIFY(xvfb.waitForStarted());
  PUTGD_TRY_VERIFY(xvfb.canReadLine());
  QCOMPARE(xvfb.readLine().trimmed(),number.toLatin1());
  auto environment=QProcessEnvironment::systemEnvironment();
  for(const auto& [key,value]:{std::pair{"XDG_CONFIG_HOME","c"},{"XDG_DATA_HOME","d"},{"XDG_CACHE_HOME","k"},{"XDG_STATE_HOME","s"}})
   environment.insert(key,directory.filePath(value));
  environment.insert("XDG_RUNTIME_DIR",runtime);
  environment.insert("XDG_CURRENT_DESKTOP","X-Cinnamon");
  environment.insert("XDG_SESSION_TYPE","x11");
  environment.insert("DISPLAY",":"+number);
  environment.remove("WAYLAND_DISPLAY");environment.remove("QT_QPA_PLATFORM");
  QProcess resident;resident.setProcessEnvironment(environment);resident.setProcessChannelMode(QProcess::ForwardedChannels);
  Stop stopResident{resident};
  resident.start(PUTGD_PRODUCT_APP_PATH,{"--background"});
  QVERIFY(resident.waitForStarted());
  const auto snapshot=[&]{
   QProcess cli;cli.start(PUTGD_PRODUCT_CLI_PATH,{"--socket",socket,"ui.snapshot"});
   if(!cli.waitForFinished(3000))return QJsonObject{};
   const auto response=QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
   return response["ok"].toBool()?response["result"].toObject():QJsonObject{};
  };
  QJsonObject ready;
  PUTGD_TRY_VERIFY_WITH_TIMEOUT((ready=snapshot())["operationsReady"].toBool(),10000);
  const auto diagnostics=ready["diagnostics"].toObject();
  // The X connection is up: the loss below is the X server's going away.
  QCOMPARE(diagnostics["shortcuts"].toObject()["available"].toBool(),true);
  QVERIFY(diagnostics["loginSession"].toObject().contains("watched"));
  PUTGD_COMPARE_STEADY(resident.state(),QProcess::Running,500);
  xvfb.terminate();QVERIFY(xvfb.waitForFinished(5000));
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(resident.state(),QProcess::NotRunning,15000);
  QCOMPARE(resident.exitStatus(),QProcess::NormalExit);
  QCOMPARE(resident.exitCode(),0);
  QByteArray log;
  for(QDirIterator it(directory.filePath("s"),{"putgd-*.jsonl"},QDir::Files,QDirIterator::Subdirectories);it.hasNext();){QFile file(it.next());if(file.open(QIODevice::ReadOnly))log+=file.readAll();}
  QVERIFY2(log.contains("Resident session ended")&&log.contains("display_lost"),log.right(4000).constData());
  QVERIFY(log.contains("Resident stopped"));
 }
};
QTEST_GUILESS_MAIN(ResidentLifecycle)
#include "test_resident_lifecycle.moc"
