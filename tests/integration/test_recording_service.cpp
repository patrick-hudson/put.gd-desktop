#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
#include "fixtures/FakeRecordingBackend.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;

namespace {
struct Call {Error error;QJsonObject result;bool done=false;};
Call call(LocalServer& server,const QString& method,const QJsonObject& params){
 Call outcome;server.dispatch({{"schemaVersion",1},{"requestId",method},{"session",server.socketPath()},{"method",method},{"params",params}},[&](Error e,QJsonObject r){outcome={e,r,true};});
 (void)putgd::test::waitFor([&]{return outcome.done;});
 return outcome;
}
QJsonObject region(){return {{"kind","region"},{"rect",QJsonArray{100,100,1280,720}}};}
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
  (void)putgd::test::waitFor([&]{return rec("inspect",{{"id",id}}).result["state"]=="ready";},3000);
  return id;
 }
};
}

class RecordingServiceContract:public QObject {
 Q_OBJECT
private slots:
 void reviewDestinationUsesTheAcceptedSnapshot_data(){
  QTest::addColumn<QString>("mode");
  for(const auto* mode:{"private","local"})QTest::newRow(mode)<<QString(mode);
 }
 void reviewDestinationUsesTheAcceptedSnapshot(){
  QFETCH(QString,mode);Harness h(mode=="local");PUTGD_TRY_VERIFY(h.operations->ready());
  if(mode!="local"){
   auto doc=h.settings.snapshot().document;auto actions=doc["actions"].toObject();auto route=actions["region"].toObject();route["publicationMode"]=mode;route["shortcut"]="";actions["record"]=route;doc["actions"]=actions;
   QVERIFY(h.settings.save(doc,h.settings.revision()));
  }
  h.backend->deferPrepare=true;
  const auto prepared=h.rec("prepare",{{"target",region()},{"format","mp4"}});QVERIFY(!prepared.error);
  PUTGD_TRY_VERIFY(bool(h.backend->pendingPrepare));
  const auto snapshot=h.backend->lastReviewOptions;
  QCOMPARE(snapshot["sharingMode"].toString(),mode=="default"?QString("share"):mode);
  if(mode=="local")QCOMPARE(snapshot["summary"].toString(),QString("Save to the local archive"));
  else {
   QCOMPARE(snapshot["destination"].toString(),QString("Synthetic opaque provider"));
   QVERIFY(snapshot["summary"].toString().contains(mode=="private"?"Private upload to":"Shared link"));
   auto doc=h.settings.snapshot().document;auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["label"]="Changed after acceptance";profiles[0]=profile;doc["profiles"]=profiles;QVERIFY(h.settings.save(doc,h.settings.revision()));
  }
  auto ready=std::move(h.backend->pendingPrepare);ready({},{});
  const auto id=prepared.result["recordingId"].toString();PUTGD_TRY_COMPARE(h.state(id),QString("ready"));
  QCOMPARE(h.backend->lastReviewOptions,snapshot);
  QVERIFY(!h.rec("cancel",{{"id",id}}).error);
 }
 void lifecycleAdmitsOnlyTheExplicitUploadExport(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto prepared=h.rec("prepare",{{"target",region()},{"format","mp4"},{"audio",QJsonObject{{"system",true}}},{"fps",24}});
  QVERIFY2(!prepared.error,qPrintable(prepared.error.message));const auto id=prepared.result["recordingId"].toString();QVERIFY(!id.isEmpty());QCOMPARE(prepared.result["operationId"].toString(),id);
  QCOMPARE(prepared.result["state"].toString(),QString("preparing"));
  PUTGD_TRY_COMPARE(h.rec("inspect",{{"id",id}}).result["state"].toString(),QString("ready"));
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["canvas"].toArray(),QJsonArray({1280,720}));
  QCOMPARE(h.rec("capabilities").result["active"].toString(),id);
  // Durable intent exists before any capture; its journal state is `recording`.
  {HistoryStore journal(h.operations->databasePath());const auto op=journal.operation(id);QCOMPARE(op.stateName(),QString("recording"));QCOMPARE(op.action,QString("record"));}
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
  QCOMPARE(stopped.result["stopReason"].toString(),QString("user"));QCOMPARE(h.rec("inspect",{{"id",id}}).result,stopped.result);
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
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  {const auto last=h.rec("inspect",{{"id",id}}).result["lastExport"].toObject();QCOMPARE(last["command"].toString(),QString("saveCopy"));QCOMPARE(last["status"].toString(),QString("saved"));QCOMPARE(last["path"].toString(),copyPath);
   QCOMPARE(last["revision"].toDouble(),2.0);QCOMPARE(last["bytes"].toDouble(),double(h.backend->exportBytes.size()));QCOMPARE(last["durationUs"].toDouble(),6000000.0);}
  {QFile copy(copyPath);QVERIFY(copy.open(QIODevice::ReadOnly));QCOMPARE(copy.readAll(),h.backend->exportBytes);}
  QVERIFY(h.backend->log.contains("savedCopy"));QCOMPARE(h.backend->lastControlParams["path"].toString(),copyPath);QCOMPARE(h.backend->lastControlParams["revision"].toDouble(),2.0);
  QCOMPARE(h.backend->exports,1);QCOMPARE(h.backend->lastEdit.trimStart,qint64(2000000));QCOMPARE(*h.backend->lastEdit.trimEnd,qint64(8000000));QCOMPARE(h.backend->lastEdit.masks.size(),1);QVERIFY(h.backend->lastConfig.audio.system);QCOMPARE(h.backend->lastConfig.fps,24);
  QCOMPARE(h.rec("saveCopy",{{"id",id},{"path",copyPath}}).error.code,QString("file_exists"));
  QVERIFY(!h.rec("saveCopy",{{"id",id},{"path",copyPath},{"overwrite",true}}).error);
  // Export acceptance precedes its queued completion. Check stale revisions
  // once the overwrite has actually returned the session to private review.
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["lastExport"].toObject()["status"].toString(),QString("saved"));
  {HistoryStore journal(h.operations->databasePath());QCOMPARE(journal.operation(id).stateName(),QString("recording"));QCOMPARE(journal.operation(id).items.size(),0);}
  // Only Upload at the reviewed revision admits bytes.
  QCOMPARE(h.rec("upload",{{"id",id},{"revision",1}}).error.code,QString("recording_stale"));
  const auto uploaded=h.rec("upload",{{"id",id},{"revision",2}});QVERIFY2(!uploaded.error,qPrintable(uploaded.error.message));QCOMPARE(uploaded.result["state"].toString(),QString("exporting"));QCOMPARE(uploaded.result["pending"].toString(),QString("upload"));QCOMPARE(uploaded.result["operationId"].toString(),id);
  PUTGD_TRY_COMPARE(h.backend->discards,1);QCOMPARE(h.backend->exports,3);
  QCOMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));
  HistoryStore journal(h.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(id).stateName(),QString("completed"));PUTGD_TRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
  const auto op=journal.operation(id);QCOMPARE(op.items.size(),1);const auto item=op.items.first();
  QCOMPARE(item.artifact.mime,QString("video/mp4"));QCOMPARE(item.artifact.category,QString("videos"));QVERIFY(!item.artifact.originalFile);QVERIFY2(item.artifact.name.startsWith("Screen-Recording-")&&item.artifact.name.endsWith(".mp4"),qPrintable(item.artifact.name));
  QCOMPARE(item.uploadStatus,QString("local_only"));QCOMPARE(item.local.status,QString("saved"));
  {QFile archived(item.local.path);QVERIFY(archived.open(QIODevice::ReadOnly));QCOMPARE(archived.readAll(),h.backend->exportBytes);}
  ArtifactStore artifacts(h.dir.filePath("data/artifacts"));QVERIFY(!artifacts.verify(item.artifact));
  PUTGD_TRY_VERIFY(!QDir(h.backend->staging).exists());
  QCOMPARE(h.provider->uploads.load(),0);QCOMPARE(h.source.reads,0);
  QVERIFY(h.rec("capabilities").result["active"].isNull());
 }
 // #1613: preview reports a cut as not kept and maps output time through the cuts.
 void previewFollowsCuts(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());
  QVERIFY(!h.rec("start",{{"id",id}}).error);QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));
  const auto cut=h.rec("edit",{{"id",id},{"revision",1},{"commands",QJsonArray{QJsonObject{{"op","setTrim"},{"start",1000000},{"end",9000000}},QJsonObject{{"op","addCut"},{"start",3000000},{"end",5000000}}}}});
  QVERIFY2(!cut.error,qPrintable(cut.error.message));QCOMPARE(cut.result["retainedUs"].toDouble(),6000000.0);
  const auto inside=h.rec("preview",{{"id",id},{"time",4000000}});QVERIFY(!inside.error);QVERIFY(!inside.result["retained"].toBool());QVERIFY(inside.result["outputTimeUs"].isNull());
  const auto after=h.rec("preview",{{"id",id},{"time",6000000}});QVERIFY(!after.error);QVERIFY(after.result["retained"].toBool());QCOMPARE(after.result["outputTimeUs"].toDouble(),3000000.0);
 }
 void cancelAndFailureNeverAdmitBytes(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  auto id=h.prepareReady();QVERIFY(!id.isEmpty());
  QCOMPARE(h.rec("start",{{"id",id}}).result["state"].toString(),QString("recording"));
  const auto cancelled=h.rec("cancel",{{"id",id}});QVERIFY(!cancelled.error);QCOMPARE(cancelled.result["state"].toString(),QString("cancelled"));
  QCOMPARE(h.backend->cancels,1);QCOMPARE(h.backend->discards,1);QCOMPARE(h.backend->exports,0);
  HistoryStore journal(h.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(id).stateName(),QString("cancelled"));QVERIFY(journal.operation(id).cancelled);QVERIFY(journal.operation(id).items.isEmpty());
  PUTGD_TRY_VERIFY(!QDir(h.backend->staging).exists());PUTGD_TRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(h.rec("cancel",{{"id",id}}).error.code,QString("recording_unavailable"));
  // A backend failure at start fails the operation; nothing is retried or uploaded.
  h.backend->failStart=true;id=h.prepareReady();QVERIFY(!id.isEmpty());
  QCOMPARE(h.rec("start",{{"id",id}}).error.code,QString("recording_start_failed"));
  PUTGD_TRY_COMPARE(journal.operation(id).stateName(),QString("failed"));QCOMPARE(journal.operation(id).error.code,QString("recording_start_failed"));PUTGD_TRY_COMPARE(h.operations->diagnostics()["activeOperations"].toInt(),0);
  // An export failure keeps the review open and the original unpublished.
  h.backend->failStart=false;h.backend->failExport=true;id=h.prepareReady();QVERIFY(!id.isEmpty());
  QVERIFY(!h.rec("start",{{"id",id}}).error);QCOMPARE(h.rec("stop",{{"id",id}}).result["state"].toString(),QString("reviewing"));
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  {const auto last=h.rec("inspect",{{"id",id}}).result["lastExport"].toObject();QCOMPARE(last["command"].toString(),QString("upload"));QCOMPARE(last["status"].toString(),QString("failed"));QCOMPARE(last["error"].toObject()["code"].toString(),QString("recording_export_failed"));}QCOMPARE(journal.operation(id).stateName(),QString("recording"));QVERIFY(journal.operation(id).items.isEmpty());
  h.backend->failExport=false;QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);PUTGD_TRY_COMPARE(journal.operation(id).stateName(),QString("completed"));
  QCOMPARE(h.provider->uploads.load(),0);
 }
 void liveDiskStopKeepsTriggerFiguresAndPrivateReview(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  constexpr qint64 gib=1LL<<30;
  auto reset=qScopeGuard([]{ArtifactStore::setDiskForTesting(-1,-1);});
  ArtifactStore::setDiskForTesting(400*gib,1000*gib);
  auto doc=configuration(h.dir.path(),true);auto limits=doc["limits"].toObject();limits["maxFileBytes"]=gib;doc["limits"]=limits;QVERIFY(h.settings.save(doc,h.settings.revision()));
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);
  QVERIFY(!h.rec("pause",{{"id",id}}).error);
  const QJsonObject sample{{"reason","disk_space"},{"freeBytes",1160LL<<20},{"reserveBytes",gib},{"availableBytes",136LL<<20},{"requiredBytes",160LL<<20},{"finalizationReserveBytes",32LL<<20},{"clipBytes",12LL<<20}};
  h.backend->sink({{"id",id},{"event","stopped"},{"reason","budget"},{"durationUs",3000000},{"canvas",QJsonArray{1280,720}},{"stopDetails",sample}});
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  const auto inspected=h.rec("inspect",{{"id",id}}).result;
  const auto details=inspected["stopDetails"].toObject();
  for(auto it=sample.begin();it!=sample.end();++it)QCOMPARE(details[it.key()],it.value());
  QVERIFY(details["message"].toString().contains("private review"));
  QCOMPARE(h.backend->lastControlParams["text"],details["message"]);
  QCOMPARE(h.rec("stop",{{"id",id}}).result,inspected);
  HistoryStore journal(h.operations->databasePath());QCOMPARE(journal.operation(id).stateName(),QString("recording"));QVERIFY(journal.operation(id).items.isEmpty());QCOMPARE(h.backend->exports,0);QCOMPARE(h.provider->uploads.load(),0);
  QVERIFY(!h.rec("preview",{{"id",id},{"time",0}}).error);
  // A failed local export leaves the review; retry is still explicitly local.
  h.backend->failExport=true;
  QVERIFY(!h.rec("saveCopy",{{"id",id},{"path",h.dir.filePath("copy.mp4")}}).error);
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["lastExport"].toObject()["status"].toString(),QString("failed"));
  h.backend->failExport=false;
  QVERIFY(!h.rec("saveCopy",{{"id",id},{"path",h.dir.filePath("copy.mp4")}}).error);
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));QVERIFY(QFileInfo::exists(h.dir.filePath("copy.mp4")));QVERIFY(journal.operation(id).items.isEmpty());
  QVERIFY(!h.rec("cancel",{{"id",id}}).error);PUTGD_TRY_COMPARE(journal.operation(id).stateName(),QString("cancelled"));QCOMPARE(h.provider->uploads.load(),0);
 }
 void diskFinalizationFailureDoesNotClaimReviewOrUpload(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);
  const Error failure{"recording_disk_full","Couldn't finish saving the recording.",{{"reason","disk_space"},{"freeBytes",0},{"finalized",false}}};
  h.backend->sink({{"id",id},{"event","failed"},{"error",failure.toJson()}});
  HistoryStore journal(h.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(id).stateName(),QString("failed"));
  QCOMPARE(journal.operation(id).error.code,failure.code);
  for(auto it=failure.details.begin();it!=failure.details.end();++it)QCOMPARE(journal.operation(id).error.details[it.key()],it.value());
  QCOMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));QVERIFY(journal.operation(id).items.isEmpty());QCOMPARE(h.backend->exports,0);QCOMPARE(h.provider->uploads.load(),0);
 }
 void retryFromReviewRepeatsOnlyTheFailedUpload(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());HistoryStore journal(h.operations->databasePath());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  // Nothing failed yet: the resident refuses the window's request; nothing exports.
  h.backend->sink({{"id",id},{"event","retryExportRequested"}});
  PUTGD_COMPARE_STEADY(h.backend->exports,0,200);
  h.backend->failExport=true;
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  QCOMPARE(journal.operation(id).items.size(),0);
  h.backend->failExport=false;
  h.backend->sink({{"id",id},{"event","retryExportRequested"}});
  PUTGD_TRY_COMPARE(journal.operation(id).stateName(),QString("completed"));
  QCOMPARE(h.backend->exports,2);QCOMPARE(journal.operation(id).items.size(),1);
 }
 // U1: every review-window proposal is answered by its number; a refused one
 // carries its code, message and the revision it was proposed at, and a
 // recording.review action whose own proposal was refused returns that refusal.
 void windowProposalsAreAnsweredAndRefusalsReachAgents(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  const auto propose=[&](int revision,int proposal,const QJsonObject& command){h.backend->sink({{"id",id},{"event","edit"},{"revision",revision},{"proposal",proposal},{"commands",QJsonArray{command}}});};
  propose(1,1,{{"op","addCut"},{"start",2000000},{"end",3000000}});
  PUTGD_TRY_COMPARE(h.backend->answers.size(),1);
  QCOMPARE(h.backend->answers[0]["proposal"].toDouble(),1.0);QVERIFY(!h.backend->answers[0].contains("refused"));
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["edit"].toObject()["revision"].toDouble(),2.0);
  propose(1,2,{{"op","resetTrim"}});
  PUTGD_TRY_COMPARE(h.backend->answers.size(),2);
  QCOMPARE(h.backend->answers[1]["proposal"].toDouble(),2.0);
  QCOMPARE(h.backend->answers[1]["refused"].toObject()["code"].toString(),QString("recording_stale"));
  QCOMPARE(h.backend->answers[1]["refused"].toObject()["revision"].toDouble(),1.0);
  propose(2,3,{{"op","setTrim"},{"start",1000000},{"end",1000001}});
  PUTGD_TRY_COMPARE(h.backend->answers.size(),3);
  const auto refused=h.backend->answers[2]["refused"].toObject();
  QCOMPARE(refused["code"].toString(),QString("invalid_request"));QVERIFY(!refused["message"].toString().isEmpty());QCOMPARE(refused["revision"].toDouble(),2.0);
  QCOMPARE(h.rec("inspect",{{"id",id}}).result["edit"].toObject()["revision"].toDouble(),2.0);
  // A recording.review action gets the outcome of its own proposals (U17): the
  // action that made proposal 3 is refused although the window's latest refusal
  // is proposal 4's, and the action that made proposal 5 succeeds although
  // proposal 4 was refused.
  propose(1,4,{{"op","resetTrim"}});
  propose(2,5,{{"op","addCut"},{"start",5000000},{"end",6000000}});
  PUTGD_TRY_COMPARE(h.backend->answers.size(),5);
  const QJsonObject latest{{"lastRefusal",QJsonObject{{"code","recording_stale"},{"message","later"},{"revision",1},{"proposal",4}}}};
  h.backend->reviewReplies={QJsonObject{{"action","cut"},{"proposals",QJsonArray{3}}},latest};
  const auto cut=h.rec("review",{{"id",id},{"action","cut"}});
  QCOMPARE(cut.error.code,QString("invalid_request"));QCOMPARE(cut.error.message,refused["message"].toString());QCOMPARE(cut.error.details["proposal"].toDouble(),3.0);
  h.backend->reviewReplies={QJsonObject{{"action","cut"},{"proposals",QJsonArray{5}}},latest};
  const auto accepted=h.rec("review",{{"id",id},{"action","cut"}});
  QVERIFY2(!accepted.error,qPrintable(accepted.error.message));QCOMPARE(accepted.result["proposals"].toArray(),(QJsonArray{5}));
  // A proposal the resident never answered is not reported as applied.
  h.backend->reviewReplies={QJsonObject{{"action","cut"},{"proposals",QJsonArray{9}}},QJsonObject{}};
  QCOMPARE(h.rec("review",{{"id",id},{"action","cut"}}).error.code,QString("recording_state"));
 }
 // U3/U19/U27: recording.review retryExport asks the window's Try again guard
 // first (its refusal, or a window that can't be asked, is the error), then
 // replies with the resident's own answer to this request: the export-start
 // state, or the resident's refusal.
 void retryExportIsGuardedByTheWindowAndAnsweredByTheResident(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  // The window allows it, but nothing failed: the resident's refusal is the answer.
  const auto nothing=h.rec("review",{{"id",id},{"action","retryExport"}});
  QCOMPARE(nothing.error.code,QString("recording_state"));QCOMPARE(h.backend->exports,0);
  QVERIFY(h.backend->lastControlParams["residentRetries"].toBool());
  h.backend->failExport=true;
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  h.backend->failExport=false;
  h.backend->reviewReplies={QJsonObject{{"error",QJsonObject{{"code","recording_state"},{"message","Place or cancel the cover first"}}}}};
  const auto placing=h.rec("review",{{"id",id},{"action","retryExport"}});
  QCOMPARE(placing.error.code,QString("recording_state"));QCOMPARE(placing.error.message,QString("Place or cancel the cover first"));
  h.backend->reviewReplies={QJsonObject{{"error",QJsonObject{{"code","recording_state"},{"message","The review window is not open"}}}}};
  QCOMPARE(h.rec("review",{{"id",id},{"action","retryExport"}}).error.code,QString("recording_state"));
  QCOMPARE(h.backend->exports,1);
  // Allowed: the export starts and the reply is its start state, as before the redesign.
  const auto retried=h.rec("review",{{"id",id},{"action","retryExport"}});
  QVERIFY2(!retried.error,qPrintable(retried.error.message));
  QCOMPARE(retried.result["lastExport"].toObject()["command"].toString(),QString("upload"));
  QCOMPARE(retried.result["recordingId"].toString(),id);
  PUTGD_TRY_COMPARE(h.backend->exports,2);
 }
 // U28: an action's proposal outcomes are read when its reply arrives, so 128
 // newer proposals answered before the follow-up inspect can't make a committed
 // edit look unanswered.
 void anActionsOutcomesSurviveNewerProposals(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  h.backend->sink({{"id",id},{"event","edit"},{"revision",1},{"proposal",1},{"commands",QJsonArray{QJsonObject{{"op","addCut"},{"start",2000000},{"end",3000000}}}}});
  PUTGD_TRY_COMPARE(h.backend->answers.size(),1);
  h.backend->reviewReplies={QJsonObject{{"action","cut"},{"proposals",QJsonArray{1}}},QJsonObject{}};
  h.backend->reviewHooks={{},[&]{for(int n=2;n<=140;++n)h.backend->sink({{"id",id},{"event","edit"},{"revision",1},{"proposal",n},{"commands",QJsonArray{QJsonObject{{"op","resetTrim"}}}}});}};
  const auto cut=h.rec("review",{{"id",id},{"action","cut"}});
  QVERIFY2(!cut.error,qPrintable(cut.error.code+": "+cut.error.message));
  QCOMPARE(cut.result["edit"].toObject()["revision"].toDouble(),2.0);
 }
 // The upload rule, checked again in the resident: an upload at a revision
 // that isn't the current one, or while an export runs, is refused with the
 // reason (the window reopens with it) and never exported.
 void anUploadOnlyExportsItsOwnRevision(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  QVERIFY(!h.rec("edit",{{"id",id},{"revision",1},{"commands",QJsonArray{QJsonObject{{"op","addCut"},{"start",2000000},{"end",3000000}}}}}).error);
  h.backend->log.clear();
  h.backend->sink({{"id",id},{"event","uploadRequested"},{"revision",1}});
  PUTGD_TRY_VERIFY(h.backend->log.contains("raise"));
  QVERIFY(h.backend->lastControlParams["error"].toString().contains("the upload didn't start"));
  QVERIFY(!h.backend->lastControlParams["retry"].toBool(true));  // U35: no Try again
  QCOMPARE(h.backend->exports,0);QCOMPARE(h.state(id),QString("reviewing"));
  // During an export (a copy being saved), an upload is refused out loud.
  h.backend->deferExports=true;
  QVERIFY(!h.rec("saveCopy",{{"id",id},{"path",h.dir.filePath("copy.mp4")}}).error);
  QCOMPARE(h.state(id),QString("exporting"));
  h.backend->log.clear();
  h.backend->sink({{"id",id},{"event","uploadRequested"},{"revision",2}});
  PUTGD_TRY_VERIFY(h.backend->log.contains("raise"));
  QVERIFY(h.backend->lastControlParams["error"].toString().contains("Another export is running"));
  QVERIFY(!h.backend->lastControlParams["retry"].toBool(true));
  // U34: a proposal answered during the export reaches the window once review resumes.
  const auto before=h.backend->answers.size();
  h.backend->sink({{"id",id},{"event","edit"},{"revision",2},{"proposal",7},{"commands",QJsonArray{QJsonObject{{"op","resetTrim"}}}}});
  PUTGD_COMPARE_STEADY(h.backend->answers.size(),before,200);
  QCOMPARE(h.backend->exports,1);
  h.backend->pendingExports.takeFirst()();
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  QCOMPARE(h.backend->exports,1);
  PUTGD_TRY_COMPARE(h.backend->answers.size(),before+1);
  QCOMPARE(h.backend->answers.last()["proposal"].toDouble(),7.0);
  QCOMPARE(h.backend->answers.last()["refused"].toObject()["code"].toString(),QString("recording_state"));
 }
 // U31: Try again after a failed Upload retries that Upload's revision; once
 // the edit changed it is refused (press Upload), never uploaded at the new one.
 void retryKeepsTheFailedUploadsRevision(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  h.backend->failExport=true;
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  h.backend->failExport=false;
  QVERIFY(!h.rec("edit",{{"id",id},{"revision",1},{"commands",QJsonArray{QJsonObject{{"op","addCut"},{"start",2000000},{"end",3000000}}}}}).error);
  const auto retried=h.rec("review",{{"id",id},{"action","retryExport"}});
  QCOMPARE(retried.error.code,QString("recording_stale"));QVERIFY(retried.error.message.contains("Select Upload"));
  h.backend->log.clear();
  h.backend->sink({{"id",id},{"event","retryExportRequested"}});
  PUTGD_TRY_VERIFY(h.backend->log.contains("raise"));
  QVERIFY(h.backend->lastControlParams["error"].toString().contains("Select Upload"));QVERIFY(!h.backend->lastControlParams["retry"].toBool(true));
  QCOMPARE(h.backend->exports,1);
 }
 // U32: the upload carries the output the window shows; any other is refused.
 // Pushes name the window's latest output choice the resident applied.
 void anUploadMustCarryTheShownOutput(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  h.backend->sink({{"id",id},{"event","configChanged"},{"seq",3},{"config",QJsonObject{{"format","webm"},{"codec","vp9"}}}});
  PUTGD_TRY_COMPARE(h.rec("inspect",{{"id",id}}).result["config"].toObject()["format"].toString(),QString("webm"));
  h.backend->sink({{"id",id},{"event","edit"},{"revision",1},{"proposal",1},{"commands",QJsonArray{QJsonObject{{"op","addCut"},{"start",2000000},{"end",3000000}}}}});
  PUTGD_TRY_COMPARE(h.backend->answers.size(),1);
  QCOMPARE(h.backend->lastPush["configSeq"].toDouble(),3.0);
  h.backend->log.clear();
  auto mp4=h.rec("inspect",{{"id",id}}).result["config"].toObject();mp4["format"]="mp4";mp4["codec"]="h264";
  h.backend->sink({{"id",id},{"event","uploadRequested"},{"revision",2},{"config",mp4}});
  PUTGD_TRY_VERIFY(h.backend->log.contains("raise"));
  QVERIFY(h.backend->lastControlParams["error"].toString().contains("ormat or audio changed"));QVERIFY(!h.backend->lastControlParams["retry"].toBool(true));
  QCOMPARE(h.backend->exports,0);
  h.backend->sink({{"id",id},{"event","uploadRequested"},{"revision",2},{"config",h.rec("inspect",{{"id",id}}).result["config"]}});
  PUTGD_TRY_COMPARE(h.backend->exports,1);
 }
 // D16: the review's Format menu can leave captured sound out of the export,
 // but never adds a source that wasn't captured.
 void reviewAudioChoiceNeverExceedsTheCapture(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  h.backend->stopAudioSources={{"system",true},{"microphone",true}};
  const auto id=h.prepareReady({{"audio",QJsonObject{{"system",true}}}});QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  const auto audio=[&]{return h.rec("inspect",{{"id",id}}).result["config"].toObject()["audio"].toObject();};
  QVERIFY(audio()["system"].toBool()&&audio()["microphone"].toBool());
  const auto choose=[&](bool system,bool microphone){h.backend->sink({{"id",id},{"event","configChanged"},{"config",QJsonObject{{"format","mp4"},{"codec","h264"},{"audio",QJsonObject{{"system",system},{"microphone",microphone}}}}}});};
  choose(false,false);
  PUTGD_TRY_VERIFY(!audio()["system"].toBool()&&!audio()["microphone"].toBool());
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);
  PUTGD_TRY_COMPARE(h.backend->exports,1);
  QVERIFY(!h.backend->lastConfig.audio.system);QVERIFY(!h.backend->lastConfig.audio.microphone);
 }
 void reviewAudioChoiceKeepsOnlyCapturedSources(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  h.backend->stopAudioSources={{"system",true},{"microphone",false}};
  const auto id=h.prepareReady({{"audio",QJsonObject{{"system",true}}}});QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  h.backend->sink({{"id",id},{"event","configChanged"},{"config",QJsonObject{{"format","webm"},{"codec","vp9"},{"audio",QJsonObject{{"system",true},{"microphone",true}}}}}});
  PUTGD_TRY_COMPARE(h.rec("inspect",{{"id",id}}).result["config"].toObject()["format"].toString(),QString("webm"));
  const auto audio=h.rec("inspect",{{"id",id}}).result["config"].toObject()["audio"].toObject();
  QVERIFY(audio["system"].toBool());QVERIFY(!audio["microphone"].toBool());
 }
 // U42: a format choice made while an export runs is applied when review
 // resumes, and the next push names it, so window and resident agree.
 void aChoiceMadeDuringAnExportIsApplied(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  h.backend->deferExports=true;h.backend->failExport=true;
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);QCOMPARE(h.state(id),QString("exporting"));
  h.backend->sink({{"id",id},{"event","configChanged"},{"seq",4},{"config",QJsonObject{{"format","webm"},{"codec","vp9"}}}});
  putgd::test::settle(50);
  h.backend->pendingExports.takeFirst()();
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  PUTGD_TRY_COMPARE(h.rec("inspect",{{"id",id}}).result["config"].toObject()["format"].toString(),QString("webm"));
  h.backend->sink({{"id",id},{"event","edit"},{"revision",1},{"proposal",1},{"commands",QJsonArray{QJsonObject{{"op","addCut"},{"start",2000000},{"end",3000000}}}}});
  PUTGD_TRY_COMPARE(h.backend->answers.size(),1);
  QCOMPARE(h.backend->lastPush["configSeq"].toDouble(),4.0);
 }
 // U43: past the export's deadline the helper still exports and takes no edit:
 // answers wait until its late result, not until the resident's timeout.
 void answersWaitForTheHelperToLeaveItsExport(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  h.backend->deferExports=true;h.backend->failExport=true;h.backend->failExportCode="recording_helper_timeout";
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);
  h.backend->sink({{"id",id},{"event","edit"},{"revision",1},{"proposal",1},{"commands",QJsonArray{QJsonObject{{"op","addCut"},{"start",2000000},{"end",3000000}}}}});
  putgd::test::settle(50);
  h.backend->pendingExports.takeFirst()();
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  h.backend->sink({{"id",id},{"event","edit"},{"revision",1},{"proposal",2},{"commands",QJsonArray{QJsonObject{{"op","addCut"},{"start",5000000},{"end",6000000}}}}});
  PUTGD_COMPARE_STEADY(h.backend->answers.size(),0,200);
  h.backend->sink({{"id",id},{"event","lateResult"},{"command","export"},{"ok",true}});
  PUTGD_TRY_COMPARE(h.backend->answers.size(),2);
  QCOMPARE(h.backend->answers[0]["proposal"].toDouble(),1.0);QCOMPARE(h.backend->answers[1]["proposal"].toDouble(),2.0);
 }
 // V2: past an export's deadline the helper still exports: a second Upload or
 // Save a copy is refused (recording_state) until its late result, so nothing is
 // sent to the busy helper and the held answers stay held.
 void noSecondExportWhileTheFirstIsFinishing(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  h.backend->deferExports=true;h.backend->failExport=true;h.backend->failExportCode="recording_helper_timeout";
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);
  h.backend->pendingExports.takeFirst()();
  PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  h.backend->failExport=false;h.backend->deferExports=false;
  const auto again=h.rec("upload",{{"id",id},{"revision",1}});
  QCOMPARE(again.error.code,QString("recording_state"));QVERIFY(again.error.message.contains("still finishing"));
  h.backend->log.clear();
  h.backend->sink({{"id",id},{"event","saveCopyRequested"},{"revision",1},{"path",h.dir.filePath("copy.mp4")}});
  PUTGD_TRY_VERIFY(h.backend->log.contains("raise"));
  QVERIFY(h.backend->lastControlParams["error"].toString().contains("still finishing"));QVERIFY(!h.backend->lastControlParams["retry"].toBool(true));
  h.backend->sink({{"id",id},{"event","edit"},{"revision",1},{"proposal",1},{"commands",QJsonArray{QJsonObject{{"op","addCut"},{"start",2000000},{"end",3000000}}}}});
  PUTGD_COMPARE_STEADY(h.backend->answers.size(),0,200);
  QCOMPARE(h.backend->exports,1);
  h.backend->sink({{"id",id},{"event","lateResult"},{"command","export"},{"ok",false}});
  PUTGD_TRY_COMPARE(h.backend->answers.size(),1);
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",2}}).error);
  PUTGD_TRY_COMPARE(h.backend->exports,2);
 }
 // D32: Try again exports exactly the output of the failed press; once the
 // output changed it is refused (press Upload again), never exported with either.
 void retryExportsTheFailedRequestsOutput(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  h.backend->stopAudioSources={{"system",true},{"microphone",false}};
  const auto id=h.prepareReady({{"audio",QJsonObject{{"system",true}}}});QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  h.backend->failExport=true;
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  const auto failed=h.backend->lastConfig;
  QVERIFY(failed.audio.system);
  h.backend->failExport=false;
  // Unchanged: the retry exports with the failed request's output.
  QVERIFY(!h.rec("review",{{"id",id},{"action","retryExport"}}).error);
  PUTGD_TRY_COMPARE(h.backend->exports,2);
  QCOMPARE(h.backend->lastConfig.toJson(),failed.toJson());
 }
 void retryRefusesAChangedOutput(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  h.backend->stopAudioSources={{"system",true},{"microphone",false}};
  const auto id=h.prepareReady({{"audio",QJsonObject{{"system",true}}}});QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);QVERIFY(!h.rec("stop",{{"id",id}}).error);
  h.backend->failExport=true;
  QVERIFY(!h.rec("upload",{{"id",id},{"revision",1}}).error);PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  h.backend->failExport=false;
  h.backend->sink({{"id",id},{"event","configChanged"},{"seq",1},{"config",QJsonObject{{"format","mp4"},{"codec","h264"},{"audio",QJsonObject{{"system",false},{"microphone",false}}}}}});
  PUTGD_TRY_VERIFY(!h.rec("inspect",{{"id",id}}).result["config"].toObject()["audio"].toObject()["system"].toBool());
  const auto retried=h.rec("review",{{"id",id},{"action","retryExport"}});
  QCOMPARE(retried.error.code,QString("recording_config"));QVERIFY(retried.error.message.contains("Select Upload"));
  h.backend->log.clear();
  h.backend->sink({{"id",id},{"event","retryExportRequested"}});
  PUTGD_TRY_VERIFY(h.backend->log.contains("raise"));
  QVERIFY(!h.backend->lastControlParams["retry"].toBool(true));
  QCOMPARE(h.backend->exports,1);
 }
 void lateStopReplyPreservesEditedPendingExport_data(){
  QTest::addColumn<QString>("command");QTest::addColumn<bool>("failedReply");
  QTest::newRow("save copy")<<QString("saveCopy")<<false;
  QTest::newRow("upload")<<QString("upload")<<false;
 }
 void lateStopReplyPreservesEditedPendingExport(){
  QFETCH(QString,command);QFETCH(bool,failedReply);Harness h;PUTGD_TRY_VERIFY(h.operations->ready());
  const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);h.backend->deferStop=true;
  Call stopped;h.server->dispatch({{"schemaVersion",1},{"requestId","pending-stop"},{"session",h.server->socketPath()},{"method","recording.stop"},{"params",QJsonObject{{"id",id}}}},[&](Error e,QJsonObject r){stopped={e,r,true};});
  QVERIFY(!stopped.done);QVERIFY(h.backend->pendingStop);
  h.backend->sink({{"id",id},{"event","stopped"},{"reason","budget"},{"durationUs",9000000.0},{"canvas",QJsonArray{1280,720}}});PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));
  const auto edited=h.rec("edit",{{"id",id},{"revision",1},{"commands",QJsonArray{QJsonObject{{"op","setTrim"},{"start",1000000},{"end",4000000}}}}});QVERIFY(!edited.error);
  h.backend->deferExports=true;
  QJsonObject params{{"id",id}};if(command=="saveCopy")params["path"]=h.dir.filePath("private.mp4");else params["revision"]=2;
  const auto exporting=h.rec(command,params);QVERIFY(!exporting.error);QCOMPARE(exporting.result["state"].toString(),QString("exporting"));QCOMPARE(h.backend->pendingExports.size(),1);
  const auto before=h.rec("inspect",{{"id",id}}).result;
  auto late=std::move(h.backend->pendingStop);late(failedReply?Error{"recording_state","already stopped",{}}:Error{},{{"durationUs",12000000.0},{"canvas",QJsonArray{1280,720}}});
  PUTGD_TRY_VERIFY(stopped.done);QVERIFY(!stopped.error);QCOMPARE(stopped.result,before);QCOMPARE(h.rec("inspect",{{"id",id}}).result,before);
  QCOMPARE(before["stopReason"].toString(),QString("budget"));QCOMPARE(before["durationUs"].toDouble(),9000000.0);QCOMPARE(before["edit"].toObject(),edited.result["edit"].toObject());
  h.backend->pendingExports.takeFirst()();
  HistoryStore journal(h.operations->databasePath());
  if(command=="upload"){PUTGD_TRY_COMPARE(journal.operation(id).stateName(),QString("completed"));QCOMPARE(h.rec("inspect",{{"id",id}}).error.code,QString("recording_unavailable"));}
  else {PUTGD_TRY_COMPARE(h.state(id),QString("reviewing"));QCOMPARE(h.rec("inspect",{{"id",id}}).result["edit"].toObject(),before["edit"].toObject());QVERIFY(!h.rec("cancel",{{"id",id}}).error);}
  QCOMPARE(h.provider->uploads.load(),0);
 }
 void lateStopReplyCannotChangeACancelledSessionsSuccessor(){
  Harness h;PUTGD_TRY_VERIFY(h.operations->ready());const auto id=h.prepareReady();QVERIFY(!id.isEmpty());QVERIFY(!h.rec("start",{{"id",id}}).error);h.backend->deferStop=true;
  int replies=0;h.server->dispatch({{"schemaVersion",1},{"requestId","pending-stop"},{"session",h.server->socketPath()},{"method","recording.stop"},{"params",QJsonObject{{"id",id}}}},[&](Error,QJsonObject){++replies;});
  QVERIFY(h.backend->pendingStop);auto late=std::move(h.backend->pendingStop);QVERIFY(!h.rec("cancel",{{"id",id}}).error);
  const auto next=h.prepareReady();QVERIFY(!next.isEmpty());QVERIFY(next!=id);
  late({},{{"durationUs",12000000.0},{"canvas",QJsonArray{1280,720}}});
  // A command round trip drains queued delivery, including the stale callback.
  QVERIFY(!h.rec("start",{{"id",next}}).error);QCOMPARE(h.state(next),QString("recording"));QCOMPARE(replies,0);QVERIFY(!h.rec("inspect",{{"id",next}}).result.contains("stopReason"));
  QVERIFY(!h.rec("cancel",{{"id",next}}).error);
 }
};

QTEST_GUILESS_MAIN(RecordingServiceContract)
#include "test_recording_service.moc"
