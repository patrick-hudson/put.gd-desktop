#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
#include "fixtures/FakeRecordingBackend.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
using namespace putgd;using namespace putgd::test;

namespace {
struct Call {Error error;QJsonObject result;bool done=false;};
Call call(LocalServer& server,const QString& method,const QJsonObject& params){
 Call outcome;server.dispatch({{"schemaVersion",1},{"requestId",method},{"session",server.socketPath()},{"method",method},{"params",params}},[&](Error e,QJsonObject r){outcome={e,r,true};});
 for(int i=0;i<500&&!outcome.done;++i)QTest::qWait(10);
 return outcome;
}
QJsonObject region(){return {{"kind","region"},{"rect",QJsonArray{100,100,1280,720}}};}
// A desktop shell like the GNOME extension: selections wait for the test to answer them.
struct FakeShell final:RecordingShell {
 bool selects=true,failFrames=false;QList<Reply> selections;int cancels=0,hides=0;QList<QJsonObject> frames;
 QJsonObject bar{{"placed",true},{"outside",true},{"rect",QJsonArray{50,430,400,40}}};
 bool selectsRegions() const override {return selects;}
 void selectRegion(Reply reply) override {selections.append(std::move(reply));}
 void cancelSelection() override {++cancels;if(!selections.isEmpty()){auto reply=selections.takeLast();reply({"cancelled","GNOME region selection cancelled",{}},{});}}
 void showFrame(const QJsonObject& frame,Reply reply) override {frames.append(frame);if(failFrames)reply({"helper_incompatible","no frame support",{}},{});else reply({},{{"bar",bar}});}
 void hideFrame() override {++hides;}
};
struct Harness {
 QTemporaryDir dir;ProviderRegistry registry;std::shared_ptr<OpaqueProvider> provider=std::make_shared<OpaqueProvider>();SettingsService settings;Source source;std::unique_ptr<OperationService> operations;std::unique_ptr<HistoryService> history;std::unique_ptr<LocalServer> server;FakeRecordingBackend* backend=nullptr;
 explicit Harness(bool local=true):settings(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);}){
  registry.add("opaque",provider);
  auto doc=configuration(dir.path(),local);if(!local){auto archive=doc["archive"].toObject();archive["keepUploads"]=true;doc["archive"]=archive;}
  if(!settings.save(doc,{}))qFatal("configuration rejected");
  open();
 }
 void open(){operations=std::make_unique<OperationService>(dir.filePath("data"),settings,registry,source);auto fake=std::make_unique<FakeRecordingBackend>();backend=fake.get();operations->setRecordingBackend(std::move(fake));history=std::make_unique<HistoryService>(*operations);server=std::make_unique<LocalServer>(settings,*operations,*history);}
 void close(){server.reset();history.reset();operations.reset();backend=nullptr;}
 Call rec(const QString& command,const QJsonObject& params={}){return call(*server,"recording."+command,params);}
 QString state(const QString& id){return rec("inspect",{{"id",id}}).result["state"].toString();}
 QString prepareReady(const QJsonObject& extra={}){
  QJsonObject params{{"target",region()},{"format","mp4"}};for(auto it=extra.begin();it!=extra.end();++it)params[it.key()]=it.value();
  const auto prepared=rec("prepare",params);if(prepared.error)return {};
  const auto id=prepared.result["recordingId"].toString();
  for(int i=0;i<300;++i){if(rec("inspect",{{"id",id}}).result["state"]=="ready")break;QTest::qWait(10);}
  return id;
 }
};
}

class RecordingServiceContract:public QObject {
 Q_OBJECT
private slots:
 void unavailableBackendReportsCapabilitiesAndRefusesRecording(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));Source source;OperationService operations(d.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);QTRY_VERIFY(operations.ready());
  const auto caps=call(server,"recording.capabilities",{});QVERIFY(!caps.error);QVERIFY(!caps.result["available"].toBool());QCOMPARE(caps.result["reason"].toString(),QString("recording_helper_missing"));QCOMPARE(caps.result["timeUnit"].toString(),QString("microseconds"));QVERIFY(caps.result["active"].isNull());
  QVERIFY(operations.capabilities()["recording"].toObject().contains("available"));
  QCOMPARE(call(server,"recording.prepare",{{"target",region()},{"format","mp4"}}).error.code,QString("recording_unavailable"));
  QCOMPARE(call(server,"recording.inspect",{{"id","nothing"}}).error.code,QString("recording_unavailable"));
  QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
 }
 void lifecycleAdmitsOnlyTheExplicitUploadExport(){
  Harness h;QTRY_VERIFY(h.operations->ready());
  const auto prepared=h.rec("prepare",{{"target",region()},{"format","mp4"},{"audio",QJsonObject{{"system",true}}},{"fps",24}});
  QVERIFY2(!prepared.error,qPrintable(prepared.error.message));const auto id=prepared.result["recordingId"].toString();QVERIFY(!id.isEmpty());QCOMPARE(prepared.result["operationId"].toString(),id);
  QCOMPARE(prepared.result["state"].toString(),QString("preparing"));
  QTRY_COMPARE(h.rec("inspect",{{"id",id}}).result["state"].toString(),QString("ready"));
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["canvas"].toArray(),QJsonArray({1280,720}));
  QCOMPARE(h.rec("capabilities").result["active"].toString(),id);
  // Durable intent exists before any capture; its journal state is `recording`.
  {HistoryStore journal(h.operations->databasePath());const auto op=journal.operation(id);QCOMPARE(op.state,QString("recording"));QCOMPARE(op.action,QString("record"));}
  QCOMPARE(h.rec("prepare",{{"target",region()},{"format","mp4"}}).error.code,QString("recording_active"));
  QCOMPARE(h.rec("upload",{{"id",id},{"revision",1}}).error.code,QString("recording_state"));
  QCOMPARE(h.rec("pause",{{"id",id}}).error.code,QString("recording_state"));
  QCOMPARE(h.rec("start",{{"id",id}}).result["state"].toString(),QString("recording"));
  QCOMPARE(h.rec("start",{{"id",id}}).error.code,QString("recording_state"));
  QCOMPARE(h.rec("pause",{{"id",id}}).result["state"].toString(),QString("paused"));
  QCOMPARE(h.rec("resume",{{"id",id}}).result["state"].toString(),QString("recording"));
  QCOMPARE(h.rec("overlay",{{"id",id},{"visible",false}}).result["state"].toString(),QString("recording"));
  QCOMPARE(h.rec("pause",{{"id",id}}).result["state"].toString(),QString("paused"));
  // Stop while paused goes straight to private review.
  const auto stopped=h.rec("stop",{{"id",id}});QVERIFY(!stopped.error);QCOMPARE(stopped.result["state"].toString(),QString("reviewing"));QCOMPARE(stopped.result["durationUs"].toDouble(),12000000.0);QCOMPARE(stopped.result["edit"].toObject()["revision"].toDouble(),1.0);
  QCOMPARE(h.backend->exports,0);
  const auto preview=h.rec("preview",{{"id",id},{"time",4000000},{"maxEdge",640}});QVERIFY(!preview.error);QVERIFY(preview.result["png"].isString());QVERIFY(preview.result["retained"].toBool());QCOMPARE(preview.result["outputTimeUs"].toDouble(),4000000.0);QCOMPARE(h.backend->lastPreviewTime,qint64(4000000));
  QCOMPARE(h.rec("preview",{{"id",id},{"time",12000000}}).error.code,QString("invalid_request"));
  QCOMPARE(h.rec("edit",{{"id",id},{"revision",7},{"commands",QJsonArray{QJsonObject{{"op","resetTrim"}}}}}).error.code,QString("recording_stale"));
  const auto edited=h.rec("edit",{{"id",id},{"revision",1},{"commands",QJsonArray{QJsonObject{{"op","setTrim"},{"start",2000000},{"end",8000000}},QJsonObject{{"op","addMask"},{"mask",QJsonObject{{"kind","blur"},{"rect",QJsonArray{10,10,100,50}},{"start",3000000},{"end",5000000}}}}}}});
  QVERIFY2(!edited.error,qPrintable(edited.error.message));QCOMPARE(edited.result["edit"].toObject()["revision"].toDouble(),2.0);QCOMPARE(edited.result["retainedUs"].toDouble(),6000000.0);QCOMPARE(edited.result["edit"].toObject()["masks"].toArray().size(),1);
  // A trimmed-out frame previews but is flagged as not retained.
  const auto outside=h.rec("preview",{{"id",id},{"time",1000000}});QVERIFY(!outside.error);QVERIFY(!outside.result["retained"].toBool());QVERIFY(outside.result["outputTimeUs"].isNull());
  // Local copies export without uploading and keep the review open.
  const auto copyPath=h.dir.filePath("copy.mp4");
  QCOMPARE(h.rec("saveCopy",{{"id",id},{"path",h.dir.filePath("copy.mkv")}}).error.code,QString("invalid_request"));
  const auto saved=h.rec("saveCopy",{{"id",id},{"path",copyPath}});QVERIFY2(!saved.error,qPrintable(saved.error.message));QCOMPARE(saved.result["state"].toString(),QString("exporting"));QCOMPARE(saved.result["pending"].toString(),QString("saveCopy"));
  QTRY_COMPARE(h.state(id),QString("reviewing"));
  {const auto last=h.rec("inspect",{{"id",id}}).result["lastExport"].toObject();QCOMPARE(last["command"].toString(),QString("saveCopy"));QCOMPARE(last["status"].toString(),QString("saved"));QCOMPARE(last["path"].toString(),copyPath);
   QCOMPARE(last["revision"].toDouble(),2.0);QCOMPARE(last["bytes"].toDouble(),double(h.backend->exportBytes.size()));QCOMPARE(last["durationUs"].toDouble(),6000000.0);}
  {QFile copy(copyPath);QVERIFY(copy.open(QIODevice::ReadOnly));QCOMPARE(copy.readAll(),h.backend->exportBytes);}
  QCOMPARE(h.backend->exports,1);QCOMPARE(h.backend->lastEdit.trimStart,qint64(2000000));QCOMPARE(*h.backend->lastEdit.trimEnd,qint64(8000000));QCOMPARE(h.backend->lastEdit.masks.size(),1);QVERIFY(h.backend->lastConfig.audio.system);QCOMPARE(h.backend->lastConfig.fps,24);
  QCOMPARE(h.rec("saveCopy",{{"id",id},{"path",copyPath}}).error.code,QString("file_exists"));
  QVERIFY(!h.rec("saveCopy",{{"id",id},{"path",copyPath},{"overwrite",true}}).error);
  {HistoryStore journal(h.operations->databasePath());QCOMPARE(journal.operation(id).state,QString("recording"));QCOMPARE(journal.operation(id).items.size(),0);}
  // Only Upload at the reviewed revision admits bytes.
  QCOMPARE(h.rec("upload",{{"id",id},{"revision",1}}).error.code,QString("recording_stale"));
  const auto uploaded=h.rec("upload",{{"id",id},{"revision",2}});QVERIFY2(!uploaded.error,qPrintable(uploaded.error.message));QCOMPARE(uploaded.result["state"].toString(),QString("exporting"));QCOMPARE(uploaded.result["pending"].toString(),QString("upload"));QCOMPARE(uploaded.result["operationId"].toString(),id);
  QTRY_COMPARE(h.backend->discards,1);QCOMPARE(h.backend->exports,3);
  QCOMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));
  HistoryStore journal(h.operations->databasePath());QTRY_COMPARE(journal.operation(id).state,QString("completed"));QTRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
  const auto op=journal.operation(id);QCOMPARE(op.items.size(),1);const auto item=op.items.first();
  QCOMPARE(item.artifact.mime,QString("video/mp4"));QCOMPARE(item.artifact.category,QString("videos"));QVERIFY(!item.artifact.originalFile);QVERIFY2(item.artifact.name.startsWith("Screen-Recording-")&&item.artifact.name.endsWith(".mp4"),qPrintable(item.artifact.name));
  QCOMPARE(item.uploadStatus,QString("local_only"));QCOMPARE(item.local.status,QString("saved"));
  {QFile archived(item.local.path);QVERIFY(archived.open(QIODevice::ReadOnly));QCOMPARE(archived.readAll(),h.backend->exportBytes);}
  ArtifactStore artifacts(h.dir.filePath("data/artifacts"));QVERIFY(!artifacts.verify(item.artifact));
  QTRY_VERIFY(!QDir(h.backend->staging).exists());
  QCOMPARE(h.provider->uploads.load(),0);QCOMPARE(h.source.reads,0);
  QVERIFY(h.rec("capabilities").result["active"].isNull());
 }
 void cancelAndFailureNeverAdmitBytes(){
  Harness h;QTRY_VERIFY(h.operations->ready());
  auto id=h.prepareReady();QVERIFY(!id.isEmpty());
  QCOMPARE(h.rec("start",{{"id",id}}).result["state"].toString(),QString("recording"));
  const auto cancelled=h.rec("cancel",{{"id",id}});QVERIFY(!cancelled.error);QCOMPARE(cancelled.result["state"].toString(),QString("cancelled"));
  QCOMPARE(h.backend->cancels,1);QCOMPARE(h.backend->discards,1);QCOMPARE(h.backend->exports,0);
  HistoryStore journal(h.operations->databasePath());QTRY_COMPARE(journal.operation(id).state,QString("cancelled"));QVERIFY(journal.operation(id).cancelled);QVERIFY(journal.operation(id).items.isEmpty());
  QTRY_VERIFY(!QDir(h.backend->staging).exists());QTRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(h.rec("cancel",{{"id",id}}).error.code,QString("recording_unavailable"));
  // A backend failure at start fails the operation; nothing is retried or uploaded.
  h.backend->failStart=true;id=h.prepareReady();QVERIFY(!id.isEmpty());
  QCOMPARE(h.rec("start",{{"id",id}}).error.code,QString("recording_start_failed"));
  QTRY_COMPARE(journal.operation(id).state,QString("failed"));QCOMPARE(journal.operation(id).error.code,QString("recording_start_failed"));QTRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
  // An export failure keeps the review open and the original unpublished.
  h.backend->failStart=false;h.backend->failExport=true;id=h.prepareReady();QVERIFY(!id.isEmpty());
  QVERIFY(!h.rec("start",{{"id",id}}).error);QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);
  QTRY_COMPARE(h.state(id),QString("reviewing"));
  {const auto last=h.rec("inspect",{{"id",id}}).result["lastExport"].toObject();QCOMPARE(last["command"].toString(),QString("upload"));QCOMPARE(last["status"].toString(),QString("failed"));QCOMPARE(last["error"].toObject()["code"].toString(),QString("recording_export_failed"));}QCOMPARE(journal.operation(id).state,QString("recording"));QVERIFY(journal.operation(id).items.isEmpty());
  h.backend->failExport=false;QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);QTRY_COMPARE(journal.operation(id).state,QString("completed"));
  QCOMPARE(h.provider->uploads.load(),0);
 }
 void helperEventsAndAnimationBudgetsAreHonoured(){
  Harness h;QTRY_VERIFY(h.operations->ready());
  auto doc=h.settings.document();auto limits=doc["limits"].toObject();limits["recordingAnimationSeconds"]=5;doc["limits"]=limits;QVERIFY(h.settings.save(doc,h.settings.revision()));
  QCOMPARE(h.rec("prepare",{{"target",region()},{"format","gif"},{"audio",QJsonObject{{"system",true}}}}).error.code,QString("invalid_request"));
  QCOMPARE(h.rec("prepare",{{"target",region()},{"format","mp4"},{"audio",QJsonObject{{"microphone",true}}}}).error.code,QString("recording_unsupported"));
  const auto id=h.prepareReady({{"format","gif"},{"maxEdge",640}});QVERIFY(!id.isEmpty());
  QVERIFY(!h.rec("start",{{"id",id}}).error);
  // The helper reports progress and then stops on its own (budget reached).
  h.backend->sink({{"id",id},{"event","progress"},{"durationUs",3000000.0},{"bytes",1234.0},{"droppedFrames",0}});
  QTRY_COMPARE(h.rec("inspect",{{"id",id}}).result["progress"].toObject()["bytes"].toDouble(),1234.0);
  h.backend->sink({{"id",id},{"event","stopped"},{"durationUs",12000000.0},{"canvas",QJsonArray{1280,720}}});
  QTRY_COMPARE(h.rec("inspect",{{"id",id}}).result["state"].toString(),QString("reviewing"));
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["outputSize"].toArray(),QJsonArray({640,360}));
  QCOMPARE(h.rec("upload",{{"id",id},{"revision",1}}).error.code,QString("recording_animation_too_long"));
  QCOMPARE(h.backend->exports,0);
  QVERIFY(!h.rec("edit",{{"id",id},{"revision",1},{"commands",QJsonArray{QJsonObject{{"op","setTrim"},{"start",1000000},{"end",5000000}}}}}).error);
  const auto uploaded=h.rec("upload",{{"id",id},{"revision",2}});QVERIFY2(!uploaded.error,qPrintable(uploaded.error.message));QCOMPARE(uploaded.result["state"].toString(),QString("exporting"));
  HistoryStore journal(h.operations->databasePath());QTRY_COMPARE(journal.operation(id).state,QString("completed"));QCOMPARE(journal.operation(id).items.first().artifact.mime,QString("image/gif"));
  QCOMPARE(journal.operation(id).items.first().artifact.category,QString("images"));QVERIFY(journal.operation(id).items.first().artifact.name.endsWith(".gif"));
  // A helper failure event ends the session as failed.
  const auto second=h.prepareReady();QVERIFY(!second.isEmpty());QVERIFY(!h.rec("start",{{"id",second}}).error);
  h.backend->sink({{"id",second},{"event","failed"},{"error",QJsonObject{{"code","recording_source_lost"},{"message","Synthetic source vanished"}}}});
  QTRY_COMPARE(journal.operation(second).state,QString("failed"));QCOMPARE(journal.operation(second).error.code,QString("recording_source_lost"));QCOMPARE(h.backend->exports,1);
 }
 void controlBarEventsDriveTheSession(){
  Harness h;QTRY_VERIFY(h.operations->ready());HistoryStore journal(h.operations->databasePath());
  // A dragged region: prepare without a rect; the helper reports the bounds it got.
  const auto prepared=h.rec("prepare",{{"target",QJsonObject{{"kind","region"}}},{"format","mp4"}});QVERIFY2(!prepared.error,qPrintable(prepared.error.message));const auto id=prepared.result["recordingId"].toString();
  QTRY_COMPARE(h.state(id),QString("ready"));
  h.backend->sink({{"id",id},{"event","configChanged"},{"config",QJsonObject{{"format","webm"},{"codec","vp9"},{"fps",30},{"audio",QJsonObject{{"system",false},{"microphone",false}}}}}});
  QTRY_COMPARE(h.rec("inspect",{{"id",id}}).result["config"].toObject()["format"].toString(),QString("webm"));
  h.backend->sink({{"id",id},{"event","started"},{"encoder","x264enc"}});
  QTRY_COMPARE(h.state(id),QString("recording"));QCOMPARE(h.rec("inspect",{{"id",id}}).result["progress"].toObject()["encoder"].toString(),QString("x264enc"));
  // Config changes after Record are ignored.
  h.backend->sink({{"id",id},{"event","configChanged"},{"config",QJsonObject{{"format","gif"}}}});
  h.backend->sink({{"id",id},{"event","paused"}});QTRY_COMPARE(h.state(id),QString("paused"));
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["config"].toObject()["format"].toString(),QString("webm"));
  h.backend->sink({{"id",id},{"event","resumed"}});QTRY_COMPARE(h.state(id),QString("recording"));
  h.backend->sink({{"id",id},{"event","stopped"},{"durationUs",5000000.0},{"canvas",QJsonArray{800,600}},{"reason","user"}});
  QTRY_COMPARE(h.state(id),QString("reviewing"));QCOMPARE(h.rec("inspect",{{"id",id}}).result["canvas"].toArray(),QJsonArray({800,600}));
  // Cancel from the bar ends the operation without any upload.
  h.backend->sink({{"id",id},{"event","cancelled"}});
  QTRY_COMPARE(journal.operation(id).state,QString("cancelled"));QCOMPARE(h.backend->exports,0);QTRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
 }
 void reviewWindowEventsRoundTripThroughTheResident(){
  Harness h;QTRY_VERIFY(h.operations->ready());HistoryStore journal(h.operations->databasePath());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));
  // A proposed edit at the shown revision is applied and pushed back; a stale one is refused but still answered with the current edit.
  h.backend->sink({{"id",id},{"event","edit"},{"revision",1},{"commands",QJsonArray{QJsonObject{{"op","setTrim"},{"start",2000000},{"end",9000000}}}}});
  QTRY_COMPARE(h.backend->editsPushed,1);QCOMPARE(h.backend->pushedEdit.revision,quint64(2));QCOMPARE(h.backend->pushedEdit.trimStart,qint64(2000000));
  h.backend->sink({{"id",id},{"event","edit"},{"revision",1},{"commands",QJsonArray{QJsonObject{{"op","resetTrim"}}}}});
  QTRY_COMPARE(h.backend->editsPushed,2);QCOMPARE(h.backend->pushedEdit.revision,quint64(2));QCOMPARE(h.backend->pushedEdit.trimStart,qint64(2000000));
  // MCP/CLI edits reach the window the same way.
  QVERIFY(!h.rec("edit",{{"id",id},{"revision",2},{"commands",QJsonArray{QJsonObject{{"op","addMask"},{"mask",QJsonObject{{"kind","solid"},{"rect",QJsonArray{1,1,10,10}},{"start",0},{"end",1000000}}}}}}}).error);
  QTRY_COMPARE(h.backend->editsPushed,3);QCOMPARE(h.backend->pushedEdit.masks.size(),1);
  // A format change in review keeps fps and cannot add audio.
  h.backend->sink({{"id",id},{"event","configChanged"},{"config",QJsonObject{{"format","gif"},{"fps",60},{"audio",QJsonObject{{"system",true}}}}}});
  QTRY_COMPARE(h.rec("inspect",{{"id",id}}).result["config"].toObject()["format"].toString(),QString("gif"));
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["config"].toObject()["fps"].toInt(),30);QVERIFY(!h.rec("inspect",{{"id",id}}).result["config"].toObject()["audio"].toObject()["system"].toBool());
  h.backend->sink({{"id",id},{"event","configChanged"},{"config",QJsonObject{{"format","mp4"}}}});
  QTRY_COMPARE(h.rec("inspect",{{"id",id}}).result["config"].toObject()["format"].toString(),QString("mp4"));
  // Save copy and Upload from the window run the same exports as the CLI; only Upload admits bytes.
  const auto copyPath=h.dir.filePath("review-copy.mp4");
  h.backend->sink({{"id",id},{"event","saveCopyRequested"},{"revision",3},{"path",copyPath}});
  QTRY_VERIFY(QFileInfo::exists(copyPath));QTRY_COMPARE(h.state(id),QString("reviewing"));QCOMPARE(journal.operation(id).items.size(),0);
  h.backend->sink({{"id",id},{"event","uploadRequested"},{"revision",1}});
  QTRY_COMPARE(h.backend->editsPushed,4);QCOMPARE(h.state(id),QString("reviewing"));
  h.backend->sink({{"id",id},{"event","uploadRequested"},{"revision",3}});
  QTRY_COMPARE(journal.operation(id).state,QString("completed"));QCOMPARE(h.backend->exports,2);QCOMPARE(journal.operation(id).items.first().artifact.mime,QString("video/mp4"));
 }
 void serverRejectsMalformedRequestsBeforeTheBackend(){
  Harness h;QTRY_VERIFY(h.operations->ready());
  const QList<QPair<QString,QJsonObject>> bad{
   {"recording.capabilities",{{"x",1}}},
   {"recording.prepare",{{"format","mp4"}}},
   {"recording.prepare",{{"target",region()}}},
   {"recording.prepare",{{"target","region"},{"format","mp4"}}},
   {"recording.prepare",{{"target",region()},{"format","mp4"},{"bitrate",5}}},
   {"recording.prepare",{{"target",region()},{"format","mp4"},{"audio",true}}},
   {"recording.start",{}},{"recording.stop",{{"id",""}}},{"recording.inspect",{{"id","x"},{"extra",1}}},
   {"recording.overlay",{{"id","x"}}},{"recording.overlay",{{"id","x"},{"visible","yes"}}},
   {"recording.edit",{{"id","x"},{"commands",QJsonArray{}}}},{"recording.edit",{{"id","x"},{"revision","1"},{"commands",QJsonArray{}}}},
   {"recording.preview",{{"id","x"}}},{"recording.preview",{{"id","x"},{"time","0"}}},
   {"recording.saveCopy",{{"id","x"}}},{"recording.saveCopy",{{"id","x"},{"path","/p.mp4"},{"overwrite","no"}}},
   {"recording.upload",{{"id","x"}}},{"recording.upload",{{"id","x"},{"revision",true}}}};
  for(const auto& [method,params]:bad){const auto outcome=call(*h.server,method,params);QCOMPARE(outcome.error.code,QString("invalid_request"));}
  QVERIFY(h.backend->log.isEmpty());
  QJsonArray many;for(int i=0;i<101;++i)many.append(QJsonObject{{"op","resetTrim"}});
  QCOMPARE(call(*h.server,"recording.edit",{{"id","x"},{"revision",1},{"commands",many}}).error.code,QString("invalid_request"));
  QCOMPARE(h.rec("prepare",{{"target",QJsonObject{{"kind","monitor"},{"identity",""}}},{"format","mp4"}}).error.code,QString("invalid_request"));
  QCOMPARE(h.rec("prepare",{{"target",region()},{"format","mp4"},{"codec","vp9"}}).error.code,QString("invalid_request"));
  QCOMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
 }
 void recordingBlocksEditingAndRestartInterruptsIt(){
  Harness h;QTRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);
  Error error;bool replied=false;h.operations->invoke("edit",[&](Error e,QJsonObject){error=e;replied=true;},{{"source","clipboard"}});QTRY_VERIFY(replied);QCOMPARE(error.code,QString("recording_active"));
  const auto staging=h.backend->staging;QVERIFY(QDir(staging).exists());
  QFile leftover(staging+"/export-9.mp4");QVERIFY(leftover.open(QIODevice::WriteOnly));leftover.write("abandoned");leftover.close();
  h.close();
  h.open();QTRY_VERIFY(h.operations->ready());
  HistoryStore journal(h.operations->databasePath());const auto op=journal.operation(id);
  QCOMPARE(op.state,QString("interrupted"));QCOMPARE(op.error.code,QString("recording_interrupted"));QVERIFY(op.items.isEmpty());
  QVERIFY(!QDir(staging).exists());
  QCOMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));
  QCOMPARE(h.provider->uploads.load(),0);
 }
 void recordActionOpensRaisesAndTogglesTheSession(){
  Harness h;QTRY_VERIFY(h.operations->ready());
  auto invoke=[&](const QJsonObject& options){Call c;h.operations->invoke("record",[&](Error e,QJsonObject r){c={e,r,true};},options);for(int i=0;i<500&&!c.done;++i)QTest::qWait(10);return c;};
  QCOMPARE(invoke({{"mode","shell"}}).error.code,QString("invalid_request"));
  QCOMPARE(invoke({{"source","clipboard"}}).error.code,QString("invalid_request"));
  auto opened=invoke({});QVERIFY2(!opened.error,qPrintable(opened.error.message));QCOMPARE(opened.result["step"].toString(),QString("prepare"));
  const auto id=opened.result["recordingId"].toString();QVERIFY(!id.isEmpty());QTRY_COMPARE(h.state(id),QString("ready"));
  QCOMPARE(h.backend->lastConfig.format,QString("mp4"));
  // Open again raises the active session instead of starting a second one.
  QCOMPARE(invoke({{"mode","open"}}).result["step"].toString(),QString("raise"));QVERIFY(h.backend->log.contains("raise"));QCOMPARE(h.state(id),QString("ready"));
  QCOMPARE(invoke({{"mode","toggle"}}).result["step"].toString(),QString("start"));QCOMPARE(h.state(id),QString("recording"));
  QVERIFY(!h.rec("pause",{{"id",id}}).error);
  QCOMPARE(invoke({{"mode","toggle"}}).result["step"].toString(),QString("stop"));QCOMPARE(h.state(id),QString("reviewing"));
  QCOMPARE(invoke({{"mode","toggle"}}).result["step"].toString(),QString("raise"));QCOMPARE(h.state(id),QString("reviewing"));
  QVERIFY(!h.rec("cancel",{{"id",id}}).error);QCOMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));
 }
 // Choosing GIF on a canvas wider than the animation limit sets maxEdge to the
 // limit; a refused Upload from the (closed) review window reopens it with the
 // reason instead of leaving the session silently in reviewing.
 void animationLimitsClampSizeAndRefusedUploadReopensTheReview(){
  Harness h;QTRY_VERIFY(h.operations->ready());
  {auto g=call(*h.server,"settings.get",{});auto doc=g.result["document"].toObject();auto limits=doc["limits"].toObject();limits["recordingAnimationEdge"]=640;limits["recordingAnimationSeconds"]=5;doc["limits"]=limits;
   const auto saved=call(*h.server,"settings.save",{{"document",doc},{"expectedRevision",g.result["metadata"].toObject()["observedRevision"]}});QVERIFY2(!saved.error,qPrintable(saved.error.message));}
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));
  // The fake backend's canvas is 1280x720; the review picks GIF.
  h.backend->sink({{"id",id},{"event","configChanged"},{"config",QJsonObject{{"format","gif"},{"fps",30},{"audio",QJsonObject{{"system",false}}}}}});QTest::qWait(50);
  auto inspected=h.rec("inspect",{{"id",id}}).result;QCOMPARE(inspected["config"].toObject()["format"].toString(),QString("gif"));QCOMPARE(inspected["config"].toObject()["maxEdge"].toInt(),640);QCOMPARE(inspected["outputSize"].toArray()[0].toInt(),640);
  QCOMPARE(h.backend->pushedConfig.maxEdge.value_or(0),640);QCOMPARE(h.backend->pushedLimits["animationEdge"].toInt(),640);QCOMPARE(h.backend->pushedLimits["animationSeconds"].toDouble(),5.0);
  // 12 s retained exceeds the 5 s limit: Upload from the review is refused and the review is raised with the reason.
  const int raises=h.backend->log.count("raise");
  h.backend->sink({{"id",id},{"event","uploadRequested"},{"revision",double(inspected["edit"].toObject()["revision"].toDouble())}});QTest::qWait(100);
  QCOMPARE(h.state(id),QString("reviewing"));QCOMPARE(h.backend->exports,0);QCOMPARE(h.backend->log.count("raise"),raises+1);
  QVERIFY2(h.backend->lastControlParams["error"].toString().contains("limited to 5 s"),qPrintable(h.backend->lastControlParams["error"].toString()));
  // Trimmed to the limit, the upload goes through.
  const qint64 revision=qint64(h.rec("inspect",{{"id",id}}).result["edit"].toObject()["revision"].toDouble());
  const auto edited=h.rec("edit",{{"id",id},{"revision",double(revision)},{"commands",QJsonArray{QJsonObject{{"op","setTrim"},{"start",0},{"end",4000000}}}}});QVERIFY2(!edited.error,qPrintable(edited.error.message));
  const auto uploaded=h.rec("upload",{{"id",id},{"revision",double(revision+1)}});QVERIFY2(!uploaded.error,qPrintable(uploaded.error.message));
  HistoryStore journal(h.operations->databasePath());QTRY_COMPARE(journal.operation(id).state,QString("completed"));QCOMPARE(journal.operation(id).items.first().artifact.mime,QString("image/gif"));
 }
 // An export can outlast any client wait (the helper allows 600 s, the CLI 15 s):
 // saveCopy and upload answer once it starts, so the production CLI reports the
 // accepted export instead of transport_failed, and inspect carries the outcome.
 void slowExportsAnswerAtOnceAndReportThroughInspect(){
  Harness h;QTRY_VERIFY(h.operations->ready());HistoryStore journal(h.operations->databasePath());
  QVERIFY(h.server->listen(h.dir.filePath("runtime")));QVERIFY(h.server->socketPath().toUtf8().size()<=107);
  auto cli=[&](const QString& method,const QJsonObject& params){
   QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",h.server->socketPath(),method,QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))});
   [&]{QTRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,10000);}();
   const auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();
   return std::pair{client.exitCode(),response};
  };
  h.backend->deferExports=true;
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));
  QVERIFY(h.rec("inspect",{{"id",id}}).result["lastExport"].isNull());
  // The copy is acknowledged while the export is still running.
  const auto copyPath=h.dir.filePath("slow-copy.mp4");
  const auto [savedExit,saved]=cli("recording.saveCopy",{{"id",id},{"path",copyPath}});
  QVERIFY2(saved["ok"].toBool(),QJsonDocument(saved).toJson().constData());QCOMPARE(savedExit,0);
  QCOMPARE(saved["result"].toObject()["state"].toString(),QString("exporting"));QCOMPARE(saved["result"].toObject()["lastExport"].toObject()["status"].toString(),QString("exporting"));
  QCOMPARE(h.backend->pendingExports.size(),1);QVERIFY(!QFileInfo::exists(copyPath));
  auto inspected=h.rec("inspect",{{"id",id}}).result;QCOMPARE(inspected["state"].toString(),QString("exporting"));QCOMPARE(inspected["pending"].toString(),QString("saveCopy"));
  QCOMPARE(inspected["lastExport"].toObject()["path"].toString(),copyPath);QCOMPARE(inspected["lastExport"].toObject()["revision"].toDouble(),1.0);
  QCOMPARE(call(*h.server,"operation.inspect",{{"id",id}}).result["phase"].toString(),QString("exporting"));
  // Progress is visible meanwhile, and the review is locked until the export ends.
  h.backend->sink({{"id",id},{"event","progress"},{"exportFraction",0.5}});
  QTRY_COMPARE(h.rec("inspect",{{"id",id}}).result["progress"].toObject()["exportFraction"].toDouble(),0.5);
  QCOMPARE(h.rec("saveCopy",{{"id",id},{"path",h.dir.filePath("second.mp4")}}).error.code,QString("recording_state"));
  QCOMPARE(h.rec("upload",{{"id",id},{"revision",1}}).error.code,QString("recording_state"));
  QCOMPARE(h.rec("edit",{{"id",id},{"revision",1},{"commands",QJsonArray{QJsonObject{{"op","resetTrim"}}}}}).error.code,QString("recording_state"));
  h.backend->pendingExports.takeFirst()();
  QTRY_COMPARE(h.state(id),QString("reviewing"));
  inspected=h.rec("inspect",{{"id",id}}).result;QVERIFY(inspected["pending"].isNull());QCOMPARE(inspected["lastExport"].toObject()["status"].toString(),QString("saved"));QCOMPARE(inspected["lastExport"].toObject()["bytes"].toDouble(),double(h.backend->exportBytes.size()));
  {QFile copy(copyPath);QVERIFY(copy.open(QIODevice::ReadOnly));QCOMPARE(copy.readAll(),h.backend->exportBytes);}
  // A failed slow export is reported the same way and writes nothing.
  const auto failedPath=h.dir.filePath("failed-copy.mp4");h.backend->failExport=true;
  QCOMPARE(h.rec("saveCopy",{{"id",id},{"path",failedPath}}).result["state"].toString(),QString("exporting"));
  h.backend->pendingExports.takeFirst()();QTRY_COMPARE(h.state(id),QString("reviewing"));
  inspected=h.rec("inspect",{{"id",id}}).result;QCOMPARE(inspected["lastExport"].toObject()["status"].toString(),QString("failed"));QCOMPARE(inspected["lastExport"].toObject()["error"].toObject()["code"].toString(),QString("recording_export_failed"));
  QVERIFY(!QFileInfo::exists(failedPath));h.backend->failExport=false;
  // Upload: acknowledged at the start of its export; the operation then carries the outcome.
  const auto [uploadExit,uploaded]=cli("recording.upload",{{"id",id},{"revision",1}});
  QVERIFY2(uploaded["ok"].toBool(),QJsonDocument(uploaded).toJson().constData());QCOMPARE(uploadExit,0);
  QCOMPARE(uploaded["result"].toObject()["state"].toString(),QString("exporting"));QCOMPARE(uploaded["result"].toObject()["pending"].toString(),QString("upload"));QCOMPARE(uploaded["result"].toObject()["operationId"].toString(),id);
  QCOMPARE(h.backend->pendingExports.size(),1);QCOMPARE(call(*h.server,"operation.inspect",{{"id",id}}).result["phase"].toString(),QString("encoding"));
  QCOMPARE(journal.operation(id).state,QString("recording"));QVERIFY(journal.operation(id).items.isEmpty());
  h.backend->pendingExports.takeFirst()();
  QTRY_COMPARE(journal.operation(id).state,QString("completed"));QCOMPARE(journal.operation(id).items.size(),1);
  QCOMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));
  // Cancelling during a slow export discards it: the late result admits nothing.
  const auto second=h.prepareReady();QVERIFY(!second.isEmpty());QVERIFY(!h.rec("start",{{"id",second}}).error);QVERIFY(!h.rec("stop",{{"id",second}}).error);
  QCOMPARE(h.rec("upload",{{"id",second},{"revision",1}}).result["state"].toString(),QString("exporting"));
  QCOMPARE(h.rec("cancel",{{"id",second}}).result["state"].toString(),QString("cancelled"));
  h.backend->pendingExports.takeFirst()();QTRY_COMPARE(journal.operation(second).state,QString("cancelled"));QVERIFY(journal.operation(second).items.isEmpty());
  QTRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);QCOMPARE(h.provider->uploads.load(),0);
 }
 // The CLI waits past every deadline the resident itself waits on, so the
 // resident's answer (recording_helper_timeout included) always reaches it.
 void clientWaitsOutlastHelperDeadlines(){
  QVERIFY(LocalServer::replyDeadlineMs("recording.stop")>recording::helperStopMs);
  for(const auto* method:{"recording.capabilities","recording.prepare","recording.select","recording.start","recording.pause","recording.resume","recording.raise","recording.overlay","recording.preview"})
   QVERIFY2(LocalServer::replyDeadlineMs(method)>recording::helperCommandMs,method);
  // Exports answer when they start, so they keep the ordinary wait.
  for(const auto* method:{"recording.saveCopy","recording.upload","recording.inspect","capabilities"})QCOMPARE(LocalServer::replyDeadlineMs(method),15000);
 }
 // The activity window's Cancel is operation.cancel: for a recording it must end
 // the session (capture or review), not only mark the journal record.
 void operationCancelEndsAnActiveRecording(){
  Harness h;QTRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));
  const auto cancelled=call(*h.server,"operation.cancel",{{"id",id}});QVERIFY2(!cancelled.error,qPrintable(cancelled.error.message));QVERIFY(cancelled.result["cancellationRequested"].toBool());
  QTRY_COMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));
  QString state;for(int i=0;i<200&&state!="cancelled";++i){state=call(*h.server,"operation.inspect",{{"id",id}}).result["state"].toString();if(state!="cancelled")QTest::qWait(10);}QCOMPARE(state,QString("cancelled"));QCOMPARE(h.backend->discards,1);
  QVERIFY(h.operations->activeObservations().isEmpty());
 }
 // GNOME: the Shell selects the rectangle before the helper starts a stream,
 // draws the ring and dimming, and moves the bar; overlayExcluded follows it.
 void shellSelectsRegionsAndFramesTheBar(){
  Harness h;QTRY_VERIFY(h.operations->ready());FakeShell shell;h.operations->recording().setShell(&shell);
  h.backend->placement="compositor";h.backend->pid=4242;
  const auto prepared=h.rec("prepare",{{"target",QJsonObject{{"kind","region"}}},{"format","mp4"}});QVERIFY2(!prepared.error,qPrintable(prepared.error.message));const auto id=prepared.result["recordingId"].toString();
  QTRY_COMPARE(shell.selections.size(),1);QVERIFY(!h.backend->log.contains("prepare"));
  auto inspected=h.rec("inspect",{{"id",id}}).result;QCOMPARE(inspected["state"].toString(),QString("preparing"));QVERIFY(inspected["selecting"].toBool());
  shell.selections.takeFirst()({},{{"rect",QJsonArray{50,60,640,360}}});
  QTRY_COMPARE(h.state(id),QString("ready"));
  QCOMPARE(h.backend->lastTarget.rect,QRect(50,60,640,360));
  inspected=h.rec("inspect",{{"id",id}}).result;QVERIFY(!inspected["selecting"].toBool());QVERIFY(inspected["overlayExcluded"].toBool());QCOMPARE(inspected["overlayPlacement"].toString(),QString("outside"));
  QCOMPARE(inspected["target"].toObject()["rect"].toArray(),QJsonArray({50,60,640,360}));
  QCOMPARE(shell.frames.size(),1);QCOMPARE(shell.frames[0]["state"].toString(),QString("ready"));QCOMPARE(shell.frames[0]["rect"].toArray(),QJsonArray({50,60,640,360}));QCOMPARE(shell.frames[0]["pid"].toInt(),4242);QCOMPARE(shell.frames[0]["kind"].toString(),QString("region"));
  QVERIFY(!h.rec("start",{{"id",id}}).error);QCOMPARE(shell.frames.last()["state"].toString(),QString("recording"));
  QVERIFY(!h.rec("pause",{{"id",id}}).error);QCOMPARE(shell.frames.last()["state"].toString(),QString("paused"));QCOMPARE(shell.hides,0);
  QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));QCOMPARE(shell.hides,1);
  QVERIFY(!h.rec("cancel",{{"id",id}}).error);QCOMPARE(shell.hides,1);
  // A bar the Shell could not place (an older extension) stays reported as possibly recorded.
  shell.failFrames=true;
  const auto second=h.prepareReady({{"target",QJsonObject{{"kind","monitor"}}}});QVERIFY(!second.isEmpty());
  inspected=h.rec("inspect",{{"id",second}}).result;QVERIFY(!inspected["overlayExcluded"].toBool());QCOMPARE(inspected["overlayPlacement"].toString(),QString("compositor"));
  QVERIFY(!h.rec("cancel",{{"id",second}}).error);QCOMPARE(shell.hides,2);
 }
 void cancellingAShellSelectionLeavesNoSession(){
  Harness h;QTRY_VERIFY(h.operations->ready());HistoryStore journal(h.operations->databasePath());FakeShell shell;h.operations->recording().setShell(&shell);
  const QJsonObject dragged{{"target",QJsonObject{{"kind","region"}}},{"format","mp4"}};
  // Escape in the Shell selector: the operation ends cancelled and no helper ever started.
  auto id=h.rec("prepare",dragged).result["recordingId"].toString();QTRY_COMPARE(shell.selections.size(),1);
  shell.selections.takeFirst()({"cancelled","GNOME region selection cancelled",{}},{});
  QTRY_COMPARE(journal.operation(id).state,QString("cancelled"));QVERIFY(!h.backend->log.contains("prepare"));QTRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
  QVERIFY(h.rec("capabilities").result["active"].isNull());
  // recording.cancel closes the open selector.
  id=h.rec("prepare",dragged).result["recordingId"].toString();QTRY_COMPARE(shell.selections.size(),1);
  QVERIFY(!h.rec("cancel",{{"id",id}}).error);QCOMPARE(shell.cancels,1);QTRY_COMPARE(journal.operation(id).state,QString("cancelled"));QVERIFY(!h.backend->log.contains("prepare"));
  // recording.select drives the selector like a drag; the superseded selector reply is ignored.
  id=h.rec("prepare",dragged).result["recordingId"].toString();QTRY_COMPARE(shell.selections.size(),1);
  QCOMPARE(h.rec("select",{{"id",id},{"rect",QJsonArray{1,2,0,5}}}).error.code,QString("invalid_request"));
  QCOMPARE(call(*h.server,"recording.select",{{"id",id}}).error.code,QString("invalid_request"));
  const auto selected=h.rec("select",{{"id",id},{"rect",QJsonArray{20,30,800,450}}});QVERIFY2(!selected.error,qPrintable(selected.error.message));QCOMPARE(shell.cancels,2);
  QTRY_COMPARE(h.state(id),QString("ready"));QCOMPARE(h.backend->lastTarget.rect,QRect(20,30,800,450));
  QCOMPARE(h.rec("select",{{"id",id},{"rect",QJsonArray{20,30,800,450}}}).error.code,QString("recording_state"));
  QVERIFY(!h.rec("cancel",{{"id",id}}).error);
  // A shell without recording support leaves the region to the helper.
  shell.selects=false;id=h.prepareReady({{"target",QJsonObject{{"kind","region"}}}});QVERIFY(!id.isEmpty());QCOMPARE(shell.selections.size(),0);
  QVERIFY(!h.rec("cancel",{{"id",id}}).error);
 }
 // X11 and KDE: the helper's own selector reports `selecting`; select forwards the rectangle.
 void helperSelectionIsDrivenThroughSelect(){
  Harness h;QTRY_VERIFY(h.operations->ready());h.backend->deferPrepare=true;
  const auto id=h.rec("prepare",{{"target",QJsonObject{{"kind","region"}}},{"format","mp4"}}).result["recordingId"].toString();QTRY_VERIFY(h.backend->log.contains("prepare"));
  QCOMPARE(h.rec("select",{{"id",id},{"rect",QJsonArray{10,10,300,200}}}).error.code,QString("recording_state"));
  h.backend->sink({{"id",id},{"event","selecting"},{"selector","kde-layer-shell"}});
  QTRY_VERIFY(h.rec("inspect",{{"id",id}}).result["selecting"].toBool());
  QVERIFY(!h.rec("select",{{"id",id},{"rect",QJsonArray{10,10,300,200}}}).error);
  QCOMPARE(h.backend->log.last(),QString("select"));QCOMPARE(h.backend->lastControlParams["rect"].toArray(),QJsonArray({10,10,300,200}));
  h.backend->pendingPrepare({},{});QTRY_COMPARE(h.state(id),QString("ready"));QVERIFY(!h.rec("inspect",{{"id",id}}).result["selecting"].toBool());
  QVERIFY(!h.rec("cancel",{{"id",id}}).error);
 }
};

QTEST_GUILESS_MAIN(RecordingServiceContract)
#include "test_recording_service.moc"
