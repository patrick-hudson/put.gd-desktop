#include "storage/proton/ProtonProvider.h"
#include "core/HistoryStore.h"
#include <QTemporaryDir>
#include <QTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QLockFile>
#include <QCryptographicHash>
#include <QUuid>
#include "fixtures/U2Fixtures.h"
#include "app/LocalServer.h"
using namespace putgd;
class SharingFixture final:public StorageProvider {
public:
 putgd::test::OpaqueProvider storage;std::atomic_int shares{0},inspections{0};bool privateDefault=false,loseShareReply=false,inspectPrivate=false,changedShareIdentity=false;
 std::function<void(const ShareRequest&)> beforeCompletion;QList<QJsonObject> createOptions;
 ProviderCapabilities capabilities(const Destination& d)const override{auto c=storage.capabilities(d);c.sharing=true;c.privateUploads=true;return c;}
 QString defaultPublicationMode(const Destination&)const override{return privateDefault?"private":"share";}
 QString serializationKey(const Destination& d,const QJsonObject& o,const QString& n)const override{return storage.serializationKey(d,o,n);}
 Error validateSettings(const QJsonObject& s)const override{return storage.validateSettings(s);}
 void checkName(const Destination& d,const QString& n,std::function<void(NameCheck)> cb)override{storage.checkName(d,n,cb);}
 void upload(const UploadRequest& r,std::function<void(UploadResult)> cb)override{storage.upload(r,[cb](UploadResult result){result.receipt.directUrl.clear();cb(result);});}
 void stat(const StorageReceipt& r,std::function<void(StatResult)> cb)override{storage.stat(r,cb);}
 void reconcile(const UploadRequest& r,std::function<void(StatResult)> cb)override{storage.reconcile(r,cb);}
 void remove(const DeleteRequest& r,DurableCheckpoint d,std::function<void(DeleteResult)> cb)override{storage.remove(r,d,cb);}
 void share(const ShareRequest& r,DurableCheckpoint durable,std::function<void(ShareResult)> cb)override{
  const auto complete=[this,r,cb](Error e){if(e){cb({"failed",r.receipt,{},e});return;}auto receipt=r.receipt;if(r.action=="inspect"){++inspections;receipt.directUrl=inspectPrivate?QString{}:QString("https://example.invalid/recorded#secret");cb({inspectPrivate?"private":"shared",receipt,{}, {}});return;}++shares;if(r.action=="create")createOptions.append(r.options);if(r.action=="create")receipt.directUrl="https://example.invalid/recorded#secret";else receipt.directUrl.clear();if(changedShareIdentity)receipt.concurrencyToken="different-revision";if(beforeCompletion)beforeCompletion(r);cb({loseShareReply?"unknown":r.action=="create"?"shared":"revoked",receipt,{}, {}});};
  if(r.action=="inspect")complete({});else durable({{"schemaVersion",1},{"phase","intent"}},complete);
 }
};
class ProtonTest : public QObject {
 Q_OBJECT
 static Operation uploaded() {
  Operation op;op.id="operation";op.created="2026-10-13T00:00:00Z";
  OperationItem item;item.artifact.id="item";item.artifact.name="sample.txt";item.uploadStatus="uploaded";
  item.receipt.destination={"proton-drive","profile","revision",ProtonProvider::defaults(),{{"mode","keychain"},{"reference","putgd/proton/profile"}}};
  item.receipt.objectReference={{"nodeUid","synthetic-node"},{"revisionUid","synthetic-revision"}};item.receipt.publicationId="publication";item.receipt.concurrencyToken="synthetic-revision";item.receipt.directUrl="https://drive.proton.me/urls/synthetic#access";item.shareStatus="shared";op.items={item};return op;
 }
 static bool sql(const QString& path,const QString& statement) {
  const auto connection=QUuid::createUuid().toString();bool ok=false;
  {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);if(db.open()){QSqlQuery q(db);ok=q.exec(statement);}db.close();}
  QSqlDatabase::removeDatabase(connection);return ok;
 }
private slots:
 void credentialModesAreValidatedByProvider() {
  ProviderRegistry registry;registry.add("proton-drive",std::make_shared<ProtonProvider>());registry.add("opaque",std::make_shared<SharingFixture>());
  QTemporaryDir tmp;auto doc=putgd::test::configuration(tmp.path());
  auto profile=doc["profiles"].toArray().first().toObject();profile["provider"]="proton-drive";profile["settings"]=ProtonProvider::defaults();doc["profiles"]=QJsonArray{profile};
  SettingsService settings(tmp.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);},nullptr,[&](auto id,auto value){return registry.validateCredentials(id,value);});
  QCOMPARE(settings.save(doc,{}).error.code,QString("config_invalid"));
  putgd::test::Source source;OperationService operations(tmp.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);Error validation;
  server.dispatch({{"schemaVersion",1},{"requestId","synthetic"},{"session",server.socketPath()},{"method","settings.validate"},{"params",QJsonObject{{"document",doc}}}},[&](Error e,QJsonObject){validation=e;});QCOMPARE(validation.code,QString("config_invalid"));
  profile["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/proton/synthetic"}};doc["profiles"]=QJsonArray{profile};QVERIFY(settings.save(doc,{}));
  doc=putgd::test::configuration(tmp.path());QVERIFY(settings.save(doc,settings.revision()));
 }
 void missingShareAfterUncertainCreateRetainsIntent() {
  QTemporaryDir tmp;ProviderRegistry registry;auto provider=std::make_shared<SharingFixture>();provider->loseShareReply=true;provider->inspectPrivate=true;registry.add("opaque",provider);
  SettingsService settings(tmp.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(putgd::test::configuration(tmp.path()),{}));putgd::test::Source source;source.payload=putgd::test::textPayload();OperationService operations(tmp.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject d){QVERIFY(!e);id=d["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(operations.databasePath());
  const auto original=store.operation(id).items.first().receipt;QCOMPARE(store.operation(id).items.first().shareIntent,QString("create"));
  bool retried=false;operations.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});QTRY_VERIFY(retried);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(store.operation(id).items.first().shareStatus,QString("private"));QCOMPARE(store.operation(id).items.first().shareIntent,QString("create"));QCOMPARE(store.operation(id).state,QString("partial"));QCOMPARE(provider->shares.load(),1);QCOMPARE(source.copies,0);
  provider->loseShareReply=false;retried=false;operations.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});QTRY_VERIFY(retried);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  const auto item=store.operation(id).items.first();QCOMPARE(item.shareStatus,QString("shared"));QVERIFY(item.shareIntent.isEmpty());QCOMPARE(item.receipt.objectReference,original.objectReference);QCOMPARE(item.receipt.concurrencyToken,original.concurrencyToken);QCOMPARE(provider->shares.load(),2);QCOMPARE(provider->inspections.load(),1);QCOMPARE(provider->storage.uploads.load(),1);
 }
 void journalFailureStopsPollingAndCanBeInspected_data() {QTest::addColumn<bool>("runningFailure");QTest::newRow("running-transition")<<true;QTest::newRow("terminal-write")<<false;}
 void journalFailureStopsPollingAndCanBeInspected() {
  QFETCH(bool,runningFailure);QTemporaryDir tmp;ProviderRegistry registry;auto provider=std::make_shared<SharingFixture>();provider->privateDefault=true;registry.add("opaque",provider);
  SettingsService settings(tmp.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(putgd::test::configuration(tmp.path()),{}));putgd::test::Source source;source.payload=putgd::test::textPayload();OperationService operations(tmp.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject d){QVERIFY(!e);id=d["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(operations.databasePath());const auto item=store.operation(id).items.first();
  const auto path=operations.databasePath();const QString fail="CREATE TRIGGER fail_share BEFORE UPDATE ON sharing_operations BEGIN SELECT RAISE(FAIL,'synthetic journal failure'); END";
  if(!runningFailure)provider->beforeCompletion=[path,fail](const ShareRequest&){QVERIFY(sql(path,fail));};
  HistoryService history(operations);QString shareId;history.changeSharing(item.artifact.id,"create",{},[&](Error e,QJsonObject r){QVERIFY(!e);shareId=r["shareOperationId"].toString();if(runningFailure)QVERIFY(sql(path,fail));});QTRY_VERIFY(!shareId.isEmpty());
  QJsonObject stopped;QTRY_VERIFY(([&]{history.inspectSharing(shareId,[&](Error e,QJsonObject d){QVERIFY(!e);stopped=d;});QTest::qWait(10);return stopped["state"]=="interrupted";})());
  QCOMPARE(stopped["status"].toString(),QString("unknown"));QCOMPARE(stopped["error"].toObject()["code"].toString(),QString("history_io"));QVERIFY(stopped["receipt"].toObject()["directUrl"].toString().isEmpty());QCOMPARE(provider->shares.load(),runningFailure?0:1);QVERIFY(!store.detail(item.artifact.id)["linkAvailable"].toBool());
  QVERIFY(sql(path,"DROP TRIGGER fail_share"));provider->beforeCompletion={};QString inspectId;history.changeSharing(item.artifact.id,"inspect",{},[&](Error e,QJsonObject d){QVERIFY(!e);inspectId=d["shareOperationId"].toString();});QTRY_VERIFY(!inspectId.isEmpty());QTRY_COMPARE(store.sharing(inspectId)["state"].toString(),QString("completed"));
  QCOMPARE(store.sharing(shareId)["state"].toString(),QString("interrupted"));QCOMPARE(provider->shares.load(),runningFailure?0:1);QCOMPARE(provider->inspections.load(),1);QCOMPARE(provider->storage.uploads.load(),1);
 }
 void expiredPreviouslySharedLinkIsNotRecreatedByRetry() {
  QTemporaryDir tmp;ProviderRegistry registry;auto provider=std::make_shared<SharingFixture>();registry.add("opaque",provider);
  SettingsService settings(tmp.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(putgd::test::configuration(tmp.path()),{}));putgd::test::Source source;source.payload=putgd::test::textPayload();OperationService operations(tmp.filePath("data"),settings,registry,source);HistoryService history(operations);QTRY_VERIFY(operations.ready());
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject d){QVERIFY(!e);id=d["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(operations.databasePath());const auto item=store.operation(id).items.first();QCOMPARE(item.shareStatus,QString("shared"));
  provider->inspectPrivate=true;QString inspectId;history.changeSharing(item.artifact.id,"inspect",{},[&](Error e,QJsonObject d){QVERIFY(!e);inspectId=d["shareOperationId"].toString();});QTRY_VERIFY(!inspectId.isEmpty());QTRY_COMPARE(store.sharing(inspectId)["status"].toString(),QString("private"));QVERIFY(store.operation(id).items.first().shareIntent.isEmpty());
  bool retried=false;operations.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});QTRY_VERIFY(retried);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(provider->shares.load(),1);QCOMPARE(provider->storage.uploads.load(),1);QVERIFY(!store.detail(item.artifact.id)["linkAvailable"].toBool());
 }
 void retryPreservesExplicitLinkProtectionOptions() {
  QTemporaryDir tmp;ProviderRegistry registry;auto provider=std::make_shared<SharingFixture>();provider->privateDefault=true;registry.add("opaque",provider);
  SettingsService settings(tmp.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(putgd::test::configuration(tmp.path()),{}));putgd::test::Source source;source.payload=putgd::test::textPayload();OperationService operations(tmp.filePath("data"),settings,registry,source);HistoryService history(operations);QTRY_VERIFY(operations.ready());
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject d){QVERIFY(!e);id=d["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(operations.databasePath());const auto item=store.operation(id).items.first();
  provider->loseShareReply=true;const QJsonObject options{{"expirationDays",2},{"passwordReference","synthetic-password-reference"}};QString shareId;history.changeSharing(item.artifact.id,"create",options,[&](Error e,QJsonObject d){QVERIFY(!e);shareId=d["shareOperationId"].toString();});QTRY_VERIFY(!shareId.isEmpty());QTRY_COMPARE(store.sharing(shareId)["status"].toString(),QString("unknown"));
  provider->inspectPrivate=true;QString inspectId;history.changeSharing(item.artifact.id,"inspect",{},[&](Error e,QJsonObject d){QVERIFY(!e);inspectId=d["shareOperationId"].toString();});QTRY_VERIFY(!inspectId.isEmpty());QTRY_COMPARE(store.sharing(inspectId)["status"].toString(),QString("private"));
  QCOMPARE(store.operation(id).items.first().shareIntentOptions,options);provider->loseShareReply=false;bool retried=false;operations.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});QTRY_VERIFY(retried);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(store.operation(id).items.first().shareStatus,QString("shared"));QCOMPARE(provider->createOptions.last(),options);QCOMPARE(provider->storage.uploads.load(),1);QCOMPARE(provider->shares.load(),2);QCOMPARE(provider->inspections.load(),1);
 }
 void automaticSharingJournalFailureCanBeInspected() {
  QTemporaryDir tmp;ProviderRegistry registry;auto provider=std::make_shared<SharingFixture>();registry.add("opaque",provider);
  SettingsService settings(tmp.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(putgd::test::configuration(tmp.path()),{}));putgd::test::Source source;source.payload=putgd::test::textPayload();OperationService operations(tmp.filePath("data"),settings,registry,source);HistoryService history(operations);QTRY_VERIFY(operations.ready());
  const auto path=operations.databasePath();provider->beforeCompletion=[path](const ShareRequest&){QVERIFY(sql(path,"CREATE TRIGGER fail_share BEFORE UPDATE ON sharing_operations BEGIN SELECT RAISE(FAIL,'synthetic journal failure'); END"));};
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject d){QVERIFY(!e);id=d["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(path);const auto detail=store.detail(store.operation(id).items.first().artifact.id);const auto shareId=detail["shareOperationId"].toString();QVERIFY(!shareId.isEmpty());QCOMPARE(detail["shareStatus"].toString(),QString("pending"));QCOMPARE(source.copies,0);
  QJsonObject stopped;history.inspectSharing(shareId,[&](Error e,QJsonObject d){QVERIFY(!e);stopped=d;});QTRY_COMPARE(stopped["state"].toString(),QString("interrupted"));
  QVERIFY(sql(path,"DROP TRIGGER fail_share"));provider->beforeCompletion={};QString inspectId;history.changeSharing(detail["id"].toString(),"inspect",{},[&](Error e,QJsonObject d){QVERIFY(!e);inspectId=d["shareOperationId"].toString();});QTRY_VERIFY(!inspectId.isEmpty());QTRY_COMPARE(store.sharing(inspectId)["status"].toString(),QString("shared"));QCOMPARE(provider->storage.uploads.load(),1);QCOMPARE(provider->shares.load(),1);QCOMPARE(provider->inspections.load(),1);
 }
 void changedShareReceiptIsNeverExposed() {
  QTemporaryDir tmp;ProviderRegistry registry;auto provider=std::make_shared<SharingFixture>();provider->changedShareIdentity=true;registry.add("opaque",provider);
  SettingsService settings(tmp.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(putgd::test::configuration(tmp.path()),{}));putgd::test::Source source;source.payload=putgd::test::textPayload();OperationService operations(tmp.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject d){QVERIFY(!e);id=d["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(operations.databasePath());const auto item=store.operation(id).items.first();
  QCOMPARE(item.shareStatus,QString("unknown"));QVERIFY(item.receipt.directUrl.isEmpty());QCOMPARE(item.receipt.concurrencyToken,provider->storage.objects.value(item.receipt.objectReference["fileId"].toString()).concurrencyToken);QCOMPARE(source.copies,0);
  HistoryService history(operations);QString inspectId;history.changeSharing(item.artifact.id,"inspect",{},[&](Error e,QJsonObject d){QVERIFY(!e);inspectId=d["shareOperationId"].toString();});QTRY_VERIFY(!inspectId.isEmpty());QTRY_COMPARE(store.sharing(inspectId)["status"].toString(),QString("shared"));
  QString shareId;history.changeSharing(item.artifact.id,"create",{},[&](Error e,QJsonObject d){QVERIFY(!e);shareId=d["shareOperationId"].toString();});QTRY_VERIFY(!shareId.isEmpty());QTRY_COMPARE(store.sharing(shareId)["status"].toString(),QString("unknown"));QCOMPARE(store.detail(item.artifact.id)["receipt"].toObject()["concurrencyToken"].toString(),item.receipt.concurrencyToken);QVERIFY(!store.detail(item.artifact.id)["linkAvailable"].toBool());QCOMPARE(provider->storage.uploads.load(),1);
 }
 void stoppedUploadIsDefiniteFailure() {
  ProtonProvider provider;provider.shutdown();UploadRequest request;request.condition="absent";UploadResult result;provider.upload(request,[&](UploadResult r){result=r;});QCOMPARE(result.status,QString("failed"));QCOMPARE(result.error.code,QString("operation_cancelled"));
 }
 void privateUploadSharesLaterWithoutReupload() {
  QTemporaryDir tmp;ProviderRegistry registry;auto provider=std::make_shared<SharingFixture>();provider->privateDefault=true;registry.add("opaque",provider);
  SettingsService settings(tmp.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(putgd::test::configuration(tmp.path()),{}));
  putgd::test::Source source;source.payload=putgd::test::textPayload();OperationService operations(tmp.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  QString operationId;operations.invoke("clipboard",[&](Error e,QJsonObject d){QVERIFY(!e);operationId=d["operationId"].toString();});QTRY_VERIFY(!operationId.isEmpty());QTRY_COMPARE(provider->storage.uploads.load(),1);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(source.copies,0);QCOMPARE(provider->shares.load(),0);HistoryStore store(operations.databasePath());const auto item=store.operation(operationId).items.first();QCOMPARE(item.shareStatus,QString("private"));
  HistoryService history(operations);QString shareId;history.changeSharing(item.artifact.id,"create",{},[&](Error e,QJsonObject r){QVERIFY(!e);shareId=r["shareOperationId"].toString();});QTRY_VERIFY(!shareId.isEmpty());QTRY_COMPARE(store.sharing(shareId)["status"].toString(),QString("shared"));QVERIFY(store.detail(item.artifact.id)["linkAvailable"].toBool());QCOMPARE(provider->storage.uploads.load(),1);
  QString revokeId;history.changeSharing(item.artifact.id,"revoke",{},[&](Error e,QJsonObject r){QVERIFY(!e);revokeId=r["shareOperationId"].toString();});QTRY_VERIFY(!revokeId.isEmpty());QTRY_COMPARE(store.sharing(revokeId)["status"].toString(),QString("revoked"));QVERIFY(!store.detail(item.artifact.id)["linkAvailable"].toBool());
  bool retried=false;operations.retry(operationId,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});QTRY_VERIFY(retried);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(provider->shares.load(),2);QCOMPARE(provider->storage.uploads.load(),1);QVERIFY(!store.detail(item.artifact.id)["linkAvailable"].toBool());
 }
 void uncertainShareInspectsWithoutReuploadOrNewMutation() {
  QTemporaryDir tmp;ProviderRegistry registry;auto provider=std::make_shared<SharingFixture>();provider->loseShareReply=true;registry.add("opaque",provider);
  SettingsService settings(tmp.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(putgd::test::configuration(tmp.path()),{}));putgd::test::Source source;source.payload=putgd::test::textPayload();OperationService operations(tmp.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject d){QVERIFY(!e);id=d["operationId"].toString();});QTRY_VERIFY(!id.isEmpty());QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryStore store(operations.databasePath());QCOMPARE(store.operation(id).items.first().shareStatus,QString("unknown"));QCOMPARE(source.copies,0);
  bool retried=false;operations.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});QTRY_VERIFY(retried);QTRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(provider->storage.uploads.load(),1);QCOMPARE(provider->shares.load(),1);QCOMPARE(provider->inspections.load(),1);QCOMPARE(store.operation(id).items.first().shareStatus,QString("shared"));
 }
 void compiledRuntimeRejectsInvalidInputWithoutAccountAccess() {
#if PUTGD_TEST_PROTON_BUNDLED
  ProtonProvider provider;Destination destination{"proton-drive","synthetic","revision",ProtonProvider::defaults(),{{"mode","keychain"},{"reference","putgd/proton/synthetic-parser-test"}}};Error error;
  provider.setup("storeSharePassword",destination,{{"reference","synthetic-reference"},{"password",""}},error);
  QCOMPARE(error.code,QString("invalid_request"));QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
#else
  QSKIP("This build intentionally excludes the separately provisioned runtime");
#endif
 }
 void shutdownRejectsNewHelperWork() {
  ProtonProvider provider;provider.shutdown();Error error;provider.setup("connect",{}, {},error);QCOMPARE(error.code,QString("operation_cancelled"));QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
 }
 void pureSettingsAndHonestCapabilities() {
  ProtonProvider provider;auto settings=provider.defaults();QVERIFY(!provider.validateSettings(settings));
  settings["password"]="must-not-be-settings";QVERIFY(provider.validateSettings(settings));
  const auto caps=provider.capabilities({});QVERIFY(caps.conditionalCreate);QVERIFY(caps.sharing);QVERIFY(caps.privateUploads);QVERIFY(!caps.conditionalReplace);QVERIFY(!caps.remove);QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
 }
 void serializationIgnoresProfileEditsAndVersions() {
  ProtonProvider provider;Destination a{"proton-drive","a","r1",{{"accountRootUid","account"},{"folderUid","folder"}},{}};
  auto b=a;b.profileId="b";b.profileRevision="r2";
  const QJsonObject ref{{"accountRootUid","account"},{"folderUid","folder"},{"name","file.txt"},{"nodeUid","node"},{"revisionUid","revision"}};
  QCOMPARE(provider.serializationKey(a,{},"file.txt"),provider.serializationKey(b,ref,{}));
 }
 void failedCheckpointPreventsHelperStart() {
  ProtonProvider provider;ShareRequest req;req.id="share";req.action="revoke";req.receipt=uploaded().items.first().receipt;int callbacks=0;
  provider.share(req,[](const QJsonObject&,auto acknowledge){acknowledge({"history_io","Synthetic commit failure",{}});},[&](ShareResult r){++callbacks;QCOMPARE(r.status,QString("failed"));QCOMPARE(r.error.code,QString("history_io"));});
  QCOMPARE(callbacks,1);QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
 }
 void revocationSurvivesOldOperationWrites() {
  QTemporaryDir tmp;HistoryStore store(tmp.filePath("history.sqlite"));auto op=uploaded();QVERIFY(!store.saveOperation(op));
  auto receipt=op.items.first().receipt;receipt.directUrl.clear();
  QJsonObject doc{{"id","share"},{"historyId","item"},{"action","revoke"},{"state","completed"},{"status","revoked"},{"receipt",receipt.toJson()},{"progress",QJsonObject{{"schemaVersion",1}}}};
  QVERIFY(!store.saveSharing(doc));QCOMPARE(StorageReceipt::fromJson(store.detail("item")["receipt"].toObject()).destination.toJson(),receipt.destination.toJson());QVERIFY(!store.saveOperation(op));
  auto item=store.detail("item");QVERIFY(!item["linkAvailable"].toBool());QCOMPARE(item["receipt"].toObject()["directUrl"].toString(),QString{});
  QVERIFY(!store.operation("operation").items.first().linkAvailable());
  QVERIFY(!store.recent(10).first().toObject()["items"].toArray().first().toObject()["receipt"].toObject()["directUrl"].toString().size());
 }
 void interruptedSharingRequiresInspection() {
  QTemporaryDir tmp;HistoryStore store(tmp.filePath("history.sqlite"));auto op=uploaded();QVERIFY(!store.saveOperation(op));
  QJsonObject doc{{"id","share"},{"historyId","item"},{"action","revoke"},{"state","running"},{"status","pending"},{"receipt",op.items.first().receipt.toJson()},{"progress",QJsonObject{{"schemaVersion",1}}}};
  QVERIFY(!store.saveSharing(doc));store.recover();
  QCOMPARE(store.sharing("share")["status"].toString(),QString("unknown"));QVERIFY(!store.detail("item")["linkAvailable"].toBool());
  QVERIFY(!store.clear());QCOMPARE(store.list({}).total,1);
 }
 void publicationOverridesRoundTrip() {
  auto doc=Settings::defaults();auto actions=doc["actions"].toObject();auto action=actions["clipboard"].toObject();action["publicationMode"]="private";actions["clipboard"]=action;doc["actions"]=actions;
  QVERIFY(!Settings::validate(doc));QCOMPARE(Settings::fromValidated(doc).actions["clipboard"].publicationMode,QString("private"));
 }
};
QTEST_GUILESS_MAIN(ProtonTest)
#include "test_proton.moc"
