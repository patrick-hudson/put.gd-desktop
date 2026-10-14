#include "fixtures/U2Fixtures.h"
#include <QScopeGuard>
#include <utility>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QUuid>
using namespace putgd;using namespace putgd::test;
class OperationsTest:public QObject {
 Q_OBJECT
private slots:
 void midflightPhaseReachesInspectionAndChangeSignal(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());int changes=0;connect(&operations,&OperationService::changed,this,[&]{++changes;});
  auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),1);
  QJsonObject inspected;bool replied=false;operations.inspect(id,[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;replied=true;});QTRY_VERIFY(replied);
  QCOMPARE(inspected["phase"].toString(),QString("uploading"));QCOMPARE(inspected["items"].toArray()[0].toObject()["phase"].toString(),QString("uploading"));QTRY_VERIFY(changes>1);
  const auto request=provider->requests.first();QVERIFY(bool(request.progress));request.progress({"uploading",1024,2048});
  replied=false;operations.inspect(id,[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;replied=true;});QTRY_VERIFY(replied);
  QCOMPARE(inspected["items"].toArray()[0].toObject()["uploadProgress"].toObject()["bytesTransferred"].toInteger(),1024);
  request.progress({"uploading",512,{}});replied=false;operations.recent([&](Error e,QJsonObject r){QVERIFY(!e);inspected=r["operations"].toArray()[0].toObject();replied=true;});QTRY_VERIFY(replied);
  const auto rollback=inspected["items"].toArray()[0].toObject()["uploadProgress"].toObject();QCOMPARE(rollback["bytesTransferred"].toInteger(),512);QVERIFY(!rollback.contains("bytesTotal"));
  HistoryStore journal(operations.databasePath());QVERIFY(!journal.operation(id).items.first().uploadProgress); // No per-byte durable checkpoints.
  QTest::qWait(300);const auto before=changes;for(int i=0;i<10000;++i)request.progress({"uploading",i,{}});QTest::qWait(300);QVERIFY(changes-before<=2);
  const auto afterBurst=changes;for(int i=0;i<100;++i)request.progress({"uploading",9999,{}});QTest::qWait(300);QCOMPARE(changes,afterBurst);
  provider->failUpload=true;provider->completions.takeFirst()();QTRY_COMPARE(journal.operation(id).state,QString("partial"));QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  request.progress({"uploading",999999,{}});replied=false;operations.inspect(id,[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;replied=true;});QTRY_VERIFY(replied);
  QCOMPARE(inspected["state"].toString(),QString("partial"));QCOMPARE(inspected["items"].toArray()[0].toObject()["uploadProgress"].toObject()["bytesTransferred"].toInteger(),9999);QCOMPARE(inspected["items"].toArray()[0].toObject()["error"].toObject()["code"].toString(),QString("fixture_failed"));
  provider->failUpload=false;bool retried=false;operations.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});QTRY_VERIFY(retried);QTRY_COMPARE(provider->completions.size(),1);
  request.progress({"uploading",999999,{}});provider->requests.last().progress({"uploading",7,{}});replied=false;operations.inspect(id,[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;replied=true;});QTRY_VERIFY(replied);
  QCOMPARE(inspected["items"].toArray()[0].toObject()["uploadProgress"].toObject()["bytesTransferred"].toInteger(),7);
 }
 void inactiveCancellationNormalizesPhaseWithoutChangingItemOutcome(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  Operation prior;prior.id="synthetic-inactive";prior.state="partial";prior.phase="uploading";prior.settings=settings.snapshot();OperationItem item;item.artifact.id="synthetic-inactive-item";item.uploadStatus="unknown";item.phase="uploading";prior.items.append(item);QVERIFY(!journal.saveOperation(prior));
  bool replied=false;operations.cancel(prior.id,[&](Error e,QJsonObject value){QVERIFY(!e);QCOMPARE(value["phase"].toString(),QString("cancelled"));replied=true;});QTRY_VERIFY(replied);const auto saved=journal.operation(prior.id);QCOMPARE(saved.phase,QString("cancelled"));QCOMPARE(saved.items.first().phase,QString("unknown"));QCOMPARE(saved.items.first().uploadStatus,QString("unknown"));QCOMPARE(source.reads,0);
 }

 void optionalProgressRoundTripsAndLegacyOperationsRemainReadable(){
  OperationItem item;item.uploadStatus="uploading";item.phase="uploading";item.uploadProgress=UploadProgress{"uploading",10,20};auto value=item.toJson();auto restored=OperationItem::fromJson(value);QVERIFY(restored.uploadProgress);QCOMPARE(restored.uploadProgress->bytesTransferred.value(),10);QCOMPARE(restored.uploadProgress->bytesTotal.value(),20);
  value.remove("phase");value.remove("uploadProgress");auto legacy=OperationItem::fromJson(value);QCOMPARE(legacy.phase,QString("uploading"));QVERIFY(!legacy.uploadProgress);
  for(const auto& invalid:QList<QJsonObject>{{{"phase","uploading"},{"bytesTransferred",-1}},{{"phase","uploading"},{"bytesTransferred",1.5}},{{"phase","uploading"},{"bytesTransferred",21},{"bytesTotal",20}},{{"phase","uploading"},{"bytesTotal",20}},{{"phase","uploading"},{"bytesTransferred",9007199254740992.0}},{{"phase","uploading"},{"bytesTransferred",1},{"url","https://example.invalid"}}})QVERIFY(!UploadProgress::fromJson(invalid));
 }

 void cleanupBlocksRetriesUntilItsSnapshotFinishes() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  bool cleaned=false;operations.cleanup([&](Error e,QJsonObject){QVERIFY(!e);cleaned=true;});
  // No event dispatch between the snapshot and retry admission: even a fast
  // worker cannot deliver its session-thread completion yet.
  Error retryError,archiveError,secondCleanupError;operations.retry("synthetic-old-operation",[&](Error e,QJsonObject){retryError=e;});operations.retryLocalSave("synthetic-old-operation",[&](Error e,QJsonObject){archiveError=e;});operations.cleanup([&](Error e,QJsonObject){secondCleanupError=e;});
  QCOMPARE(retryError.code,QString("cleanup_busy"));QCOMPARE(archiveError.code,QString("cleanup_busy"));QCOMPARE(secondCleanupError.code,QString("cleanup_busy"));QTRY_VERIFY(cleaned);
  bool retried=false;operations.retry("synthetic-old-operation",[&](Error e,QJsonObject){QCOMPARE(e.code,QString("operation_not_found"));retried=true;});QTRY_VERIFY(retried);
 }
 void batchItemCheckpointsHaveLinearLedgerWrites() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.payload.kind="files";
  constexpr int count=100;for(int i=0;i<count;++i)source.payload.files.append(fixture(d.path(),QString("batch-%1.txt").arg(i),"synthetic batch content"));
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());const auto connection=QUuid::createUuid().toString();
  {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(operations.databasePath());QVERIFY(db.open());QSqlQuery q(db);QVERIFY(q.exec("CREATE TABLE checkpoint_count(value INTEGER NOT NULL)"));QVERIFY(q.exec("INSERT INTO checkpoint_count VALUES(0)"));
   for(const auto* verb:{"INSERT","UPDATE"})QVERIFY(q.exec(QString("CREATE TRIGGER count_%1 AFTER %1 ON history BEGIN UPDATE checkpoint_count SET value=value+1; END").arg(verb)));
   QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());HistoryStore store(operations.databasePath());QTRY_COMPARE_WITH_TIMEOUT(store.operation(id).copyStatus,QString("copied"),30000);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
   const auto op=store.operation(id);QCOMPARE(op.items.size(),count);for(const auto& item:op.items){QCOMPARE(item.local.status,QString("saved"));QCOMPARE(item.uploadStatus,QString("local_only"));QVERIFY(QFile::exists(item.local.path));}
   QVERIFY(q.exec("SELECT value FROM checkpoint_count"));QVERIFY(q.next());const auto writes=q.value(0).toInt();QVERIFY2(writes<=5*count,qPrintable(QString("Expected linear ledger writes; %1 files caused %2 writes").arg(count).arg(writes)));q.finish();db.close();
  }QSqlDatabase::removeDatabase(connection);
 }
 void startupHeapCleanupRunsOnceAfterSuccessfulInitialization() {
#ifndef __GLIBC__
  QSKIP("Allocator reclamation uses glibc on supported Linux builds");
#else
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,registry,source);QSignalSpy initialized(&operations,&OperationService::initialized);
  QTRY_COMPARE(initialized.count(),1);QVERIFY(operations.ready());
  const auto count=[&]{return operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt();};
  QVERIFY(operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());QCOMPARE(count(),0);
  QTRY_COMPARE_WITH_TIMEOUT(count(),1,5000);
  QVERIFY(!operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  QTest::qWait(2200);QCOMPARE(count(),1);QCOMPARE(initialized.count(),1);
  QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
#endif
 }
 void failedInitializationDoesNotRequestHeapCleanup() {
#ifndef __GLIBC__
  QSKIP("Allocator reclamation uses glibc on supported Linux builds");
#else
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));Source source;
  const auto blocked=fixture(directory.path(),"not-a-directory","synthetic obstruction");QVERIFY(!blocked.isEmpty());
  OperationService operations(blocked,settings,registry,source);QSignalSpy initialized(&operations,&OperationService::initialized);
  QTRY_COMPARE(initialized.count(),1);QVERIFY(!operations.ready());
  QString code;operations.invoke("clipboard",[&](Error error,QJsonObject){code=error.code;});QCOMPARE(code,QString("history_io"));
  QVERIFY(!operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  QTest::qWait(2200);QCOMPARE(operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt(),0);
  QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
#endif
 }
 void shutdownCancelsPendingStartupHeapCleanup() {
#ifndef __GLIBC__
  QSKIP("Allocator reclamation uses glibc on supported Linux builds");
#else
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  QVERIFY(operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  operations.shutdown();QTest::qWait(2200);
  QCOMPARE(operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt(),0);
  QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
#endif
 }
 void startupHeapCleanupWaitsForBackgroundWorker() {
#ifndef __GLIBC__
  QSKIP("Allocator reclamation uses glibc on supported Linux builds");
#else
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,registry,source);
  auto barrier=std::make_shared<std::promise<void>>();auto future=barrier->get_future().share();bool released=false,replied=false;
  auto release=qScopeGuard([&]{if(!released)barrier->set_value();});
  operations.background([future](Error&){future.wait();return QJsonObject{};},[&](Error error,QJsonObject){QVERIFY(!error);replied=true;});
  QTRY_VERIFY(operations.ready());
  const auto count=[&]{return operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt();};
  QVERIFY(operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  QTest::qWait(2200);QCOMPARE(count(),0);QVERIFY(!replied);
  barrier->set_value();released=true;QTRY_VERIFY(replied);QTRY_COMPARE_WITH_TIMEOUT(count(),1,5000);
  QVERIFY(!operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
#endif
 }
 void queuedCaptureNameUsesOriginalActionTime() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->completions.size(),1);
  source.payload.kind="image";source.payload.bytes.clear();source.payload.image=QImage(2,2,QImage::Format_ARGB32);source.payload.image.fill(Qt::blue);QString queued;
  operations.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);queued=r["operationId"].toString();});QTRY_COMPARE(source.reads,2);HistoryStore journal(operations.databasePath());
  const auto accepted=QDateTime::fromString(journal.operation(queued).created,Qt::ISODateWithMs).toLocalTime();QVERIFY(accepted.isValid());
  QTest::qWait(1100); // Force queue persistence to cross the one-second naming boundary.
  auto complete=provider->completions.takeFirst();complete();QTRY_COMPARE(provider->completions.size(),1);
  QCOMPARE(journal.operation(queued).items.first().artifact.name,QString("Screen-Capture-")+accepted.toString("yyyy-MM-dd-HH-mm-ss")+".png");
 }
 void queuedImagesRespectByteBudgetAndCancellationReleasesCapacity() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["pendingImageBytes"]=256;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->completions.size(),1);
  source.payload.kind="image";source.payload.bytes.clear();source.payload.image=QImage(8,8,QImage::Format_ARGB32);source.payload.image.fill(Qt::blue);QString queued;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);queued=r["operationId"].toString();});QTRY_COMPARE(source.reads,2);QTest::qWait(20);
  source.payload.image=QImage(8,8,QImage::Format_ARGB32);source.payload.image.fill(Qt::red);QString rejected;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);rejected=r["operationId"].toString();});QTRY_COMPARE(source.reads,3);HistoryStore journal(operations.databasePath());
  QTRY_COMPARE_WITH_TIMEOUT(journal.operation(rejected).error.code,QString("image_memory_limit"),1000);QCOMPARE(provider->uploads.load(),1);QVERIFY(journal.operation(rejected).items.isEmpty());
  operations.cancel(queued,[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(journal.operation(queued).state,QString("cancelled"));QString admitted;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);admitted=r["operationId"].toString();});QTRY_COMPARE(source.reads,4);QTest::qWait(20);QCOMPARE(journal.operation(admitted).error.code,QString{});
  operations.cancel(admitted,[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(journal.operation(admitted).state,QString("cancelled"));QCOMPARE(provider->uploads.load(),1);
 }
 void decodedImagesReleaseBeforeBlockedUploadAndQueuedCancel() {
  class MovingSource final:public SourceAdapter {
  public:
   ClipboardPayload payload;int reads=0;
   quint64 clipboardRevision()const override{return 0;}
   QJsonObject capabilities()const override{return {};}
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override{++reads;reply({std::exchange(payload,{}),{}});}
   void cancelAcquisition()override{}
   void copy(const CopyPayload&,std::function<void(Error)> reply)override{reply({});}
  } source;
  struct Buffer {uchar* data;std::atomic_int* freed;};
  std::atomic_int freed{0};
  auto image=[&] {auto* buffer=new Buffer{new uchar[256],&freed};std::fill_n(buffer->data,256,uchar(42));return QImage(buffer->data,8,8,32,QImage::Format_ARGB32,[](void* context){auto* b=static_cast<Buffer*>(context);delete[] b->data;++*b->freed;delete b;},buffer);};
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));
  OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});
  source.payload.kind="image";source.payload.image=image();QString first;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),1);
  QCOMPARE(freed.load(),1); // The provider is still blocked: disk artifact, not pixels, owns the upload.
#ifdef __GLIBC__
  const auto reclamations=[&]{return operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt();};
  QVERIFY(operations.diagnostics()["memory"].toObject()["allocatorReclamationSupported"].toBool());
  QTest::qWait(2200);
  QCOMPARE(reclamations(),0); // Released pixels must not trigger cleanup during the blocked upload.
#endif
  HistoryStore journal(operations.databasePath());const auto artifact=journal.operation(first).items.first().artifact;QVERIFY(!ArtifactStore(d.filePath("data/artifacts")).verify(artifact));
  source.payload.kind="image";source.payload.image=image();QString second;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});QTRY_COMPARE(source.reads,2);QTest::qWait(20);QCOMPARE(freed.load(),1);
  bool cancelled=false;operations.cancel(second,[&](Error e,QJsonObject){QVERIFY(!e);cancelled=true;});QTRY_VERIFY(cancelled);QTRY_COMPARE(freed.load(),2);
  QTRY_COMPARE(journal.operation(second).state,QString("cancelled"));QCOMPARE(provider->uploads.load(),1);
  provider->failUpload=true;auto complete=provider->completions.takeFirst();complete();QTRY_COMPARE(journal.operation(first).state,QString("partial"));
  QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  provider->failUpload=false;bool retry=false;operations.retry(first,[&](Error e,QJsonObject){QVERIFY(!e);retry=true;});QTRY_VERIFY(retry);QTRY_COMPARE(provider->completions.size(),1);
  QCOMPARE(provider->requests.last().artifact.path,artifact.path);QCOMPARE(provider->requests.last().artifact.sha256,artifact.sha256);QCOMPARE(source.reads,2);QCOMPARE(freed.load(),2);
  complete=provider->completions.takeFirst();complete();QTRY_COMPARE(journal.operation(first).state,QString("completed"));
#ifdef __GLIBC__
  QTRY_COMPARE_WITH_TIMEOUT(reclamations(),1,5000);
  QVERIFY(!operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  QTest::qWait(2200);
  QCOMPARE(reclamations(),1); // One quiet-period cleanup, with no repeating idle timer.
#endif
 }
 void imageHeapCleanupWaitsForSelectionAndBackgroundWork() {
#ifndef __GLIBC__
  QSKIP("Allocator reclamation uses glibc on supported Linux builds");
#else
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));Source source;
  source.payload.kind="image";source.payload.image=QImage(16,16,QImage::Format_ARGB32);source.payload.image.fill(Qt::cyan);
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  const auto count=[&]{return operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt();};
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.copies,1);
  QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  source.defer=true;operations.invoke("region",[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(source.acquisitions.size(),1);
  QTest::qWait(2200);QCOMPARE(count(),0); // A selector can be active with no operation worker thread.
  auto barrier=std::make_shared<std::promise<void>>();auto future=barrier->get_future().share();bool released=false;
  auto release=qScopeGuard([&]{if(!released)barrier->set_value();});
  operations.background([future](Error&){future.wait();return QJsonObject{};},[](Error,QJsonObject){});
  source.acquisitions.takeFirst()({{}, {"cancelled","Selection cancelled",{}}});
  QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QTest::qWait(2200);QCOMPARE(count(),0); // Unrelated file/history work must also drain.
  barrier->set_value();released=true;QTRY_COMPARE_WITH_TIMEOUT(count(),1,5000);
#endif
 }
 void capturesReceiveStableTimestampNamesBeforeLocalArchive() {
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  auto document=configuration(directory.path(),true);auto naming=document["naming"].toObject();naming["mode"]="original";document["naming"]=naming;QVERIFY(settings.save(document,{}));
  Source source;source.payload.kind="image";source.payload.image=QImage(3,2,QImage::Format_ARGB32);source.payload.image.fill(Qt::cyan);
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());QString id;
  operations.invoke("region",[&](Error error,QJsonObject response){QVERIFY(!error);id=response["operationId"].toString();});QTRY_COMPARE(source.copies,1);
  HistoryStore journal(operations.databasePath());auto operation=journal.operation(id);QCOMPARE(operation.items.size(),1);
  const auto artifact=operation.items[0].artifact;
  QVERIFY(QRegularExpression("^Screen-Capture-\\d{4}-\\d{2}-\\d{2}-\\d{2}-\\d{2}-\\d{2}\\.png$").match(artifact.name).hasMatch());
  QCOMPARE(QFileInfo(operation.items[0].local.path).fileName(),artifact.name);
  ArtifactStore spool(directory.filePath("data/artifacts"));auto recovered=spool.recover(id);QVERIFY(!recovered.error);QCOMPARE(recovered.artifacts[0].name,artifact.name);
 }
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
 void failedAcceptanceReleasesPreparedRequestWithoutAcquiring() {
  class PreparedSource final:public SourceAdapter {
  public:
   QString path;std::atomic_int retained{0};int acquired=0;bool injected=false;ResourceLimits preparedLimits;
   quint64 clipboardRevision() const override{return 0;}
   QJsonObject capabilities() const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits& limits) override {
    preparedLimits=limits;
    auto lifetime=std::shared_ptr<int>(new int,[this](int* value){delete value;--retained;});++retained;
    // Inject a write failure after startup recovery, before durable acceptance.
    const auto connection=QUuid::createUuid().toString();
    {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);if(db.open()){QSqlQuery query(db);injected=query.exec("DROP TABLE operations");}db.close();}
    QSqlDatabase::removeDatabase(connection);
    return [this,lifetime](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquired;reply({{}, {"cancelled","Synthetic cancellation",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override{++acquired;}
   void cancelAcquisition() override{}
   void copy(const CopyPayload&,std::function<void(Error)> reply) override{reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());source.path=operations.databasePath();
  int replies=0;Error error;operations.invoke("region",[&](Error value,QJsonObject){error=value;++replies;});
  QTRY_COMPARE(replies,1);QVERIFY(source.injected);QCOMPARE(error.code,QString("history_io"));QTRY_COMPARE(source.retained.load(),0);
  QCOMPARE(source.acquired,0);QCOMPARE(source.preparedLimits.imagePixels,settings.snapshot().limits.imagePixels);
  operations.shutdown();QCOMPARE(replies,1);
 }
 void cancellationDuringAcquiringCommitNeverBeginsPreparedSource() {
  class PreparedSource final:public SourceAdapter {
  public:
   std::atomic_int retained{0};int acquired=0,cancels=0;
   quint64 clipboardRevision() const override{return 0;}
   QJsonObject capabilities() const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&) override {
    auto lifetime=std::shared_ptr<int>(new int,[this](int* value){delete value;--retained;});++retained;
    return [this,lifetime](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquired;reply({{}, {"cancelled","Synthetic cancellation",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override{++acquired;}
   void cancelAcquisition() override{++cancels;}
   void copy(const CopyPayload&,std::function<void(Error)> reply) override{reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  const auto connection=QUuid::createUuid().toString();
  {
   auto blocker=QSqlDatabase::addDatabase("QSQLITE",connection);blocker.setDatabaseName(operations.databasePath());QVERIFY(blocker.open());
   QString id;bool requested=false;
   operations.invoke("region",[&](Error error,QJsonObject result){
    QVERIFY(!error);id=result["operationId"].toString();QSqlQuery lock(blocker);QVERIFY(lock.exec("BEGIN IMMEDIATE"));
    QTimer::singleShot(30,&operations,[&]{
     operations.cancel(id,[&](Error error,QJsonObject){QVERIFY(!error);requested=true;});
     QSqlQuery unlock(blocker);QVERIFY(unlock.exec("ROLLBACK"));
    });
   });
   QTRY_VERIFY(requested);QTRY_VERIFY(([&]{HistoryStore store(operations.databasePath());return store.operation(id).state=="cancelled";})());
   QTRY_COMPARE(source.retained.load(),0);QCOMPARE(source.acquired,0);QCOMPARE(source.cancels,1);operations.shutdown();blocker.close();
  }
  QSqlDatabase::removeDatabase(connection);
 }
 void earlySelectionTerminalNeverBeginsPixels_data() {
  QTest::addColumn<QString>("code");QTest::addColumn<bool>("afterAcceptance");
  QTest::newRow("escape-before-acceptance")<<QString("cancelled")<<false;
  QTest::newRow("failure-before-acceptance")<<QString("display_changed")<<false;
  QTest::newRow("escape-during-acquiring")<<QString("cancelled")<<true;
  QTest::newRow("failure-during-acquiring")<<QString("display_changed")<<true;
 }
 void earlySelectionTerminalNeverBeginsPixels() {
  QFETCH(QString,code);QFETCH(bool,afterAcceptance);
  class PreviewSource final:public SourceAdapter {
  public:
   std::function<void(Error)> terminal;int acquired=0,previews=0;
   quint64 clipboardRevision()const override{return 0;}
   QJsonObject capabilities()const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)> notify)override {
    ++previews;terminal=std::move(notify);
    return [this](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquired;reply({{}, {"cancelled","Unexpected acquisition",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override{++acquired;reply({{}, {"cancelled","Unexpected legacy acquisition",{}}});}
   void cancelAcquisition()override{}
   void copy(const CopyPayload&,std::function<void(Error)> reply)override{reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  const auto connection=QUuid::createUuid().toString();
  {
   auto blocker=QSqlDatabase::addDatabase("QSQLITE",connection);blocker.setDatabaseName(operations.databasePath());QVERIFY(blocker.open());
   if(!afterAcceptance){QSqlQuery lock(blocker);QVERIFY(lock.exec("BEGIN IMMEDIATE"));}
   QString id;bool accepted=false;
   operations.invoke("region",[&](Error error,QJsonObject result){
    QVERIFY(!error);id=result["operationId"].toString();accepted=true;
    if(afterAcceptance){QSqlQuery lock(blocker);QVERIFY(lock.exec("BEGIN IMMEDIATE"));QTimer::singleShot(30,&operations,[&]{source.terminal({code,"Synthetic early terminal",{}});QSqlQuery unlock(blocker);QVERIFY(unlock.exec("ROLLBACK"));});}
   });
   QCOMPARE(source.previews,1);QVERIFY(bool(source.terminal));
   if(!afterAcceptance){source.terminal({code,"Synthetic early terminal",{}});QVERIFY(!accepted);QSqlQuery unlock(blocker);QVERIFY(unlock.exec("ROLLBACK"));}
   QTRY_VERIFY(accepted);QTRY_VERIFY(([&]{HistoryStore store(operations.databasePath());return store.operation(id).state==(code=="cancelled"?"cancelled":"failed");})());
   HistoryStore store(operations.databasePath());const auto op=store.operation(id);QCOMPARE(op.error.code,code);QVERIFY(op.items.isEmpty());QCOMPARE(source.acquired,0);
   operations.shutdown();blocker.close();
  }
  QSqlDatabase::removeDatabase(connection);
 }
 void explicitCancelOverridesEarlyDisplayErrorWhileAcquiringIsBlocked() {
  class PreviewSource final:public SourceAdapter {
  public:
   std::function<void(Error)> terminal;int acquired=0,cancels=0,copies=0;
   quint64 clipboardRevision()const override{return 0;}
   QJsonObject capabilities()const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)> notify)override {
    terminal=std::move(notify);
    return [this](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquired;reply({{}, {"capture_failed","Unexpected pixel acquisition",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override{++acquired;reply({{}, {"capture_failed","Unexpected legacy acquisition",{}}});}
   void cancelAcquisition()override{++cancels;}
   void copy(const CopyPayload&,std::function<void(Error)> reply)override{++copies;reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  const auto connection=QUuid::createUuid().toString();
  {
   auto blocker=QSqlDatabase::addDatabase("QSQLITE",connection);blocker.setDatabaseName(operations.databasePath());QVERIFY(blocker.open());
   bool lockHeld=false,injected=false,cancelReplied=false;int acceptanceReplies=0;QString id;
   // An assertion must not leave the worker blocked while shutdown joins it.
   const auto release=qScopeGuard([&]{if(lockHeld){QSqlQuery unlock(blocker);unlock.exec("ROLLBACK");}});
   operations.invoke("region",[&](Error error,QJsonObject result){
    QVERIFY(!error);++acceptanceReplies;id=result["operationId"].toString();QVERIFY(!id.isEmpty());
    QSqlQuery lock(blocker);QVERIFY(lock.exec("BEGIN IMMEDIATE"));lockHeld=true;
    // Let the queued acquiring continuation run against the held writer lock.
    QTimer::singleShot(30,&operations,[&]{
     QSqlQuery checkpoint(blocker);checkpoint.prepare("SELECT json_extract(data,'$.state') FROM operations WHERE id=?");checkpoint.addBindValue(id);
     QVERIFY(checkpoint.exec());QVERIFY(checkpoint.next());QCOMPARE(checkpoint.value(0).toString(),QString("accepted"));checkpoint.finish();
     QVERIFY(bool(source.terminal));source.terminal({"display_changed","Synthetic early display error",{}});injected=true;
     operations.cancel(id,[&](Error cancelError,QJsonObject){QVERIFY(!cancelError);cancelReplied=true;});
     QSqlQuery unlock(blocker);QVERIFY(unlock.exec("ROLLBACK"));lockHeld=false;
    });
   });
   QTRY_VERIFY(injected&&cancelReplied);QCOMPARE(acceptanceReplies,1);
   QTRY_VERIFY(([&]{HistoryStore store(operations.databasePath());return store.operation(id).state=="cancelled";})());
   HistoryStore store(operations.databasePath());const auto operation=store.operation(id);
   QVERIFY(operation.cancelled);QVERIFY(!operation.error);QVERIFY(operation.items.isEmpty());
   QCOMPARE(source.acquired,0);QCOMPARE(source.cancels,1);QCOMPARE(source.copies,0);
   QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
   operations.shutdown();blocker.close();
  }
  QSqlDatabase::removeDatabase(connection);
 }

 void drainedEarlySelectionAllowsNextRequestBeforeJournalCompletes() {
  class PreviewSource final:public SourceAdapter {
  public:
   QList<std::function<void(Error)>> terminals;QList<std::function<void()>> releases;int acquisitions=0;
   quint64 clipboardRevision()const override{return 0;}QJsonObject capabilities()const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)> terminal,std::function<void()> released)override {
    terminals.append(std::move(terminal));releases.append(std::move(released));
    auto release=releases.last();
    return [this,release](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquisitions;release();reply({{}, {"cancelled","Synthetic cancellation",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)override{QFAIL("Unexpected legacy acquisition");}
   void cancelAcquisition()override{}void copy(const CopyPayload&,std::function<void(Error)> reply)override{reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());const auto connection=QUuid::createUuid().toString();
  {
   auto blocker=QSqlDatabase::addDatabase("QSQLITE",connection);blocker.setDatabaseName(operations.databasePath());QVERIFY(blocker.open());bool locked=true;
   const auto unlock=qScopeGuard([&]{if(locked){QSqlQuery q(blocker);q.exec("ROLLBACK");}});QSqlQuery lock(blocker);QVERIFY(lock.exec("BEGIN IMMEDIATE"));
   QString first,second;int replies=0;
   operations.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();++replies;});QCOMPARE(source.terminals.size(),1);
   source.terminals[0]({"cancelled","Early Escape",{}});QCOMPARE(replies,0);QCOMPARE(source.acquisitions,0);
   // Error metadata alone must not release an undrained selector/helper.
   QString busy;operations.invoke("region",[&](Error e,QJsonObject){busy=e.code;});QCOMPARE(busy,QString("busy_selecting"));
   source.releases[0]();operations.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();++replies;});
   QCOMPARE(source.terminals.size(),2);QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),2);QCOMPARE(replies,0);QCOMPARE(source.acquisitions,0);
   // Late/stale release belongs only to the old operation.
   source.releases[0]();busy.clear();operations.invoke("region",[&](Error e,QJsonObject){busy=e.code;});QCOMPARE(busy,QString("busy_selecting"));
   QSqlQuery release(blocker);QVERIFY(release.exec("ROLLBACK"));locked=false;
   QTRY_COMPARE(replies,2);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(source.acquisitions,1);
   HistoryStore store(operations.databasePath());QCOMPARE(store.operation(first).state,QString("cancelled"));QCOMPARE(store.operation(second).state,QString("cancelled"));
   operations.shutdown();blocker.close();
  }
  QSqlDatabase::removeDatabase(connection);
 }
 void durableEarlyCancellationKeepsReservationUntilSourceDrains() {
  class PreviewSource final:public SourceAdapter {
  public:
   std::function<void(Error)> terminal;std::function<void()> released;int acquisitions=0;
   quint64 clipboardRevision()const override{return 0;}QJsonObject capabilities()const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)> notify,std::function<void()> release)override {
    terminal=std::move(notify);released=std::move(release);
    return [this](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquisitions;released();reply({{}, {"cancelled","Synthetic cancellation",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)override{QFAIL("Unexpected legacy acquisition");}
   void cancelAcquisition()override{}void copy(const CopyPayload&,std::function<void(Error)> reply)override{reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());QString id;
  operations.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QVERIFY(bool(source.terminal));source.terminal({"cancelled","Early Escape",{}});
  QTRY_VERIFY(!id.isEmpty());QTRY_VERIFY(([&]{HistoryStore store(operations.databasePath());return store.operation(id).state=="cancelled";})());
  QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),1);QCOMPARE(source.acquisitions,0);
  QString busy;operations.invoke("region",[&](Error e,QJsonObject){busy=e.code;});QCOMPARE(busy,QString("busy_selecting"));
  source.released();QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QString next;operations.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);next=r["operationId"].toString();});
  QTRY_VERIFY(!next.isEmpty());QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(source.acquisitions,1);operations.shutdown();
 }
 void previewCheckpointFailuresReleasePreparationWithoutPixels_data() {
  QTest::addColumn<QString>("checkpoint");
  QTest::newRow("accepted-insert")<<QString("accepted");
  QTest::newRow("acquiring-update")<<QString("acquiring");
 }
 void previewCheckpointFailuresReleasePreparationWithoutPixels() {
  QFETCH(QString,checkpoint);
  class PreparedSource final:public SourceAdapter {
  public:
   std::function<void(Error)> terminal;
   std::atomic_int retained{0},released{0};
   int prepared=0,pixels=0,legacyAcquisitions=0,ordinaryReplies=0,copies=0;
   quint64 clipboardRevision()const override{return 0;}
   QJsonObject capabilities()const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)> notify)override {
    ++prepared;terminal=std::move(notify);++retained;
    auto lifetime=std::shared_ptr<int>(new int,[this](int* value){delete value;--retained;++released;});
    return [this,lifetime](const ResourceLimits&,std::function<void(PayloadResult)> reply){
     ++pixels;++ordinaryReplies;reply({{}, {"capture_failed","Unexpected pixel acquisition",{}}});
    };
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override {
    ++legacyAcquisitions;++ordinaryReplies;reply({{}, {"capture_failed","Unexpected legacy acquisition",{}}});
   }
   void cancelAcquisition()override{}
   void copy(const CopyPayload&,std::function<void(Error)> reply)override{++copies;reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  const auto connection=QUuid::createUuid().toString();
  {
   auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(operations.databasePath());QVERIFY(db.open());
   {
    QSqlQuery inject(db);
    const auto sql=checkpoint=="accepted"
     ? QString("CREATE TRIGGER synthetic_checkpoint_failure BEFORE INSERT ON operations WHEN json_extract(NEW.data,'$.state')='accepted' BEGIN SELECT RAISE(ABORT,'synthetic accepted checkpoint failure'); END")
     : QString("CREATE TRIGGER synthetic_checkpoint_failure BEFORE UPDATE ON operations WHEN json_extract(NEW.data,'$.state')='acquiring' BEGIN SELECT RAISE(ABORT,'synthetic acquiring checkpoint failure'); END");
    QVERIFY(inject.exec(sql));
   }
   db.close();
  }
  QSqlDatabase::removeDatabase(connection);
  int acceptanceReplies=0;Error acceptanceError;QJsonObject acceptanceResult;
  operations.invoke("region",[&](Error error,QJsonObject result){++acceptanceReplies;acceptanceError=error;acceptanceResult=std::move(result);});
  QCOMPARE(source.prepared,1);QVERIFY(bool(source.terminal));
  QTRY_COMPARE(acceptanceReplies,1);
  if(checkpoint=="accepted") {
   QCOMPARE(acceptanceError.code,QString("history_io"));QVERIFY(acceptanceResult.isEmpty());
   {
    auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(operations.databasePath());QVERIFY(db.open());
    {QSqlQuery rows(db);QVERIFY(rows.exec("SELECT count(*) FROM operations"));QVERIFY(rows.next());QCOMPARE(rows.value(0).toInt(),0);}
    db.close();
   }
   QSqlDatabase::removeDatabase(connection);
  } else {
   QVERIFY(!acceptanceError);const auto id=acceptanceResult["operationId"].toString();QVERIFY(!id.isEmpty());
   QCOMPARE(acceptanceResult["operation"].toObject()["state"].toString(),QString("accepted"));
   QTRY_VERIFY(([&]{HistoryStore store(operations.databasePath());return store.operation(id).state=="failed";})());
   HistoryStore store(operations.databasePath());Error readError;const auto operation=store.operation(id,&readError);
   QVERIFY(!readError);QCOMPARE(operation.error.code,QString("history_io"));QVERIFY(!operation.cancelled);
   QVERIFY(operation.items.isEmpty());QCOMPARE(operation.copyStatus,QString("no_result"));
  }
  {HistoryStore store(operations.databasePath());const auto history=store.list({});QVERIFY(!history.error);QCOMPARE(history.total,qint64(0));QVERIFY(history.records.isEmpty());}
  QTRY_COMPARE(source.retained.load(),0);QCOMPARE(source.released.load(),1);
  QCOMPARE(source.pixels,0);QCOMPARE(source.legacyAcquisitions,0);QCOMPARE(source.ordinaryReplies,0);QCOMPARE(source.copies,0);
  QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  operations.shutdown();QCOMPARE(acceptanceReplies,1);QCOMPARE(source.prepared,1);
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
#ifdef __GLIBC__
  QTRY_COMPARE_WITH_TIMEOUT(operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt(),1,5000);
#endif
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
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;doc["limits"]=limits;auto naming=doc["naming"].toObject();naming["mode"]="generated";doc["naming"]=naming;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());QString older,newer;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);older=r["operationId"].toString();});ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);newer=r["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),2);HistoryStore journal(ops.databasePath());const auto marker=journal.operation(newer).items[0].publicationId;int newerIndex=provider->requests[0].publicationId==marker?0:1;auto complete=provider->completions.takeAt(newerIndex);provider->requests.removeAt(newerIndex);complete();QTRY_COMPARE(source.copies,1);complete=provider->completions.takeFirst();complete();QTRY_VERIFY_WITH_TIMEOUT(([&]{HistoryStore store(ops.databasePath());return store.operation(older).copyStatus=="suppressed";})(),5000);QCOMPARE(source.copies,1);HistoryStore store(ops.databasePath());QCOMPARE(store.operation(older).items[0].uploadStatus,QString("uploaded"));QCOMPARE(store.operation(newer).items[0].uploadStatus,QString("uploaded"));
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
