#include "fixtures/U2Fixtures.h"
using namespace putgd;using namespace putgd::test;
class StorageContract:public QObject {
 Q_OBJECT
private slots:
 void opaqueReceiptDeletionAndDurableProgress() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);QTRY_VERIFY(ops.ready());QString operation;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());const auto uploaded=store.operation(operation);const auto id=uploaded.items[0].artifact.id;auto record=store.detail(id);auto receipt=StorageReceipt::fromJson(record["receipt"].toObject());QVERIFY(receipt.objectReference["fileId"].toString().contains("/%?#"));QVERIFY(!record.contains("bucket"));QVERIFY(!record.contains("key"));
  // Remove the current profile: deletion still uses the recorded destination/reference.
  QVERIFY(settings.save(configuration(d.path(),true),settings.revision()));QJsonObject preview;history.prepareDelete({id},[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});QTRY_VERIFY(!preview.isEmpty());bool denied=false;history.deleteConfirmed({id},"wrong",[&](Error e,QJsonObject){QCOMPARE(e.code,QString("delete_confirmation_changed"));denied=true;});QTRY_VERIFY(denied);QCOMPARE(provider->deletes.load(),0);
  std::atomic_bool persisted{false};provider->checkpointObserver=[&](const DeleteRequest& request) {HistoryStore reopened(ops.databasePath());auto journal=reopened.deletion(request.id);persisted=journal["items"].toArray()[0].toObject()["progress"].toObject()["phase"]=="recorded-intent";};QString deletion;history.deleteConfirmed({id},preview["acknowledgement"].toString(),[&](Error e,QJsonObject r){QVERIFY(!e);deletion=r["deleteOperationId"].toString();});QTRY_VERIFY(!deletion.isEmpty());QTRY_COMPARE(store.detail(id)["remoteStatus"].toString(),QString("removed"));QVERIFY(persisted.load());QCOMPARE(provider->deletes.load(),1);QCOMPARE(provider->uploads.load(),1);QCOMPARE(store.detail(id)["receipt"].toObject(),receipt.toJson());QVERIFY(!store.detail(id).isEmpty());
 }
 void partialDeletionRetainsIdentityAndHistoryClearNeverCallsProvider() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"a.txt","fixture a"),fixture(d.path(),"b.txt","fixture b")};QString deletion,unknownId;QStringList ids;
  {OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);QTRY_VERIFY(ops.ready());QString operation;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto op=store.operation(operation);for(const auto& item:op.items)ids.append(item.artifact.id);unknownId=ids[1];auto fileId=op.items[1].receipt.objectReference["fileId"].toString();provider->deletionResults[fileId]="unknown";QJsonObject preview;history.prepareDelete(ids,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});QTRY_VERIFY(!preview.isEmpty());history.deleteConfirmed(ids,preview["acknowledgement"].toString(),[&](Error e,QJsonObject r){QVERIFY(!e);deletion=r["deleteOperationId"].toString();});QTRY_VERIFY(!deletion.isEmpty());QTRY_COMPARE(store.detail(unknownId)["remoteStatus"].toString(),QString("unknown"));QCOMPARE(store.detail(ids[0])["remoteStatus"].toString(),QString("removed"));QVERIFY(!store.clear());QVERIFY(store.detail(ids[0]).isEmpty());QVERIFY(!store.detail(unknownId).isEmpty());QCOMPARE(provider->deletes.load(),2);}
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
  {OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());{HistoryService history(operations);QString operation;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);HistoryStore store(operations.databasePath());for(const auto& item:store.operation(operation).items)historyIds.append(item.artifact.id);QCOMPARE(historyIds.size(),2);QJsonObject preview;history.prepareDelete(historyIds,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});QTRY_VERIFY(!preview.isEmpty());history.deleteConfirmed(historyIds,preview["acknowledgement"].toString(),[](Error e,QJsonObject){QVERIFY(!e);});QTRY_COMPARE(provider->deletes.load(),1);}HistoryStore store(operations.databasePath());QCOMPARE(store.detail(historyIds[0])["remoteStatus"].toString(),QString("removed"));QCOMPARE(store.detail(historyIds[1])["remoteStatus"].toString(),QString("pending"));}
  QTest::qWait(25);QCOMPARE(provider->uploads.load(),2);QCOMPARE(provider->deletes.load(),1);
 }
 void changedAndUnsupportedEntriesCannotDelete() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);HistoryService history(ops);QTRY_VERIFY(ops.ready());QString operation;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);operation=r["operationId"].toString();});QTRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto item=store.operation(operation).items[0];auto id=item.artifact.id;provider->allowDelete=false;QString code;history.prepareDelete({id},[&](Error e,QJsonObject){code=e.code;});QTRY_COMPARE(code,QString("unsupported_capability"));provider->allowDelete=true;QJsonObject preview;history.prepareDelete({id},[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});QTRY_VERIFY(!preview.isEmpty());{QMutexLocker lock(&provider->mutex);auto replacement=item.receipt;replacement.publicationId="foreign";provider->objects[item.receipt.objectReference["fileId"].toString()]=replacement;}bool accepted=false;history.deleteConfirmed({id},preview["acknowledgement"].toString(),[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});QTRY_VERIFY(accepted);QTRY_COMPARE(store.detail(id)["remoteStatus"].toString(),QString("changed"));QCOMPARE(provider->deletes.load(),0);
 }
};
QTEST_GUILESS_MAIN(StorageContract)
#include "test_storage_contract.moc"
