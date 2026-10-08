#include "fixtures/U2Fixtures.h"
#include <QSignalSpy>
using namespace putgd;using namespace putgd::test;
class OperationsTest:public QObject {
 Q_OBJECT
private slots:
 void newerInvocationInvalidatesDeferredNativeClipboardCommit() {
  class DeferredSource final:public SourceAdapter {
  public:
   int invalidations=0;std::function<void(Error)> pending;
   quint64 clipboardRevision()const override{return 0;}
   QJsonObject capabilities()const override{return {};}
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)override{}
   void cancelAcquisition()override{}
   void invalidateCopyEligibility()override{++invalidations;if(pending){auto callback=std::move(pending);callback({"copy_suppressed","Newer action superseded native commit",{}});}}
   void copy(const CopyPayload&,std::function<void(Error)> reply)override{pending=std::move(reply);}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  bool completed=false;Error error;
  operations.copyHistoryUrl("https://example.invalid/synthetic",[&](Error e,QJsonObject){error=e;completed=true;});
  QVERIFY(bool(source.pending));
  operations.invalidateCopyEligibility();
  QTRY_VERIFY_WITH_TIMEOUT(completed,500);
  QCOMPARE(error.code,QString("copy_suppressed"));
  QCOMPARE(source.invalidations,2);
 }
 void captureTargetIsPreparedBeforeAsynchronousAcceptance() {
  class FocusSource final : public SourceAdapter {
  public:
   QString focus="external-window-at-invocation",copied;
   int preparations=0,legacyReads=0,copies=0;
   quint64 clipboardRevision() const override {return 0;}
   QJsonObject capabilities() const override {return {{"window",true}};}
   PreparedAcquisition prepareAcquisition(const QString&) override {
    ++preparations;
    const auto frozen=focus;
    return [frozen](const ResourceLimits&,std::function<void(PayloadResult)> reply) {
     ClipboardPayload payload;payload.kind="text";payload.bytes=frozen.toUtf8();reply({payload,{}});
    };
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply) override {
    ++legacyReads;ClipboardPayload payload;payload.kind="text";payload.bytes=focus.toUtf8();reply({payload,{}});
   }
   void cancelAcquisition() override {}
   void copy(const CopyPayload& payload,std::function<void(Error)> reply) override {++copies;copied=payload.text;reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);
  QTRY_VERIFY(operations.ready());
  operations.invoke("window",[](Error error,QJsonObject) {QVERIFY(!error);});
  source.focus="different-window-after-invocation";
  QTRY_COMPARE(source.copies,1);
  QCOMPARE(source.preparations,1);
  QCOMPARE(source.legacyReads,0);
  QCOMPARE(source.copied,QString("external-window-at-invocation"));
 }
 void imageResultSuppliesEncodedPngBeforeClipboardOwnership() {
  QTemporaryDir directory;ProviderRegistry registry;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  Source source;source.payload.kind="image";source.payload.image=QImage(3,2,QImage::Format_ARGB32);source.payload.image.fill(Qt::magenta);
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  operations.invoke("region",[](Error error,QJsonObject){QVERIFY(!error);});QTRY_COMPARE(source.copies,1);
  QCOMPARE(source.copied.kind,QString("image"));
  QVERIFY(source.copied.encodedImage.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));
  QCOMPARE(QImage::fromData(source.copied.encodedImage),source.payload.image);
 }
 void selectorCancellationIsTerminalWithoutPublication() {
  QTemporaryDir directory;ProviderRegistry registry;
  auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.defer=true;
  OperationService operations(directory.filePath("data"),settings,registry,source);
  QTRY_VERIFY(operations.ready());QString id;
  operations.invoke("region",[&](Error error,QJsonObject response){QVERIFY(!error);id=response["operationId"].toString();});
  QTRY_COMPARE(source.acquisitions.size(),1);
  source.acquisitions.takeFirst()({{}, {"cancelled","Selection cancelled",{}}});
  HistoryStore journal(operations.databasePath());
  QTRY_COMPARE(journal.operation(id).state,QString("cancelled"));
  QVERIFY(journal.operation(id).cancelled);
  QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.copies,0);
 }
 void acquisitionFailureSettlesWithoutPendingCopy() {
  QTemporaryDir directory;ProviderRegistry registry;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  Source source;source.defer=true;
  OperationService operations(directory.filePath("data"),settings,registry,source);
  QTRY_VERIFY(operations.ready());QString id;
  operations.invoke("clipboard",[&](Error error,QJsonObject response){QVERIFY(!error);id=response["operationId"].toString();});
  QTRY_COMPARE(source.acquisitions.size(),1);
  source.acquisitions.takeFirst()({{}, {"source_unsupported","No supported source",{}}});
  QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryStore journal(operations.databasePath());const auto operation=journal.operation(id);
  QCOMPARE(operation.state,QString("failed"));
  QCOMPARE(operation.copyStatus,QString("no_result"));
  QCOMPARE(source.copies,0);QVERIFY(operation.items.isEmpty());
 }
 void objectLocksRetainOnlyActiveOwners() {
  QTemporaryDir directory;
  ProviderRegistry registry;
  registry.add("opaque",std::make_shared<OpaqueProvider>());
  SettingsService settings(directory.filePath("config.json"));
  Source source;
  OperationService operations(directory.filePath("data"),settings,registry,source);
  QTRY_VERIFY(operations.ready());
  Destination original{"opaque","first","revision-1",{{"workspace","fixture"}}, {}};
  auto first=operations.objectLock(original,{},"same-object");
  auto alias=original;
  alias.profileId="second";
  alias.profileRevision="revision-2";
  auto second=operations.objectLock(alias,{},"same-object");
  QVERIFY(first==second);
  std::weak_ptr<std::mutex> lifetime=first;
  first.reset();
  QVERIFY(!lifetime.expired());
  second.reset();
  QVERIFY(lifetime.expired());
 }
 void reconciliationCannotReplaceAcceptedDestination() {
  QTemporaryDir directory;
  ProviderRegistry registry;
  auto provider=std::make_shared<OpaqueProvider>();
  provider->loseReply=true;
  registry.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value) {
   return registry.validate(id,value);
  });
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;
  source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,registry,source);
  QTRY_VERIFY(operations.ready());
  QString id;
  operations.invoke("clipboard",[&](Error error,QJsonObject response) {
   QVERIFY(!error);
   id=response["operationId"].toString();
  });
  QTRY_VERIFY(!id.isEmpty());
  HistoryStore journal(operations.databasePath());
  QTRY_COMPARE(journal.operation(id).copyStatus,QString("no_result"));
  QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  const auto candidate=journal.operation(id).items[0].candidate;
  {
   QMutexLocker guard(&provider->mutex);
   QCOMPARE(provider->objects.size(),1);
   provider->objects.begin().value().destination.settings["workspace"]="different-destination";
  }
  bool accepted=false;
  operations.retry(id,[&](Error error,QJsonObject) {
   QVERIFY(!error);
   accepted=true;
  });
  QTRY_VERIFY(accepted);
  QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  const auto item=journal.operation(id).items[0];
  QCOMPARE(item.uploadStatus,QString("unknown"));
  QCOMPARE(item.candidate,candidate);
  QVERIFY(item.receipt.directUrl.isEmpty());
  QCOMPARE(provider->uploads.load(),1);
  QCOMPARE(provider->deletes.load(),0);
 }
 void localActionsAndUnavailableBatch() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);id=result["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());QTRY_COMPARE(source.copies,1);QCOMPARE(source.copied.kind,QString("text"));QCOMPARE(source.copied.text,QString("synthetic source content"));QCOMPARE(provider->uploads.load(),0);
  auto doc=configuration(d.path());QVERIFY(settings.save(doc,settings.revision()));source.payload.kind="files";source.payload.files={fixture(d.path(),"ok.txt","synthetic"),d.filePath("missing")};QString batch;ops.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);batch=result["operationId"].toString();});QTRY_VERIFY(!batch.isEmpty());QJsonObject inspected;ops.inspect(batch,[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;});QTRY_VERIFY(!inspected.isEmpty());
  QTRY_VERIFY_WITH_TIMEOUT(([&]{HistoryStore store(ops.databasePath());return store.operation(batch).state=="failed";})(),5000);HistoryStore store(ops.databasePath());QCOMPARE(store.operation(batch).error.code,QString("source_unavailable"));QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.copies,1);
 }
 void reverseCompletionAndOwnershipSuppressOldCopy() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString older,newer;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);older=r["operationId"].toString();});ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);newer=r["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),2);HistoryStore journal(ops.databasePath());const auto marker=journal.operation(newer).items[0].publicationId;int newerIndex=provider->requests[0].publicationId==marker?0:1;auto complete=provider->completions.takeAt(newerIndex);provider->requests.removeAt(newerIndex);complete();QTRY_COMPARE(source.copies,1);complete=provider->completions.takeFirst();complete();QTRY_VERIFY_WITH_TIMEOUT(([&]{HistoryStore store(ops.databasePath());return store.operation(older).copyStatus=="suppressed";})(),5000);QCOMPARE(source.copies,1);HistoryStore store(ops.databasePath());QCOMPARE(store.operation(older).items[0].uploadStatus,QString("uploaded"));QCOMPARE(store.operation(newer).items[0].uploadStatus,QString("uploaded"));
  QString third;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);third=r["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),1);++source.revision;provider->completions.takeFirst()();QTRY_VERIFY(([&]{HistoryStore s(ops.databasePath());return s.operation(third).copyStatus=="suppressed";})());QCOMPARE(source.copies,1);
 }
 void unknownRestartReconcilesWithoutNewUploadAndSnapshotsSettings() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->loseReply=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();QString id,candidate;
  {OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());QTRY_VERIFY(([&]{HistoryStore s(ops.databasePath());auto o=s.operation(id);return !o.items.isEmpty()&&o.items[0].uploadStatus=="unknown"&&o.copyStatus=="no_result";})());HistoryStore s(ops.databasePath());candidate=s.operation(id).items[0].candidate;}
  auto changed=configuration(d.path(),true);QVERIFY(settings.save(changed,settings.revision()));OperationService restarted(d.filePath("data"),settings,registry,source);QTRY_VERIFY(restarted.ready());QCOMPARE(provider->uploads.load(),1);bool accepted=false;restarted.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});QTRY_VERIFY(accepted);QTRY_VERIFY(([&]{HistoryStore s(restarted.databasePath());auto o=s.operation(id);return o.items[0].uploadStatus=="uploaded";})());QCOMPARE(provider->uploads.load(),1);HistoryStore s(restarted.databasePath());auto op=s.operation(id);QCOMPARE(op.items[0].candidate,candidate);QCOMPARE(op.destination.profileId,QString("test"));QCOMPARE(op.items[0].receipt.destination.settings["workspace"].toString(),QString("fixture-workspace"));
 }
 void archiveFailureDoesNotStopUploadAndRetryUsesSpool() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=true;const auto blocker=fixture(d.path(),"blocked","synthetic");archive["root"]=blocker;doc["archive"]=archive;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto op=store.operation(id);QCOMPARE(op.items[0].uploadStatus,QString("uploaded"));QCOMPARE(op.items[0].local.status,QString("failed"));QVERIFY(QFile::remove(blocker));QVERIFY(QDir().mkpath(blocker));QTRY_COMPARE(store.operation(id).copyStatus,QString("copied"));QTRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);bool done=false;ops.retryLocalSave(id,[&](Error e,QJsonObject){QVERIFY(!e);done=true;});QTRY_VERIFY(done);op=store.operation(id);QCOMPARE(op.items[0].local.status,QString("saved"));QCOMPARE(provider->uploads.load(),1);QFile archived(op.items[0].local.path);QVERIFY(archived.open(QIODevice::ReadOnly));QCOMPARE(archived.readAll(),QByteArray("synthetic source content"));
 }
 void overwriteBatchMarksBothAndSkipsNeverCopy() {
  QTemporaryDir d;QDir().mkpath(d.filePath("a"));QDir().mkpath(d.filePath("b"));auto first=fixture(d.filePath("a"),"report.pdf","%PDF-1.4\nfixture-a"),second=fixture(d.filePath("b"),"report.pdf","%PDF-1.4\nfixture-b"),third=fixture(d.path(),"notes.txt","fixture-notes");ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());doc["naming"]=QJsonObject{{"mode","original"},{"collisionPolicy","overwrite"}};QVERIFY(settings.save(doc,{}));Source source;source.payload.kind="files";source.payload.files={first,second,third};OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto op=store.operation(id);QCOMPARE(op.items[0].uploadStatus,QString("conflict"));QCOMPARE(op.items[1].uploadStatus,QString("conflict"));QCOMPARE(op.items[2].uploadStatus,QString("uploaded"));QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.copied.text,op.items[2].receipt.directUrl);
  doc["naming"]=QJsonObject{{"mode","original"},{"collisionPolicy","skip"}};QVERIFY(settings.save(doc,settings.revision()));source.payload.files={third};QString skipped;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);skipped=r["operationId"].toString();});QTRY_VERIFY(([&]{auto o=store.operation(skipped);return o.copyStatus=="no_result";})());QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.copies,1);QVERIFY(store.operation(skipped).items[0].receipt.publicationId.isEmpty());
 }
 void onlyTheResidentOwnerRecoversAbandonedStaging() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;const auto root=d.filePath("data"),staging=root+"/artifacts/.817effa3-cd94-4a25-b066-d85f7fbbdb9a-4c0b1d4a-13a3-46ba-9316-90859b0cfe4e";
  {OperationService owner(root,settings,registry,source);QTRY_VERIFY(owner.ready());QVERIFY(QDir().mkpath(staging));const auto artifact=fixture(staging,"cad9f544-c88c-4d8e-875b-576db75867dd","unfinished synthetic bytes"),local=fixture(staging,"user-owned-copy.txt","user archive");OperationService second(root,settings,registry,source);QSignalSpy initialized(&second,&OperationService::initialized);QTRY_COMPARE(initialized.count(),1);QVERIFY(!second.ready());QString code;second.invoke("clipboard",[&](Error e,QJsonObject){code=e.code;});QCOMPARE(code,QString("journal_busy"));QVERIFY(QFile::exists(artifact));QVERIFY(QFile::exists(local));}
  OperationService restarted(root,settings,registry,source);QTRY_VERIFY(restarted.ready());QVERIFY(!QFile::exists(staging+"/cad9f544-c88c-4d8e-875b-576db75867dd"));QVERIFY(QFile::exists(staging+"/user-owned-copy.txt"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void committedAcquisitionRecoversAfterMissingItemCheckpoint() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));const auto root=d.filePath("data"),id=QString("2e0c5cf4-ce1a-4c36-979d-e56c7d28c9e5");Operation accepted;accepted.id=id;accepted.action="clipboard";accepted.state="acquiring";accepted.created="2026-10-07T00:00:00.000Z";accepted.settings=settings.snapshot();accepted.settingsRevision=settings.revision();accepted.destination={"opaque","test",settings.revision(),{{"workspace","fixture-workspace"}},{{"mode","session"},{"reference",QJsonValue::Null}}};HistoryStore store(root+"/history.sqlite");QVERIFY(!store.saveOperation(accepted));ArtifactStore artifacts(root+"/artifacts");auto acquired=artifacts.acquire(id,textPayload(),ResourceLimits{});QVERIFY(!acquired.error);Source source;source.payload=textPayload();OperationService ops(root,settings,registry,source);QTRY_VERIFY(ops.ready());auto recovered=store.operation(id);QCOMPARE(recovered.state,QString("interrupted"));QCOMPARE(recovered.items.size(),1);QCOMPARE(recovered.items[0].artifact.sha256,acquired.artifacts[0].sha256);QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.reads,0);bool retried=false;ops.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});QTRY_VERIFY(retried);QTRY_COMPARE(store.operation(id).items[0].uploadStatus,QString("uploaded"));QVERIFY(!store.operation(id).items[0].candidate.isEmpty());QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,0);
 }
 void retriesRespectPendingOperationBound() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["pendingOperations"]=1;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString first,second;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),1);QTRY_VERIFY(!first.isEmpty()&&!second.isEmpty());HistoryStore store(ops.databasePath());const auto running=store.operation(first).items.isEmpty()?second:first;Operation prior=store.operation(running);QCOMPARE(prior.items.size(),1);prior.id="c6b8d7c1-eaf8-4f74-a341-e7d3af064e18";prior.state="partial";prior.items[0].uploadStatus="failed";prior.items[0].artifact.id="historical-item";QVERIFY(!store.saveOperation(prior));QString copyCode;ops.copyResult(running,[&](Error e,QJsonObject){copyCode=e.code;});QCOMPARE(copyCode,QString("operation_busy"));QString retryCode,localCode;bool retryResponded=false,localResponded=false;ops.retry(prior.id,[&](Error e,QJsonObject){retryCode=e.code;retryResponded=true;});ops.retryLocalSave(prior.id,[&](Error e,QJsonObject){localCode=e.code;localResponded=true;});QTRY_VERIFY(retryResponded&&localResponded);provider->deferUploads=false;provider->completions.takeFirst()();QTRY_VERIFY(([&]{return store.operation(second).copyStatus=="copied"||store.operation(second).copyStatus=="suppressed";})());QCOMPARE(retryCode,QString("queue_full"));QCOMPARE(localCode,QString("queue_full"));
 }
 void concurrentAcquisitionRespectsAggregateBudget() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;limits["pendingArtifactBytes"]=10;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"six.txt","123456")};OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString first,second;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});QTRY_VERIFY(!first.isEmpty()&&!second.isEmpty());HistoryStore store(ops.databasePath());QTRY_VERIFY(([&]{auto a=store.operation(first),b=store.operation(second);return (a.error.code=="input_excessive"||b.error.code=="input_excessive")&&(a.state=="partial"||b.state=="partial");})());ArtifactStore artifacts(d.filePath("data/artifacts"));QCOMPARE(artifacts.bytes(),6);QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.copies,0);
 }
 void emptyRegularFilesRejectTheEntireBatch() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=true;doc["archive"]=archive;QVERIFY(settings.save(doc,{}));Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"valid.txt","synthetic bytes"),fixture(d.path(),"empty.txt",{})};OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());HistoryStore store(ops.databasePath());QTRY_COMPARE(store.operation(id).state,QString("failed"));const auto op=store.operation(id);QCOMPARE(op.error.code,QString("source_empty"));QCOMPARE(op.error.details["itemIndex"].toInt(),1);QVERIFY(op.items.isEmpty());QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.copies,0);QVERIFY(!QDir(d.filePath("archive")).exists());ArtifactStore artifacts(d.filePath("data/artifacts"));QCOMPARE(artifacts.bytes(),0);
 }
 void lateAcquisitionAfterDestructionIsIgnored() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.defer=true;std::function<void(PayloadResult)> late;
  {OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString id;ops.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QTRY_COMPARE(source.acquisitions.size(),1);late=source.acquisitions.takeFirst();}
  late({textPayload(),{}});QTest::qWait(20);QCOMPARE(source.copies,0);QCOMPARE(QDir(d.filePath("data/artifacts")).entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),0);
 }
 void selectorBelongsToItsOwnOperation() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString first,second;ops.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),1);source.defer=true;ops.invoke("window",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});QTRY_COMPARE(source.acquisitions.size(),1);ops.cancel(first,[](Error e,QJsonObject){QVERIFY(!e);});QCOMPARE(source.acquisitions.size(),1);provider->completions.takeFirst()();QTRY_VERIFY(([&]{HistoryStore store(ops.databasePath());return store.operation(first).state=="cancelled";})());QString code;ops.invoke("desktop",[&](Error e,QJsonObject){code=e.code;});QCOMPARE(code,QString("busy_selecting"));ops.cancel(second,[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.acquisitions.size(),0);
 }
 void zeroRetentionAndActiveRetryPreserveArtifacts() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto retention=doc["retention"].toObject();retention["failedArtifactDays"]=0;doc["retention"]=retention;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QTRY_VERIFY(([&]{HistoryStore store(ops.databasePath());return store.operation(id).copyStatus=="no_result";})());HistoryStore store(ops.databasePath());auto op=store.operation(id);op.created="2000-01-01T00:00:00.000Z";QVERIFY(!store.saveOperation(op));const auto spool=op.items[0].artifact.path;bool cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);QCOMPARE(r["removedOperations"].toInt(),0);cleaned=true;});QTRY_VERIFY(cleaned);QVERIFY(QFile::exists(spool));
  provider->failUpload=false;provider->deferUploads=true;provider->dispatcher=this;ops.retry(id,[](Error e,QJsonObject){QVERIFY(!e);});cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);QCOMPARE(r["removedOperations"].toInt(),0);cleaned=true;});QTRY_VERIFY(cleaned);QVERIFY(QFile::exists(spool));QTRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();QTRY_VERIFY(([&]{return store.operation(id).items[0].uploadStatus=="uploaded";})());
 }
 void interactiveAcceptanceAndCancellation() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.defer=true;OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString id;bool startedAtAcceptance=true;ops.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();startedAtAcceptance=source.reads!=0;});QTRY_VERIFY(!id.isEmpty());QVERIFY(!startedAtAcceptance);QTRY_COMPARE(source.acquisitions.size(),1);QString code;ops.invoke("window",[&](Error e,QJsonObject){code=e.code;});QCOMPARE(code,QString("busy_selecting"));ops.cancel(id,[](Error e,QJsonObject){QVERIFY(!e);});QTRY_VERIFY(([&]{HistoryStore store(ops.databasePath());return store.operation(id).state=="cancelled";})());QCOMPARE(source.copies,0);
 }
};
QTEST_GUILESS_MAIN(OperationsTest)
#include "test_operations.moc"
