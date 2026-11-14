#include "core/HistoryStore.h"
#include "fixtures/U2Fixtures.h"
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QJsonDocument>
#include <QTest>
#include <QSet>
#include <QElapsedTimer>
using namespace putgd;
class HistoryTest:public QObject {
 Q_OBJECT
private slots:
 void expiredLinkRefreshPreservesIdentityAndSurvivesReopen() {
  QTemporaryDir d;const auto path=d.filePath("history.sqlite");HistoryStore store(path);
  Operation op;op.id="expiring";op.created="2026-11-09T00:00:00.000Z";op.restore(OperationState::Completed);op.settings=Settings::fromValidated(Settings::defaults());
  OperationItem item;item.artifact.id="expired-item";item.artifact.name="synthetic.txt";item.uploadStatus="uploaded";
  auto& receipt=item.receipt;receipt.destination={"opaque","original","original-revision",{{"opaque","saved"}},{{"mode","keychain"},{"reference","original-reference"}}};
  receipt.objectReference={{"schemaVersion",1},{"opaque","object-id"},{"linkExpiresAt","2000-01-01T00:00:00.000Z"}};
  receipt.publicationId="publication";receipt.concurrencyToken="token";receipt.directUrl="https://example.invalid/old?token=synthetic";op.items={item};
  QVERIFY(!store.saveOperation(op));QVERIFY(!store.detail(item.artifact.id)["linkAvailable"].toBool());QVERIFY(store.detail(item.artifact.id)["linkRefreshable"].toBool());
  auto renewed=receipt;renewed.directUrl="https://example.invalid/fresh?token=synthetic";renewed.objectReference["linkExpiresAt"]="2099-01-01T00:00:00.000Z";
  auto wrong=renewed;wrong.destination.profileId="current";QCOMPARE(store.refreshLink(item.artifact.id,receipt,wrong).code,QString("link_identity_changed"));
  QVERIFY(!store.refreshLink(item.artifact.id,receipt,renewed));
  HistoryStore reopened(path);QCOMPARE(reopened.detail(item.artifact.id)["receipt"].toObject(),renewed.toJson());QVERIFY(reopened.detail(item.artifact.id)["linkAvailable"].toBool());QCOMPARE(reopened.operation(op.id).items[0].receipt.toJson(),renewed.toJson());
  QCOMPARE(store.refreshLink(item.artifact.id,receipt,renewed).code,QString("link_changed"));
  QVERIFY(!store.updateRemote(item.artifact.id,"removed"));QVERIFY(!store.detail(item.artifact.id)["linkRefreshable"].toBool());
  item.artifact.id="public-item";item.receipt.objectReference.remove("linkExpiresAt");op.items={item};QVERIFY(!store.saveOperation(op));const auto publicLink=store.detail(item.artifact.id);QVERIFY(publicLink.contains("linkRefreshable"));QCOMPARE(publicLink["linkRefreshable"].toBool(),false);
 }

 void remoteMetadataReplacementDropsStaleNestedFields() {
  QTemporaryDir d;HistoryStore store(d.filePath("history.sqlite"));Operation op;op.id="metadata-operation";OperationItem item;item.artifact.id="metadata-item";item.uploadStatus="uploaded";op.items={item};QVERIFY(!store.saveOperation(op));
  auto deletion=[&](QString id,QString status,QJsonObject error,QJsonObject progress) {return QJsonObject{{"id",id},{"state","completed"},{"items",QJsonArray{QJsonObject{{"historyId",item.artifact.id},{"status",status},{"error",error},{"progress",progress}}}}};};
  QVERIFY(!store.saveDeletion(deletion("failed-delete","failed",Error{"synthetic","Failed",{{"stale","old"}}}.toJson(),{{"oldMarker","old"}})));
  QVERIFY(!store.saveDeletion(deletion("successful-delete","removed",Error{}.toJson(),{{"phase","done"}})));
  const auto detail=store.detail(item.artifact.id);QCOMPARE(detail["remoteError"].toObject(),Error{}.toJson());QCOMPARE(detail["deleteProgress"].toObject(),QJsonObject({{"phase","done"}}));
  QVERIFY(!store.saveDeletion({{"id","pending-delete"},{"state","accepted"},{"items",QJsonArray{QJsonObject{{"historyId",item.artifact.id},{"status","pending"}}}}}));QVERIFY(!store.detail(item.artifact.id).contains("remoteError"));QVERIFY(!store.detail(item.artifact.id).contains("deleteProgress"));
 }
 void largeClearedLegacyMigrationExpandsDeletionDocumentsOnce() {
  QTemporaryDir d;const auto path=d.filePath("history.sqlite");Operation op;op.id="bulk-legacy";
  for(int i=0;i<1000;++i){OperationItem item;item.artifact.id=QString("legacy-item-%1").arg(i);item.uploadStatus="uploaded";item.receipt.directUrl="https://example.invalid/deleted";op.items.append(item);}
  {HistoryStore store(path);QVERIFY(!store.saveOperation(op));QVERIFY(!store.clear());}
  const auto connection=QString("large-schema-four");{auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);QVERIFY(db.open());QVERIFY(db.transaction());QSqlQuery q(db);QVERIFY(q.exec("DROP TABLE item_state"));QVERIFY(q.exec("PRAGMA user_version=4"));
   for(int doc=0;doc<200;++doc){QJsonArray targets;for(int target=0;target<100;++target){const auto id=doc<10?QString("legacy-item-%1").arg(doc*100+target):QString("other-%1-%2").arg(doc).arg(target);targets.append(QJsonObject{{"historyId",id},{"status","removed"},{"progress",QJsonObject{{"padding",QString(512,'x')}}}});}q.prepare("INSERT INTO deletions VALUES(?,?)");q.addBindValue(QString("legacy-delete-%1").arg(doc));q.addBindValue(QJsonDocument(QJsonObject{{"items",targets}}).toJson(QJsonDocument::Compact));QVERIFY(q.exec());}QVERIFY(db.commit());db.close();}QSqlDatabase::removeDatabase(connection);
  QElapsedTimer timer;timer.start();HistoryStore upgraded(path);QVERIFY(!upgraded.error());const auto elapsed=timer.elapsed();qInfo()<<"Legacy migration: 1000 cleared items, 200 documents, 20000 targets; elapsed ms"<<elapsed;QVERIFY2(elapsed<10000,qPrintable(QString("Legacy migration took %1 ms").arg(elapsed)));
  const auto restored=upgraded.operation(op.id);QCOMPARE(restored.items.size(),1000);for(const auto& item:restored.items){QCOMPARE(item.remoteStatus,QString("removed"));QVERIFY(!item.linkAvailable());}QCOMPARE(upgraded.list({}).total,0);
 }
 void changedItemCheckpointPreservesRemoteStateAndRollsBackAsAUnit() {
  QTemporaryDir d;const auto path=d.filePath("history.sqlite");HistoryStore store(path);Operation op;op.id="checkpoint-operation";op.created="2020-01-01T00:00:00.000Z";
  for(int i=0;i<2;++i){OperationItem item;item.artifact.id=QString("checkpoint-item-%1").arg(i);item.artifact.name=QString("item-%1.txt").arg(i);item.uploadStatus="uploaded";item.receipt.directUrl="https://example.invalid/synthetic";op.items.append(item);}QVERIFY(!store.saveOperation(op));QVERIFY(!store.updateRemote(op.items[0].artifact.id,"removed"));
  op.restore(OperationState::Running);op.items[0].local.status="saved";op.items[1].uploadStatus="failed";QVERIFY(!store.checkpointOperation(op,{op.items[0].artifact.id}));QCOMPARE(store.detail(op.items[0].artifact.id)["remoteStatus"].toString(),QString("removed"));QCOMPARE(store.detail(op.items[1].artifact.id)["uploadStatus"].toString(),QString("uploaded"));QCOMPARE(store.operation(op.id).items[1].uploadStatus,QString("failed"));
  const auto connection=QUuid::createUuid().toString();{
   auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);QVERIFY(db.open());QSqlQuery q(db);QVERIFY(q.exec("CREATE TRIGGER refuse_checkpoint BEFORE UPDATE ON history BEGIN SELECT RAISE(ABORT,'synthetic checkpoint failure'); END"));
   op.restore(OperationState::Completed);op.items[1].uploadStatus="local_only";QCOMPARE(store.checkpointOperation(op,{op.items[1].artifact.id}).code,QString("history_io"));QCOMPARE(store.operation(op.id).stateName(),QString("running"));QCOMPARE(store.detail(op.items[1].artifact.id)["uploadStatus"].toString(),QString("uploaded"));db.close();
  }QSqlDatabase::removeDatabase(connection);
 }
 void remoteOutcomesHydrateRecentAndSurviveHistoryCleanup() {
  for(bool expire:{false,true}) {
   QTemporaryDir d;const auto path=d.filePath("history.sqlite");Operation op;op.id="op";op.restore(OperationState::Completed);op.created="2000-01-01T00:00:00.000Z";op.settings=Settings::fromValidated(Settings::defaults());
   OperationItem item;item.artifact.id="item";item.artifact.name="synthetic.txt";item.uploadStatus="uploaded";item.receipt.directUrl="https://example.invalid/removed";op.items={item};
   {HistoryStore store(path);QVERIFY(!store.saveOperation(op));
    QJsonObject deletion{{"id","delete"},{"state","completed"},{"items",QJsonArray{QJsonObject{{"historyId","item"},{"status","removed"},{"progress",QJsonObject{{"phase","done"}}}}}}};
    QVERIFY(!store.saveDeletion(deletion));QCOMPARE(store.operation("op").items[0].remoteStatus,QString("removed"));
    QCOMPARE(store.recent(1)[0].toObject()["items"].toArray()[0].toObject()["remoteStatus"].toString(),QString("removed"));
    QVERIFY(!(expire?store.expireBefore("2026-10-11T00:00:00.000Z"):store.clear()));QCOMPARE(store.list({}).total,0);
    QVERIFY(!store.saveOperation(op));QCOMPARE(store.list({}).total,0);
   }
   HistoryStore reopened(path);QVERIFY(!reopened.error());const auto restored=reopened.operation("op");QCOMPARE(restored.items[0].remoteStatus,QString("removed"));QVERIFY(!restored.items[0].linkAvailable());
  }
 }
 void knownRemovalSurvivesClearAndExpiry() {
  for(bool expire:{false,true}) {
   QTemporaryDir d;const auto path=d.filePath("history.sqlite");Operation op;op.id="op";op.restore(OperationState::Completed);op.created="2000-01-01T00:00:00.000Z";op.settings=Settings::fromValidated(Settings::defaults());OperationItem item;item.artifact.id="item";item.artifact.name="synthetic.txt";item.uploadStatus="uploaded";item.receipt.directUrl="https://example.invalid/removed";op.items={item};
   {HistoryStore store(path);QVERIFY(!store.saveOperation(op));QVERIFY(!store.updateRemote("item","removed"));QVERIFY(!(expire?store.expireBefore("2026-10-11T00:00:00.000Z"):store.clear()));QVERIFY(!store.saveOperation(op));QCOMPARE(store.list({}).total,0);}
   HistoryStore reopened(path);const auto restored=reopened.operation("op");QCOMPARE(restored.items[0].remoteStatus,QString("removed"));QVERIFY(!restored.items[0].linkAvailable());
  }
 }
 void versionFourMigrationPreservesDeletionState() {
  QTemporaryDir d;const auto path=d.filePath("history.sqlite");Operation op;op.id="op";op.restore(OperationState::Completed);op.settings=Settings::fromValidated(Settings::defaults());OperationItem item;item.artifact.id="item";item.artifact.name="synthetic";item.uploadStatus="uploaded";item.receipt.directUrl="https://example.invalid/original";op.items={item};
  {HistoryStore store(path);QVERIFY(!store.saveOperation(op));QVERIFY(!store.updateRemote("item","removed"));}
  const auto connection=QString("schema-four");{auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);QVERIFY(db.open());QSqlQuery q(db);QVERIFY(q.exec("DROP TABLE item_state"));QVERIFY(q.exec("PRAGMA user_version=4"));db.close();}QSqlDatabase::removeDatabase(connection);
  HistoryStore upgraded(path);QVERIFY(!upgraded.error());QVERIFY(!upgraded.clear());QCOMPARE(upgraded.operation("op").items[0].remoteStatus,QString("removed"));QVERIFY(!upgraded.operation("op").items[0].linkAvailable());
 }
 void versionFourClearedDeletionCannotRestoreItsLink() {
  QTemporaryDir d;const auto path=d.filePath("history.sqlite");Operation op;op.id="legacy-cleared";op.restore(OperationState::Completed);OperationItem item;item.artifact.id="legacy-cleared-item";item.uploadStatus="uploaded";item.receipt.directUrl="https://example.invalid/deleted";op.items={item};
  {HistoryStore store(path);QVERIFY(!store.saveOperation(op));QVERIFY(!store.saveDeletion({{"id","legacy-deletion"},{"state","completed"},{"items",QJsonArray{QJsonObject{{"historyId",item.artifact.id},{"status","removed"}}}}}));QVERIFY(!store.clear());}
  const auto connection=QString("cleared-schema-four");{auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);QVERIFY(db.open());QSqlQuery q(db);QVERIFY(q.exec("DROP TABLE item_state"));QVERIFY(q.exec("PRAGMA user_version=4"));db.close();}QSqlDatabase::removeDatabase(connection);
  HistoryStore upgraded(path);QVERIFY(!upgraded.error());QCOMPARE(upgraded.operation(op.id).items[0].remoteStatus,QString("removed"));QVERIFY(!upgraded.operation(op.id).items[0].linkAvailable());QVERIFY(!upgraded.saveOperation(op));QCOMPARE(upgraded.list({}).total,0);QVERIFY(!upgraded.operation(op.id).items[0].linkAvailable());
 }
 void acceptedDeletionRecoversWithoutLosingPendingIdentity() {
  QTemporaryDir d;const auto path=d.filePath("history.sqlite");const QJsonObject item{{"historyId","item"},{"status","pending"},{"receipt",QJsonObject{{"opaque","original"}}},{"progress",QJsonObject{{"phase","accepted"}}}};
  {HistoryStore store(path);QVERIFY(!store.saveDeletion({{"id","accepted-delete"},{"state","accepted"},{"items",QJsonArray{item}}}));}
  HistoryStore reopened(path);Error error;reopened.recover(&error);QVERIFY(!error);const auto deletion=reopened.deletion("accepted-delete");QCOMPARE(deletion["state"].toString(),QString("interrupted"));QCOMPARE(deletion["items"].toArray()[0].toObject(),item);
 }
 void schemaThreeBackfillUsesProviderIdentityAcrossProfilesAndPages() {
  QTemporaryDir d;const auto path=d.filePath("history.sqlite");ProviderRegistry registry;registry.add("opaque",std::make_shared<putgd::test::OpaqueProvider>());
  Operation original;original.id="legacy";original.created="2026-10-07T00:00:00.000Z";original.settings=Settings::fromValidated(Settings::defaults());original.destination={"opaque","original-profile","original-revision",{{"workspace","original-space"}}, {}};
  for(int n=0;n<201;++n) {OperationItem ignored;ignored.artifact.id=QString("a%1").arg(n,3,10,QChar('0'));ignored.artifact.name="synthetic";ignored.uploadStatus="uploaded";original.items.append(ignored);}
  OperationItem item;item.artifact.id="z-original";item.artifact.name="synthetic";item.uploadStatus="uploaded";item.receipt.destination=original.destination;item.receipt.publicationId="first";item.receipt.objectReference={{"fileId","opaque-object"},{"revisionId","immutable-first"}};item.receipt.directUrl="https://example.invalid/original";original.items.append(item);
  {HistoryStore store(path);QVERIFY(!store.saveOperation(original));}
  const auto connection=QString("legacy-three");{auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);QVERIFY(db.open());QSqlQuery q(db);QVERIFY(q.exec("DROP TABLE item_state"));QVERIFY(q.exec("DROP INDEX history_publication"));QVERIFY(q.exec("ALTER TABLE history DROP COLUMN publication_identity"));QVERIFY(q.exec("PRAGMA user_version=3"));db.close();}QSqlDatabase::removeDatabase(connection);
  HistoryStore upgraded(path);QVERIFY(!upgraded.error());QVERIFY(!upgraded.indexPublications(registry));
  Operation replacement=original;replacement.id="replacement";replacement.destination.profileId="alias-profile";replacement.destination.profileRevision="changed-config";replacement.items={item};auto& next=replacement.items[0];next.artifact.id="z-replacement";next.condition="match";next.receipt.destination=replacement.destination;next.receipt.publicationId="second";next.receipt.objectReference["revisionId"]="immutable-second";QVERIFY(!upgraded.saveOperation(replacement,&registry));
  QCOMPARE(upgraded.detail(item.artifact.id)["remoteStatus"].toString(),QString("superseded"));QCOMPARE(upgraded.detail(item.artifact.id)["receipt"].toObject(),item.receipt.toJson());QCOMPARE(upgraded.detail("a000")["remoteStatus"].toString(),QString("not_requested"));
  Operation other=replacement;other.id="other-workspace";other.items[0].artifact.id="other-item";other.items[0].receipt.destination.settings["workspace"]="another-space";other.items[0].receipt.publicationId="third";QVERIFY(!upgraded.saveOperation(other,&registry));QCOMPARE(upgraded.detail(next.artifact.id)["remoteStatus"].toString(),QString("not_requested"));
 }
 void clearedHistoryStaysClearedAndRemoteErrorsSurviveUpdates() {
  QTemporaryDir d;HistoryStore store(d.filePath("history.sqlite"));Operation op;op.id="operation";op.created="2026-10-07T00:00:00.000Z";op.settings=Settings::fromValidated(Settings::defaults());op.destination.profileId="test";OperationItem item;item.artifact.id="item";item.artifact.name="fixture.txt";item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="uploaded";op.items={item};QVERIFY(!store.saveOperation(op));
  QVERIFY(!store.updateRemote("item","failed",{"permission_denied","Recorded deletion was denied",{}}));op.copyStatus="copied";QVERIFY(!store.saveOperation(op));QCOMPARE(store.detail("item")["remoteError"].toObject()["code"].toString(),QString("permission_denied"));QVERIFY(!store.clear());QCOMPARE(store.list({}).total,0);op.copyStatus="suppressed";QVERIFY(!store.saveOperation(op));QCOMPARE(store.list({}).total,0);
 }
 void expiryIsExplicitAndRecentActivityIsBounded() {
  QTemporaryDir d;HistoryStore store(d.filePath("history.sqlite"));Operation op;op.id="operation";op.created="2000-01-01T00:00:00.000Z";op.settings=Settings::fromValidated(Settings::defaults());op.destination.profileId="test";OperationItem item;item.artifact.id="item";item.artifact.name="fixture.txt";item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="uploaded";op.items={item};QVERIFY(!store.saveOperation(op));QCOMPARE(store.list({}).total,1);QCOMPARE(store.recent(1).size(),1);QVERIFY(!store.expireBefore("2026-10-07T00:00:00.000Z"));QCOMPARE(store.list({}).total,0);QCOMPARE(store.recent(1).size(),1);QVERIFY(!store.saveOperation(op));QCOMPARE(store.list({}).total,0);
 }
 // A text recognition (#1428) found mid-job after a crash is marked interrupted
 // and stays so: nothing reads the screen or runs the helper again on its own.
 void interruptedTextRecognitionIsNeverRerun() {
  QTemporaryDir d;const auto path=d.filePath("history.sqlite");
  {HistoryStore store(path);Operation op;op.id="text-op";op.action="text";op.restore(OperationState::Recognizing);op.phase="recognizing";op.settings=Settings::fromValidated(Settings::defaults());QVERIFY(!store.saveOperation(op));}
  HistoryStore reopened(path);Error error;QCOMPARE(reopened.recover(&error).size(),1);QVERIFY(!error);
  const auto saved=reopened.operation("text-op");QCOMPARE(saved.stateName(),QString("interrupted"));QCOMPARE(saved.error.code,QString("text_interrupted"));QCOMPARE(saved.copyStatus,QString("no_result"));
  QVERIFY(!saved.active());
  reopened.recover(&error);QVERIFY(!error);QCOMPARE(reopened.operation("text-op").toJson(),saved.toJson());
 }
 // The journal and wire strings and the one table of state steps (#1433). A step
 // missing from the table leaves an operation stuck; an extra one lets a finished
 // row be rewritten; a changed string misreads every row another build saved.
 void operationStatesAndStepsMatchTheJournal() {
  using S=OperationState;
  const QList<std::pair<S,QString>> states{{S::Accepted,"accepted"},{S::Acquiring,"acquiring"},{S::Editing,"editing"},{S::Recording,"recording"},{S::Recognizing,"recognizing"},{S::Stored,"stored"},
   {S::Running,"running"},{S::Completed,"completed"},{S::Partial,"partial"},{S::Failed,"failed"},{S::Cancelled,"cancelled"},{S::Interrupted,"interrupted"},{S::Unknown,"unknown"}};
  const QSet<QString> steps{"accepted>acquiring","accepted>recording","accepted>cancelled","accepted>failed","accepted>interrupted",
   "acquiring>editing","acquiring>recognizing","acquiring>stored","acquiring>cancelled","acquiring>failed","acquiring>interrupted",
   "editing>acquiring","editing>cancelled","editing>failed","editing>interrupted","recording>acquiring","recording>cancelled","recording>failed","recording>interrupted",
   "recognizing>completed","recognizing>cancelled","recognizing>failed","recognizing>interrupted","stored>running","stored>completed","stored>cancelled","stored>failed","stored>interrupted",
   "running>completed","running>partial","running>cancelled","running>failed","running>interrupted","running>stored","completed>failed","partial>failed","cancelled>failed",
   "completed>stored","partial>stored","failed>stored","cancelled>stored","interrupted>stored","unknown>stored"};
  for(const auto& [state,name]:states) {
   if(state!=S::Unknown){QCOMPARE(operationStateName(state),name);QCOMPARE(operationState(name),state);}
   for(const auto& [to,toName]:states) {
    const bool allowed=steps.contains(name+">"+toName);
    QVERIFY2(operationStep(state,to)==allowed,qPrintable(name+">"+toName));
    Operation op;op.restore(state);const bool moved=op.advance(to);
    QVERIFY2(moved==(allowed||state==to)&&op.state()==(moved?to:state),qPrintable(name+">"+toName));
   }
  }
  // Older builds never wrote "queued" as a state; a string no build writes stays as it is.
  QCOMPARE(operationState("queued"),S::Unknown);
  const auto newer=Operation::fromJson({{"state","newer-state"}});QCOMPARE(newer.state(),S::Unknown);QCOMPARE(newer.toJson()["state"].toString(),QString("newer-state"));QVERIFY(!newer.active());
 }
 void interruptedStatesNeverPublish() {QTemporaryDir d;HistoryStore store(d.filePath("history.sqlite"));Operation op;op.id="op";op.restore(OperationState::Running);op.phase="uploading";op.settings=Settings::fromValidated(Settings::defaults());OperationItem item;item.artifact.id="item";item.uploadStatus="uploading";item.phase="uploading";item.uploadProgress=UploadProgress{"uploading",1024,{}};item.candidate="fixed";item.publicationId="marker";op.items={item};QVERIFY(!store.saveOperation(op));auto recovered=store.recover();QCOMPARE(recovered.size(),1);auto saved=store.operation("op");QCOMPARE(saved.stateName(),QString("interrupted"));QCOMPARE(saved.phase,QString("interrupted"));QCOMPARE(saved.copyStatus,QString("no_result"));QCOMPARE(saved.items[0].phase,QString("unknown"));QCOMPARE(saved.items[0].uploadProgress->bytesTransferred.value(),1024);QCOMPARE(saved.items[0].uploadStatus,QString("unknown"));QCOMPARE(saved.items[0].candidate,QString("fixed"));QCOMPARE(saved.items[0].publicationId,QString("marker"));}
 void recoveredInterruptedCopiesSettleWithoutChangingConfirmedReceipts_data() {
  QTest::addColumn<QString>("state");QTest::addColumn<QString>("copy");
  for(const auto& state:QStringList{"accepted","acquiring","stored","running","interrupted"})QTest::newRow(qPrintable(state))<<state<<QString("pending");
  QTest::newRow("already-copied")<<QString("interrupted")<<QString("copied");
  QTest::newRow("already-suppressed")<<QString("interrupted")<<QString("suppressed");
 }
 void recoveredInterruptedCopiesSettleWithoutChangingConfirmedReceipts() {
  QFETCH(QString,state);QFETCH(QString,copy);QTemporaryDir d;const auto path=d.filePath("history.sqlite");
  Operation op;op.id="proton-recovery";op.restore(operationState(state));op.copyStatus=copy;op.settings=Settings::fromValidated(Settings::defaults());
  OperationItem item;item.artifact.id="confirmed-item";item.uploadStatus="uploaded";item.shareStatus="shared";item.publicationId="saved-publication";
  item.receipt.destination={"proton-drive","saved-profile","saved-revision",{},{{"mode","keychain"},{"reference","synthetic-reference"}}};
  item.receipt.objectReference={{"nodeUid","synthetic-node"},{"revisionUid","synthetic-revision"}};item.receipt.concurrencyToken="synthetic-revision";item.receipt.publicationId=item.publicationId;item.receipt.directUrl="https://synthetic.invalid/share#capability";op.items={item};
  {HistoryStore store(path);QVERIFY(!store.saveOperation(op));}
  HistoryStore reopened(path);Error error;reopened.recover(&error);QVERIFY(!error);const auto recovered=reopened.operation(op.id);
  QCOMPARE(recovered.stateName(),QString("interrupted"));QCOMPARE(recovered.copyStatus,copy=="pending"?QString("no_result"):copy);
  QCOMPARE(recovered.items.first().receipt.toJson(),item.receipt.toJson());QCOMPARE(recovered.items.first().publicationId,item.publicationId);
  reopened.recover(&error);QVERIFY(!error);QCOMPARE(reopened.operation(op.id).toJson(),recovered.toJson());
 }
 void populatedLegacyMigrationPreservesReceipts() {
  QTemporaryDir d;const auto path=d.filePath("history.sqlite"),connection=QString("legacy-success");QJsonObject row{{"id","item"},{"operationId","operation"},{"itemId","item"},{"created","2026-10-07T00:00:00.000Z"},{"name","fixture.txt"},{"mime","text/plain"},{"category","text"},{"profileId","original"},{"uploadStatus","uploaded"},{"local",QJsonObject{{"status","saved"},{"path",d.filePath("user-copy.txt")}}},{"remoteStatus","failed"},{"receipt",QJsonObject{{"objectReference",QJsonObject{{"opaque","unparsed/%?#"}}},{"directUrl","https://example.invalid/unrelated"}}}};
  {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);QVERIFY(db.open());QSqlQuery q(db);QVERIFY(q.exec("CREATE TABLE history(id TEXT PRIMARY KEY,data BLOB)"));q.prepare("INSERT INTO history VALUES(?,?)");q.addBindValue("item");q.addBindValue(QJsonDocument(row).toJson());QVERIFY(q.exec());QVERIFY(q.exec("PRAGMA user_version=1"));db.close();}QSqlDatabase::removeDatabase(connection);HistoryStore upgraded(path);QVERIFY(!upgraded.error());auto expected=row;expected["linkAvailable"]=true;expected["linkFinishable"]=false;expected["linkRefreshable"]=false;QCOMPARE(upgraded.detail("item"),expected);QCOMPARE(upgraded.list({}).total,1);
 }
 void migrationIsTransactional() {QTemporaryDir d;const auto path=d.filePath("history.sqlite");const auto connection=QString("legacy");{auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);QVERIFY(db.open());QSqlQuery q(db);QVERIFY(q.exec("CREATE TABLE history(id TEXT PRIMARY KEY,data BLOB)"));QVERIFY(q.exec("INSERT INTO history VALUES('preserve','{}')"));QVERIFY(q.exec("PRAGMA user_version=1"));db.close();}QSqlDatabase::removeDatabase(connection);{HistoryStore store(path);QCOMPARE(store.error().code,QString("history_migration"));}{auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);QVERIFY(db.open());QSqlQuery q(db);QVERIFY(q.exec("PRAGMA user_version"));QVERIFY(q.next());QCOMPARE(q.value(0).toInt(),1);QVERIFY(q.exec("SELECT id FROM history"));QVERIFY(q.next());QCOMPARE(q.value(0).toString(),QString("preserve"));db.close();}QSqlDatabase::removeDatabase(connection);}
};
QTEST_GUILESS_MAIN(HistoryTest)
#include "test_history.moc"
