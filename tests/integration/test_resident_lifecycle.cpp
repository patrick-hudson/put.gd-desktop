#include "app/ResidentSession.h"
#include "core/Settings.h"
#include "core/SettingsService.h"
#include "fixtures/Waits.h"
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
  const auto peer=localSessionPeer(socket);
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
  const auto peer=localSessionPeer(socket);QVERIFY(peer.pid>0);
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
