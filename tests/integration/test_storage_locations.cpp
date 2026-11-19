#include "fixtures/U2Fixtures.h"
#include "app/StorageLocations.h"
#include "app/DestinationTestJournal.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QDirIterator>
#include <algorithm>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;
// Synthetic profiles in the layout releases before #56 used (putgd/putgd),
// moved by StorageLocations into <generic>/putgd before anything opens them.
class StorageLocationsTest:public QObject {
 Q_OBJECT
 using Tree=QMap<QString,QByteArray>;
 // Relative path -> content hash (files), "dir", or "link:<target>".
 static Tree snapshot(const QString& root,const QStringList& skip={}) {
  Tree tree;if(!QFileInfo(root).isDir())return tree;
  QDirIterator it(root,QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDirIterator::Subdirectories);
  while(it.hasNext()) {
   const QFileInfo info(it.next());const auto relative=QDir(root).relativeFilePath(info.filePath());
   if(std::any_of(skip.begin(),skip.end(),[&](const QString& s){return relative==s||relative.startsWith(s+"/");}))continue;
   if(info.isSymLink())tree.insert(relative,"link:"+info.symLinkTarget().toUtf8());
   else if(info.isDir())tree.insert(relative,"dir");
   else {QFile file(info.filePath());if(!file.open(QIODevice::ReadOnly))tree.insert(relative,"unreadable");else tree.insert(relative,QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex());}
  }
  return tree;
 }
 static bool copyTree(const QString& from,const QString& to) {
  if(!QDir().mkpath(to))return false;
  QDirIterator it(from,QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDirIterator::Subdirectories);
  QList<QPair<QString,QFileDevice::Permissions>> directories;
  while(it.hasNext()) {
   const QFileInfo info(it.next());const auto target=to+"/"+QDir(from).relativeFilePath(info.filePath());
   if(info.isSymLink()){if(!QFile::link(info.symLinkTarget(),target))return false;}
   else if(info.isDir()){if(!QDir().mkpath(target))return false;directories.append({target,info.permissions()});}
   else if(!QFile::copy(info.filePath(),target))return false;
  }
  for(const auto& [path,permissions]:directories)QFile::setPermissions(path,permissions);
  return true;
 }
 // A journal lock in Qt's own format whose owner has exited, as a crashed
 // earlier resident leaves it.
 static bool staleLock(const QString& path) {
  QTemporaryDir scratch;QLockFile live(scratch.filePath("probe.lock"));if(!live.tryLock(0))return false;
  QFile probe(scratch.filePath("probe.lock"));if(!probe.open(QIODevice::ReadOnly))return false;auto lines=probe.readAll().split('\n');probe.close();live.unlock();
  QProcess exited;exited.start("true");if(!exited.waitForStarted(3000))return false;const auto pid=exited.processId();if(!exited.waitForFinished(3000)||pid<=0)return false;lines[0]=QByteArray::number(pid);
  QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(lines.join('\n'))>0;
 }
 struct Profile {QString configRevision,failed,interrupted,item,preview;};
 static QString legacyConfig(const QString& base) {return base+"/c/putgd/putgd";}
 static QString legacyData(const QString& base) {return base+"/d/putgd/putgd";}
 // A used earlier install: accepted settings, a failed upload with its retained
 // artifact and preview, an acquisition interrupted before its items were
 // journaled, a crashed resident's journal lock, and the recording helper's
 // own sibling folder.
 static Profile populate(const QString& base) {
  Profile profile;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;registry.add("opaque",provider);
  SettingsService settings(legacyConfig(base)+"/config.json",[&](auto id,auto value){return registry.validate(id,value);});
  if(settings.save(configuration(base),{}).error)return {};
  profile.configRevision=settings.revision();
  Source source;source.payload=textPayload();
  {
   OperationService operations(legacyData(base),settings,registry,source);if(!putgd::test::waitFor([&]{return operations.ready();}))return {};
   operations.invoke("clipboard",[&](Error e,QJsonObject r){if(!e)profile.failed=r["operationId"].toString();});
   if(!putgd::test::waitFor([&]{return !profile.failed.isEmpty()&&operations.diagnostics()["activeOperations"].toInt()==0;}))return {};
  }
  HistoryStore store(legacyData(base)+"/history.sqlite");auto op=store.operation(profile.failed);if(op.items.size()!=1||op.items[0].uploadStatus!="failed")return {};
  profile.item=op.items[0].artifact.id;
  profile.preview=legacyData(base)+"/artifacts/../previews/"+profile.item+".png";
  if(!QDir().mkpath(legacyData(base)+"/previews")||fixture(legacyData(base)+"/previews",profile.item+".png","synthetic preview").isEmpty())return {};
  op.items[0].artifact.previewPath=profile.preview;if(store.saveOperation(op))return {};
  profile.interrupted="2e0c5cf4-ce1a-4c36-979d-e56c7d28c9e5";
  Operation accepted;accepted.id=profile.interrupted;accepted.action="clipboard";accepted.restore(OperationState::Acquiring);accepted.created="2026-10-07T00:00:00.000Z";accepted.settings=settings.snapshot();accepted.settingsRevision=settings.revision();accepted.destination={"opaque","test",settings.revision(),{{"workspace","fixture-workspace"}},{{"mode","session"},{"reference",QJsonValue::Null}}};
  if(store.saveOperation(accepted))return {};
  if(ArtifactStore(legacyData(base)+"/artifacts").acquire(profile.interrupted,textPayload(),ResourceLimits{}).error)return {};
  if(!staleLock(legacyData(base)+"/resident.lock"))return {};
  if(!QDir().mkpath(base+"/d/putgd/putgd-record")||fixture(base+"/d/putgd/putgd-record","recording-portal-tokens.json","{}").isEmpty())return {};
  return profile;
 }
 QTemporaryDir templateDir_;Profile template_;Tree configBefore_,dataBefore_;
 // A resumed move reports "current" for a root the interrupted start finished.
 void verifyMoved(const QString& base,const StorageLocations& locations,bool resumed=false) {
  QVERIFY2(!locations.error,qPrintable(locations.error.code+": "+locations.error.message));
  QCOMPARE(locations.configRoot,base+"/c/putgd");QCOMPARE(locations.dataRoot,base+"/d/putgd");
  for(const auto& root:{"config","data"}) {
   const auto state=locations.report[root].toObject()["state"].toString();
   QVERIFY2(state=="migrated"||(resumed&&state=="current"),qPrintable(QString(root)+" "+state));
  }
  QCOMPARE(snapshot(base+"/c/putgd"),configBefore_);
  // The stale journal lock is released; the helper's folder stays where it was.
  auto expected=dataBefore_;expected.remove("resident.lock");
  QCOMPARE(snapshot(base+"/d/putgd",{"putgd-record"}),expected);
  QVERIFY(QFile::exists(base+"/d/putgd/putgd-record/recording-portal-tokens.json"));
  for(const auto& gone:{legacyConfig(base),legacyData(base),StorageLocations::stagingRoot(base+"/c/putgd"),StorageLocations::stagingRoot(base+"/d/putgd"),base+"/d/putgd/resident.lock"})QVERIFY2(!QFileInfo::exists(gone),qPrintable(gone));
 }
private slots:
 void initTestCase() {
  QVERIFY(templateDir_.isValid());template_=populate(templateDir_.path());QVERIFY(!template_.failed.isEmpty());
  configBefore_=snapshot(legacyConfig(templateDir_.path()));dataBefore_=snapshot(legacyData(templateDir_.path()));
  QVERIFY(configBefore_.contains("config.json"));QVERIFY(dataBefore_.contains("history.sqlite"));QVERIFY(dataBefore_.contains("resident.lock"));
  QVERIFY(dataBefore_.contains("artifacts/"+template_.interrupted+"/_manifest.json"));QVERIFY(dataBefore_.contains("previews/"+template_.item+".png"));
 }
 void freshInstallUsesSingleNameRoots() {
  QTemporaryDir d;const auto locations=StorageLocations::prepare(d.filePath("c"),d.filePath("d"));
  QVERIFY(!locations.error);QCOMPARE(locations.configRoot,d.filePath("c/putgd"));QCOMPARE(locations.dataRoot,d.filePath("d/putgd"));QCOMPARE(locations.configPath(),d.filePath("c/putgd/config.json"));
  QCOMPARE(locations.report["config"].toObject(),(QJsonObject{{"path",d.filePath("c/putgd")},{"state","current"}}));QCOMPARE(locations.report["data"].toObject(),(QJsonObject{{"path",d.filePath("d/putgd")},{"state","current"}}));
  QVERIFY(!QFileInfo::exists(d.filePath("c")));QVERIFY(!QFileInfo::exists(d.filePath("d")));
  ProviderRegistry registry;SettingsService settings(locations.configPath());QVERIFY(settings.save(Settings::defaults(),{}));Source source;
  {OperationService operations(locations.dataRoot,settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());}
  QVERIFY(QFile::exists(d.filePath("c/putgd/config.json")));QVERIFY(QFile::exists(d.filePath("d/putgd/history.sqlite")));
  QVERIFY(!QFileInfo::exists(d.filePath("c/putgd/putgd")));QVERIFY(!QFileInfo::exists(d.filePath("d/putgd/putgd")));
  QCOMPARE(StorageLocations::prepare(d.filePath("c"),d.filePath("d")).report,locations.report);
 }
 void upgradeKeepsSettingsHistoryAndQueuedUploads() {
  QTemporaryDir d;QVERIFY(copyTree(templateDir_.path(),d.path()));
  const auto locations=StorageLocations::prepare(d.filePath("c"),d.filePath("d"));verifyMoved(d.path(),locations);if(QTest::currentTestFailed())return;
  const auto shared=QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|QFile::ReadOther|QFile::WriteOther|QFile::ExeOther;
  for(const auto& root:{locations.configRoot,locations.dataRoot})QVERIFY2(!(QFileInfo(root).permissions()&shared),qPrintable(root));
  ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(locations.configPath(),[&](auto id,auto value){return registry.validate(id,value);});
  QCOMPARE(settings.revision(),template_.configRevision);QVERIFY(settings.metadata()["validation"].toObject()["ok"].toBool());QCOMPARE(settings.path(),d.filePath("c/putgd/config.json"));
  Source source;OperationService operations(locations.dataRoot,settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  HistoryStore store(operations.databasePath());
  // Absolute paths journaled under the earlier root read under the new one.
  auto failed=store.operation(template_.failed);QCOMPARE(failed.items.size(),1);
  QCOMPARE(failed.items[0].artifact.path,d.filePath("d/putgd/artifacts/"+template_.failed+"/"+template_.item));QVERIFY(QFile::exists(failed.items[0].artifact.path));
  QCOMPARE(failed.items[0].artifact.previewPath,d.filePath("d/putgd/artifacts/../previews/"+template_.item+".png"));QVERIFY(QFile::exists(failed.items[0].artifact.previewPath));
  QCOMPARE(store.detail(template_.item)["previewPath"].toString(),failed.items[0].artifact.previewPath);
  const auto page=store.list({});QCOMPARE(page.records.size(),2);
  for(const auto& record:page.records)if(record.toObject()["id"]==template_.item)QCOMPARE(record.toObject()["previewPath"].toString(),failed.items[0].artifact.previewPath);
  const auto recovered=store.operation(template_.interrupted);QCOMPARE(recovered.stateName(),QString("interrupted"));QCOMPARE(recovered.items.size(),1);
  QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.reads,0);
  for(const auto& id:{template_.failed,template_.interrupted}) {
   bool retried=false;operations.retry(id,[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.code));retried=true;});PUTGD_TRY_VERIFY(retried);
   PUTGD_TRY_COMPARE(store.operation(id).items[0].uploadStatus,QString("uploaded"));
  }
  QCOMPARE(provider->uploads.load(),2);QCOMPARE(source.reads,0);
  QVERIFY(!QFileInfo::exists(legacyData(d.path())));
 }
 void legacyResidentStillRunningBlocksTheMove() {
  QTemporaryDir d;QVERIFY(copyTree(templateDir_.path(),d.path()));QVERIFY(QFile::remove(legacyData(d.path())+"/resident.lock"));
  QLockFile running(legacyData(d.path())+"/resident.lock");QVERIFY(running.tryLock(0));const auto before=snapshot(d.path());
  const auto locations=StorageLocations::prepare(d.filePath("c"),d.filePath("d"));
  QCOMPARE(locations.error.code,QString("storage_busy"));QCOMPARE(snapshot(d.path()),before);
  // The same step as journal recovery gives: no owner named, the lock's path to remove by hand (#1576).
  QCOMPARE(locations.error.details["lockPath"].toString(),QDir::toNativeSeparators(legacyData(d.path())+"/resident.lock"));
  QVERIFY2(locations.error.message.startsWith("The operation journal lock could not be acquired. If no other put.gd is running, quit put.gd, remove "),qPrintable(locations.error.message));
  running.unlock();verifyMoved(d.path(),StorageLocations::prepare(d.filePath("c"),d.filePath("d")));
 }
 // A lock file that cannot be created is not a held lock: no removal step for a file that never existed (#1576).
 void unwritableDataFolderIsNotReportedAsAHeldLock() {
  QTemporaryDir d;QVERIFY(copyTree(templateDir_.path(),d.path()));
  const auto data=d.filePath("d/putgd");QVERIFY(QFileInfo(data).isDir());QVERIFY(!QFileInfo::exists(data+"/resident.lock"));
  QVERIFY(QFile::setPermissions(data,QFile::ReadOwner|QFile::ExeOwner));
  if(QFileInfo(data).isWritable()) {QFile::setPermissions(data,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);QSKIP("permissions are not enforced for this user");}
  const auto locations=StorageLocations::prepare(d.filePath("c"),d.filePath("d"));
  QFile::setPermissions(data,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
  QCOMPARE(locations.error.code,QString("storage_unavailable"));
  QCOMPARE(locations.error.message,QString("Cannot create the journal lock file"));
  QCOMPARE(locations.error.details["lockPath"].toString(),QDir::toNativeSeparators(data+"/resident.lock"));
 }
 void bothRootsExistLeavesBothUnchanged() {
  QTemporaryDir d;QVERIFY(copyTree(templateDir_.path(),d.path()));
  SettingsService current(d.filePath("c/putgd/config.json"));QVERIFY(current.save(Settings::defaults(),{}));
  {HistoryStore journal(d.filePath("d/putgd/history.sqlite"));QVERIFY(!journal.error());}
  const auto before=snapshot(d.path());
  const auto locations=StorageLocations::prepare(d.filePath("c"),d.filePath("d"));QVERIFY(!locations.error);
  QCOMPARE(snapshot(d.path()),before);
  QCOMPARE(locations.configRoot,d.filePath("c/putgd"));QCOMPARE(locations.dataRoot,d.filePath("d/putgd"));
  QCOMPARE(locations.report["config"].toObject(),(QJsonObject{{"path",d.filePath("c/putgd")},{"state","legacy_retained"},{"legacyPath",legacyConfig(d.path())}}));
  QCOMPARE(locations.report["data"].toObject(),(QJsonObject{{"path",d.filePath("d/putgd")},{"state","legacy_retained"},{"legacyPath",legacyData(d.path())}}));
  QCOMPARE(StorageLocations::prepare(d.filePath("c"),d.filePath("d")).report,locations.report);QCOMPARE(snapshot(d.path()),before);
 }
 void eachRootIsDecidedOnItsOwn() {
  QTemporaryDir d;QVERIFY(copyTree(templateDir_.path(),d.path()));
  SettingsService current(d.filePath("c/putgd/config.json"));QVERIFY(current.save(Settings::defaults(),{}));const auto config=snapshot(d.filePath("c"));
  const auto locations=StorageLocations::prepare(d.filePath("c"),d.filePath("d"));QVERIFY(!locations.error);
  QCOMPARE(locations.report["config"].toObject()["state"].toString(),QString("legacy_retained"));QCOMPARE(snapshot(d.filePath("c")),config);
  QCOMPARE(locations.report["data"].toObject()["state"].toString(),QString("migrated"));QVERIFY(QFile::exists(d.filePath("d/putgd/history.sqlite")));QVERIFY(!QFileInfo::exists(legacyData(d.path())));
 }
 void interruptedMoveResumesWithoutLoss() {
  int steps=0;
  {QTemporaryDir d;QVERIFY(copyTree(templateDir_.path(),d.path()));verifyMoved(d.path(),StorageLocations::prepare(d.filePath("c"),d.filePath("d"),[&](const QString&,const QString&){++steps;return true;}));}
  // Two root renames, two staging removals and one rename per top-level entry.
  QCOMPARE(steps,4+int(QDir(legacyConfig(templateDir_.path())).entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot).size()+QDir(legacyData(templateDir_.path())).entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot).size()-1));
  for(int stop=0;stop<steps;++stop) {
   QTemporaryDir d;QVERIFY(copyTree(templateDir_.path(),d.path()));int step=0;
   const auto interrupted=StorageLocations::prepare(d.filePath("c"),d.filePath("d"),[&](const QString&,const QString&){return step++<stop;});
   QCOMPARE(interrupted.error.code,QString("storage_migration_interrupted"));
   // Every entry is in exactly one place: the legacy tree, staging or the new root.
   for(const auto& [root,before]:{std::pair{d.filePath("c/putgd"),configBefore_},std::pair{d.filePath("d/putgd"),dataBefore_}}) {
    Tree merged;
    for(const auto& part:{snapshot(root+"/putgd"),snapshot(StorageLocations::stagingRoot(root)),snapshot(root,{"putgd",".putgd-migration","putgd-record","resident.lock"})})
     for(auto it=part.cbegin();it!=part.cend();++it){QVERIFY2(!merged.contains(it.key()),qPrintable(it.key()));merged.insert(it.key(),it.value());}
    // Only the crashed resident's stale journal lock is released on the way.
    auto expected=before;if(!merged.contains("resident.lock"))expected.remove("resident.lock");QCOMPARE(merged,expected);
   }
   verifyMoved(d.path(),StorageLocations::prepare(d.filePath("c"),d.filePath("d")),true);if(QTest::currentTestFailed()){qWarning("stopped before step %d",stop);return;}
   ProviderRegistry registry;registry.add("opaque",std::make_shared<OpaqueProvider>());
   SettingsService settings(d.filePath("c/putgd/config.json"),[&](auto id,auto value){return registry.validate(id,value);});QCOMPARE(settings.revision(),template_.configRevision);
   HistoryStore store(d.filePath("d/putgd/history.sqlite"));QCOMPARE(store.operation(template_.failed).items[0].artifact.sha256,QString::fromLatin1(QCryptographicHash::hash("synthetic source content",QCryptographicHash::Sha256).toHex()));
   QVERIFY(!ArtifactStore(d.filePath("d/putgd/artifacts")).recover(template_.interrupted).error);
  }
 }
 // #1680: every storage test checkpoint is scrubbed at the journal boundary, so a receipt with a
 // missing or forged provider ID still reaches the journal without its signed query.
 void destinationTestCheckpointsLoseSignedLinksWhateverTheyClaim() {
  const QString link="https://s3.example/b/putgd-test-1.txt?X-Amz-Credential=AKIDSYNTHETIC&X-Amz-Signature=abc123";
  const QJsonObject unlabelled{{"directUrl",link},{"publicationId","pub-1"}};
  const QJsonObject forged{{"destination",QJsonObject{{"providerId","custom-http"}}},{"directUrl",link},{"publicationId","pub-1"}};
  const QJsonObject checkpoint{{"phase","finished"},{"receipt",unlabelled},{"target",forged},
   {"result",QJsonObject{{"receipt",forged},{"cleanupProgress",QJsonObject{{"target",unlabelled}}}}}};
  const auto written=QJsonDocument(withoutSignedTestLinks(checkpoint)).toJson();
  QVERIFY(!written.contains("X-Amz-")&&!written.contains("AKIDSYNTHETIC"));QVERIFY(written.contains("putgd-test-1.txt"));
 }
 // Journals of earlier tests are rewritten only when their own records show an S3 or B2 test, and
 // then lose every link's signed query while keeping what traces the test object. Anything else
 // in the folder stays byte for byte: Custom uploader journals (however labelled), journals whose
 // records don't say, symbolic links, other names and other schemas.
 void destinationTestJournalsLoseOnlySignedLinkQueries() {
  QTemporaryDir d;const auto dir=d.filePath("destination-tests");QVERIFY(QDir().mkpath(dir));
  auto receipt=[](const QString& provider,const QString& link,QJsonObject reference){reference["key"]="putgd-test-1.txt";
   QJsonObject r{{"directUrl",link},{"publicationId","pub-1"},{"objectReference",reference}};if(!provider.isEmpty())r["destination"]=QJsonObject{{"providerId",provider}};return r;};
  const QString s3Link="https://s3.example/b/putgd-test-1.txt?X-Amz-Credential=AKIDSYNTHETIC&X-Amz-Signature=abc123";
  const auto s3=receipt("s3",s3Link,{{"versionId","v1"}});
  const auto s3Unlabelled=receipt({},s3Link,{{"versionId","v1"}});
  const auto b2=receipt("backblaze-b2","https://f000.example/file/b/putgd-test-1.txt?Authorization=b2tokensynthetic",{{"fileId","4_z1"},{"bucketId","b1"}});
  const auto custom=receipt("custom-http","https://up.example/i/1.png",{{"nonce","n1"},{"deletionUrl","https://up.example/delete/1?Authorization=customsecret"}});
  const auto customAsS3=receipt("s3",s3Link,{{"nonce","n1"},{"deletionUrl","https://up.example/delete/1?Authorization=customsecret"}});
  auto journal=[](QJsonArray checkpoints){return QJsonDocument(QJsonObject{{"schemaVersion",1},{"checkpoints",checkpoints}}).toJson();};
  auto write=[&](const QString& name,const QByteArray& bytes){QFile f(dir+"/"+name);QVERIFY(f.open(QIODevice::WriteOnly));f.write(bytes);};
  auto read=[&](const QString& name){QFile f(dir+"/"+name);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();};
  auto uuid=[](char c){return QString(8,c)+"-"+QString(4,c)+"-"+QString(4,c)+"-"+QString(4,c)+"-"+QString(12,c)+".json";};
  const QJsonObject s3Intent{{"phase","synthetic_upload_intent"},{"destination",QJsonObject{{"providerId","s3"}}}};
  write(uuid('1'),journal({s3Intent,QJsonObject{{"phase","synthetic_uploaded"},{"receipt",s3}},QJsonObject{{"phase","object_intent"},{"target",s3Unlabelled}},
   QJsonObject{{"phase","finished"},{"result",QJsonObject{{"uploadStatus","uploaded"},{"receipt",s3},{"cleanupStatus","removed"},{"cleanupProgress",QJsonObject{{"target",s3}}}}}}}));
  write(uuid('2'),journal({QJsonObject{{"phase","delete_intent"},{"target",b2}}}));
  QMap<QString,QByteArray> unchanged;
  unchanged[uuid('3')]=journal({QJsonObject{{"phase","uploaded"},{"receipt",custom}}});
  unchanged[uuid('6')]=journal({s3Intent,QJsonObject{{"phase","uploaded"},{"receipt",customAsS3}}});
  unchanged[uuid('7')]=journal({QJsonObject{{"phase","synthetic_uploaded"},{"receipt",s3}}});
  // Custom uploader records with no label and no nonce, and an S3 destination on a record that
  // never saves one: their deletion URL keeps its token. B2 receipts need real file and bucket IDs.
  const QJsonObject bareCustom{{"directUrl",s3Link},{"publicationId","pub-1"},{"objectReference",QJsonObject{{"deletionUrl","https://up.example/delete/1?Authorization=customsecret"}}}};
  const QJsonObject s3Destination{{"providerId","s3"}};
  unchanged[uuid('8')]=journal({QJsonObject{{"phase","upload_intent"},{"name","putgd-test-1.txt"}},QJsonObject{{"phase","uploaded"},{"destination",s3Destination},{"receipt",bareCustom}}});
  const QJsonObject urlOnly{{"directUrl",s3Link},{"objectReference",QJsonObject{{"url","https://up.example/i/1.png"}}}};
  unchanged[uuid('9')]=journal({QJsonObject{{"phase","uploaded"},{"destination",s3Destination},{"receipt",urlOnly}}});
  unchanged[uuid('b')]=journal({QJsonObject{{"phase","finished"},{"destination",s3Destination},{"result",QJsonObject{{"link","https://up.example/i/1.png"},{"receipt",QJsonObject{{"directUrl",s3Link}}}}}}});
  unchanged[uuid('a')]=journal({QJsonObject{{"phase","delete_intent"},{"target",receipt("backblaze-b2",s3Link,{{"fileId",""},{"bucketId",""}})}}});
  unchanged["notes.json"]=journal({s3Intent,QJsonObject{{"receipt",s3}}});
  unchanged[uuid('5')]=QJsonDocument(QJsonObject{{"checkpoints",QJsonArray{s3Intent,QJsonObject{{"receipt",s3}}}}}).toJson();
  for(auto it=unchanged.begin();it!=unchanged.end();++it)write(it.key(),it.value());
  QTemporaryDir outside;QFile target(outside.filePath("elsewhere.json"));QVERIFY(target.open(QIODevice::WriteOnly));target.write(unchanged["notes.json"]);target.close();
  QVERIFY(QFile::link(target.fileName(),dir+"/"+uuid('4')));
  scrubDestinationTestJournals(dir);
  for(const auto& name:{uuid('1'),uuid('2')}){
   const auto bytes=read(name);
   QVERIFY2(!bytes.contains("X-Amz-")&&!bytes.contains("AKIDSYNTHETIC")&&!bytes.contains("Authorization=")&&!bytes.contains("b2tokensynthetic"),qPrintable(name));
   QVERIFY2(bytes.contains("putgd-test-1.txt")&&bytes.contains("pub-1"),qPrintable(name));
  }
  QVERIFY(read(uuid('1')).contains("\"phase\":\"finished\""));
  for(auto it=unchanged.begin();it!=unchanged.end();++it)QVERIFY2(read(it.key())==it.value(),qPrintable(it.key()));
  QVERIFY(QFileInfo(dir+"/"+uuid('4')).isSymLink());
  QFile untouched(target.fileName());QVERIFY(untouched.open(QIODevice::ReadOnly));QCOMPARE(untouched.readAll(),unchanged["notes.json"]);
 }
 // #1690: when a test's worker ends without a result, each step reports only what its own
 // terminal checkpoint shows; a step without one is unknown.
 void destinationTestTrailReportsOnlyFinishedSteps() {
  auto completed=[](QList<QJsonObject> checkpoints){DestinationTestTrail trail;for(const auto& c:checkpoints)trail.record(c);return trail.completed({});};
  const QJsonObject intent{{"phase","synthetic_upload_intent"}},uploaded{{"phase","synthetic_uploaded"},{"receipt",QJsonObject{{"publicationId","pub-1"}}}};
  QVERIFY(!completed({}).contains("uploadStatus"));
  QCOMPARE(completed({intent})["uploadStatus"].toString(),QString("unknown"));
  auto done=completed({intent,uploaded,QJsonObject{{"phase","object_intent"},{"target",QJsonObject{}}},QJsonObject{{"phase","done"},{"target",QJsonObject{}}}});
  QCOMPARE(done["uploadStatus"].toString(),QString("uploaded"));QCOMPARE(done["cleanupStatus"].toString(),QString("removed"));
  auto marker=completed({intent,uploaded,QJsonObject{{"phase","object_failed"},{"target",QJsonObject{}},{"markerVersionId","m1"}}});
  QCOMPARE(marker["cleanupStatus"].toString(),QString("hidden_version_retained"));
  // A version deletion in flight behind a marker may have succeeded: unknown, not retained.
  auto inFlight=completed({intent,uploaded,QJsonObject{{"phase","version_intent"},{"target",QJsonObject{}},{"markerVersionId","m1"}}});
  QCOMPARE(inFlight["cleanupStatus"].toString(),QString("unknown"));
  QCOMPARE(completed({intent,uploaded})["cleanupStatus"].toString(),QString("unknown"));
  QCOMPARE(completed({QJsonObject{{"phase","upload_intent"}},QJsonObject{{"phase","uploaded"},{"receipt",QJsonObject{{"publicationId","pub-1"}}}}})["uploadStatus"].toString(),QString("uploaded"));
  const QJsonObject probe{{"phase","synthetic_multipart_intent"}};
  QCOMPARE(completed({intent,uploaded,probe})["multipart"].toObject()["status"].toString(),QString("unknown"));
  const auto aborted=completed({intent,uploaded,probe,QJsonObject{{"phase","synthetic_multipart_finished"},{"abort","aborted"}}})["multipart"].toObject();
  QCOMPARE(aborted["status"].toString(),QString("unknown"));QCOMPARE(aborted["abort"].toString(),QString("aborted"));
  const QJsonObject large{{"phase","synthetic_large_intent"}};
  QCOMPARE(completed({intent,uploaded,large})["largeFile"].toObject()["start"].toString(),QString("unknown"));
  const auto started=completed({intent,uploaded,large,QJsonObject{{"phase","synthetic_large_started"},{"fileId","f1"}}})["largeFile"].toObject();
  QCOMPARE(started["start"].toString(),QString("ok"));QCOMPARE(started["fileId"].toString(),QString("f1"));QCOMPARE(started["cancel"].toString(),QString("unknown"));
  // A saved finish record stands on its own when the start record wasn't saved.
  const auto finishedOnly=completed({intent,uploaded,large,QJsonObject{{"phase","synthetic_large_finished"},{"fileId","f1"},{"cancel","cancelled"}}})["largeFile"].toObject();
  QCOMPARE(finishedOnly["fileId"].toString(),QString("f1"));QCOMPARE(finishedOnly["cancel"].toString(),QString("cancelled"));
  const QJsonObject reported{{"uploadStatus","failed"}};
  DestinationTestTrail trail;trail.record(intent);QCOMPARE(trail.completed(reported),reported);
 }
 void stagedEntryNeverReplacesTheRoot() {
  QTemporaryDir d;const auto root=d.filePath("d/putgd"),staging=StorageLocations::stagingRoot(root);
  QVERIFY(QDir().mkpath(staging+"/artifacts"));QVERIFY(!fixture(staging,"history.sqlite","staged").isEmpty());QVERIFY(!fixture(staging+"/artifacts","kept","staged artifact").isEmpty());
  QVERIFY(!fixture(root,"history.sqlite","current").isEmpty());
  const auto locations=StorageLocations::prepare(d.filePath("c"),d.filePath("d"));QVERIFY(!locations.error);
  QCOMPARE(locations.report["data"].toObject(),(QJsonObject{{"path",root},{"state","conflict"},{"stagingPath",staging},{"conflicts",QJsonArray{"history.sqlite"}}}));
  QFile current(root+"/history.sqlite");QVERIFY(current.open(QIODevice::ReadOnly));QCOMPARE(current.readAll(),QByteArray("current"));
  QFile staged(staging+"/history.sqlite");QVERIFY(staged.open(QIODevice::ReadOnly));QCOMPARE(staged.readAll(),QByteArray("staged"));
  QVERIFY(QFile::exists(root+"/artifacts/kept"));
 }
 void linkedLegacyRootStaysActive() {
  QTemporaryDir d;QVERIFY(QDir().mkpath(d.filePath("elsewhere")));QVERIFY(!fixture(d.filePath("elsewhere"),"history.sqlite","linked").isEmpty());
  QVERIFY(QDir().mkpath(d.filePath("d/putgd")));QVERIFY(QFile::link(d.filePath("elsewhere"),d.filePath("d/putgd/putgd")));const auto before=snapshot(d.path());
  const auto locations=StorageLocations::prepare(d.filePath("c"),d.filePath("d"));QVERIFY(!locations.error);
  QCOMPARE(locations.dataRoot,d.filePath("d/putgd/putgd"));QCOMPARE(locations.report["data"].toObject()["state"].toString(),QString("legacy_active"));QCOMPARE(locations.report["data"].toObject()["reason"].toString(),QString("symlink"));
  QCOMPARE(snapshot(d.path()),before);
 }
 void sharedRootMovesOnce() {
  // Windows resolves the generic config and data locations to one folder.
  QTemporaryDir d;const auto root=d.filePath("local/putgd");QVERIFY(QDir().mkpath(root+"/putgd/artifacts"));
  QVERIFY(!fixture(root+"/putgd","config.json","{}").isEmpty());QVERIFY(!fixture(root+"/putgd","history.sqlite","journal").isEmpty());
  const auto locations=StorageLocations::prepare(d.filePath("local"),d.filePath("local"));QVERIFY(!locations.error);
  QCOMPARE(locations.configRoot,root);QCOMPARE(locations.dataRoot,root);QCOMPARE(locations.report["config"],locations.report["data"]);
  QCOMPARE(locations.report["data"].toObject()["state"].toString(),QString("migrated"));
  QVERIFY(QFile::exists(root+"/config.json"));QVERIFY(QFile::exists(root+"/history.sqlite"));QVERIFY(QFileInfo(root+"/artifacts").isDir());QVERIFY(!QFileInfo::exists(root+"/putgd"));
 }
#ifdef Q_OS_LINUX
 // The installed resident and CLI: upgraded paths, diagnostics and history.
 void residentReportsMovedAndRetainedRoots() {
  QTemporaryDir d;QVERIFY(copyTree(templateDir_.path(),d.path()));
  const auto runtime=d.filePath("r"),socket=runtime+"/putgd/session.sock";QVERIFY2(socket.toUtf8().size()<=107,"Resolved test socket exceeds Linux's limit");
  QVERIFY(QDir().mkpath(runtime));QVERIFY(QFile::setPermissions(runtime,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  // The product registry has no opaque provider; keep that profile out of the
  // resident's validation by restoring a default configuration file first.
  QVERIFY(QFile::remove(legacyConfig(d.path())+"/config.json"));SettingsService seed(legacyConfig(d.path())+"/config.json");QVERIFY(seed.save(Settings::defaults(),{}));
  QProcessEnvironment environment;
  for(const auto& key:{"PATH","HOME","LANG","LD_LIBRARY_PATH","ASAN_OPTIONS","UBSAN_OPTIONS","TSAN_OPTIONS","LSAN_OPTIONS"})if(qEnvironmentVariableIsSet(key))environment.insert(key,qEnvironmentVariable(key));
  environment.insert("QT_QPA_PLATFORM","offscreen");environment.insert("XDG_CONFIG_HOME",d.filePath("c"));environment.insert("XDG_DATA_HOME",d.filePath("d"));
  environment.insert("XDG_CACHE_HOME",d.filePath("k"));environment.insert("XDG_STATE_HOME",d.filePath("s"));environment.insert("XDG_RUNTIME_DIR",runtime);environment.insert("DBUS_SESSION_BUS_ADDRESS","unix:path="+d.filePath("no-bus"));
  auto request=[&](const QString& method) {
   QProcess cli;cli.setProcessEnvironment(environment);cli.start(PUTGD_CLI_PATH,{"--socket",socket,method});
   if(!cli.waitForFinished(5000))return QJsonObject{};
   return QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
  };
  auto run=[&](std::function<void(const QJsonObject& diagnostics,const QJsonObject& settings,const QJsonObject& history)> check) {
   QProcess resident;resident.setProcessEnvironment(environment);resident.setProcessChannelMode(QProcess::ForwardedErrorChannel);
   struct Stop {QProcess& process;~Stop(){if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(3000);}}} stop{resident};
   resident.start(PUTGD_APP_PATH,{"--background"});QVERIFY(resident.waitForStarted());
   QJsonObject response;PUTGD_TRY_VERIFY2_WITH_TIMEOUT((response=request("diagnostics"))["result"].toObject()["journalReady"].toBool(),QJsonDocument(response).toJson(QJsonDocument::Compact).constData(),10000);
   const auto diagnostics=response["result"].toObject();
   const auto settings=request("settings.get")["result"].toObject();const auto history=request("history.list")["result"].toObject();
   check(diagnostics,settings,history);
   QVERIFY(request("ui.quit")["ok"].toBool());PUTGD_TRY_COMPARE_WITH_TIMEOUT(resident.state(),QProcess::NotRunning,10000);QCOMPARE(resident.exitCode(),0);
  };
  run([&](const QJsonObject& diagnostics,const QJsonObject& settings,const QJsonObject& history) {
   const auto locations=diagnostics["locations"].toObject();
   QCOMPARE(locations["config"].toObject(),(QJsonObject{{"path",d.filePath("c/putgd")},{"state","migrated"}}));QCOMPARE(locations["data"].toObject(),(QJsonObject{{"path",d.filePath("d/putgd")},{"state","migrated"}}));
   QCOMPARE(settings["metadata"].toObject()["path"].toString(),d.filePath("c/putgd/config.json"));
   QCOMPARE(history["total"].toInt(),2);
   // #74: the resident's log and running marker live under XDG_STATE_HOME.
   QCOMPARE(diagnostics["logging"].toObject()["directory"].toString(),d.filePath("s/putgd/logs"));QVERIFY(!diagnostics["loggingDegraded"].toBool());
   QVERIFY(!diagnostics["uncleanExit"].toObject()["detected"].toBool());QVERIFY(QFileInfo::exists(d.filePath("s/putgd/running.json")));
  });
  if(QTest::currentTestFailed())return;
  {
   QByteArray logs;for(const auto& name:QDir(d.filePath("s/putgd/logs")).entryList({"putgd-*.jsonl"})){QFile file(d.filePath("s/putgd/logs/"+name));QVERIFY(file.open(QIODevice::ReadOnly));logs+=file.readAll();}
   QVERIFY(logs.contains("\"Resident started\""));QVERIFY(logs.contains("\"Resident stopped\""));
   QVERIFY2(!QFileInfo::exists(d.filePath("s/putgd/running.json")),"a clean quit removes the running marker");
  }
  QVERIFY(!QFileInfo::exists(legacyConfig(d.path())));QVERIFY(!QFileInfo::exists(legacyData(d.path())));
  // An earlier version started afterwards recreates its own tree; the resident
  // keeps using the moved roots, leaves that tree alone and names it.
  QVERIFY(QDir().mkpath(legacyData(d.path())));QVERIFY(!fixture(legacyData(d.path()),"history.sqlite","earlier").isEmpty());
  SettingsService earlier(legacyConfig(d.path())+"/config.json");QVERIFY(earlier.save(Settings::defaults(),{}));
  const auto legacyConfigTree=snapshot(legacyConfig(d.path())),legacyDataTree=snapshot(legacyData(d.path()));
  run([&](const QJsonObject& diagnostics,const QJsonObject& settings,const QJsonObject& history) {
   const auto locations=diagnostics["locations"].toObject();
   QCOMPARE(locations["config"].toObject(),(QJsonObject{{"path",d.filePath("c/putgd")},{"state","legacy_retained"},{"legacyPath",legacyConfig(d.path())}}));
   QCOMPARE(locations["data"].toObject(),(QJsonObject{{"path",d.filePath("d/putgd")},{"state","legacy_retained"},{"legacyPath",legacyData(d.path())}}));
   QCOMPARE(settings["metadata"].toObject()["path"].toString(),d.filePath("c/putgd/config.json"));QCOMPARE(history["total"].toInt(),2);
  });
  QCOMPARE(snapshot(legacyConfig(d.path())),legacyConfigTree);QCOMPARE(snapshot(legacyData(d.path())),legacyDataTree);
 }
#endif
};
QTEST_GUILESS_MAIN(StorageLocationsTest)
#include "test_storage_locations.moc"
