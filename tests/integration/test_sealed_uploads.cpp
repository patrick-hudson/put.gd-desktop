#include "fixtures/U2Fixtures.h"
#include "fixtures/Waits.h"
#include "seal/SealCrypto.h"
#include "seal/SealPage.h"
#include <QBuffer>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUuid>
using namespace putgd;using namespace putgd::test;
namespace {
// A destination that keeps what it was sent, by name, and serves it under one public folder:
// the sealed page and its data file are read back from here as a browser would get them.
class KeepingProvider final:public StorageProvider {
public:
 OpaqueProvider storage;mutable QMutex mutex;QHash<QString,QByteArray> files;QString sealUnavailable;bool sealSplit=true;int failNext=0;
 ProviderCapabilities capabilities(const Destination& d) const override {auto c=storage.capabilities(d);c.sealUnavailable=sealUnavailable;c.sealSplit=sealSplit;return c;}
 QString serializationKey(const Destination& d,const QJsonObject& o,const QString& n) const override {return storage.serializationKey(d,o,n);}
 Error validateSettings(const QJsonObject& s) const override {return storage.validateSettings(s);}
 void checkName(const Destination& d,const QString& n,std::function<void(NameCheck)> cb,std::shared_ptr<std::atomic_bool> c={}) override {storage.checkName(d,n,cb,c);}
 void upload(const UploadRequest& r,std::function<void(UploadResult)> cb) override {
  {QMutexLocker lock(&mutex);if(failNext>0){--failNext;cb({"failed",{},{"fixture_failed","Synthetic failure",{}}});return;}}
  QFile file(r.artifact.path);QByteArray bytes;if(file.open(QIODevice::ReadOnly))bytes=file.readAll();
  storage.upload(r,[this,r,bytes,cb](UploadResult result){
   if(result.status=="uploaded"){QMutexLocker lock(&mutex);files.insert(r.candidate,bytes);result.receipt.directUrl="https://files.example.invalid/f/"+r.candidate;}
   cb(result);
  });
 }
 void stat(const StorageReceipt& r,std::function<void(StatResult)> cb) override {storage.stat(r,cb);}
 void reconcile(const UploadRequest& r,std::function<void(StatResult)> cb) override {storage.reconcile(r,cb);}
 void remove(const DeleteRequest& r,DurableCheckpoint d,std::function<void(DeleteResult)> cb) override {storage.remove(r,d,cb);}
 QByteArray file(const QString& name) const {QMutexLocker lock(&mutex);return files.value(name);}
 int count() const {QMutexLocker lock(&mutex);return int(files.size());}
};
QByteArray pattern(qint64 size) {QByteArray out(size,'\0');for(qint64 i=0;i<size;++i)out[i]=char((i*131+17)%253);return out;}
// Opens a sealed page as a recipient would: the page, and for the split form its data file beside it.
Error open(const KeepingProvider& provider,const QString& link,const QString& passphrase,QByteArray& plain,seal::Meta& meta) {
 const auto hash=link.indexOf('#');if(hash<0)return {"no_key","no key",{}};
 const auto name=QUrl(link.left(hash)).fileName();seal::ParsedPage parsed;
 if(auto e=seal::parsePage(provider.file(name),parsed))return e;
 const auto container=parsed.split?provider.file(parsed.bin):QByteArray::fromBase64(parsed.base64);
 QBuffer input;input.setData(container);input.open(QIODevice::ReadOnly);plain.clear();
 return seal::open(input,container.size(),seal::keyFromFragment(link.mid(hash)),passphrase,[&plain](const char* d,qint64 n){plain.append(d,n);return true;},&meta);
}
struct Fixture {
 QTemporaryDir dir;ProviderRegistry registry;std::shared_ptr<KeepingProvider> provider=std::make_shared<KeepingProvider>();
 std::unique_ptr<SettingsService> settings;Source source;std::unique_ptr<OperationService> operations;
 QString checkState="passed",passphrase;
 explicit Fixture(const QJsonObject& seal) {
  registry.add("opaque",provider);
  settings=std::make_unique<SettingsService>(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(dir.path());auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();if(!seal.isEmpty())profile["seal"]=seal;profiles[0]=profile;doc["profiles"]=profiles;
  if(!settings->save(doc,{}))qFatal("settings");
  operations=std::make_unique<OperationService>(dir.filePath("data"),*settings,registry,source);
  operations->setSealHelperForTesting(QStringLiteral(PUTGD_CLI_PATH));
  operations->setSealHooks([this](const QString&,const QJsonObject&){return QJsonObject{{"state",checkState},{"reason","Synthetic check refusal."}};},[this](const QString&,const QJsonObject&){return passphrase;});
 }
 // Uploads one file and waits until its operation ends; returns the operation.
 Operation upload(const QByteArray& bytes,const QString& name,const QJsonObject& seal={}) {
  const auto path=fixture(dir.path(),name,bytes);QString id;
  operations->uploadFiles({path},"test",{},[&](Error e,QJsonObject r){if(!e)id=r["operationId"].toString();else id="error:"+e.code;},"report",seal);
  if(!waitFor([&]{return !id.isEmpty();},10000)||id.startsWith("error:"))return {};
  HistoryStore journal(operations->databasePath());
  (void)waitFor([&]{return !journal.operation(id).active()&&operations->diagnostics()["activeOperations"].toInt()==0;},120000);
  return journal.operation(id);
 }
 QJsonObject detail(const QString& id) {HistoryStore journal(operations->databasePath());return journal.detail(id);}
};
}
// Sealed links (#2079) through the upload pipeline: what is sent, the link, Retry and History.
class SealedUploadsTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase() {QVERIFY(QFileInfo(QStringLiteral(PUTGD_CLI_PATH)).isExecutable());}
 // A destination set to seal uploads a sealed page under a random name; its link carries the
 // key after #, opens to the file with its name, and is what History and the clipboard copy.
 void sealedUploadSendsOnlyThePage() {
  Fixture f({{"mode","on"}});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto bytes=QByteArray("a secret capture, not for the bucket\n");
  const auto op=f.upload(bytes,"Screen-Capture-secret.txt");
  QCOMPARE(op.stateName(),QString("completed"));const auto item=op.items.first();
  QVERIFY(item.sealed());QCOMPARE(item.seal["form"].toString(),QString("inline"));QVERIFY(!item.seal["passphrase"].toBool());
  QVERIFY2(QRegularExpression("^[A-Za-z0-9]{12}\\.html$").match(item.candidate).hasMatch(),qPrintable(item.candidate));
  QCOMPARE(f.provider->count(),1);QVERIFY(!f.provider->file(item.candidate).contains("secret"));
  // The artifact is still the file: previews and local copies use it.
  QCOMPARE(item.artifact.name,QString("Screen-Capture-secret.txt"));
  const auto link=item.link();QVERIFY(link.startsWith(item.receipt.directUrl+"#v1."));QCOMPARE(link.size(),item.receipt.directUrl.size()+47);
  QByteArray plain;seal::Meta meta;const auto e=open(*f.provider,link,{},plain,meta);QVERIFY2(!e,qPrintable(e.message));
  QCOMPARE(plain,bytes);QCOMPARE(meta.name,QString("Screen-Capture-secret.txt"));QCOMPARE(meta.type,item.artifact.mime);
  QVERIFY(f.source.copiedTexts.contains(link));
  const auto record=f.detail(item.artifact.id);QCOMPARE(record["link"].toString(),link);QVERIFY(record["sealed"].toBool());
  QVERIFY(!record["seal"].toObject()["page"].toObject().contains("path"));
  // The public operation keeps the private files private.
  QVERIFY(!op.publicJson()["items"].toArray()[0].toObject()["seal"].toObject()["page"].toObject().contains("path"));
 }
 // The upload's own choice beats the destination's, both ways.
 void perUploadChoiceOverridesTheDestination() {
  Fixture off({});PUTGD_TRY_VERIFY(off.operations->ready());
  auto op=off.upload("plain","a.txt",{{"enabled",true}});QVERIFY(op.items.first().sealed());
  Fixture on({{"mode","on"}});PUTGD_TRY_VERIFY(on.operations->ready());
  op=on.upload("plain","b.txt",{{"enabled",false}});QVERIFY(op.items.first().seal.isEmpty());QCOMPARE(on.provider->file("b.txt"),QByteArray("plain"));
 }
 // Sealing that was asked for and can't happen here never uploads the file unsealed.
 void sealingThatCantHappenUploadsNothing_data() {
  QTest::addColumn<QString>("check");QTest::addColumn<QString>("unavailable");QTest::addColumn<QString>("mode");QTest::addColumn<QString>("code");
  QTest::newRow("untested")<<"untested"<<""<<"on"<<"seal_check_required";
  QTest::newRow("check-failed")<<"failed"<<""<<"on"<<"seal_check_required";
  QTest::newRow("destination-type")<<"passed"<<"Proton Drive already encrypts files end to end."<<"on"<<"seal_unavailable";
  QTest::newRow("passphrase-not-loaded")<<"passed"<<""<<"passphrase"<<"seal_passphrase_locked";
 }
 void sealingThatCantHappenUploadsNothing() {
  QFETCH(QString,check);QFETCH(QString,unavailable);QFETCH(QString,mode);QFETCH(QString,code);
  QJsonObject seal{{"mode",mode}};if(mode=="passphrase")seal["passphraseCredentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};
  Fixture f(seal);f.checkState=check;f.provider->sealUnavailable=unavailable;PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload("must stay home","c.txt");
  QCOMPARE(op.items.first().uploadStatus,QString("failed"));QCOMPARE(op.items.first().error.code,code);QCOMPARE(f.provider->count(),0);
  // Once what refused it is put right, Retry seals it and sends the page, never the file.
  f.checkState="passed";f.provider->sealUnavailable.clear();f.passphrase="put right";
  bool accepted=false;f.operations->retry(op.id,[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.message));accepted=true;});PUTGD_TRY_VERIFY(accepted);
  HistoryStore journal(f.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(op.id).items.first().uploadStatus,QString("uploaded"));
  const auto item=journal.operation(op.id).items.first();QVERIFY(item.sealed());QCOMPARE(f.provider->count(),1);QVERIFY(f.provider->file(item.candidate).startsWith("<!doctype html>"));
 }
 // With a passphrase, the link alone opens nothing.
 void passphraseIsNeededBesideTheLink() {
  Fixture f({{"mode","passphrase"},{"passphraseCredentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}});
  f.passphrase="correct horse";PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload("behind two locks","d.txt");const auto item=op.items.first();
  QVERIFY(item.sealed());QVERIFY(item.seal["passphrase"].toBool());
  QByteArray plain;seal::Meta meta;
  QCOMPARE(open(*f.provider,item.link(),{},plain,meta).code,QString("seal_passphrase_required"));
  QCOMPARE(open(*f.provider,item.link(),"wrong",plain,meta).code,QString("seal_passphrase"));
  QVERIFY(!open(*f.provider,item.link(),"correct horse",plain,meta));QCOMPARE(plain,QByteArray("behind two locks"));
  QVERIFY(!QJsonDocument(op.toJson()).toJson().contains("correct horse"));
 }
 // A passphrase that came with the request (putgd-cli upload --passphrase-file) is never kept:
 // a Retry that still has to seal refuses rather than seal without it.
 void retryNeverSealsWithoutTheRequestsPassphrase() {
  Fixture f({{"mode","on"}});f.checkState="untested";PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload("needs its passphrase","p.txt",{{"enabled",true},{"passphrase","given once"}});
  QCOMPARE(op.items.first().error.code,QString("seal_check_required"));QCOMPARE(op.seal["passphraseFrom"].toString(),QString("request"));
  QVERIFY(!QJsonDocument(op.toJson()).toJson().contains("given once"));
  f.checkState="passed";bool accepted=false;f.operations->retry(op.id,[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.message));accepted=true;});PUTGD_TRY_VERIFY(accepted);
  HistoryStore journal(f.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(op.id).items.first().error.code,QString("seal_passphrase_missing"));
  QCOMPARE(f.provider->count(),0);
 }
 // A data file in flight when put.gd stopped is reconciled on Retry, never sent blindly again.
 void recoveryMarksAnInFlightDataFileUnknown() {
  QTemporaryDir dir;const auto path=dir.filePath("history.sqlite");
  Operation op;op.id=QUuid::createUuid().toString(QUuid::WithoutBraces);op.restore(operationState("running"));op.settings=Settings::fromValidated(Settings::defaults());
  OperationItem item;item.artifact.id="sealed-item";item.uploadStatus="stored";item.publicationId="page";
  item.seal={{"status","sealed"},{"form","split"},{"key","v1.x"},{"data",QJsonObject{{"uploadStatus","uploading"},{"candidate","abc.bin"},{"publicationId","data"}}}};
  op.items={item};
  {HistoryStore store(path);QVERIFY(!store.saveOperation(op));}
  HistoryStore reopened(path);Error error;reopened.recover(&error);QVERIFY(!error);
  const auto recovered=reopened.operation(op.id).items.first();
  QCOMPARE(recovered.seal["data"].toObject()["uploadStatus"].toString(),QString("unknown"));QCOMPARE(recovered.uploadStatus,QString("unknown"));
 }
 // The page and its data file share one random stem.
 void splitNamesShareOneStem() {
  Fixture f({{"mode","on"}});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto item=f.upload(pattern(seal::inlineLimit+1),"v.webm").items.first();
  QCOMPARE(item.seal["data"].toObject()["candidate"].toString(),item.candidate.chopped(5)+".bin");
 }
 // Retry sends the same sealed page, so a link someone already has keeps working.
 void retryUploadsTheSameCiphertext() {
  Fixture f({{"mode","on"}});f.provider->failNext=1;PUTGD_TRY_VERIFY(f.operations->ready());
  auto op=f.upload("retry me","e.txt");auto item=op.items.first();
  QCOMPARE(item.uploadStatus,QString("failed"));QVERIFY(item.sealed());
  const auto key=item.seal["key"].toString(),page=item.seal["page"].toObject()["sha256"].toString(),name=item.candidate;
  bool accepted=false;f.operations->retry(op.id,[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.message));accepted=true;});PUTGD_TRY_VERIFY(accepted);
  HistoryStore journal(f.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(op.id).items.first().uploadStatus,QString("uploaded"));
  item=journal.operation(op.id).items.first();
  QCOMPARE(item.seal["key"].toString(),key);QCOMPARE(item.seal["page"].toObject()["sha256"].toString(),page);QCOMPARE(item.candidate,name);
  QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(f.provider->file(name),QCryptographicHash::Sha256).toHex()),page);
 }
 // Over 64 MB: the page and its data file side by side; the page fetches ./NAME.bin.
 void largeFilesSealAsPageAndDataFile() {
  Fixture f({{"mode","on"}});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto bytes=pattern(seal::inlineLimit+12345);
  const auto op=f.upload(bytes,"recording.webm");const auto item=op.items.first();
  QCOMPARE(op.stateName(),QString("completed"));QVERIFY(item.sealed());QCOMPARE(item.seal["form"].toString(),QString("split"));
  const auto data=item.seal["data"].toObject();QCOMPARE(data["uploadStatus"].toString(),QString("uploaded"));
  QVERIFY(QRegularExpression("^[A-Za-z0-9]{12}\\.bin$").match(data["candidate"].toString()).hasMatch());
  QCOMPARE(f.provider->count(),2);QVERIFY(f.provider->file(item.candidate).size()<20000);
  QByteArray plain;seal::Meta meta;const auto e=open(*f.provider,item.link(),{},plain,meta);QVERIFY2(!e,qPrintable(e.message));QCOMPARE(plain,bytes);
 }
 // A sealed request that sealing can't take is never sent unsealed: it fails before anything
 // goes out, and Retry keeps it sealed (#2079, the design's limits).
 void fileTooLargeToSealIsNeverSent() {
  Fixture f({{"mode","on"}});f.operations->setSealLimitsForTesting(64,128);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload(pattern(129),"huge.bin");const auto item=op.items.first();
  QCOMPARE(item.uploadStatus,QString("failed"));QCOMPARE(item.error.code,QString("seal_too_large"));QVERIFY(item.error.message.contains("2 GB"));
  QVERIFY(item.seal.isEmpty());QCOMPARE(f.provider->count(),0);
  bool accepted=false;f.operations->retry(op.id,[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.message));accepted=true;});PUTGD_TRY_VERIFY(accepted);
  HistoryStore journal(f.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(op.id).items.first().error.code,QString("seal_too_large"));
  QCOMPARE(f.provider->count(),0);
  // Turning Seal off for a new upload is how it goes out as an ordinary link.
  const auto plain=f.upload(pattern(129),"huge-again.bin",{{"enabled",false}});QCOMPARE(plain.items.first().uploadStatus,QString("uploaded"));QCOMPARE(f.provider->count(),1);
 }
 void twoFilesWhereLinksCantServeThemIsNeverSent() {
  Fixture f({{"mode","on"}});f.provider->sealSplit=false;f.operations->setSealLimitsForTesting(64,1LL<<30);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload(pattern(65),"big.bin");const auto item=op.items.first();
  QCOMPARE(item.uploadStatus,QString("failed"));QCOMPARE(item.error.code,QString("seal_split_unavailable"));QVERIFY(item.seal.isEmpty());QCOMPARE(f.provider->count(),0);
  // The same file fits one page where the limit allows it, and seals.
  const auto small=f.upload(pattern(64),"fits.bin");QVERIFY(small.items.first().sealed());QCOMPARE(f.provider->count(),1);
 }
 // Forget key: History keeps no way to open it, and offers no link.
 void forgetKeyLeavesNoLink() {
  Fixture f({{"mode","on"}});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload("forget me","g.txt");const auto id=op.items.first().artifact.id;
  HistoryStore journal(f.operations->databasePath());QVERIFY(!journal.forgetSealKey(id));
  const auto record=f.detail(id);QVERIFY(!record["linkAvailable"].toBool());QVERIFY(record["link"].toString().isEmpty());QVERIFY(record["seal"].toObject()["keyForgotten"].toBool());
  QVERIFY(journal.operation(op.id).items.first().seal["key"].toString().isEmpty());
  // A writer that read the operation before Forget key can't put the key back.
  QVERIFY(!journal.saveOperation(op));
  QVERIFY(journal.operation(op.id).items.first().seal["key"].toString().isEmpty());QVERIFY(f.detail(id)["link"].toString().isEmpty());
  QCOMPARE(journal.forgetSealKey("missing-id").code,QString("history_not_found"));
  const auto plain=f.upload("not sealed","h.txt",{{"enabled",false}});QCOMPARE(journal.forgetSealKey(plain.items.first().artifact.id).code,QString("history_not_sealed"));
 }
};
QTEST_GUILESS_MAIN(SealedUploadsTest)
#include "test_sealed_uploads.moc"
