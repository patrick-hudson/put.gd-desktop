#include "fixtures/U2Fixtures.h"
#include <QSqlQuery>
using namespace putgd;using namespace putgd::test;
class StorageContract:public QObject {
 Q_OBJECT
private slots:
 void acceptedDeletionRestartRequiresExplicitRetry() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();QString operation,historyId;QJsonObject accepted;
  {OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto item=store.operation(operation).items[0];historyId=item.artifact.id;accepted={{"id","accepted-delete"},{"state","accepted"},{"items",QJsonArray{QJsonObject{{"historyId",historyId},{"status","pending"},{"receipt",item.receipt.toJson()},{"progress",QJsonObject{{"schemaVersion",1}}}}}}};QVERIFY(!store.saveDeletion(accepted));}
  OperationService restarted(d.filePath("data"),settings,registry,source);HistoryService history(restarted);QTRY_VERIFY(restarted.ready());QCOMPARE(provider->deletes.load(),0);QJsonObject inspected;history.inspectDelete("accepted-delete",[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;});QTRY_VERIFY(!inspected.isEmpty());QCOMPARE(inspected["state"].toString(),QString("interrupted"));QCOMPARE(inspected["items"],accepted["items"]);
  bool replied=false;history.retryDelete("accepted-delete",[&](Error e,QJsonObject){QVERIFY(!e);replied=true;});QTRY_VERIFY(replied);HistoryStore store(restarted.databasePath());QTRY_COMPARE(store.detail(historyId)["remoteStatus"].toString(),QString("removed"));QCOMPARE(provider->deletes.load(),1);
  QVERIFY(!store.clear());const int copies=source.copies;replied=false;restarted.copyResult(operation,[&](Error,QJsonObject){replied=true;});QTRY_VERIFY(replied);QCOMPARE(source.copies,copies);
 }
 void deletionDuringBatchCompletionExcludesTheRemovedUrl_data() {
  QTest::addColumn<QString>("mode");QTest::newRow("each-link")<<QString("each_link");QTest::newRow("list")<<QString("list");
 }
 void deletionDuringBatchCompletionExcludesTheRemovedUrl() {
  QFETCH(QString,mode);
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]=mode;doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));
  Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"first.txt","first synthetic file"),fixture(d.path(),"second.txt","second synthetic file")};OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);QTRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);id=result["operationId"].toString();});QTRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();QTRY_COMPARE(provider->completions.size(),1);
  HistoryStore ledger(ops.databasePath());auto first=ledger.operation(id).items.first();QCOMPARE(first.uploadStatus,QString("uploaded"));QJsonObject prepared;history.prepareDelete({first.artifact.id},[&](Error e,QJsonObject result){QVERIFY(!e);prepared=result;});QTRY_VERIFY(!prepared.isEmpty());history.deleteConfirmed({first.artifact.id},prepared["acknowledgement"].toString(),[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(ledger.detail(first.artifact.id)["remoteStatus"].toString(),QString("removed"));provider->completions.takeFirst()();
  QTRY_COMPARE(source.copies,mode=="list"?1:2);QTRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);auto completed=ledger.operation(id);QCOMPARE(source.copied.text,completed.items.last().receipt.directUrl);QCOMPARE(provider->uploads.load(),2);QCOMPARE(provider->deletes.load(),1);
 }
 void knownRemovedHistoryNeverWritesClipboard() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));Source source;
  OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);QTRY_VERIFY(ops.ready());
  HistoryStore store(ops.databasePath());Operation op;op.id="removed-operation";op.created="2026-10-07T00:00:00.000Z";op.settings=settings.snapshot();
  OperationItem item;item.artifact.id="removed-item";item.artifact.name="fixture.txt";item.uploadStatus="uploaded";item.receipt.directUrl="https://example.invalid/original";op.items={item};QVERIFY(!store.saveOperation(op));
  for(const auto& status:QStringList{"removed","already_missing","hidden_version_retained","changed"}) {
   QVERIFY(!store.updateRemote(item.artifact.id,status));bool replied=false;QString code;
   history.copyLink(item.artifact.id,[&](Error e,QJsonObject){code=e.code;replied=true;});QTRY_VERIFY(replied);
   QCOMPARE(source.copies,0);QCOMPARE(code,QString("link_unavailable"));QCOMPARE(store.detail(item.artifact.id)["receipt"].toObject()["directUrl"].toString(),item.receipt.directUrl);
   replied=false;ops.copyResult(op.id,[&](Error,QJsonObject){replied=true;});QTRY_VERIFY(replied);QCOMPARE(source.copies,0);
  }
 }
 void confirmedOverwriteMarksOnlyOriginalObjectHistory() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());doc["naming"]=QJsonObject{{"mode","original"},{"collisionPolicy","overwrite"}};QVERIFY(settings.save(doc,{}));
  Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"same.txt","first synthetic version")};OperationService ops(d.filePath("data"),settings,registry,source);QTRY_VERIFY(ops.ready());
  QString first;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);
  HistoryStore store(ops.databasePath());const auto original=store.operation(first).items[0];const auto originalReceipt=original.receipt.toJson();
  source.payload.files={fixture(d.path(),"same.txt","replacement synthetic version")};QString second;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});QTRY_COMPARE(source.copies,2);
  QCOMPARE(store.detail(original.artifact.id)["remoteStatus"].toString(),QString("superseded"));QCOMPARE(store.detail(original.artifact.id)["receipt"].toObject(),originalReceipt);
  const auto replacement=store.operation(second).items[0];QCOMPARE(store.detail(original.artifact.id)["supersededBy"].toString(),replacement.artifact.id);
  QCOMPARE(store.detail(replacement.artifact.id)["remoteStatus"].toString(),QString("not_requested"));
  // Late copy bookkeeping cannot restore the older entry to its former current state.
  auto late=store.operation(first);late.copyStatus="suppressed";QVERIFY(!store.saveOperation(late));QCOMPARE(store.detail(original.artifact.id)["remoteStatus"].toString(),QString("superseded"));
  QCOMPARE(provider->uploads.load(),2);QCOMPARE(provider->deletes.load(),0);
 }
 void opaqueReceiptDeletionAndDurableProgress() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);QTRY_VERIFY(ops.ready());QString operation;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());const auto uploaded=store.operation(operation);const auto id=uploaded.items[0].artifact.id;auto record=store.detail(id);auto receipt=StorageReceipt::fromJson(record["receipt"].toObject());QVERIFY(receipt.objectReference["fileId"].toString().contains("/%?#"));QVERIFY(!record.contains("bucket"));QVERIFY(!record.contains("key"));
  // Remove the current profile: deletion still uses the recorded destination/reference.
  QVERIFY(settings.save(configuration(d.path(),true),settings.revision()));QJsonObject preview;history.prepareDelete({id},[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});QTRY_VERIFY(!preview.isEmpty());bool denied=false;history.deleteConfirmed({id},"wrong",[&](Error e,QJsonObject){QCOMPARE(e.code,QString("delete_confirmation_changed"));denied=true;});QTRY_VERIFY(denied);QCOMPARE(provider->deletes.load(),0);
  std::atomic_bool persisted{false};provider->checkpointObserver=[&](const DeleteRequest& request) {HistoryStore reopened(ops.databasePath());auto journal=reopened.deletion(request.id);persisted=journal["items"].toArray()[0].toObject()["progress"].toObject()["phase"]=="recorded-intent";};QString deletion;history.deleteConfirmed({id},preview["acknowledgement"].toString(),[&](Error e,QJsonObject r){QVERIFY(!e);deletion=r["deleteOperationId"].toString();});QTRY_VERIFY(!deletion.isEmpty());QTRY_COMPARE(store.detail(id)["remoteStatus"].toString(),QString("removed"));QVERIFY(persisted.load());QCOMPARE(provider->deletes.load(),1);QCOMPARE(provider->uploads.load(),1);QCOMPARE(store.detail(id)["receipt"].toObject(),receipt.toJson());QVERIFY(!store.detail(id).isEmpty());
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
  QTRY_VERIFY(operations.ready());
  QString operationId;
  operations.invoke("clipboard",[&](Error error,QJsonObject result) {QVERIFY(!error);operationId=result["operationId"].toString();});
  QTRY_COMPARE(source.copies,1);
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
  QTRY_VERIFY(!preview.isEmpty());
  QString deletionId;
  history.deleteConfirmed({item.artifact.id},preview["acknowledgement"].toString(),[&](Error error,QJsonObject result) {QVERIFY(!error);deletionId=result["deleteOperationId"].toString();});
  QTRY_VERIFY(!deletionId.isEmpty());
  QTRY_COMPARE(store.deletion(deletionId)["state"].toString(),QString("completed"));
  const auto failed=store.deletion(deletionId)["items"].toArray()[0].toObject();
  QCOMPARE(failed["status"].toString(),QString("failed"));
  QVERIFY(!failed["error"].toObject()["code"].toString().isEmpty());
  QCOMPARE(provider->deletes.load(),0);
  // The provider returns no new progress after the failed acknowledgement.
  // History must retain only its last committed checkpoint.
  QCOMPARE(failed["progress"].toObject(),QJsonObject({{"schemaVersion",1}}));
 }
 void partialDeletionRetainsIdentityAndHistoryClearNeverCallsProvider() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"a.txt","fixture a"),fixture(d.path(),"b.txt","fixture b")};QString deletion,unknownId;QStringList ids;
  {OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);QTRY_VERIFY(ops.ready());QString operation;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});QTRY_COMPARE(source.copies,2);QTRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(ops.databasePath());auto op=store.operation(operation);for(const auto& item:op.items)ids.append(item.artifact.id);unknownId=ids[1];auto fileId=op.items[1].receipt.objectReference["fileId"].toString();provider->deletionResults[fileId]="unknown";QJsonObject preview;history.prepareDelete(ids,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});QTRY_VERIFY(!preview.isEmpty());history.deleteConfirmed(ids,preview["acknowledgement"].toString(),[&](Error e,QJsonObject r){QVERIFY(!e);deletion=r["deleteOperationId"].toString();});QTRY_VERIFY(!deletion.isEmpty());QTRY_COMPARE(store.detail(unknownId)["remoteStatus"].toString(),QString("unknown"));QCOMPARE(store.detail(ids[0])["remoteStatus"].toString(),QString("removed"));QVERIFY(!store.clear());QVERIFY(store.detail(ids[0]).isEmpty());QVERIFY(!store.detail(unknownId).isEmpty());QCOMPARE(provider->deletes.load(),2);}
  provider->deletionResults.clear();OperationService restarted(d.filePath("data"),settings,registry,source);HistoryService history(restarted);QTRY_VERIFY(restarted.ready());QCOMPARE(provider->deletes.load(),2);bool accepted=false;history.retryDelete(deletion,[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});QTRY_VERIFY(accepted);HistoryStore store(restarted.databasePath());QTRY_COMPARE(store.detail(unknownId)["remoteStatus"].toString(),QString("removed"));QCOMPARE(provider->deletes.load(),3);QCOMPARE(provider->uploads.load(),2);
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
  QTRY_VERIFY(operations.ready());
  QString operationId;
  operations.invoke("clipboard",[&](Error error,QJsonObject result) {QVERIFY(!error);operationId=result["operationId"].toString();});
  QTRY_COMPARE(source.copies,1);
  HistoryStore store(operations.databasePath());
  const auto item=store.operation(operationId).items[0];
  QJsonObject preview;
  history.prepareDelete({item.artifact.id},[&](Error error,QJsonObject result) {QVERIFY(!error);preview=result;});
  QTRY_VERIFY(!preview.isEmpty());
  provider->wrongStatDestination=true;
  QString deletionId;
  history.deleteConfirmed({item.artifact.id},preview["acknowledgement"].toString(),[&](Error error,QJsonObject result) {QVERIFY(!error);deletionId=result["deleteOperationId"].toString();});
  QTRY_VERIFY(!deletionId.isEmpty());
  QTRY_COMPARE(store.detail(item.artifact.id)["remoteStatus"].toString(),QString("changed"));
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
  QTRY_VERIFY(operations.ready());
  QString operationId;
  operations.invoke("clipboard",[&](Error error,QJsonObject result) {QVERIFY(!error);operationId=result["operationId"].toString();});
  QTRY_COMPARE(source.copies,1);
  HistoryStore store(operations.databasePath());
  const auto item=store.operation(operationId).items[0];
  const auto objectId=item.receipt.objectReference["fileId"].toString();
  provider->deletionResults[objectId]="unknown";
  QJsonObject preview;
  history.prepareDelete({item.artifact.id},[&](Error error,QJsonObject result) {QVERIFY(!error);preview=result;});
  QTRY_VERIFY(!preview.isEmpty());
  QString deleteId;
  history.deleteConfirmed({item.artifact.id},preview["acknowledgement"].toString(),[&](Error error,QJsonObject result) {QVERIFY(!error);deleteId=result["deleteOperationId"].toString();});
  QTRY_COMPARE(store.detail(item.artifact.id)["remoteStatus"].toString(),QString("unknown"));
  QTRY_VERIFY(!store.deletion(deleteId)["items"].toArray()[0].toObject()["progress"].toObject()["phase"].toString().isEmpty());
  QTest::qWait(25);
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
  QTRY_VERIFY(accepted);
  QTRY_COMPARE(store.detail(item.artifact.id)["remoteStatus"].toString(),QString("changed"));
  QCOMPARE(provider->deletes.load(),1);
  QCOMPARE(provider->objects[objectId].publicationId,QString("external-replacement"));
 }
 void shutdownDrainsPendingDeleteBeforeHistoryDestruction() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->slowDelete=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"a.txt","fixture a"),fixture(d.path(),"b.txt","fixture b")};QStringList historyIds;
  {OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());{HistoryService history(operations);QString operation;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});QTRY_COMPARE(source.copies,2);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(operations.databasePath());for(const auto& item:store.operation(operation).items)historyIds.append(item.artifact.id);QCOMPARE(historyIds.size(),2);QJsonObject preview;history.prepareDelete(historyIds,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});QTRY_VERIFY(!preview.isEmpty());history.deleteConfirmed(historyIds,preview["acknowledgement"].toString(),[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->deletes.load(),1);}HistoryStore store(operations.databasePath());QCOMPARE(store.detail(historyIds[0])["remoteStatus"].toString(),QString("removed"));QCOMPARE(store.detail(historyIds[1])["remoteStatus"].toString(),QString("pending"));}
  QTest::qWait(25);QCOMPARE(provider->uploads.load(),2);QCOMPARE(provider->deletes.load(),1);
 }
 void changedAndUnsupportedEntriesCannotDelete() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);QTRY_VERIFY(ops.ready());QString operation;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto item=store.operation(operation).items[0];auto id=item.artifact.id;provider->allowDelete=false;QString code;history.prepareDelete({id},[&](Error e,QJsonObject){code=e.code;});QTRY_COMPARE(code,QString("unsupported_capability"));provider->allowDelete=true;QJsonObject preview;history.prepareDelete({id},[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});QTRY_VERIFY(!preview.isEmpty());{QMutexLocker lock(&provider->mutex);auto replacement=item.receipt;replacement.publicationId="foreign";provider->objects[item.receipt.objectReference["fileId"].toString()]=replacement;}bool accepted=false;history.deleteConfirmed({id},preview["acknowledgement"].toString(),[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});QTRY_VERIFY(accepted);QTRY_COMPARE(store.detail(id)["remoteStatus"].toString(),QString("changed"));QCOMPARE(provider->deletes.load(),0);
 }
};
QTEST_GUILESS_MAIN(StorageContract)
#include "test_storage_contract.moc"
