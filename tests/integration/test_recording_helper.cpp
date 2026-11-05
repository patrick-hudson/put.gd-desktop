#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
#include "app/RecordingHelperBackend.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
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
 void probeAndLifecycleRunThroughTheHelperProtocol(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  // Capabilities come from a separate probe run and are cached.
  QCOMPARE(h.backend->capabilities()["reason"].toString(),QString("recording_not_probed"));
  const auto caps=h.rec("capabilities");QVERIFY(!caps.error);QVERIFY(caps.result["available"].toBool());QCOMPARE(caps.result["fixture"].toString(),QString(""));QVERIFY(h.backend->diagnostics()["probed"].toBool());
  const auto id=h.prepare();QVERIFY(!id.isEmpty());
  PUTGD_TRY_COMPARE(h.state(id),QString("ready"));QVERIFY(h.backend->diagnostics()["active"].toBool());QVERIFY(h.backend->diagnostics()["processId"].toDouble()>0);
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["canvas"].toArray(),QJsonArray({1280,720}));
  // #198: a scheme change reaches the running helper as an appearance frame,
  // and the protocol carries on (the bootstrap already carried the scheme).
  h.backend->setAppearance("light");h.backend->setAppearance("dark");
  QCOMPARE(h.rec("start",{{"id",id}}).result["state"].toString(),QString("recording"));
  PUTGD_TRY_COMPARE(h.rec("inspect",{{"id",id}}).result["progress"].toObject()["bytes"].toDouble(),4321.0);
  QCOMPARE(h.rec("pause",{{"id",id}}).result["state"].toString(),QString("paused"));
  QCOMPARE(h.rec("resume",{{"id",id}}).result["state"].toString(),QString("recording"));
  QCOMPARE(h.rec("overlay",{{"id",id},{"visible",false}}).result["state"].toString(),QString("recording"));
  const auto stopped=h.rec("stop",{{"id",id}});QVERIFY2(!stopped.error,qPrintable(stopped.error.message));QCOMPARE(stopped.result["state"].toString(),QString("reviewing"));QCOMPARE(stopped.result["durationUs"].toDouble(),12000000.0);
  QCOMPARE(stopped.result["stopReason"].toString(),QString("user"));
  QCOMPARE(h.rec("inspect",{{"id",id}}).result,stopped.result);
  const auto reviewed=h.rec("review",{{"id",id},{"action","inspect"}});QVERIFY(!reviewed.error);QCOMPARE(reviewed.result["reviewOptions"].toObject()["summary"].toString(),QString("Save to the local archive"));
  const auto preview=h.rec("preview",{{"id",id},{"time",2000000}});QVERIFY2(!preview.error,qPrintable(preview.error.message));QVERIFY(preview.result["png"].isString());QCOMPARE(preview.result["timeUs"].toDouble(),2000000.0);
  QVERIFY(!h.rec("edit",{{"id",id},{"revision",1},{"commands",QJsonArray{QJsonObject{{"op","setTrim"},{"start",1000000},{"end",7000000}}}}}).error);
  const auto uploaded=h.rec("upload",{{"id",id},{"revision",2}});QVERIFY2(!uploaded.error,qPrintable(uploaded.error.message));QCOMPARE(uploaded.result["state"].toString(),QString("exporting"));
  HistoryStore journal(h.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));
  const auto item=journal.operation(id).items.first();QCOMPARE(item.artifact.mime,QString("video/mp4"));QCOMPARE(item.artifact.bytes,qint64(2060));QCOMPARE(item.local.status,QString("saved"));
  // Discard asked the helper to shut down; it must exit normally.
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());
  const auto exit=h.backend->diagnostics()["lastExit"].toObject();QCOMPARE(exit["status"].toString(),QString("normal"));QCOMPARE(exit["code"].toInt(),0);
  PUTGD_TRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);QCOMPARE(h.provider->uploads.load(),0);
 }
 void helperCrashFailsTheSessionWithoutUpload(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());qputenv("PUTGD_TEST_RECORD_MODE","crash-after-start");
  const auto id=h.prepare();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(h.state(id),QString("ready"));
  QVERIFY(!h.rec("start",{{"id",id}}).error);
  HistoryStore journal(h.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(id).state,QString("failed"));
  QCOMPARE(journal.operation(id).error.code,QString("recording_helper_crashed"));QVERIFY(journal.operation(id).items.isEmpty());
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());QCOMPARE(h.backend->diagnostics()["lastExit"].toObject()["status"].toString(),QString("crash"));
  PUTGD_TRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));
 }
 void helperInitiatedStopOpensReview(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());qputenv("PUTGD_TEST_RECORD_MODE","stop-event");
  const auto id=h.prepare();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(h.state(id),QString("ready"));
  QVERIFY(!h.rec("start",{{"id",id}}).error);
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));QCOMPARE(h.rec("inspect",{{"id",id}}).result["durationUs"].toDouble(),9000000.0);
  QCOMPARE(h.rec("cancel",{{"id",id}}).result["state"].toString(),QString("cancelled"));
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());
 }
 void cancelShutsTheHelperDownWithoutAKill_data(){
  QTest::addColumn<QString>("state");
  for(const auto* state:{"ready","recording","paused","reviewing"})QTest::newRow(state)<<QString(state);
 }
 void cancelShutsTheHelperDownWithoutAKill(){
  QFETCH(QString,state);Harness h;PUTGD_TRY_VERIFY(h.operations->ready());qputenv("PUTGD_TEST_RECORD_MODE","slow-shutdown");
  // Repeat with a successor to catch stale replies/timeouts crossing generations.
  for(int round=0;round<2;++round){
   const auto id=h.prepare();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(h.state(id),QString("ready"));
   if(state!="ready")QCOMPARE(h.rec("start",{{"id",id}}).result["state"].toString(),QString("recording"));
   if(state=="paused")QCOMPARE(h.rec("pause",{{"id",id}}).result["state"].toString(),QString("paused"));
   if(state=="reviewing")QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));
   QCOMPARE(h.rec("cancel",{{"id",id}}).result["state"].toString(),QString("cancelled"));
   PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());
   const auto exit=h.backend->diagnostics()["lastExit"].toObject();QCOMPARE(exit["status"].toString(),QString("normal"));QCOMPARE(exit["code"].toInt(),0);
   HistoryStore journal(h.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(id).state,QString("cancelled"));QVERIFY(journal.operation(id).items.isEmpty());
   QCOMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));QCOMPARE(h.provider->uploads.load(),0);
  }
 }
 void protocolFaultsStillFailTheHelper_data(){
  QTest::addColumn<QString>("mode");
  QTest::newRow("unknown request")<<QString("unknown-result");
  QTest::newRow("malformed result")<<QString("malformed-result");
  QTest::newRow("foreign shutdown reply")<<QString("slow-shutdown-foreign");
 }
 void protocolFaultsStillFailTheHelper(){
  QFETCH(QString,mode);Harness h;PUTGD_TRY_VERIFY(h.operations->ready());qputenv("PUTGD_TEST_RECORD_MODE",mode.toUtf8());
  const auto id=h.prepare();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(h.state(id),QString("ready"));
  if(mode=="slow-shutdown-foreign")QVERIFY(!h.rec("cancel",{{"id",id}}).error);
  else QCOMPARE(h.rec("start",{{"id",id}}).error.code,QString("recording_helper_protocol"));
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());
  QCOMPARE(h.backend->diagnostics()["lastExit"].toObject()["status"].toString(),QString("crash"));
  HistoryStore journal(h.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(id).state,mode=="slow-shutdown-foreign"?QString("cancelled"):QString("failed"));
  if(mode!="slow-shutdown-foreign")QCOMPARE(journal.operation(id).error.code,QString("recording_helper_protocol"));
  QVERIFY(journal.operation(id).items.isEmpty());QCOMPARE(h.provider->uploads.load(),0);
 }
 void budgetEventBeforeStopReplyKeepsItsReasonAndClip_data(){
  QTest::addColumn<QString>("mode");
  for(const auto* mode:{"stop-budget-race","stop-budget-adjacent","stop-budget-adjacent-error"})QTest::newRow(mode)<<QString(mode);
 }
 void budgetEventBeforeStopReplyKeepsItsReasonAndClip(){
  QFETCH(QString,mode);Harness h;PUTGD_TRY_VERIFY(h.operations->ready());qputenv("PUTGD_TEST_RECORD_MODE",mode.toUtf8());
  const auto id=h.prepare();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(h.state(id),QString("ready"));QVERIFY(!h.rec("start",{{"id",id}}).error);
  const auto stopped=h.rec("stop",{{"id",id}});QVERIFY(!stopped.error);QCOMPARE(stopped.result["state"].toString(),QString("reviewing"));
  QCOMPARE(stopped.result["stopReason"].toString(),QString("budget"));QCOMPARE(stopped.result["durationUs"].toDouble(),9000000.0);QVERIFY(!stopped.result["stopDetails"].toObject().isEmpty());
  QCOMPARE(h.rec("inspect",{{"id",id}}).result,stopped.result);QCOMPARE(h.rec("stop",{{"id",id}}).result,stopped.result);
  QVERIFY(!h.rec("cancel",{{"id",id}}).error);PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());
 }
 void silentAndIncompatibleHelpersFailBeforeReady(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());HistoryStore journal(h.operations->databasePath());
  qputenv("PUTGD_TEST_RECORD_MODE","silent");
  auto id=h.prepare();QVERIFY(!id.isEmpty());
  PUTGD_TRY_COMPARE(journal.operation(id).state,QString("failed"));QCOMPARE(journal.operation(id).error.code,QString("recording_helper_start_timeout"));
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());
  qputenv("PUTGD_TEST_RECORD_MODE","wrong-build");
  id=h.prepare();QVERIFY(!id.isEmpty());
  PUTGD_TRY_COMPARE(journal.operation(id).state,QString("failed"));QCOMPARE(journal.operation(id).error.code,QString("recording_incompatible"));
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());PUTGD_TRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
 }
 void exportTimeoutKeepsTheReviewOpen(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());qputenv("PUTGD_TEST_RECORD_MODE","slow-export");
  const auto id=h.prepare();QVERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(h.state(id),QString("ready"));
  QVERIFY(!h.rec("start",{{"id",id}}).error);QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));
  // The upload is accepted when its export starts; the helper's missed deadline is its outcome.
  QCOMPARE(h.rec("upload",{{"id",id},{"revision",1}}).result["state"].toString(),QString("exporting"));
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  const auto last=h.rec("inspect",{{"id",id}}).result["lastExport"].toObject();QCOMPARE(last["status"].toString(),QString("failed"));QCOMPARE(last["error"].toObject()["code"].toString(),QString("recording_helper_timeout"));
  HistoryStore journal(h.operations->databasePath());QCOMPARE(journal.operation(id).state,QString("recording"));QVERIFY(journal.operation(id).items.isEmpty());
  QCOMPARE(h.rec("cancel",{{"id",id}}).result["state"].toString(),QString("cancelled"));
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());
 }
 void brokenProbesReportUnavailableTruthfully(){
  qputenv("PUTGD_TEST_RECORD_MODE","probe-garbage");
  {Harness h;PUTGD_TRY_VERIFY(h.operations->ready());const auto caps=h.rec("capabilities");QVERIFY(!caps.error);QVERIFY(!caps.result["available"].toBool());QCOMPARE(caps.result["reason"].toString(),QString("recording_probe_invalid"));
   QCOMPARE(h.rec("prepare",{{"target",region()},{"format","mp4"}}).error.code,QString("recording_unavailable"));}
  qputenv("PUTGD_TEST_RECORD_MODE","probe-fails");
  {Harness h;PUTGD_TRY_VERIFY(h.operations->ready());QCOMPARE(h.rec("capabilities").result["reason"].toString(),QString("recording_probe_failed"));}
  {Harness h(QDir::tempPath()+"/putgd-no-such-helper");PUTGD_TRY_VERIFY(h.operations->ready());QCOMPARE(h.rec("capabilities").result["reason"].toString(),QString("recording_helper_missing"));
   QCOMPARE(h.backend->capabilities()["reason"].toString(),QString("recording_helper_missing"));}
 }
#ifdef PUTGD_RECORD_HELPER
 void productHelperAnswersTheHandshakeAndRefusesCaptureTruthfully(){
  Harness h(PUTGD_RECORD_HELPER);PUTGD_TRY_VERIFY(h.operations->ready());h.backend->setTimeouts(10000,5000,10000,5000);
  const auto caps=h.rec("capabilities");QVERIFY2(!caps.error,qPrintable(caps.error.message));
  QCOMPARE(caps.result["schemaVersion"].toInt(),1);QVERIFY(caps.result["available"].isBool());QVERIFY(caps.result["available"].toBool()||caps.result["reason"].isString());
  QVERIFY(caps.result["gstreamer"].isObject());QVERIFY(caps.result["ffmpeg"].isObject());QVERIFY(caps.result["formats"].isArray());QVERIFY(caps.result["liveCapture"].isObject());
  // Usable encoders come from real start attempts and are a subset of the registered ones.
  const auto gst=caps.result["gstreamer"].toObject();
  if(gst["available"].toBool()){QVERIFY(gst["usableH264Encoders"].isArray());for(const auto& name:gst["usableH264Encoders"].toArray())QVERIFY(gst["liveH264Encoders"].toArray().contains(name));
   QCOMPARE(caps.result["liveCapture"].toObject()["encoder"].toBool(),!gst["usableH264Encoders"].toArray().isEmpty());}
  QVERIFY(caps.result["ffmpeg"].toObject()["h264Decoder"].isObject());
  const auto reason=caps.result["reason"].toString();
  if(reason=="live_encoder_missing"||reason=="h264_encoder_unusable"||reason=="h264_decoder_unusable"){QVERIFY(!caps.result["message"].toString().isEmpty());QVERIFY(!caps.result["hint"].toString().isEmpty());}
  qInfo().noquote()<<QJsonDocument(QJsonObject{{"reason",caps.result["reason"]},{"session",caps.result["session"]},{"formats",caps.result["formats"]},{"liveCapture",caps.result["liveCapture"]}}).toJson(QJsonDocument::Compact);
  // Without a display the service refuses to prepare; the helper itself still
  // completes the identity handshake either way.
  if(!caps.result["available"].toBool())QCOMPARE(h.rec("prepare",{{"target",region()},{"format","mp4"}}).error.code,QString("recording_unavailable"));
  RecordingTarget target;QVERIFY(!recording::parseTarget(region(),target));RecordingConfig config;QVERIFY(!recording::parseConfig({{"format","mp4"}},config));
  QJsonObject failure;Error error;bool replied=false;
  h.backend->setEventSink([&](QJsonObject event){failure=event;});
  h.backend->prepare("direct-id",target,config,h.dir.path(),[&](Error e,QJsonObject){error=e;replied=true;});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(replied,15000);
  // Headless runners have no X11 display: the helper refuses the target truthfully.
  // On a real X11 session it resolves the region instead; either way it exits normally.
  const QStringList acceptedCodes{"capture_unavailable","recording_target"};
  if(error)QVERIFY2(acceptedCodes.contains(error.code),qPrintable(error.code+": "+error.message));
  qInfo().noquote()<<"direct prepare:"<<(error?error.code:QString("ok"));
  h.backend->discard("direct-id");
  PUTGD_TRY_VERIFY(!h.backend->diagnostics()["active"].toBool());
  const auto exit=h.backend->diagnostics()["lastExit"].toObject();QCOMPARE(exit["status"].toString(),QString("normal"));QCOMPARE(exit["code"].toInt(),0);QVERIFY(failure.isEmpty());
 }
#endif
};
QTEST_GUILESS_MAIN(RecordingHelperContract)
#include "test_recording_helper.moc"
