#include "fixtures/U2Fixtures.h"
#include "core/Log.h"
#include <QScopeGuard>
#include <utility>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QUuid>
#include <QBuffer>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <atomic>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fixtures/Waits.h"
#endif
using namespace putgd;using namespace putgd::test;
class DeferredClipboardSource final:public SourceAdapter {
public:
 quint64 revision=0;std::optional<quint64> copiedRevision;int reads=0,copies=0;
 QString current="synthetic initial clipboard";CopyPayload proposed;ClipboardPayload payload=textPayload();
 bool workerReply=false,holdImports=false;QList<std::function<void(PayloadResult)>> imports;
 std::function<void(Error)> pending;
 quint64 clipboardRevision()const override{return revision;}
 std::optional<quint64> lastCopyRevision()const override{return copiedRevision;}
 QJsonObject capabilities()const override{return {};}
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override {
  ++reads;if(holdImports)imports.append(std::move(reply));else reply({payload,{}});
 }
 void cancelAcquisition()override{}
 void copy(const CopyPayload& payload,std::function<void(Error)> reply)override {
  QVERIFY2(!pending,"Two native clipboard commits overlapped");++copies;proposed=payload;pending=std::move(reply);
 }
 void acknowledge(Error error={}) {
  QVERIFY(pending);if(!error){current=proposed.text;copiedRevision=++revision;}
  auto reply=std::exchange(pending,{});
  if(workerReply){auto thread=QThread::create([reply,error]{reply(error);});thread->start();thread->wait();delete thread;}else reply(error);
 }
 void invalidateCopyEligibility()override {
  if(pending)std::exchange(pending,{})({"copy_suppressed","Synthetic unsent native commit cancelled",{}});
 }
};
// A destination whose link comes from a separate sharing step after the
// transfer, as Proton Drive's does (#478): uploads of `shareLater` names (or
// all of them) return no link, and share() waits until the test releases it.
class LateLinkProvider final:public StorageProvider {
public:
 OpaqueProvider storage;bool shareAll=false;QSet<QString> shareLater;QObject* dispatcher=nullptr;QList<std::function<void()>> shares;
 ProviderCapabilities capabilities(const Destination& d) const override {auto c=storage.capabilities(d);c.sharing=true;return c;}
 int maximumConcurrentRequests(const Destination& d) const override {return storage.maximumConcurrentRequests(d);}
 QString serializationKey(const Destination& d,const QJsonObject& o,const QString& n) const override {return storage.serializationKey(d,o,n);}
 Error validateSettings(const QJsonObject& s) const override {return storage.validateSettings(s);}
 void checkName(const Destination& d,const QString& n,std::function<void(NameCheck)> cb, std::shared_ptr<std::atomic_bool> cancellation = {}) override {storage.checkName(d,n,cb,cancellation);}
 void upload(const UploadRequest& r,std::function<void(UploadResult)> cb) override {
  const bool late=shareAll||shareLater.contains(r.candidate);
  storage.upload(r,[late,cb](UploadResult result){if(late)result.receipt.directUrl.clear();cb(result);});
 }
 void stat(const StorageReceipt& r,std::function<void(StatResult)> cb) override {storage.stat(r,cb);}
 void reconcile(const UploadRequest& r,std::function<void(StatResult)> cb) override {storage.reconcile(r,cb);}
 void remove(const DeleteRequest& r,DurableCheckpoint d,std::function<void(DeleteResult)> cb) override {storage.remove(r,d,cb);}
 void share(const ShareRequest& r,DurableCheckpoint durable,std::function<void(ShareResult)> cb) override {
  durable({{"schemaVersion",1},{"phase","intent"}},[this,r,cb](Error e){
   if(e){cb({"failed",r.receipt,{},e});return;}
   auto complete=[r,cb]{auto receipt=r.receipt;receipt.directUrl="https://example.invalid/shared#"+r.receipt.publicationId;cb({"shared",receipt,{},{}});};
   QMetaObject::invokeMethod(dispatcher,[this,complete]{shares.append(complete);},Qt::QueuedConnection);
  });
 }
};
class OperationsTest:public QObject {
 Q_OBJECT
private slots:
 void destinationAndLocalCopiesAreIndependent_data() {
  QTest::addColumn<bool>("destination");QTest::addColumn<bool>("localCopy");QTest::addColumn<bool>("inherit");QTest::addColumn<bool>("unconfiguredUpload");
  // A local-only action never uploads, whether set on the action or inherited, and an
  // unconfigured upload keeps its local copy without sending anything.
  QTest::newRow("local-only")<<false<<true<<false<<false;
  QTest::newRow("local-only-inherited")<<false<<true<<true<<false;
  QTest::newRow("unconfigured-with-local")<<false<<true<<false<<true;
 }
 void destinationAndLocalCopiesAreIndependent() {
  QFETCH(bool,destination);QFETCH(bool,localCopy);QFETCH(bool,inherit);QFETCH(bool,unconfiguredUpload);
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(d.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=localCopy;doc["archive"]=archive;
  QJsonObject output{{"destination",destination||unconfiguredUpload?"upload":"local"},{"profileId",destination?QJsonValue("test"):QJsonValue(QJsonValue::Null)},{"publicationMode","default"}};
  auto actions=doc["actions"].toObject();for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();action["destination"]=inherit?QJsonValue("default"):output["destination"];action["profileId"]=inherit?QJsonValue(QJsonValue::Null):output["profileId"];actions[name]=action;}doc["actions"]=actions;if(inherit)doc["captureDefaults"]=output;QVERIFY(settings.save(doc,{}));
  Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QString id;Error error;bool replied=false;operations.invoke("clipboard",[&](Error e,QJsonObject result){error=e;id=result["operationId"].toString();replied=true;});PUTGD_TRY_VERIFY(replied);
  if(!destination&&!localCopy){QCOMPARE(error.code,QString("output_required"));QVERIFY(id.isEmpty());QCOMPARE(source.reads,0);QCOMPARE(provider->uploads.load(),0);QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),0);return;}
  QVERIFY2(!error,qPrintable(error.message));QVERIFY(!id.isEmpty());HistoryStore journal(operations.databasePath());PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);const auto op=journal.operation(id);QCOMPARE(op.items.size(),1);const auto item=op.items.first();QCOMPARE(provider->uploads.load(),destination?1:0);QCOMPARE(item.uploadStatus,destination?QString("uploaded"):QString("local_only"));QCOMPARE(item.local.status=="saved",localCopy);if(localCopy){QFile file(item.local.path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),source.payload.bytes);}
 }
 void cancellingBeforeNativeCommitDoesNotWriteTheLink() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));DeferredClipboardSource source;
  source.payload.kind="files";for(int i=0;i<2;++i)source.payload.files.append(fixture(d.path(),QString("cancel-%1.txt").arg(i),"synthetic batch"));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();if(source.pending)source.acknowledge();});QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(source.copies,1);QVERIFY(source.pending);PUTGD_TRY_COMPARE(provider->completions.size(),1);
  operations.cancel(id,[](Error e,QJsonObject){QVERIFY(!e);});if(source.pending)source.acknowledge();provider->completions.takeFirst()();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(source.current,QString("synthetic initial clipboard"));QCOMPARE(source.revision,quint64(0));QCOMPARE(journal.operation(id).state,QString("cancelled"));QCOMPARE(journal.operation(id).copyStatus,QString("no_result"));QVERIFY(journal.operation(id).items.first().linkAvailable());
 }
 void cancelledListMemberIsRemovedFromAnUnsentNativeCommit() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]="list";doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));DeferredClipboardSource source;
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();if(source.pending)source.acknowledge();});QStringList ids;
  for(int i=0;i<2;++i){QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.reads,i+1);ids.append(id);}
  for(int i=0;i<2;++i){PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();}PUTGD_TRY_COMPARE(source.copies,1);QVERIFY(source.pending);
  operations.cancel(ids.first(),[](Error e,QJsonObject){QVERIFY(!e);});if(source.pending)source.acknowledge();
  PUTGD_TRY_VERIFY(source.copies==2||operations.diagnostics()["activeOperations"].toInt()==0);if(source.pending)source.acknowledge();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(source.current,journal.operation(ids[1]).items.first().receipt.directUrl);QCOMPARE(journal.operation(ids[0]).copyStatus,QString("no_result"));QCOMPARE(journal.operation(ids[1]).copyStatus,QString("copied"));QCOMPARE(source.revision,quint64(1));
 }
 void resumedCopiesRecheckDurableRemoval_data() {QTest::addColumn<QString>("mode");QTest::newRow("each-link")<<QString("each_link");QTest::newRow("list")<<QString("list");}
 void resumedCopiesRecheckDurableRemoval() {
  QFETCH(QString,mode);QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]=mode;doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));DeferredClipboardSource source;
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();for(auto imported:source.imports)imported({textPayload(),{}});source.imports.clear();if(source.pending)source.acknowledge();});QStringList ids;
  for(int i=0;i<2;++i){QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.reads,i+1);ids.append(id);}
  for(int i=0;i<2;++i){PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();}PUTGD_TRY_COMPARE(source.copies,1);QVERIFY(source.pending);PUTGD_TRY_COMPARE(journal.operation(ids[1]).state,QString("completed"));
  source.holdImports=true;QString third;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);third=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.imports.size(),1);QVERIFY(!source.pending);
  const QStringList removed{journal.operation(ids[0]).items.first().artifact.id};QJsonObject preview;history.prepareDelete(removed,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});PUTGD_TRY_VERIFY(!preview.isEmpty());QVERIFY(!preview["acknowledgement"].toString().isEmpty());history.deleteConfirmed(removed,preview["acknowledgement"].toString(),[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(journal.detail(removed.first())["remoteStatus"].toString(),QString("removed"));
  source.imports.takeFirst()({textPayload(),{}});PUTGD_TRY_COMPARE(source.copies,2);QVERIFY(source.pending);QCOMPARE(source.proposed.text,journal.operation(ids[1]).items.first().receipt.directUrl);source.acknowledge();
  PUTGD_TRY_COMPARE(journal.operation(ids[0]).copyStatus,QString("no_result"));PUTGD_TRY_COMPARE(journal.operation(ids[1]).copyStatus,QString("copied"));PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(source.copies,3);source.acknowledge();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(source.current,journal.operation(third).items.first().receipt.directUrl);QCOMPARE(source.revision,quint64(2));QCOMPARE(provider->deletes.load(),1);
 }
 void deferredCopiesSerializeAndKeepPostCommitUserEdits_data() {
  QTest::addColumn<bool>("userChanged");QTest::addColumn<bool>("offThread");QTest::newRow("own-copies")<<false<<false;QTest::newRow("user-edit-before-callback-dispatch")<<true<<false;QTest::newRow("worker-reply")<<false<<true;QTest::newRow("worker-reply-with-user-edit")<<true<<true;
 }
 void deferredCopiesSerializeAndKeepPostCommitUserEdits() {
  QFETCH(bool,userChanged);QFETCH(bool,offThread);QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));DeferredClipboardSource source;source.workerReply=offThread;
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();if(source.pending)source.acknowledge();});QStringList ids;
  for(int i=0;i<2;++i){QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.reads,i+1);ids.append(id);}
  // The uploads share one name and its object lock, which either operation may take first
  // (#289): follow the operations in the order their uploads finish.
  QStringList finished;
  for(int i=0;i<2;++i) {
   PUTGD_TRY_COMPARE(provider->completions.size(),1);
   QString id;for(const auto& candidate:ids)if(provider->requests.last().artifact.path.contains(candidate))id=candidate;
   QVERIFY(!id.isEmpty());QVERIFY(!finished.contains(id));finished.append(id);provider->completions.takeFirst()();
   if(i==0){PUTGD_TRY_COMPARE(source.copies,1);QVERIFY(source.pending);}
   else {PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));QCOMPARE(source.copies,1);}
  }
  source.acknowledge();
  if(userChanged){++source.revision;source.current="synthetic user edit";}
  else {PUTGD_TRY_COMPARE(source.copies,2);source.acknowledge();}
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(journal.operation(finished[0]).copyStatus,QString("copied"));QCOMPARE(journal.operation(finished[1]).copyStatus,QString(userChanged?"suppressed":"copied"));
  QCOMPARE(source.copies,userChanged?1:2);QCOMPARE(source.current,userChanged?QString("synthetic user edit"):journal.operation(finished[1]).items.first().receipt.directUrl);
 }


 void cleanupBlocksRetriesUntilItsSnapshotFinishes() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  bool cleaned=false;operations.cleanup([&](Error e,QJsonObject){QVERIFY(!e);cleaned=true;});
  // No event dispatch between the snapshot and retry admission: even a fast
  // worker cannot deliver its session-thread completion yet.
  Error retryError,archiveError,secondCleanupError;operations.retry("synthetic-old-operation",[&](Error e,QJsonObject){retryError=e;});operations.retryLocalSave("synthetic-old-operation",[&](Error e,QJsonObject){archiveError=e;});operations.cleanup([&](Error e,QJsonObject){secondCleanupError=e;});
  QCOMPARE(retryError.code,QString("cleanup_busy"));QCOMPARE(archiveError.code,QString("cleanup_busy"));QCOMPARE(secondCleanupError.code,QString("cleanup_busy"));PUTGD_TRY_VERIFY(cleaned);
  bool retried=false;operations.retry("synthetic-old-operation",[&](Error e,QJsonObject){QCOMPARE(e.code,QString("operation_not_found"));retried=true;});PUTGD_TRY_VERIFY(retried);
 }


 void selectorCancellationIsTerminalWithoutPublication() {
  QTemporaryDir directory;ProviderRegistry registry;
  auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.defer=true;
  OperationService operations(directory.filePath("data"),settings,registry,source);
  PUTGD_TRY_VERIFY(operations.ready());QString id;
  operations.invoke("region",[&](Error error,QJsonObject response){QVERIFY(!error);id=response["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.acquisitions.size(),1);
  source.acquisitions.takeFirst()({{}, {"cancelled","Selection cancelled",{}}});
  HistoryStore journal(operations.databasePath());
  PUTGD_TRY_COMPARE(journal.operation(id).state,QString("cancelled"));
  QVERIFY(journal.operation(id).cancelled);
  QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.copies,0);
#ifdef __GLIBC__
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt(),1,5000);
#endif
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
  PUTGD_TRY_VERIFY(operations.ready());
  QString id;
  operations.invoke("clipboard",[&](Error error,QJsonObject response) {
   QVERIFY(!error);
   id=response["operationId"].toString();
  });
  PUTGD_TRY_VERIFY(!id.isEmpty());
  HistoryStore journal(operations.databasePath());
  PUTGD_TRY_COMPARE(journal.operation(id).copyStatus,QString("no_result"));
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
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
  PUTGD_TRY_VERIFY(accepted);
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  const auto item=journal.operation(id).items[0];
  QCOMPARE(item.uploadStatus,QString("unknown"));
  QCOMPARE(item.candidate,candidate);
  QVERIFY(item.receipt.directUrl.isEmpty());
  QCOMPARE(provider->uploads.load(),1);
  QCOMPARE(provider->deletes.load(),0);
 }
 void reverseCompletionCopiesBothLinksAndUserChangesRemainProtected() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;provider->concurrency=8;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;doc["limits"]=limits;auto naming=doc["naming"].toObject();naming["mode"]="generated";doc["naming"]=naming;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString older,newer;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);older=r["operationId"].toString();});ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);newer=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),2);HistoryStore journal(ops.databasePath());const auto marker=journal.operation(newer).items[0].publicationId;int newerIndex=provider->requests[0].publicationId==marker?0:1;auto complete=provider->completions.takeAt(newerIndex);provider->requests.removeAt(newerIndex);complete();PUTGD_TRY_COMPARE(source.copies,1);complete=provider->completions.takeFirst();complete();PUTGD_TRY_VERIFY_WITH_TIMEOUT(([&]{HistoryStore store(ops.databasePath());return store.operation(older).copyStatus=="copied";})(),5000);QCOMPARE(source.copies,2);HistoryStore store(ops.databasePath());QCOMPARE(store.operation(older).items[0].uploadStatus,QString("uploaded"));QCOMPARE(store.operation(newer).items[0].uploadStatus,QString("uploaded"));
  QString third;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);third=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);++source.revision;provider->completions.takeFirst()();PUTGD_TRY_VERIFY(([&]{HistoryStore s(ops.databasePath());return s.operation(third).copyStatus=="suppressed";})());QCOMPARE(source.copies,2);
 }
 void unknownRestartReconcilesWithoutNewUploadAndSnapshotsSettings() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->loseReply=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();QString id,candidate;
  {OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_VERIFY(([&]{HistoryStore s(ops.databasePath());auto o=s.operation(id);return !o.items.isEmpty()&&o.items[0].uploadStatus=="unknown"&&o.copyStatus=="no_result";})());HistoryStore s(ops.databasePath());candidate=s.operation(id).items[0].candidate;}
  auto changed=configuration(d.path(),true);QVERIFY(settings.save(changed,settings.revision()));OperationService restarted(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(restarted.ready());QCOMPARE(provider->uploads.load(),1);bool accepted=false;restarted.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});PUTGD_TRY_VERIFY(accepted);PUTGD_TRY_VERIFY(([&]{HistoryStore s(restarted.databasePath());auto o=s.operation(id);return o.items[0].uploadStatus=="uploaded";})());QCOMPARE(provider->uploads.load(),1);HistoryStore s(restarted.databasePath());auto op=s.operation(id);QCOMPARE(op.items[0].candidate,candidate);QCOMPARE(op.destination.profileId,QString("test"));QCOMPARE(op.items[0].receipt.destination.settings["workspace"].toString(),QString("fixture-workspace"));
 }
 void archiveFailureDoesNotStopUploadAndRetryUsesSpool() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=true;const auto blocker=fixture(d.path(),"blocked","synthetic");archive["root"]=blocker;doc["archive"]=archive;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto op=store.operation(id);QCOMPARE(op.items[0].uploadStatus,QString("uploaded"));QCOMPARE(op.items[0].local.status,QString("failed"));QVERIFY(QFile::remove(blocker));QVERIFY(QDir().mkpath(blocker));PUTGD_TRY_COMPARE(store.operation(id).copyStatus,QString("copied"));PUTGD_TRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);bool done=false;ops.retryLocalSave(id,[&](Error e,QJsonObject){QVERIFY(!e);done=true;});PUTGD_TRY_VERIFY(done);op=store.operation(id);QCOMPARE(op.items[0].local.status,QString("saved"));QCOMPARE(provider->uploads.load(),1);QFile archived(op.items[0].local.path);QVERIFY(archived.open(QIODevice::ReadOnly));QCOMPARE(archived.readAll(),QByteArray("synthetic source content"));
 }
 void overwriteBatchMarksBothAndSkipsNeverCopy() {
  QTemporaryDir d;QDir().mkpath(d.filePath("a"));QDir().mkpath(d.filePath("b"));auto first=fixture(d.filePath("a"),"report.pdf","%PDF-1.4\nfixture-a"),second=fixture(d.filePath("b"),"report.pdf","%PDF-1.4\nfixture-b"),third=fixture(d.path(),"notes.txt","fixture-notes");ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());doc["naming"]=QJsonObject{{"mode","original"},{"collisionPolicy","overwrite"}};QVERIFY(settings.save(doc,{}));Source source;source.payload.kind="files";source.payload.files={first,second,third};OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto op=store.operation(id);QCOMPARE(op.items[0].uploadStatus,QString("conflict"));QCOMPARE(op.items[1].uploadStatus,QString("conflict"));QCOMPARE(op.items[2].uploadStatus,QString("uploaded"));QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.copied.text,op.items[2].receipt.directUrl);
  doc["naming"]=QJsonObject{{"mode","original"},{"collisionPolicy","skip"}};QVERIFY(settings.save(doc,settings.revision()));source.payload.files={third};QString skipped;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);skipped=r["operationId"].toString();});PUTGD_TRY_VERIFY(([&]{auto o=store.operation(skipped);return o.copyStatus=="no_result";})());QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.copies,1);QVERIFY(store.operation(skipped).items[0].receipt.publicationId.isEmpty());
 }
 void onlyTheResidentOwnerRecoversAbandonedStaging() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;const auto root=d.filePath("data"),staging=root+"/artifacts/.817effa3-cd94-4a25-b066-d85f7fbbdb9a-4c0b1d4a-13a3-46ba-9316-90859b0cfe4e";
  {OperationService owner(root,settings,registry,source);PUTGD_TRY_VERIFY(owner.ready());QVERIFY(QDir().mkpath(staging));const auto artifact=fixture(staging,"cad9f544-c88c-4d8e-875b-576db75867dd","unfinished synthetic bytes"),local=fixture(staging,"user-owned-copy.txt","user archive");OperationService second(root,settings,registry,source);QSignalSpy initialized(&second,&OperationService::initialized);PUTGD_TRY_COMPARE(initialized.count(),1);QVERIFY(!second.ready());QString code;second.invoke("clipboard",[&](Error e,QJsonObject){code=e.code;});QCOMPARE(code,QString("journal_busy"));QVERIFY(QFile::exists(artifact));QVERIFY(QFile::exists(local));}
  OperationService restarted(root,settings,registry,source);PUTGD_TRY_VERIFY(restarted.ready());QVERIFY(!QFile::exists(staging+"/cad9f544-c88c-4d8e-875b-576db75867dd"));QVERIFY(QFile::exists(staging+"/user-owned-copy.txt"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void committedAcquisitionRecoversAfterMissingItemCheckpoint() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));const auto root=d.filePath("data"),id=QString("2e0c5cf4-ce1a-4c36-979d-e56c7d28c9e5");Operation accepted;accepted.id=id;accepted.action="clipboard";accepted.state="acquiring";accepted.created="2026-10-07T00:00:00.000Z";accepted.settings=settings.snapshot();accepted.settingsRevision=settings.revision();accepted.destination={"opaque","test",settings.revision(),{{"workspace","fixture-workspace"}},{{"mode","session"},{"reference",QJsonValue::Null}}};HistoryStore store(root+"/history.sqlite");QVERIFY(!store.saveOperation(accepted));ArtifactStore artifacts(root+"/artifacts");auto acquired=artifacts.acquire(id,textPayload(),ResourceLimits{});QVERIFY(!acquired.error);Source source;source.payload=textPayload();OperationService ops(root,settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());auto recovered=store.operation(id);QCOMPARE(recovered.state,QString("interrupted"));QCOMPARE(recovered.items.size(),1);QCOMPARE(recovered.items[0].artifact.sha256,acquired.artifacts[0].sha256);QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.reads,0);bool retried=false;ops.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});PUTGD_TRY_VERIFY(retried);PUTGD_TRY_COMPARE(store.operation(id).items[0].uploadStatus,QString("uploaded"));QVERIFY(!store.operation(id).items[0].candidate.isEmpty());QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,0);
 }
 void resumableUploadJournalSurvivesForRetry() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->journalThenUnknown=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));
  Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());
  HistoryStore store(ops.databasePath());bool earlyAnswered=false;Error earlyError;
  // Synchronously observe the durable result before finish() can release its
  // owner. This forces the ordering that the old inspect/poll occasionally saw
  // under contention, without sleeps or a production scheduling hook.
  const auto connection=connect(&ops,&OperationService::changed,this,[&](const QString& changed){
   if(earlyAnswered||store.operation(changed).state!="partial")return;
   ops.retry(changed,[&](Error error,QJsonObject){earlyError=error;earlyAnswered=true;});
  });
  const auto disconnectEarly=qScopeGuard([&]{disconnect(connection);});
  QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  PUTGD_TRY_VERIFY(earlyAnswered);disconnect(connection);
  QCOMPARE(earlyError.code,QString("operation_busy"));
  QJsonObject op;PUTGD_TRY_VERIFY([&]{auto done=std::make_shared<bool>(false);ops.inspect(id,[&op,done](Error,QJsonObject r){op=r;*done=true;});if(!putgd::test::waitFor([&]{return *done;}))return false;return op["state"]=="partial";}());
  QCOMPARE(op["items"].toArray()[0].toObject()["uploadStatus"].toString(),QString("unknown"));
  // The journal is durable on the item but never part of the public operation.
  QVERIFY(!op["items"].toArray()[0].toObject().contains("uploadJournal"));
  Error error;const auto saved=store.operation(id,&error);QVERIFY(!error);
  QCOMPARE(saved.items[0].uploadJournal,QJsonObject({{"schemaVersion",1},{"upload","opaque-resumable"}}));
  // The worker's durable partial state precedes finish()/clipboard bookkeeping.
  // Wait for the owner to release the operation, rather than racing retry()
  // against that separate completion (which correctly returns operation_busy).
  PUTGD_TRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);
  provider->journalThenUnknown=false;bool retried=false;ops.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});PUTGD_TRY_VERIFY(retried);
  PUTGD_TRY_COMPARE(provider->uploads.load(),2);PUTGD_TRY_VERIFY(!ops.storageActive()&&ops.activeObservations().isEmpty());
  QMutexLocker lock(&provider->mutex);QVERIFY(provider->resumes[0].isEmpty());QCOMPARE(provider->resumes[1],saved.items[0].uploadJournal);
 }
 // #202: retention applies without the user, on the resident's own clock.
 void automaticCleanupAppliesRetentionWithoutTheUser() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->journalThenFail=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));
  QCOMPARE(settings.snapshot().failedArtifactDays,7);
  Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);
  auto now=QDateTime::currentDateTimeUtc();ops.setClockForTesting([&]{return now;});ops.setCleanupScheduleForTesting({0,60*60*1000,60*1000,50});
  const auto last=[&]{return ops.diagnostics()["artifactCleanup"].toObject()["last"].toObject();};
  const auto passAt=[&](const QDateTime& at){return last()["finished"].toString()==at.toString(Qt::ISODateWithMs)&&!ops.diagnostics()["artifactCleanup"].toObject()["running"].toBool();};
  // A pass runs after journal recovery (a minute later by default), with nobody asking.
  PUTGD_TRY_VERIFY(ops.ready());PUTGD_TRY_VERIFY(passAt(now));QCOMPARE(last()["trigger"].toString(),QString("automatic"));QCOMPARE(last()["removedOperations"].toInt(),0);
  QCOMPARE(ops.diagnostics()["artifactCleanup"].toObject()["intervalMs"].toInt(),60*60*1000);
  QStringList ids;
  for(int i=0;i<4;++i){QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
   PUTGD_TRY_VERIFY(([&]{HistoryStore store(ops.databasePath());return QStringList({"partial","failed"}).contains(store.operation(id).state);})());PUTGD_TRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);ids<<id;}
  HistoryStore store(ops.databasePath());QStringList spools;for(const auto& id:ids)spools<<store.operation(id).items[0].artifact.path;
  // A preview the user's cache holds is left to Clean artifacts.
  const auto preview=d.filePath("data/previews/"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".png");QVERIFY(QDir().mkpath(QFileInfo(preview).path()));
  {QFile file(preview);QVERIFY(file.open(QIODevice::WriteOnly));file.write("png");}
  // Six days on, every failed artifact stays for Retry.
  now=now.addDays(6);ops.cleanupExpired();PUTGD_TRY_VERIFY(passAt(now));
  QCOMPARE(last()["removedOperations"].toInt(),0);QVERIFY(provider->abandoned.isEmpty());for(const auto& spool:spools)QVERIFY(QFile::exists(spool));
  // Past retention a pass takes at most its batch, aborting each retained upload (#146).
  ops.setCleanupScheduleForTesting({0,60*60*1000,60*60*1000,1});now=now.addDays(2);ops.cleanupExpired();PUTGD_TRY_VERIFY(passAt(now));
  QCOMPARE(last()["removedOperations"].toInt(),1);QCOMPARE(last()["abortedUploads"].toInt(),1);QVERIFY(last()["more"].toBool());QCOMPARE(provider->abandoned.size(),1);
  QCOMPARE(int(std::count_if(spools.begin(),spools.end(),[](const QString& path){return !QFile::exists(path);})),1);QVERIFY(QFile::exists(preview));
  // A Retry during a pass stops it at the next operation and runs once it ends,
  // and a manual cleanup waits for it too instead of failing.
  ops.setCleanupScheduleForTesting({0,60*60*1000,60*60*1000,50});
  QSemaphore entered,release;std::atomic_bool block{true};provider->beforeAbandon=[&]{if(block.exchange(false)){entered.release();release.acquire();}};
  auto unblock=qScopeGuard([&]{release.release();});
  now=now.addSecs(60);ops.cleanupExpired();QVERIFY(entered.tryAcquire(1,10000));
  QStringList remaining;for(int i=0;i<ids.size();++i)if(QFile::exists(spools[i]))remaining<<ids[i];QCOMPARE(remaining.size(),3);std::sort(remaining.begin(),remaining.end());
  const auto retried=remaining.last();Error retryError;bool retryAnswered=false,manualAnswered=false;QJsonObject manual;
  ops.retry(retried,[&](Error e,QJsonObject){retryError=e;retryAnswered=true;});
  ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);manual=r;manualAnswered=true;});
  putgd::test::settle(100);PUTGD_TRY_VERIFY(!retryAnswered);PUTGD_TRY_VERIFY(!manualAnswered);
  release.release();unblock.dismiss();
  PUTGD_TRY_VERIFY(retryAnswered);QVERIFY2(!retryError,qPrintable(retryError.message));PUTGD_TRY_VERIFY(manualAnswered);
  // The pass ended after the operation it was on; the Retry kept its own artifact.
  QCOMPARE(QFile::exists(spools[ids.indexOf(remaining.first())]),false);QVERIFY(QFile::exists(spools[ids.indexOf(retried)]));
  QCOMPARE(manual["trigger"].toString(),QString("manual"));QVERIFY(!QFile::exists(preview));
  PUTGD_TRY_VERIFY(ops.activeObservations().isEmpty());ops.shutdown();provider->beforeAbandon={};
 }
 // #478: Proton Drive's link arrives from the sharing step, after the file's
 // transfer has settled. each_link must still copy it.
 void linkThatFollowsTheTransferIsCopied() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<LateLinkProvider>();provider->dispatcher=this;provider->shareAll=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));
  Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{(void)putgd::test::waitFor([&]{while(!provider->shares.isEmpty())provider->shares.takeFirst()();return operations.diagnostics()["activeOperations"].toInt()==0;},10000);});
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  // The transfer has ended without a link; the sharing step is still running.
  PUTGD_TRY_COMPARE(provider->shares.size(),1);
  {const auto op=journal.operation(id);QCOMPARE(op.items[0].uploadStatus,QString("uploaded"));QVERIFY(op.items[0].receipt.directUrl.isEmpty());QCOMPARE(op.copyStatus,QString("pending"));}
  PUTGD_COMPARE_STEADY(source.copies,0,100);
  provider->shares.takeFirst()();
  PUTGD_TRY_COMPARE(journal.operation(id).copyStatus,QString("copied"));
  const auto op=journal.operation(id);QCOMPARE(op.state,QString("completed"));QCOMPARE(op.items[0].shareStatus,QString("shared"));QVERIFY(!op.error);
  QCOMPARE(source.copies,1);QVERIFY(op.items[0].receipt.directUrl.startsWith("https://example.invalid/shared#"));QCOMPARE(source.copied.text,op.items[0].receipt.directUrl);
 }
 void zeroRetentionAndActiveRetryPreserveArtifacts() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto retention=doc["retention"].toObject();retention["failedArtifactDays"]=0;doc["retention"]=retention;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(([&]{HistoryStore store(ops.databasePath());return store.operation(id).copyStatus=="no_result";})());HistoryStore store(ops.databasePath());auto op=store.operation(id);op.created="2000-01-01T00:00:00.000Z";QVERIFY(!store.saveOperation(op));const auto spool=op.items[0].artifact.path;bool cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);QCOMPARE(r["removedOperations"].toInt(),0);cleaned=true;});PUTGD_TRY_VERIFY(cleaned);QVERIFY(QFile::exists(spool));
  provider->failUpload=false;provider->deferUploads=true;provider->dispatcher=this;ops.retry(id,[](Error e,QJsonObject){QVERIFY(!e);});cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);QCOMPARE(r["removedOperations"].toInt(),0);cleaned=true;});PUTGD_TRY_VERIFY(cleaned);QVERIFY(QFile::exists(spool));PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_VERIFY(([&]{return store.operation(id).items[0].uploadStatus=="uploaded";})());
 }
};
QTEST_GUILESS_MAIN(OperationsTest)
#include "test_operations.moc"
