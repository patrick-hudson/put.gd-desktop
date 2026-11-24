#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
#include "app/RecordingHelperBackend.h"
#include <QTemporaryDir>
#include <QFileInfo>
#include <QJsonArray>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;

namespace {
struct Call {Error error;QJsonObject result;bool done=false;};
Call call(LocalServer& server,const QString& method,const QJsonObject& params){
 Call outcome;server.dispatch({{"schemaVersion",1},{"requestId",method},{"session",server.socketPath()},{"method",method},{"params",params}},[&](Error e,QJsonObject r){outcome={e,r,true};});
 (void)putgd::test::waitFor([&]{return outcome.done;},15000);
 return outcome;
}
QJsonObject region(){return {{"kind","region"},{"rect",QJsonArray{100,100,1280,720}}};}
// The resident with the helper supervisor pointed at the fixture executable.
struct Harness {
 QTemporaryDir dir;ProviderRegistry registry;std::shared_ptr<OpaqueProvider> provider=std::make_shared<OpaqueProvider>();SettingsService settings;Source source;std::unique_ptr<OperationService> operations;std::unique_ptr<HistoryService> history;std::unique_ptr<LocalServer> server;RecordingHelperBackend* backend=nullptr;
 explicit Harness(const QString& helper=PUTGD_RECORD_FIXTURE):settings(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);}){
  registry.add("opaque",provider);
  if(!settings.save(configuration(dir.path(),true),{}))qFatal("configuration rejected");
  operations=std::make_unique<OperationService>(dir.filePath("data"),settings,registry,source);
  auto owned=std::make_unique<RecordingHelperBackend>(helper);backend=owned.get();backend->setTimeouts(1500,1500,3000,1500);
  operations->setRecordingBackend(std::move(owned));
  history=std::make_unique<HistoryService>(*operations);server=std::make_unique<LocalServer>(settings,*operations,*history);
 }
 Call rec(const QString& command,const QJsonObject& params={}){return call(*server,"recording."+command,params);}
 QString prepare(){const auto prepared=rec("prepare",{{"target",region()},{"format","mp4"}});if(prepared.error)return {};return prepared.result["recordingId"].toString();}
 QString state(const QString& id){return rec("inspect",{{"id",id}}).result["state"].toString();}
};
}

class RecordingHelperContract:public QObject {
 Q_OBJECT
private slots:
 void init(){qunsetenv("PUTGD_TEST_RECORD_MODE");}
 void helperCrashFailsTheSessionWithoutUpload(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());qputenv("PUTGD_TEST_RECORD_MODE","crash-after-start");
  const auto id=h.prepare();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(h.state(id),QString("ready"));
  QVERIFY(!h.rec("start",{{"id",id}}).error);
  HistoryStore journal(h.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(id).stateName(),QString("failed"));
  QCOMPARE(journal.operation(id).error.code,QString("recording_helper_crashed"));QVERIFY(journal.operation(id).items.isEmpty());
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());QCOMPARE(h.backend->diagnostics()["lastExit"].toObject()["status"].toString(),QString("crash"));
  PUTGD_TRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));
 }
 void lateAnswerAfterATimeoutKeepsTheRecording(){
  // #1443: the caller gets the timeout, the helper's late answer does not end the session (it holds the only copy
  // of the clip), and the session follows the pause the helper finished late (#1434).
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());qputenv("PUTGD_TEST_RECORD_MODE","late-pause");
  const auto id=h.prepare();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(h.state(id),QString("ready"));
  QVERIFY(!h.rec("start",{{"id",id}}).error);
  QCOMPARE(h.rec("pause",{{"id",id}}).error.code,QString("recording_helper_timeout"));
  // The fixture's event follows its late answer on the same pipe.
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(h.rec("inspect",{{"id",id}}).result["progress"].toObject()["lateAnswered"].toBool(),10000);
  QCOMPARE(h.state(id),QString("paused"));QVERIFY(h.backend->diagnostics()["active"].toBool());
  QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));
 }
 void aStuckHelperIsEndedAndTheNextRecordingWorks_data(){
  // #1994: pipewiresrc's failed connect can hang the helper's main thread for good, when PipeWire goes away as a
  // recording's source starts (the start command) or reopens mid-recording (no command at all).
  QTest::addColumn<QString>("mode");QTest::addColumn<QString>("code");
  QTest::newRow("starting")<<QString("hang-on-start")<<QString("recording_helper_timeout");
  QTest::newRow("starting, source failed")<<QString("hang-after-source-error")<<QString("recording_start_failed");
  QTest::newRow("recording")<<QString("hang-while-recording")<<QString("recording_helper_unresponsive");
 }
 void aStuckHelperIsEndedAndTheNextRecordingWorks(){
  QFETCH(QString,mode);QFETCH(QString,code);Harness h;PUTGD_TRY_VERIFY(h.operations->ready());qputenv("PUTGD_TEST_RECORD_MODE",mode.toUtf8());
  const auto id=h.prepare();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(h.state(id),QString("ready"));
  const qint64 pid=h.backend->processId(id);QVERIFY(pid>0);
  const auto started=h.rec("start",{{"id",id}});
  if(mode!="hang-while-recording")QCOMPARE(started.error.code,code);else QVERIFY(!started.error);
  if(mode=="hang-after-source-error")QCOMPARE(started.error.details["reason"].toString(),QString("pipewire_stopped"));
  // The session ends with the error that says what happened, and nothing is kept or uploaded.
  HistoryStore journal(h.operations->databasePath());PUTGD_TRY_COMPARE_WITH_TIMEOUT(journal.operation(id).stateName(),QString("failed"),10000);
  QCOMPARE(journal.operation(id).error.code,code);QVERIFY(journal.operation(id).items.isEmpty());QCOMPARE(h.provider->uploads.load(),0);
  // That exact helper is gone: killed after it ignored shutdown, not left hanging.
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());
  QCOMPARE(h.backend->diagnostics()["lastExit"].toObject()["status"].toString(),QString("crash"));
  QVERIFY(!QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid)));
  PUTGD_TRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
  // The next recording starts a fresh helper and records.
  qunsetenv("PUTGD_TEST_RECORD_MODE");
  const auto next=h.prepare();QVERIFY(!next.isEmpty());PUTGD_TRY_COMPARE(h.state(next),QString("ready"));
  QVERIFY(h.backend->processId(next)!=pid);
  QVERIFY(!h.rec("start",{{"id",next}}).error);QCOMPARE(h.state(next),QString("recording"));
  QCOMPARE(h.rec("stop",{{"id",next}}).result["state"].toString(),QString("reviewing"));
 }
 void capabilitiesBeforeTheFirstProbeAnswerSayPending(){
  // #1455: reading the empty cache must not fill it with nulls that replace the pending reason.
  RecordingHelperBackend backend(PUTGD_RECORD_FIXTURE);bool probed=false;
  backend.refreshCapabilities([&]{probed=true;});QVERIFY(!probed);
  QCOMPARE(backend.capabilities()["reason"].toString(),QString("recording_probe_pending"));
  PUTGD_TRY_VERIFY(probed);QVERIFY(backend.capabilities()["available"].toBool());
 }
 void protocolFaultsStillFailTheHelper_data(){
  QTest::addColumn<QString>("mode");
  QTest::newRow("unknown request")<<QString("unknown-result");
  QTest::newRow("malformed result")<<QString("malformed-result");
 }
 void protocolFaultsStillFailTheHelper(){
  QFETCH(QString,mode);Harness h;PUTGD_TRY_VERIFY(h.operations->ready());qputenv("PUTGD_TEST_RECORD_MODE",mode.toUtf8());
  const auto id=h.prepare();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(h.state(id),QString("ready"));
  QCOMPARE(h.rec("start",{{"id",id}}).error.code,QString("recording_helper_protocol"));
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());
  QCOMPARE(h.backend->diagnostics()["lastExit"].toObject()["status"].toString(),QString("crash"));
  HistoryStore journal(h.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(id).stateName(),QString("failed"));
  QCOMPARE(journal.operation(id).error.code,QString("recording_helper_protocol"));
  QVERIFY(journal.operation(id).items.isEmpty());QCOMPARE(h.provider->uploads.load(),0);
 }
};
QTEST_GUILESS_MAIN(RecordingHelperContract)
#include "test_recording_helper.moc"
