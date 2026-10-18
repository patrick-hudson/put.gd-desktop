#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
#include "app/RecordingBackend.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
using namespace putgd;using namespace putgd::test;

namespace {
// A synchronous stand-in for the recording helper supervisor: it never touches
// media, writes a synthetic export file and records every command it saw.
class FakeBackend final:public RecordingBackend {
public:
 Event sink;QStringList log;QString staging;bool failStart=false,failExport=false,available=true;int exports=0,previews=0,discards=0,cancels=0;qint64 durationUs=12000000;
 QByteArray exportBytes=QByteArray("\0\0\0\x18" "ftypisom",12)+QByteArray(4096,'r');RecordingEdit lastEdit;RecordingConfig lastConfig;qint64 lastPreviewTime=-1;
 QJsonObject capabilities() const override {return {{"available",available},{"reason",available?QString():QString("fake_off")},{"targets",QJsonArray{"monitor","desktop","region","window"}},{"formats",QJsonArray{"mp4","webm","gif","webp"}},{"codecs",QJsonObject{{"mp4",QJsonArray{"h264","h265"}},{"webm",QJsonArray{"vp9"}}}},{"audio",QJsonObject{{"system",true},{"microphone",false}}},{"overlayExclusion",QJsonObject{{"region",true}}}};}
 void setEventSink(Event e) override {sink=std::move(e);}
 void prepare(const QString&,const RecordingTarget& target,const RecordingConfig& config,const QString& dir,Reply reply) override {log<<"prepare";staging=dir;lastConfig=config;reply({},{{"canvas",QJsonArray{1280,720}},{"rect",target.rect.isEmpty()?QJsonArray{300,250,1280,720}:QJsonArray{target.rect.x(),target.rect.y(),target.rect.width(),target.rect.height()}},{"overlayExcluded",true}});}
 void control(const QString&,const QString& command,const QJsonObject&,Reply reply) override {
  log<<command;
  if(command=="cancel"){++cancels;reply({},{});return;}
  if(command=="start"&&failStart){reply({"recording_start_failed","Synthetic source refused",{}},{});return;}
  if(command=="stop"){reply({},{{"durationUs",double(durationUs)},{"canvas",QJsonArray{1280,720}}});return;}
  reply({},{});
 }
 void preview(const QString&,const RecordingEdit& edit,const RecordingConfig&,qint64 time,int,Reply reply) override {++previews;lastEdit=edit;lastPreviewTime=time;reply({},{{"png",QString::fromLatin1(QByteArray("\x89PNG\r\n\x1a\n",8).toBase64())}});}
 void exportClip(const QString&,const RecordingEdit& edit,const RecordingConfig& config,const QString& output,Reply reply) override {
  ++exports;lastEdit=edit;lastConfig=config;log<<"export";
  if(failExport){reply({"recording_export_failed","Synthetic export failure",{}},{});return;}
  QFile file(output);if(!file.open(QIODevice::WriteOnly)){reply({"recording_io","cannot write",{}},{});return;}file.write(exportBytes);file.close();
  reply({},{{"bytes",double(exportBytes.size())},{"durationUs",double(edit.exportDuration(durationUs))}});
 }
 void discard(const QString&) override {++discards;}
 int editsPushed=0;RecordingEdit pushedEdit;
 void setEdit(const QString&,const RecordingEdit& edit,const RecordingConfig&) override {++editsPushed;pushedEdit=edit;}
};
struct Call {Error error;QJsonObject result;bool done=false;};
Call call(LocalServer& server,const QString& method,const QJsonObject& params){
 Call outcome;server.dispatch({{"schemaVersion",1},{"requestId",method},{"session",server.socketPath()},{"method",method},{"params",params}},[&](Error e,QJsonObject r){outcome={e,r,true};});
 for(int i=0;i<500&&!outcome.done;++i)QTest::qWait(10);
 return outcome;
}
QJsonObject region(){return {{"kind","region"},{"rect",QJsonArray{100,100,1280,720}}};}
struct Harness {
 QTemporaryDir dir;ProviderRegistry registry;std::shared_ptr<OpaqueProvider> provider=std::make_shared<OpaqueProvider>();SettingsService settings;Source source;std::unique_ptr<OperationService> operations;std::unique_ptr<HistoryService> history;std::unique_ptr<LocalServer> server;FakeBackend* backend=nullptr;
 explicit Harness(bool local=true):settings(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);}){
  registry.add("opaque",provider);
  auto doc=configuration(dir.path(),local);if(!local){auto archive=doc["archive"].toObject();archive["keepUploads"]=true;doc["archive"]=archive;}
  if(!settings.save(doc,{}))qFatal("configuration rejected");
  open();
 }
 void open(){operations=std::make_unique<OperationService>(dir.filePath("data"),settings,registry,source);auto fake=std::make_unique<FakeBackend>();backend=fake.get();operations->setRecordingBackend(std::move(fake));history=std::make_unique<HistoryService>(*operations);server=std::make_unique<LocalServer>(settings,*operations,*history);}
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
  const auto saved=h.rec("saveCopy",{{"id",id},{"path",copyPath}});QVERIFY2(!saved.error,qPrintable(saved.error.message));QCOMPARE(saved.result["state"].toString(),QString("reviewing"));
  {QFile copy(copyPath);QVERIFY(copy.open(QIODevice::ReadOnly));QCOMPARE(copy.readAll(),h.backend->exportBytes);}
  QCOMPARE(h.backend->exports,1);QCOMPARE(h.backend->lastEdit.trimStart,qint64(2000000));QCOMPARE(*h.backend->lastEdit.trimEnd,qint64(8000000));QCOMPARE(h.backend->lastEdit.masks.size(),1);QVERIFY(h.backend->lastConfig.audio.system);QCOMPARE(h.backend->lastConfig.fps,24);
  QCOMPARE(h.rec("saveCopy",{{"id",id},{"path",copyPath}}).error.code,QString("file_exists"));
  QVERIFY(!h.rec("saveCopy",{{"id",id},{"path",copyPath},{"overwrite",true}}).error);
  {HistoryStore journal(h.operations->databasePath());QCOMPARE(journal.operation(id).state,QString("recording"));QCOMPARE(journal.operation(id).items.size(),0);}
  // Only Upload at the reviewed revision admits bytes.
  QCOMPARE(h.rec("upload",{{"id",id},{"revision",1}}).error.code,QString("recording_stale"));
  const auto uploaded=h.rec("upload",{{"id",id},{"revision",2}});QVERIFY2(!uploaded.error,qPrintable(uploaded.error.message));QCOMPARE(uploaded.result["state"].toString(),QString("submitted"));QCOMPARE(uploaded.result["mime"].toString(),QString("video/mp4"));
  QCOMPARE(h.backend->exports,3);QCOMPARE(h.backend->discards,1);
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
  QCOMPARE(h.rec("upload",{{"id",id},{"revision",1}}).error.code,QString("recording_export_failed"));
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["state"].toString(),QString("reviewing"));QCOMPARE(journal.operation(id).state,QString("recording"));QVERIFY(journal.operation(id).items.isEmpty());
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
  const auto uploaded=h.rec("upload",{{"id",id},{"revision",2}});QVERIFY2(!uploaded.error,qPrintable(uploaded.error.message));QCOMPARE(uploaded.result["mime"].toString(),QString("image/gif"));
  HistoryStore journal(h.operations->databasePath());QTRY_COMPARE(journal.operation(id).state,QString("completed"));
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
  QCOMPARE(h.rec("prepare",{{"target",QJsonObject{{"kind","monitor"}}},{"format","mp4"}}).error.code,QString("invalid_request"));
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
};
QTEST_GUILESS_MAIN(RecordingServiceContract)
#include "test_recording_service.moc"
