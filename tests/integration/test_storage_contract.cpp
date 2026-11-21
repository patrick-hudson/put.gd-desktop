#include "fixtures/U2Fixtures.h"
#include <QSqlQuery>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;
class StorageContract:public QObject {
 Q_OBJECT
private slots:
 void acceptedDeletionRestartRequiresExplicitRetry() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();QString operation,historyId;QJsonObject accepted;
  {OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto item=store.operation(operation).items[0];historyId=item.artifact.id;accepted={{"id","accepted-delete"},{"state","accepted"},{"items",QJsonArray{QJsonObject{{"historyId",historyId},{"status","pending"},{"receipt",item.receipt.toJson()},{"progress",QJsonObject{{"schemaVersion",1}}}}}}};QVERIFY(!store.saveDeletion(accepted));}
  OperationService restarted(d.filePath("data"),settings,registry,source);HistoryService history(restarted);PUTGD_TRY_VERIFY(restarted.ready());QCOMPARE(provider->deletes.load(),0);QJsonObject inspected;history.inspectDelete("accepted-delete",[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;});PUTGD_TRY_VERIFY(!inspected.isEmpty());QCOMPARE(inspected["state"].toString(),QString("interrupted"));QCOMPARE(inspected["items"],accepted["items"]);
  bool replied=false;history.retryDelete("accepted-delete",[&](Error e,QJsonObject){QVERIFY(!e);replied=true;});PUTGD_TRY_VERIFY(replied);HistoryStore store(restarted.databasePath());PUTGD_TRY_COMPARE(store.detail(historyId)["remoteStatus"].toString(),QString("removed"));QCOMPARE(provider->deletes.load(),1);
  QVERIFY(!store.clear());const int copies=source.copies;replied=false;restarted.copyResult(operation,[&](Error,QJsonObject){replied=true;});PUTGD_TRY_VERIFY(replied);QCOMPARE(source.copies,copies);
 }
 void deletionDuringBatchCompletionExcludesTheRemovedUrl_data() {
  QTest::addColumn<QString>("mode");QTest::newRow("each-link")<<QString("each_link");QTest::newRow("list")<<QString("list");
 }
 void deletionDuringBatchCompletionExcludesTheRemovedUrl() {
  QFETCH(QString,mode);
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]=mode;doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));
  Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"first.txt","first synthetic file"),fixture(d.path(),"second.txt","second synthetic file")};OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);id=result["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(provider->completions.size(),1);
  HistoryStore ledger(ops.databasePath());auto first=ledger.operation(id).items.first();QCOMPARE(first.uploadStatus,QString("uploaded"));QJsonObject prepared;history.prepareDelete({first.artifact.id},[&](Error e,QJsonObject result){QVERIFY(!e);prepared=result;});PUTGD_TRY_VERIFY(!prepared.isEmpty());history.deleteConfirmed({first.artifact.id},prepared["acknowledgement"].toString(),[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(ledger.detail(first.artifact.id)["remoteStatus"].toString(),QString("removed"));
  // each_link may have copied the first URL before deletion, or its queued
  // durable lookup may already see the deletion and skip it. Both are valid.
  const auto copiesBeforeCompletion=source.copiedTexts.size();
  provider->completions.takeFirst()();
  PUTGD_TRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);auto completed=ledger.operation(id);
  QVERIFY(!first.receipt.directUrl.isEmpty());
  QVERIFY(!completed.items.last().receipt.directUrl.isEmpty());
  QCOMPARE(source.copied.text,completed.items.last().receipt.directUrl);
  for(const auto& text:source.copiedTexts.mid(copiesBeforeCompletion))
   QVERIFY(!text.split('\n').contains(first.receipt.directUrl));
  QCOMPARE(provider->uploads.load(),2);QCOMPARE(provider->deletes.load(),1);
 }
 void knownRemovedHistoryNeverWritesClipboard() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));Source source;
  OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);PUTGD_TRY_VERIFY(ops.ready());
  HistoryStore store(ops.databasePath());Operation op;op.id="removed-operation";op.created="2026-10-07T00:00:00.000Z";op.settings=settings.snapshot();
  OperationItem item;item.artifact.id="removed-item";item.artifact.name="fixture.txt";item.uploadStatus="uploaded";item.receipt.directUrl="https://example.invalid/original";op.items={item};QVERIFY(!store.saveOperation(op));
  for(const auto& status:QStringList{"removed","already_missing","hidden_version_retained","changed"}) {
   QVERIFY(!store.updateRemote(item.artifact.id,status));bool replied=false;QString code;
   history.copyLink(item.artifact.id,[&](Error e,QJsonObject){code=e.code;replied=true;});PUTGD_TRY_VERIFY(replied);
   QCOMPARE(source.copies,0);QCOMPARE(code,QString("link_unavailable"));QCOMPARE(store.detail(item.artifact.id)["receipt"].toObject()["directUrl"].toString(),item.receipt.directUrl);
   replied=false;ops.copyResult(op.id,[&](Error,QJsonObject){replied=true;});PUTGD_TRY_VERIFY(replied);QCOMPARE(source.copies,0);
  }
 }
 void confirmedOverwriteMarksOnlyOriginalObjectHistory() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());doc["naming"]=QJsonObject{{"mode","original"},{"collisionPolicy","overwrite"}};QVERIFY(settings.save(doc,{}));
  Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"same.txt","first synthetic version")};OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());
  QString first;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);
  HistoryStore store(ops.databasePath());const auto original=store.operation(first).items[0];const auto originalReceipt=original.receipt.toJson();
  source.payload.files={fixture(d.path(),"same.txt","replacement synthetic version")};QString second;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,2);
  QCOMPARE(store.detail(original.artifact.id)["remoteStatus"].toString(),QString("superseded"));QCOMPARE(store.detail(original.artifact.id)["receipt"].toObject(),originalReceipt);
  const auto replacement=store.operation(second).items[0];QCOMPARE(store.detail(original.artifact.id)["supersededBy"].toString(),replacement.artifact.id);
  QCOMPARE(store.detail(replacement.artifact.id)["remoteStatus"].toString(),QString("not_requested"));
  // Late copy bookkeeping cannot restore the older entry to its former current state.
  auto late=store.operation(first);late.copyStatus="suppressed";QVERIFY(!store.saveOperation(late));QCOMPARE(store.detail(original.artifact.id)["remoteStatus"].toString(),QString("superseded"));
  QCOMPARE(provider->uploads.load(),2);QCOMPARE(provider->deletes.load(),0);
 }
 void opaqueReceiptDeletionAndDurableProgress() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);PUTGD_TRY_VERIFY(ops.ready());QString operation;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());const auto uploaded=store.operation(operation);const auto id=uploaded.items[0].artifact.id;auto record=store.detail(id);auto receipt=StorageReceipt::fromJson(record["receipt"].toObject());QVERIFY(receipt.objectReference["fileId"].toString().contains("/%?#"));QVERIFY(!record.contains("bucket"));QVERIFY(!record.contains("key"));
  // #1759: with its destination removed from Settings nothing is deleted, and the preview offers Remove from History only.
  QVERIFY(settings.save(configuration(d.path(),true),settings.revision()));QJsonObject preview;
  history.prepareDelete({id},[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});PUTGD_TRY_VERIFY(!preview.isEmpty());
  QVERIFY(preview["targets"].toArray()[0].toObject()["destinationRemoved"].toBool());QVERIFY(preview["targets"].toArray()[0].toObject()["forgettable"].toBool());
  bool refused=false;history.deleteConfirmed({id},preview["acknowledgement"].toString(),[&](Error e,QJsonObject){QCOMPARE(e.code,QString("destination_removed"));refused=true;});PUTGD_TRY_VERIFY(refused);QCOMPARE(provider->deletes.load(),0);
  // Added back for the same place, deletion uses the recorded destination/reference again.
  QVERIFY(settings.save(configuration(d.path()),settings.revision()));preview={};
  history.prepareDelete({id},[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});PUTGD_TRY_VERIFY(!preview.isEmpty());QVERIFY(!preview["targets"].toArray()[0].toObject().contains("destinationRemoved"));bool denied=false;history.deleteConfirmed({id},"wrong",[&](Error e,QJsonObject){QCOMPARE(e.code,QString("delete_confirmation_changed"));denied=true;});PUTGD_TRY_VERIFY(denied);QCOMPARE(provider->deletes.load(),0);
  std::atomic_bool persisted{false};provider->checkpointObserver=[&](const DeleteRequest& request) {HistoryStore reopened(ops.databasePath());auto journal=reopened.deletion(request.id);persisted=journal["items"].toArray()[0].toObject()["progress"].toObject()["phase"]=="recorded-intent";};QString deletion;history.deleteConfirmed({id},preview["acknowledgement"].toString(),[&](Error e,QJsonObject r){QVERIFY(!e);deletion=r["deleteOperationId"].toString();});PUTGD_TRY_VERIFY(!deletion.isEmpty());PUTGD_TRY_COMPARE(store.detail(id)["remoteStatus"].toString(),QString("removed"));QVERIFY(persisted.load());QCOMPARE(provider->deletes.load(),1);QCOMPARE(provider->uploads.load(),1);QCOMPARE(store.detail(id)["receipt"].toObject(),receipt.toJson());QVERIFY(!store.detail(id).isEmpty());
 }
 void failedCheckpointDoesNotReplaceAcknowledgedProgress() {
  QTemporaryDir directory;
  ProviderRegistry registry;
  auto provider=std::make_shared<OpaqueProvider>();
  registry.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value) {return registry.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,registry,source);
  HistoryService history(operations);
  PUTGD_TRY_VERIFY(operations.ready());
  QString operationId;
  operations.invoke("clipboard",[&](Error error,QJsonObject result) {QVERIFY(!error);operationId=result["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.copies,1);
  HistoryStore store(operations.databasePath());
  const auto item=store.operation(operationId).items[0];
  const auto connection=QString("opaque-checkpoint-failure");
  {
   auto db=QSqlDatabase::addDatabase("QSQLITE",connection);
   db.setDatabaseName(operations.databasePath());QVERIFY(db.open());QSqlQuery query(db);
   QVERIFY(query.exec("CREATE TRIGGER fail_opaque_checkpoint BEFORE INSERT ON deletions "
      "WHEN json_extract(NEW.data,'$.items[0].status')='deleting' "
      "AND json_extract(NEW.data,'$.items[0].progress.phase')='recorded-intent' "
      "BEGIN SELECT RAISE(ABORT,'synthetic checkpoint failure'); END"));
  }
  QSqlDatabase::removeDatabase(connection);
  QJsonObject preview;
  history.prepareDelete({item.artifact.id},[&](Error error,QJsonObject result) {QVERIFY(!error);preview=result;});
  PUTGD_TRY_VERIFY(!preview.isEmpty());
  QString deletionId;
  history.deleteConfirmed({item.artifact.id},preview["acknowledgement"].toString(),[&](Error error,QJsonObject result) {QVERIFY(!error);deletionId=result["deleteOperationId"].toString();});
  PUTGD_TRY_VERIFY(!deletionId.isEmpty());
  PUTGD_TRY_COMPARE(store.deletion(deletionId)["state"].toString(),QString("completed"));
  const auto failed=store.deletion(deletionId)["items"].toArray()[0].toObject();
  QCOMPARE(failed["status"].toString(),QString("failed"));
  QVERIFY(!failed["error"].toObject()["code"].toString().isEmpty());
  QCOMPARE(provider->deletes.load(),0);
  // The provider returns no new progress after the failed acknowledgement.
  // History must retain only its last committed checkpoint.
  QCOMPARE(failed["progress"].toObject(),QJsonObject({{"schemaVersion",1}}));
 }
 // Review of #1603. Quit lands while a driver's checkpoint of a completed server action
 // (as S3's marker_done with its marker ID) is being saved: the helper gets no
 // acknowledgement, but the checkpoint is kept, so after a restart Retry continues from it.
 void quitKeepsACheckpointOfACompletedServerAction() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();QString deletion,id;
  std::atomic_bool intentAcknowledged{false},markerAcknowledged{true};
  {OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);PUTGD_TRY_VERIFY(ops.ready());QString operation;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);PUTGD_TRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(ops.databasePath());id=store.operation(operation).items[0].artifact.id;
   provider->removeOverride=[&](const DeleteRequest&,DurableCheckpoint checkpoint,std::function<void(DeleteResult)> cb){
    checkpoint({{"schemaVersion",1},{"phase","marker_intent"}},[&](Error e){intentAcknowledged=!e;});
    // The server made the marker; then Quit begins while its checkpoint is saved.
    history.stopDeletions();
    checkpoint({{"schemaVersion",1},{"phase","marker_done"},{"markerVersionId","marker-1"}},[&](Error e){markerAcknowledged=!e;});
    cb({"hidden_version_retained",{},{"cancelled","Synthetic driver stopped",{}}});
   };
   QJsonObject preview;history.prepareDelete({id},[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});PUTGD_TRY_VERIFY(!preview.isEmpty());history.deleteConfirmed({id},preview["acknowledgement"].toString(),[&](Error e,QJsonObject r){QVERIFY(!e);deletion=r["deleteOperationId"].toString();});PUTGD_TRY_VERIFY(!deletion.isEmpty());
   PUTGD_TRY_COMPARE(store.deletion(deletion)["items"].toArray()[0].toObject()["status"].toString(),QString("hidden_version_retained"));
   QVERIFY(intentAcknowledged.load());QVERIFY(!markerAcknowledged.load());
   QCOMPARE(store.deletion(deletion)["items"].toArray()[0].toObject()["progress"].toObject()["markerVersionId"].toString(),QString("marker-1"));}
  QJsonObject resumed;provider->removeOverride=[&](const DeleteRequest& request,DurableCheckpoint,std::function<void(DeleteResult)> cb){resumed=request.progress;cb({"removed",request.progress,{}});};
  OperationService restarted(d.filePath("data"),settings,registry,source);HistoryService history(restarted);PUTGD_TRY_VERIFY(restarted.ready());bool accepted=false;history.retryDelete(deletion,[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});PUTGD_TRY_VERIFY(accepted);HistoryStore store(restarted.databasePath());PUTGD_TRY_COMPARE(store.detail(id)["remoteStatus"].toString(),QString("removed"));
  QCOMPARE(resumed["phase"].toString(),QString("marker_done"));QCOMPARE(resumed["markerVersionId"].toString(),QString("marker-1"));
 }
 void partialDeletionRetainsIdentityAndHistoryClearNeverCallsProvider() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"a.txt","fixture a"),fixture(d.path(),"b.txt","fixture b")};QString deletion,unknownId;QStringList ids;
  {OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);PUTGD_TRY_VERIFY(ops.ready());QString operation;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,2);PUTGD_TRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(ops.databasePath());auto op=store.operation(operation);for(const auto& item:op.items)ids.append(item.artifact.id);unknownId=ids[1];auto fileId=op.items[1].receipt.objectReference["fileId"].toString();provider->deletionResults[fileId]="unknown";QJsonObject preview;history.prepareDelete(ids,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});PUTGD_TRY_VERIFY(!preview.isEmpty());history.deleteConfirmed(ids,preview["acknowledgement"].toString(),[&](Error e,QJsonObject r){QVERIFY(!e);deletion=r["deleteOperationId"].toString();});PUTGD_TRY_VERIFY(!deletion.isEmpty());PUTGD_TRY_COMPARE(store.detail(unknownId)["remoteStatus"].toString(),QString("unknown"));QCOMPARE(store.detail(ids[0])["remoteStatus"].toString(),QString("removed"));QVERIFY(!store.clear());QVERIFY(store.detail(ids[0]).isEmpty());QVERIFY(!store.detail(unknownId).isEmpty());QCOMPARE(provider->deletes.load(),2);}
  provider->deletionResults.clear();OperationService restarted(d.filePath("data"),settings,registry,source);HistoryService history(restarted);PUTGD_TRY_VERIFY(restarted.ready());QCOMPARE(provider->deletes.load(),2);bool accepted=false;history.retryDelete(deletion,[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});PUTGD_TRY_VERIFY(accepted);HistoryStore store(restarted.databasePath());PUTGD_TRY_COMPARE(store.detail(unknownId)["remoteStatus"].toString(),QString("removed"));QCOMPARE(provider->deletes.load(),3);QCOMPARE(provider->uploads.load(),2);
 }
 void wrongStatDestinationNeverAuthorizesDeletion() {
  QTemporaryDir directory;
  ProviderRegistry registry;
  auto provider=std::make_shared<OpaqueProvider>();
  registry.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value) {return registry.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;
  source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,registry,source);
  HistoryService history(operations);
  PUTGD_TRY_VERIFY(operations.ready());
  QString operationId;
  operations.invoke("clipboard",[&](Error error,QJsonObject result) {QVERIFY(!error);operationId=result["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.copies,1);
  HistoryStore store(operations.databasePath());
  const auto item=store.operation(operationId).items[0];
  QJsonObject preview;
  history.prepareDelete({item.artifact.id},[&](Error error,QJsonObject result) {QVERIFY(!error);preview=result;});
  PUTGD_TRY_VERIFY(!preview.isEmpty());
  provider->wrongStatDestination=true;
  QString deletionId;
  history.deleteConfirmed({item.artifact.id},preview["acknowledgement"].toString(),[&](Error error,QJsonObject result) {QVERIFY(!error);deletionId=result["deleteOperationId"].toString();});
  PUTGD_TRY_VERIFY(!deletionId.isEmpty());
  PUTGD_TRY_COMPARE(store.detail(item.artifact.id)["remoteStatus"].toString(),QString("changed"));
  QCOMPARE(provider->deletes.load(),0);
 }
 void unknownGenericDeletionRetryRejectsReplacement() {
  QTemporaryDir directory;
  ProviderRegistry registry;
  auto provider=std::make_shared<OpaqueProvider>();
  registry.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value) {return registry.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;
  source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,registry,source);
  HistoryService history(operations);
  PUTGD_TRY_VERIFY(operations.ready());
  QString operationId;
  operations.invoke("clipboard",[&](Error error,QJsonObject result) {QVERIFY(!error);operationId=result["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.copies,1);
  HistoryStore store(operations.databasePath());
  const auto item=store.operation(operationId).items[0];
  const auto objectId=item.receipt.objectReference["fileId"].toString();
  provider->deletionResults[objectId]="unknown";
  QJsonObject preview;
  history.prepareDelete({item.artifact.id},[&](Error error,QJsonObject result) {QVERIFY(!error);preview=result;});
  PUTGD_TRY_VERIFY(!preview.isEmpty());
  QString deleteId;
  history.deleteConfirmed({item.artifact.id},preview["acknowledgement"].toString(),[&](Error error,QJsonObject result) {QVERIFY(!error);deleteId=result["deleteOperationId"].toString();});
  PUTGD_TRY_COMPARE(store.detail(item.artifact.id)["remoteStatus"].toString(),QString("unknown"));
  PUTGD_TRY_VERIFY(!store.deletion(deleteId)["items"].toArray()[0].toObject()["progress"].toObject()["phase"].toString().isEmpty());
  putgd::test::settle(25);
  {
   QMutexLocker lock(&provider->mutex);
   auto replacement=item.receipt;
   replacement.publicationId="external-replacement";
   replacement.concurrencyToken="replacement-token";
   provider->objects[objectId]=replacement;
  }
  provider->deletionResults.clear();
  bool accepted=false;
  history.retryDelete(deleteId,[&](Error error,QJsonObject) {QVERIFY(!error);accepted=true;});
  PUTGD_TRY_VERIFY(accepted);
  PUTGD_TRY_COMPARE(store.detail(item.artifact.id)["remoteStatus"].toString(),QString("changed"));
  QCOMPARE(provider->deletes.load(),1);
  QCOMPARE(provider->objects[objectId].publicationId,QString("external-replacement"));
 }
 void shutdownDrainsPendingDeleteBeforeHistoryDestruction() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->slowDelete=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"a.txt","fixture a"),fixture(d.path(),"b.txt","fixture b")};QStringList historyIds;
  {OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());{HistoryService history(operations);QString operation;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,2);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(operations.databasePath());for(const auto& item:store.operation(operation).items)historyIds.append(item.artifact.id);QCOMPARE(historyIds.size(),2);QJsonObject preview;history.prepareDelete(historyIds,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});PUTGD_TRY_VERIFY(!preview.isEmpty());history.deleteConfirmed(historyIds,preview["acknowledgement"].toString(),[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->deletes.load(),1);}HistoryStore store(operations.databasePath());QCOMPARE(store.detail(historyIds[0])["remoteStatus"].toString(),QString("removed"));QCOMPARE(store.detail(historyIds[1])["remoteStatus"].toString(),QString("pending"));}
  putgd::test::settle(25);PUTGD_TRY_COMPARE(provider->uploads.load(),2);PUTGD_TRY_COMPARE(provider->deletes.load(),1);
 }
 void changedAndUnsupportedEntriesCannotDelete() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);PUTGD_TRY_VERIFY(ops.ready());QString operation;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto item=store.operation(operation).items[0];auto id=item.artifact.id;provider->allowDelete=false;QString code;history.prepareDelete({id},[&](Error e,QJsonObject){code=e.code;});PUTGD_TRY_COMPARE(code,QString("unsupported_capability"));provider->allowDelete=true;QJsonObject preview;history.prepareDelete({id},[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});PUTGD_TRY_VERIFY(!preview.isEmpty());{QMutexLocker lock(&provider->mutex);auto replacement=item.receipt;replacement.publicationId="foreign";provider->objects[item.receipt.objectReference["fileId"].toString()]=replacement;}bool accepted=false;history.deleteConfirmed({id},preview["acknowledgement"].toString(),[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});PUTGD_TRY_VERIFY(accepted);PUTGD_TRY_COMPARE(store.detail(id)["remoteStatus"].toString(),QString("changed"));QCOMPARE(provider->deletes.load(),0);
 }
};
QTEST_GUILESS_MAIN(StorageContract)
#include "test_storage_contract.moc"
