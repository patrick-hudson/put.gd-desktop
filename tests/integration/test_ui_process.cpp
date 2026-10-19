#include "app/UiProcessController.h"
#include <QtTest>
using namespace putgd;
class UiProcessTest:public QObject {
 Q_OBJECT
 static Error valid(const QJsonObject& value){return value["schemaVersion"]==1?Error{}:Error{"invalid_state","Keep the draft open",{}};}
 static QString helper(){return QString::fromUtf8(PUTGD_UI_FIXTURE_PATH);}
private slots:
 void init(){qunsetenv("PUTGD_TEST_UI_MODE");}
 void cleanup(){qunsetenv("PUTGD_TEST_UI_MODE");}
 void recordingWaitsForNativeAcknowledgementAndRestores(){
  qputenv("PUTGD_TEST_UI_MODE","recording-cycle");UiProcessController controller("/unused","epoch",helper(),valid);
  QList<bool> requests;UiProcessController::Reply suspension;
  controller.setShortcutRecordingHandler([&](bool active,auto reply){requests.append(active);if(active)suspension=std::move(reply);else reply({});});
  controller.activate("settings");QTRY_COMPARE(requests,QList<bool>{true});QTest::qWait(50);QCOMPARE(requests.size(),1);
  QVERIFY(suspension);suspension({});QTRY_COMPARE(requests,(QList<bool>{true,false}));
  bool stopped=false;controller.shutdown([&]{stopped=true;});QTRY_VERIFY(stopped);QCOMPARE(requests.size(),2);
 }
 void helperCrashRestoresPendingAndAcknowledgedSuspension_data(){
  QTest::addColumn<QByteArray>("mode");QTest::newRow("pending")<<QByteArray("recording-crash-pending");QTest::newRow("acknowledged")<<QByteArray("recording-crash");
 }
 void helperCrashRestoresPendingAndAcknowledgedSuspension(){
  QFETCH(QByteArray,mode);qputenv("PUTGD_TEST_UI_MODE",mode);UiProcessController controller("/unused","epoch",helper(),valid);
  QList<bool> requests;UiProcessController::Reply late;
  controller.setShortcutRecordingHandler([&](bool active,auto reply){requests.append(active);if(active&&mode=="recording-crash-pending")late=std::move(reply);else reply({});});
  controller.activate("settings");QTRY_COMPARE(requests,(QList<bool>{true,false}));QTRY_COMPARE(controller.status()["pid"].toInteger(),qint64(0));
  if(late)late({});
  QCOMPARE(requests.size(),2);
 }
 void nativeSuspensionFailureRestoresBeforeRejecting(){
  qputenv("PUTGD_TEST_UI_MODE","recording-rejected");UiProcessController controller("/unused","epoch",helper(),valid);QList<bool> requests;UiProcessController::Reply restore;
  controller.setShortcutRecordingHandler([&](bool active,auto reply){requests.append(active);if(active)reply({"synthetic_failure","Native pause rejected",{}});else restore=std::move(reply);});
  controller.activate("settings");QTRY_COMPARE(requests,(QList<bool>{true,false}));QTest::qWait(100);QCOMPARE(requests.size(),2);
  // The fixture exits only upon rejection: it must remain alive until the
  // asynchronous native restoration has actually completed.
  QVERIFY(controller.status()["pid"].toInteger()>0);QVERIFY(restore);restore({});
  QTRY_COMPARE(controller.status()["pid"].toInteger(),qint64(0));QCOMPARE(requests.size(),2);
 }
 void confirmedCloseStillReportsAnAbnormalExit(){
  qputenv("PUTGD_TEST_UI_MODE","crash-after-close");UiProcessController controller("/unused","epoch",helper(),valid);QSignalSpy failures(&controller,&UiProcessController::failure);bool shown=false;
  controller.activate("settings",[&](Error e){QVERIFY(!e);shown=true;});QTRY_VERIFY(shown);QTRY_COMPARE(controller.status()["pid"].toInteger(),qint64(0));QCOMPARE(failures.size(),1);QCOMPARE(controller.status()["lastExit"].toObject()["code"].toInt(),77);
 }
 void stalledActivationFailsBeforeTheClientsDeadline(){
  qputenv("PUTGD_TEST_UI_MODE","stall");UiProcessController controller("/unused","epoch",helper(),valid);bool replied=false;Error error;QElapsedTimer elapsed;elapsed.start();
  controller.activate("settings",[&](Error e){error=e;replied=true;});QTRY_VERIFY_WITH_TIMEOUT(replied,14500);QCOMPARE(error.code,QString("ui_show_timeout"));QVERIFY(elapsed.elapsed()<15000);
 }
 void missingHelperFailsWithoutRestarting(){
  UiProcessController controller("/unused","epoch","/does-not-exist/putgd-ui",valid);bool replied=false;Error error;
  controller.activate("settings",[&](Error e){error=e;replied=true;});QTRY_VERIFY(replied);QCOMPARE(error.code,QString("ui_start_failed"));QCOMPARE(controller.status()["pid"].toInteger(),qint64(0));
 }
 void helperProtocolFloodIsBounded(){
  qputenv("PUTGD_TEST_UI_MODE","flood");UiProcessController controller("/unused","epoch",helper(),valid);bool replied=false;Error error;
  controller.activate("settings",[&](Error e){error=e;replied=true;});QTRY_VERIFY(replied);QVERIFY(error);QTRY_COMPARE(controller.status()["pid"].toInteger(),qint64(0));
 }
 void failureCallbackCanImmediatelyReopenTheUi(){
  qputenv("PUTGD_TEST_UI_MODE","crash");UiProcessController controller("/unused","epoch",helper(),valid);bool recovered=false;Error error;
  controller.activate("activity",[&](Error failure){QVERIFY(failure);qunsetenv("PUTGD_TEST_UI_MODE");controller.activate("settings",[&](Error e){error=e;recovered=true;});});
  QTRY_VERIFY(recovered);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(controller.status()["state"].toString(),QString("ready"));bool stopped=false;controller.shutdown([&]{stopped=true;});QTRY_VERIFY(stopped);
 }
 void cancelCloseResumesPendingViewsAndInvalidations(){
  qputenv("PUTGD_TEST_UI_MODE","cancel");UiProcessController controller("/unused","epoch",helper(),valid);int shown=0;Error error;controller.invalidate();
  controller.activate("settings",[&](Error e){error=e;++shown;});QTRY_COMPARE(controller.status()["state"].toString(),QString("closing"));
  controller.activate("about",[&](Error e){error=e;++shown;});controller.invalidate();QTRY_COMPARE(shown,2);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(controller.status()["state"].toString(),QString("ready"));
  controller.invalidate();QTest::qWait(100);QCOMPARE(controller.status()["state"].toString(),QString("ready"));bool stopped=false;controller.shutdown([&]{stopped=true;});QTRY_VERIFY(stopped);
 }
 void commandsReachOnlyAReadyHelper(){
  UiProcessController controller("/unused/test.sock","epoch",helper(),valid);int launches=0;
  connect(&controller,&UiProcessController::helperPidChanged,this,[&](qint64 pid){if(pid)++launches;});
  Error error;QJsonObject result;bool done=false;auto reply=[&](Error e,QJsonObject value){error=e;result=value;done=true;};
  controller.command("moveDestination",{{"direction","up"}},reply);QVERIFY(done);QCOMPARE(error.code,QString("ui_view_closed"));QCOMPARE(launches,0);
  bool shown=false;controller.activate("settings",[&](Error e){QVERIFY(!e);shown=true;});QTRY_VERIFY(shown);
  done=false;controller.command("moveDestination",{{"direction","up"}},reply);QTRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.code));
  QCOMPARE(result["command"].toString(),QString("moveDestination"));QCOMPARE(result["params"].toObject(),QJsonObject({{"direction","up"}}));QCOMPARE(launches,1);
  bool stopped=false;controller.shutdown([&]{stopped=true;});QTRY_VERIFY(stopped);
 }
 void helperExitFailsOutstandingCommands(){
  qputenv("PUTGD_TEST_UI_MODE","command-crash");UiProcessController controller("/unused/test.sock","epoch",helper(),valid);
  bool shown=false;controller.activate("settings",[&](Error){shown=true;});QTRY_VERIFY(shown);
  Error error;bool done=false;controller.command("moveDestination",{{"direction","down"}},[&](Error e,QJsonObject){error=e;done=true;});QTRY_VERIFY(done);QVERIFY(bool(error));
 }
 void startsOneHelperAndShowsAllRequestedViews(){
  UiProcessController controller("/unused/test.sock","epoch",helper(),valid);int launches=0,shown=0;Error error;
  connect(&controller,&UiProcessController::helperPidChanged,this,[&](qint64 pid){if(pid)++launches;});
  auto reply=[&](Error e){error=e;++shown;};controller.activate("settings",reply);controller.activate("about",reply);controller.activate("settings",reply);
  QTRY_COMPARE(shown,3);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(launches,1);QCOMPARE(controller.status()["state"].toString(),QString("ready"));
  bool stopped=false;controller.shutdown([&]{stopped=true;});QTRY_VERIFY(stopped);QCOMPARE(controller.status()["pid"].toInteger(),qint64(0));
 }
 void closeRetainsDraftAndActivationDuringExitStartsFresh(){
  qputenv("PUTGD_TEST_UI_MODE","close");UiProcessController controller("/unused/test.sock","epoch",helper(),valid);int launches=0,shown=0;Error error;
  connect(&controller,&UiProcessController::helperPidChanged,this,[&](qint64 pid){if(pid)++launches;});
  QVERIFY(!controller.viewState().contains("settings"));
  controller.activate("settings",[&](Error e){error=e;++shown;});QTRY_COMPARE(controller.status()["state"].toString(),QString("closing"));
  QVERIFY(controller.status()["checkpointBytes"].toInteger()>0);
  // Only the derived capture routing is readable; the retained draft stays private.
  const auto view=controller.viewState();QCOMPARE(view["state"].toString(),QString("closing"));QCOMPARE(view["settings"].toObject(),(QJsonObject{{"section",1},{"captureRouting",QJsonObject{{"default",QJsonObject{{"profileId","synthetic"}}}}}}));QVERIFY(!QJsonDocument(view).toJson().contains("retained"));
  qputenv("PUTGD_TEST_UI_MODE","expect-state");
  controller.activate("about",[&](Error e){error=e;++shown;});QTRY_COMPARE(shown,2);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(launches,2);
  bool stopped=false;controller.shutdown([&]{stopped=true;});QTRY_VERIFY(stopped);
 }
 void crashRequiresNewActivationAndMismatchFails(){
  qputenv("PUTGD_TEST_UI_MODE","crash");UiProcessController controller("/unused/test.sock","epoch",helper(),valid);int launches=0;bool replied=false;Error error;
  connect(&controller,&UiProcessController::helperPidChanged,this,[&](qint64 pid){if(pid)++launches;});
  controller.activate("activity",[&](Error e){error=e;replied=true;});QTRY_VERIFY(replied);QVERIFY(error);QTRY_COMPARE(controller.status()["pid"].toInteger(),qint64(0));QTest::qWait(50);QCOMPARE(launches,1);
  qputenv("PUTGD_TEST_UI_MODE","mismatch");replied=false;controller.activate("about",[&](Error e){error=e;replied=true;});QTRY_VERIFY(replied);QCOMPARE(error.code,QString("ui_incompatible"));QVERIFY(error.message.contains("Restart put.gd"));QTRY_COMPARE(controller.status()["pid"].toInteger(),qint64(0));
  qunsetenv("PUTGD_TEST_UI_MODE");replied=false;controller.activate("settings",[&](Error e){error=e;replied=true;});QTRY_VERIFY(replied);QVERIFY(!error);bool stopped=false;controller.shutdown([&]{stopped=true;});QTRY_VERIFY(stopped);
 }
 void rejectedCheckpointKeepsHelperAlive(){
  qputenv("PUTGD_TEST_UI_MODE","bad-state");UiProcessController controller("/unused/test.sock","epoch",helper(),valid);bool shown=false;controller.activate("settings",[&](Error e){QVERIFY(!e);shown=true;});QTRY_VERIFY(shown);QTest::qWait(50);QCOMPARE(controller.status()["state"].toString(),QString("ready"));QCOMPARE(controller.status()["checkpointBytes"].toInteger(),qint64(0));bool stopped=false;controller.shutdown([&]{stopped=true;});QTRY_VERIFY(stopped);
 }
};
QTEST_GUILESS_MAIN(UiProcessTest)
#include "test_ui_process.moc"
