#include "fixtures/U2Fixtures.h"
#include "fixtures/Waits.h"
#include "encrypt/EncryptCrypto.h"
#include "encrypt/EncryptPage.h"
#include "app/EncryptCheck.h"
#include "core/EncryptJob.h"
#include "app/DestinationStatus.h"
#include "app/SetupService.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <QBuffer>
#include <QDir>
#include <future>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUuid>
using namespace putgd;using namespace putgd::test;
namespace {
// A destination that keeps what it was sent, by name, and serves it under one public folder:
// the encrypted page and its data file are read back from here as a browser would get them.
class KeepingProvider final:public StorageProvider {
public:
 OpaqueProvider storage;mutable QMutex mutex;QHash<QString,QByteArray> files;QString encryptUnavailable;bool encryptSplit=true,privateUploads=false;int failNext=0;QString linkBase=QStringLiteral("https://files.example.invalid/f/");
 // Like SFTP: an upload journals its temporary file through the checkpoint first, and refuses without one.
 bool requireCheckpoint=false;std::atomic_bool checkpointAnswered{false};QStringList revisionsSeen;
 ProviderCapabilities capabilities(const Destination& d) const override {auto c=storage.capabilities(d);c.encryptUnavailable=encryptUnavailable;c.encryptSplit=encryptSplit;c.privateUploads=privateUploads;return c;}
 QString serializationKey(const Destination& d,const QJsonObject& o,const QString& n) const override {return storage.serializationKey(d,o,n);}
 Error validateSettings(const QJsonObject& s) const override {return storage.validateSettings(s);}
 void checkName(const Destination& d,const QString& n,std::function<void(NameCheck)> cb,std::shared_ptr<std::atomic_bool> c={}) override {storage.checkName(d,n,cb,c);}
 void upload(const UploadRequest& r,std::function<void(UploadResult)> cb) override {
  {QMutexLocker lock(&mutex);revisionsSeen.append(r.destination.profileRevision);if(failNext>0){--failNext;cb({"failed",{},{"fixture_failed","Synthetic failure",{}}});return;}}
  if(requireCheckpoint) {
   if(!r.checkpoint){cb({"failed",{},{"checkpoint_failed","This request keeps no journal",{}}});return;}
   std::promise<Error> acked;r.checkpoint({{"schemaVersion",1},{"temporary","synthetic"}},[&acked](Error e){acked.set_value(e);});
   if(acked.get_future().get()){cb({"failed",{},{"checkpoint_failed","Checkpoint refused",{}}});return;}
   checkpointAnswered=true;
  }
  QFile file(r.artifact.path);QByteArray bytes;if(file.open(QIODevice::ReadOnly))bytes=file.readAll();
  storage.upload(r,[this,r,bytes,cb](UploadResult result){
   if(result.status=="uploaded"){QMutexLocker lock(&mutex);files.insert(r.candidate,bytes);result.receipt.directUrl=linkBase+r.candidate;}
   cb(result);
  });
 }
 void stat(const StorageReceipt& r,std::function<void(StatResult)> cb) override {storage.stat(r,cb);}
 void reconcile(const UploadRequest& r,std::function<void(StatResult)> cb) override {storage.reconcile(r,cb);}
 void remove(const DeleteRequest& r,DurableCheckpoint d,std::function<void(DeleteResult)> cb) override {storage.remove(r,d,cb);}
 QByteArray file(const QString& name) const {QMutexLocker lock(&mutex);return files.value(name);}
 int count() const {QMutexLocker lock(&mutex);return int(files.size());}
};
// A stand-in for putgd-cli --encrypt-job: reads the request, sets $page to its page path, then runs `body`.
QString encryptHelper(const QTemporaryDir& dir,const QString& body) {
 const auto path=dir.filePath("helper.sh");QFile script(path);
 if(!script.open(QIODevice::WriteOnly))return {};
 script.write(("#!/bin/sh\nrequest=$(cat)\npage=$(printf '%s' \"$request\" | sed -n 's/.*\"page\":\"\\([^\"]*\\)\".*/\\1/p')\n"+body).toUtf8());script.close();
 QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);return path;
}
Artifact input(const QTemporaryDir& dir) {Artifact file;file.path=fixture(dir.path(),"in.txt","plain");file.bytes=5;file.name="in.txt";file.mime="text/plain";return file;}
QByteArray read(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
QByteArray pattern(qint64 size) {QByteArray out(size,'\0');for(qint64 i=0;i<size;++i)out[i]=char((i*131+17)%253);return out;}
// Opens an encrypted page as a recipient would: the page, and for the split form its data file beside it.
Error open(const KeepingProvider& provider,const QString& link,const QString& passphrase,QByteArray& plain,encrypt::Meta& meta) {
 const auto hash=link.indexOf('#');if(hash<0)return {"no_key","no key",{}};
 const auto name=QUrl(link.left(hash)).fileName();encrypt::ParsedPage parsed;
 if(auto e=encrypt::parsePage(provider.file(name),parsed))return e;
 const auto container=parsed.split?provider.file(parsed.bin):QByteArray::fromBase64(parsed.base64);
 QBuffer input;input.setData(container);input.open(QIODevice::ReadOnly);plain.clear();
 return encrypt::open(input,container.size(),encrypt::keyFromFragment(link.mid(hash)),passphrase,[&plain](const char* d,qint64 n){plain.append(d,n);return true;},&meta);
}
struct Fixture {
 QTemporaryDir dir;ProviderRegistry registry;std::shared_ptr<KeepingProvider> provider=std::make_shared<KeepingProvider>();
 std::unique_ptr<SettingsService> settings;Source source;std::unique_ptr<OperationService> operations;
 QString checkState="passed",passphrase;
 explicit Fixture(const QJsonObject& encrypt,const QString& sharing={}) {
  registry.add("opaque",provider);provider->privateUploads=sharing=="private";
  settings=std::make_unique<SettingsService>(dir.filePath("config.json"),[this](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(dir.path());auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();if(!encrypt.isEmpty())profile["encrypt"]=encrypt;profiles[0]=profile;doc["profiles"]=profiles;
  if(!sharing.isEmpty())doc["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId","test"},{"publicationMode",sharing}};
  if(!settings->save(doc,{}))qFatal("settings");
  operations=std::make_unique<OperationService>(dir.filePath("data"),*settings,registry,source);
  operations->setEncryptHelperForTesting(QStringLiteral(PUTGD_CLI_PATH));
  operations->setEncryptHooks([this](const QString&,const QJsonObject&){return QJsonObject{{"state",checkState},{"reason","Synthetic check refusal."}};},[this](const QString&,const QJsonObject&){return passphrase;});
 }
 // Uploads one file and waits until its operation ends; returns the operation.
 Operation upload(const QByteArray& bytes,const QString& name,const QJsonObject& encrypt={}) {
  const auto path=fixture(dir.path(),name,bytes);QString id;
  operations->uploadFiles({path},"test",{},[&](Error e,QJsonObject r){if(!e)id=r["operationId"].toString();else id="error:"+e.code;},"report",encrypt);
  if(!waitFor([&]{return !id.isEmpty();},10000)||id.startsWith("error:"))return {};
  HistoryStore journal(operations->databasePath());
  (void)waitFor([&]{return !journal.operation(id).active()&&operations->diagnostics()["activeOperations"].toInt()==0;},120000);
  return journal.operation(id);
 }
 QJsonObject detail(const QString& id) {HistoryStore journal(operations->databasePath());return journal.detail(id);}
 // Several files in one upload.
 Operation uploadAll(const QStringList& names) {
  QStringList paths;for(const auto& name:names)paths.append(fixture(dir.path(),name,name.toUtf8()));QString id;
  operations->uploadFiles(paths,"test",{},[&](Error e,QJsonObject r){id=e?"error:"+e.code:r["operationId"].toString();},"report");
  if(!waitFor([&]{return !id.isEmpty();},10000)||id.startsWith("error:"))return {};
  HistoryStore journal(operations->databasePath());
  (void)waitFor([&]{return !journal.operation(id).active()&&operations->diagnostics()["activeOperations"].toInt()==0;},120000);
  return journal.operation(id);
 }
};
// Serves what the provider keeps at http://127.0.0.1:<port>/f/<name> as text/html, as a link
// address would, with `extra` bytes after it; a probe on this computer may use plain http.
struct LinkServer {
 const KeepingProvider& provider;QByteArray extra;int listener=-1;quint16 port=0;std::thread thread;std::atomic_bool stop{false};
 LinkServer(const KeepingProvider& keeping,QByteArray more):provider(keeping),extra(std::move(more)) {
  listener=::socket(AF_INET,SOCK_STREAM,0);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  socklen_t size=sizeof(address);
  if(::bind(listener,reinterpret_cast<sockaddr*>(&address),size)!=0||::listen(listener,8)!=0||::getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size)!=0)return;
  port=ntohs(address.sin_port);
  thread=std::thread([this]{while(!stop){const int client=::accept(listener,nullptr,nullptr);if(client<0)break;
   QByteArray request;char chunk[4096];while(!request.contains("\r\n\r\n")){const auto n=::read(client,chunk,sizeof(chunk));if(n<=0)break;request.append(chunk,n);}
   const auto target=request.split(' ').value(1);const auto body=provider.file(QString::fromLatin1(target.mid(target.lastIndexOf('/')+1)))+extra;
   const auto reply="HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body;
   for(qsizetype sent=0;sent<reply.size();){const auto n=::send(client,reply.constData()+sent,size_t(reply.size()-sent),MSG_NOSIGNAL);if(n<=0)break;sent+=n;}
   ::close(client);}});
 }
 QString base() const {return QStringLiteral("http://127.0.0.1:%1/f/").arg(port);}
 ~LinkServer(){stop=true;::shutdown(listener,SHUT_RDWR);::close(listener);if(thread.joinable())thread.join();}
};
// A Test through SetupService whose destination test is held until `complete` is called, and
// whose encrypted-link check is held until `encryptDone` is.
struct HeldTest {
 Fixture fixture{QJsonObject{}};CredentialStore secrets;DestinationStatus status{fixture.dir.filePath("destinations.json"),*fixture.settings};
 std::function<void(Error)> complete;std::function<void(QJsonObject)> encryptDone;int checks=0;
 SetupService setup{*fixture.settings,secrets,[this](const QString&,const QJsonObject&,auto reply){complete=std::move(reply);}};
 QString id;
 HeldTest() {setup.setDestinationStatus(&status);setup.setEncryptChecker([this](const QString&,const QJsonObject&,std::shared_ptr<std::atomic_bool>,std::function<void(QJsonObject)> done){++checks;encryptDone=std::move(done);});}
 QJsonObject job() {QJsonObject out;setup.inspect(id,[&out](Error,QJsonObject value){out=value;});return out;}
};
}
// Encrypted links (#2079) through the upload pipeline: what is sent, the link, Retry and History.
class EncryptedUploadsTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase() {QVERIFY(QFileInfo(QStringLiteral(PUTGD_CLI_PATH)).isExecutable());}
 // A destination set to encrypt uploads an encrypted page under a random name; its link carries the
 // key after #, opens to the file with its name, and is what History and the clipboard copy.
 void encryptedUploadSendsOnlyThePage() {
  Fixture f({{"mode","on"}});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto bytes=QByteArray("a secret capture, not for the bucket\n");
  const auto op=f.upload(bytes,"Screen-Capture-secret.txt");
  QCOMPARE(op.stateName(),QString("completed"));const auto item=op.items.first();
  QVERIFY(item.encrypted());QCOMPARE(item.encrypt["form"].toString(),QString("inline"));QVERIFY(!item.encrypt["passphrase"].toBool());
  QVERIFY2(QRegularExpression("^[A-Za-z0-9]{12}\\.html$").match(item.candidate).hasMatch(),qPrintable(item.candidate));
  QCOMPARE(f.provider->count(),1);QVERIFY(!f.provider->file(item.candidate).contains("secret"));
  // The artifact is still the file: previews and local copies use it.
  QCOMPARE(item.artifact.name,QString("Screen-Capture-secret.txt"));
  const auto link=item.link();QVERIFY(link.startsWith(item.receipt.directUrl+"#v1."));QCOMPARE(link.size(),item.receipt.directUrl.size()+47);
  QByteArray plain;encrypt::Meta meta;const auto e=open(*f.provider,link,{},plain,meta);QVERIFY2(!e,qPrintable(e.message));
  QCOMPARE(plain,bytes);QCOMPARE(meta.name,QString("Screen-Capture-secret.txt"));QCOMPARE(meta.type,item.artifact.mime);
  QVERIFY(f.source.copiedTexts.contains(link));
  const auto record=f.detail(item.artifact.id);QCOMPARE(record["link"].toString(),link);QVERIFY(record["encrypted"].toBool());
  QVERIFY(!record["encrypt"].toObject()["page"].toObject().contains("path"));
  // The public operation keeps the private files private.
  QVERIFY(!op.publicJson()["items"].toArray()[0].toObject()["encrypt"].toObject()["page"].toObject().contains("path"));
 }
 // The upload's own choice beats the destination's, both ways.
 void perUploadChoiceOverridesTheDestination() {
  Fixture off({});PUTGD_TRY_VERIFY(off.operations->ready());
  auto op=off.upload("plain","a.txt",{{"enabled",true}});QVERIFY(op.items.first().encrypted());
  Fixture on({{"mode","on"}});PUTGD_TRY_VERIFY(on.operations->ready());
  op=on.upload("plain","b.txt",{{"enabled",false}});QVERIFY(op.items.first().encrypt.isEmpty());QCOMPARE(on.provider->file("b.txt"),QByteArray("plain"));
 }
 // Encrypting that was asked for and can't happen here never uploads the file unencrypted.
 void encryptingThatCantHappenUploadsNothing_data() {
  QTest::addColumn<QString>("check");QTest::addColumn<QString>("unavailable");QTest::addColumn<QString>("mode");QTest::addColumn<QString>("code");
  QTest::newRow("untested")<<"untested"<<""<<"on"<<"encrypt_check_required";
  QTest::newRow("check-failed")<<"failed"<<""<<"on"<<"encrypt_check_required";
  QTest::newRow("destination-type")<<"passed"<<"Proton Drive already encrypts files end to end."<<"on"<<"encrypt_unavailable";
  QTest::newRow("passphrase-not-loaded")<<"passed"<<""<<"passphrase"<<"encrypt_passphrase_locked";
 }
 void encryptingThatCantHappenUploadsNothing() {
  QFETCH(QString,check);QFETCH(QString,unavailable);QFETCH(QString,mode);QFETCH(QString,code);
  QJsonObject encrypt{{"mode",mode}};if(mode=="passphrase")encrypt["passphraseCredentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};
  Fixture f(encrypt);f.checkState=check;f.provider->encryptUnavailable=unavailable;PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload("must stay home","c.txt");
  QCOMPARE(op.items.first().uploadStatus,QString("failed"));QCOMPARE(op.items.first().error.code,code);QCOMPARE(f.provider->count(),0);
  // Once what refused it is put right, Retry encrypts it and sends the page, never the file.
  f.checkState="passed";f.provider->encryptUnavailable.clear();f.passphrase="put right";
  bool accepted=false;f.operations->retry(op.id,[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.message));accepted=true;});PUTGD_TRY_VERIFY(accepted);
  HistoryStore journal(f.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(op.id).items.first().uploadStatus,QString("uploaded"));
  const auto item=journal.operation(op.id).items.first();QVERIFY(item.encrypted());QCOMPARE(f.provider->count(),1);QVERIFY(f.provider->file(item.candidate).startsWith("<!doctype html>"));
 }
 // #2118: an upload that can't encrypt because the destination's check hasn't passed names the
 // destination, says why, and points at Test upload and link; one that asks to encrypt from
 // putgd-cli or MCP is refused before anything is sent, and none is once the check passed.
 void checkRequiredSaysWhyAndWhatToDo_data() {
  QTest::addColumn<QString>("check");QTest::addColumn<QString>("says");
  QTest::newRow("untested")<<"untested"<<"needs one passed Test upload and link before it can encrypt. It checks that encrypted pages open over https. Select it in Settings → Destinations.";
  QTest::newRow("changed")<<"changed"<<"changed after its encrypted-page check. Select Test upload and link in Settings → Destinations again.";
  QTest::newRow("failed")<<"failed"<<"didn't pass the encrypted-page check: Synthetic check refusal. Fix that, then select Test upload and link in Settings → Destinations.";
 }
 void checkRequiredSaysWhyAndWhatToDo() {
  QFETCH(QString,check);QFETCH(QString,says);
  Fixture f({{"mode","on"}});f.checkState=check;PUTGD_TRY_VERIFY(f.operations->ready());
  const auto label=f.settings->snapshot().profiles.value("test").value("label").toString();QVERIFY(!label.isEmpty());
  const auto refusal=f.operations->encryptRefusal(QString::fromLatin1(OperationService::fileManagerAction),"test");
  QCOMPARE(refusal.code,QString("encrypt_check_required"));QCOMPARE(refusal.message,label+" "+says+" Or upload it without encryption.");
  const auto op=f.upload("must stay home","w.txt");
  QCOMPARE(op.items.first().error.code,QString("encrypt_check_required"));QCOMPARE(op.items.first().error.message,label+" "+says+" Or turn off Encrypt to upload it as an ordinary link.");
  QCOMPARE(f.provider->count(),0);
  f.checkState="passed";QVERIFY(!f.operations->encryptRefusal(QString::fromLatin1(OperationService::fileManagerAction),"test"));
 }
 // A private upload gets no link, and an encrypted file opens only from its link: it is refused
 // before encrypting, never sent, and the editor's toggle says why.
 void privateUploadIsNeverEncrypted() {
  Fixture f({{"mode","on"}},"private");PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload("no link to open it","p.txt");const auto item=op.items.first();
  QCOMPARE(item.uploadStatus,QString("failed"));QCOMPARE(item.error.code,QString("encrypt_needs_link"));QVERIFY(item.error.message.contains("Private upload"));
  QVERIFY(item.encrypt.isEmpty());QCOMPARE(f.provider->count(),0);QCOMPARE(f.provider->storage.uploads.load(),0);
  // Encrypt off, the same upload goes out privately.
  const auto plain=f.upload("no link to open it","q.txt",{{"enabled",false}});QCOMPARE(plain.items.first().uploadStatus,QString("uploaded"));QCOMPARE(f.provider->count(),1);
 }
 // With a passphrase, the link alone opens nothing.
 void passphraseIsNeededBesideTheLink() {
  Fixture f({{"mode","passphrase"},{"passphraseCredentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}});
  f.passphrase="correct horse";PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload("behind two locks","d.txt");const auto item=op.items.first();
  QVERIFY(item.encrypted());QVERIFY(item.encrypt["passphrase"].toBool());
  QByteArray plain;encrypt::Meta meta;
  QCOMPARE(open(*f.provider,item.link(),{},plain,meta).code,QString("encrypt_passphrase_required"));
  QCOMPARE(open(*f.provider,item.link(),"wrong",plain,meta).code,QString("encrypt_passphrase"));
  QVERIFY(!open(*f.provider,item.link(),"correct horse",plain,meta));QCOMPARE(plain,QByteArray("behind two locks"));
  QVERIFY(!QJsonDocument(op.toJson()).toJson().contains("correct horse"));
 }
 // A passphrase that came with the request (putgd-cli upload --passphrase-file) is never kept:
 // a Retry that still has to encrypt refuses rather than encrypt without it.
 void retryNeverEncryptsWithoutTheRequestsPassphrase() {
  Fixture f({{"mode","on"}});f.checkState="untested";PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload("needs its passphrase","p.txt",{{"enabled",true},{"passphrase","given once"}});
  QCOMPARE(op.items.first().error.code,QString("encrypt_check_required"));QCOMPARE(op.encrypt["passphraseFrom"].toString(),QString("request"));
  QVERIFY(!QJsonDocument(op.toJson()).toJson().contains("given once"));
  f.checkState="passed";bool accepted=false;f.operations->retry(op.id,[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.message));accepted=true;});PUTGD_TRY_VERIFY(accepted);
  HistoryStore journal(f.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(op.id).items.first().error.code,QString("encrypt_passphrase_missing"));
  QCOMPARE(f.provider->count(),0);
 }
 // The Test check's requests carry the destination's settings revision like any other: a worker
 // refuses one without it, so the check never sends such a request (#2079, lab d69).
 // The destination the production check builds carries the accepted revision, and the provider
 // sees it on every request; an upload that journals (SFTP) gets its checkpoints answered.
 void encryptCheckSendsTheRevisionAndAnswersCheckpoints() {
  KeepingProvider provider;provider.requireCheckpoint=true;QTemporaryDir dir;
  const QJsonObject profile{{"provider","opaque"},{"settings",QJsonObject{{"workspace","w"}}},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}};
  const auto destination=encryptCheckDestination("test",profile,"revision-7");
  QCOMPARE(destination.profileRevision,QString("revision-7"));QCOMPARE(destination.profileId,QString("test"));QCOMPARE(destination.providerId,QString("opaque"));
  const auto result=runEncryptCheck(provider,destination,dir.filePath("encrypt-check"),QStringLiteral(PUTGD_CLI_PATH),{});
  // Its link (an .invalid address) can't be opened, so the check fails after the upload.
  QCOMPARE(result["state"].toString(),QString("failed"));QCOMPARE(result["reason"].toString(),QString("Couldn't open the test page's link."));
  QCOMPARE(provider.storage.uploads.load(),1);QCOMPARE(provider.revisionsSeen,QStringList({"revision-7"}));QVERIFY(provider.checkpointAnswered);
 }
 // The probe passes only for exactly the page sent: one byte more, or more than any page, fails.
 void encryptCheckNeedsExactlyThePage_data() {
  QTest::addColumn<QByteArray>("extra");QTest::addColumn<QString>("state");
  QTest::newRow("exact")<<QByteArray()<<"passed";QTest::newRow("one byte more")<<QByteArray("x")<<"failed";QTest::newRow("larger than any page")<<QByteArray(2*1024*1024,'x')<<"failed";
 }
 void encryptCheckNeedsExactlyThePage() {
  QFETCH(QByteArray,extra);QFETCH(QString,state);
  KeepingProvider provider;QTemporaryDir dir;LinkServer server(provider,extra);QVERIFY(server.port);provider.linkBase=server.base();
  const QJsonObject profile{{"provider","opaque"},{"settings",QJsonObject{{"workspace","w"}}},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}};
  const auto result=runEncryptCheck(provider,encryptCheckDestination("test",profile,"revision-7"),dir.filePath("encrypt-check"),QStringLiteral(PUTGD_CLI_PATH),{});
  QCOMPARE(result["state"].toString(),state);
  if(state=="failed")QCOMPARE(result["reason"].toString(),QString("The destination changed the test page's bytes, so an encrypted page can't be checked."));
 }
 // A Test whose destination changed while it ran checks nothing for encrypted links: untested.
 void encryptCheckSkipsAConnectionChangedDuringTheTest() {
  HeldTest t;PUTGD_TRY_VERIFY(t.fixture.operations->ready());
  t.setup.start("testDestination","test",t.fixture.settings->revision(),{},[&](Error e,QJsonObject r){QVERIFY2(!e,qPrintable(e.message));t.id=r["setupId"].toString();});
  QVERIFY(t.complete);
  auto doc=t.fixture.settings->document();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();
  profile["settings"]=QJsonObject{{"workspace","another-workspace"}};profiles[0]=profile;doc["profiles"]=profiles;QVERIFY(t.fixture.settings->save(doc,t.fixture.settings->revision()));
  t.complete({});
  PUTGD_TRY_COMPARE(t.job()["state"].toString(),QString("completed"));
  QCOMPARE(t.checks,0);QCOMPARE(t.job()["result"].toObject()["encrypt"].toObject()["state"].toString(),QString("untested"));
 }
 // A Cancel during the encrypted-link check, even one its answer doesn't show, ends the Test
 // cancelled; the connection it checked keeps its pass.
 void cancelDuringTheEncryptCheckCancelsTheTest() {
  HeldTest t;PUTGD_TRY_VERIFY(t.fixture.operations->ready());
  t.setup.start("testDestination","test",t.fixture.settings->revision(),{},[&](Error e,QJsonObject r){QVERIFY2(!e,qPrintable(e.message));t.id=r["setupId"].toString();});
  QVERIFY(t.complete);t.complete({});PUTGD_TRY_VERIFY(t.encryptDone);
  bool asked=false;t.setup.cancel(t.id,[&](Error,QJsonObject r){asked=r["cancelRequested"].toBool();});QVERIFY(asked);
  t.encryptDone({{"state","passed"},{"cleanup","removed"}});
  PUTGD_TRY_COMPARE(t.job()["state"].toString(),QString("failed"));
  const auto job=t.job();QCOMPARE(job["error"].toObject()["code"].toString(),QString("cancelled"));QCOMPARE(job["result"].toObject()["encrypt"].toObject()["state"].toString(),QString("cancelled"));
  QCOMPARE(t.status.publicJson()["test"].toObject()["state"].toString(),QString("passed"));
 }
 void encryptCheckNeedsTheSettingsRevision() {
  KeepingProvider provider;QTemporaryDir dir;
  const Destination destination{"opaque","test",{},QJsonObject{{"workspace","w"}},QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}};
  const auto result=runEncryptCheck(provider,destination,dir.filePath("encrypt-check"),QStringLiteral(PUTGD_CLI_PATH),{});
  QCOMPARE(result["state"].toString(),QString("failed"));QCOMPARE(provider.storage.checks.load(),0);QCOMPARE(provider.storage.uploads.load(),0);
 }
 // A data file in flight when put.gd stopped is reconciled on Retry, never sent blindly again.
 void recoveryMarksAnInFlightDataFileUnknown() {
  QTemporaryDir dir;const auto path=dir.filePath("history.sqlite");
  Operation op;op.id=QUuid::createUuid().toString(QUuid::WithoutBraces);op.restore(operationState("running"));op.settings=Settings::fromValidated(Settings::defaults());
  OperationItem item;item.artifact.id="encrypted-item";item.uploadStatus="stored";item.publicationId="page";
  item.encrypt={{"status","encrypted"},{"form","split"},{"key","v1.x"},{"data",QJsonObject{{"uploadStatus","uploading"},{"candidate","abc.bin"},{"publicationId","data"}}}};
  op.items={item};
  {HistoryStore store(path);QVERIFY(!store.saveOperation(op));}
  HistoryStore reopened(path);Error error;reopened.recover(&error);QVERIFY(!error);
  const auto recovered=reopened.operation(op.id).items.first();
  QCOMPARE(recovered.encrypt["data"].toObject()["uploadStatus"].toString(),QString("unknown"));QCOMPARE(recovered.uploadStatus,QString("unknown"));
 }
 // The page and its data file share one random stem.
 void splitNamesShareOneStem() {
  Fixture f({{"mode","on"}});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto item=f.upload(pattern(encrypt::inlineLimit+1),"v.webm").items.first();
  QCOMPARE(item.encrypt["data"].toObject()["candidate"].toString(),item.candidate.chopped(5)+".bin");
 }
 // Retry sends the same encrypted page, so a link someone already has keeps working.
 void retryUploadsTheSameCiphertext() {
  Fixture f({{"mode","on"}});f.provider->failNext=1;PUTGD_TRY_VERIFY(f.operations->ready());
  auto op=f.upload("retry me","e.txt");auto item=op.items.first();
  QCOMPARE(item.uploadStatus,QString("failed"));QVERIFY(item.encrypted());
  const auto key=item.encrypt["key"].toString(),page=item.encrypt["page"].toObject()["sha256"].toString(),name=item.candidate;
  bool accepted=false;f.operations->retry(op.id,[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.message));accepted=true;});PUTGD_TRY_VERIFY(accepted);
  HistoryStore journal(f.operations->databasePath());PUTGD_TRY_COMPARE(journal.operation(op.id).items.first().uploadStatus,QString("uploaded"));
  item=journal.operation(op.id).items.first();
  QCOMPARE(item.encrypt["key"].toString(),key);QCOMPARE(item.encrypt["page"].toObject()["sha256"].toString(),page);QCOMPARE(item.candidate,name);
  QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(f.provider->file(name),QCryptographicHash::Sha256).toHex()),page);
 }
 // Over 64 MB: the page and its data file side by side; the page fetches ./NAME.bin.
 void largeFilesEncryptAsPageAndDataFile() {
  Fixture f({{"mode","on"}});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto bytes=pattern(encrypt::inlineLimit+12345);
  const auto op=f.upload(bytes,"recording.webm");const auto item=op.items.first();
  QCOMPARE(op.stateName(),QString("completed"));QVERIFY(item.encrypted());QCOMPARE(item.encrypt["form"].toString(),QString("split"));
  const auto data=item.encrypt["data"].toObject();QCOMPARE(data["uploadStatus"].toString(),QString("uploaded"));
  QVERIFY(QRegularExpression("^[A-Za-z0-9]{12}\\.bin$").match(data["candidate"].toString()).hasMatch());
  QCOMPARE(f.provider->count(),2);QVERIFY(f.provider->file(item.candidate).size()<20000);
  QByteArray plain;encrypt::Meta meta;const auto e=open(*f.provider,item.link(),{},plain,meta);QVERIFY2(!e,qPrintable(e.message));QCOMPARE(plain,bytes);
 }
 // An encrypted request that encrypting can't take is never sent unencrypted: it fails before anything
 // goes out, and Retry keeps it encrypted (#2079, the design's limits).
 void fileTooLargeToEncryptIsNeverSent() {
  Fixture f({{"mode","on"}});f.operations->setEncryptLimitsForTesting(64,128);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload(pattern(129),"huge.bin");const auto item=op.items.first();
  QCOMPARE(item.uploadStatus,QString("failed"));QCOMPARE(item.error.code,QString("encrypt_too_large"));QVERIFY(item.error.message.contains("2 GB"));
  QVERIFY(item.encrypt.isEmpty());QCOMPARE(f.provider->count(),0);
  bool accepted=false;f.operations->retry(op.id,[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.message));accepted=true;});PUTGD_TRY_VERIFY(accepted);
  // The Retry itself finishes, and fails the same way, before anything is asked of the destination.
  HistoryStore journal(f.operations->databasePath());
  PUTGD_TRY_VERIFY(!journal.operation(op.id).active()&&f.operations->diagnostics()["activeOperations"].toInt()==0);
  // A failed file never leaves its operation completed (put.gd reports it partial).
  QVERIFY(journal.operation(op.id).stateName()!="completed");QCOMPARE(journal.operation(op.id).items.first().error.code,QString("encrypt_too_large"));
  QCOMPARE(f.provider->count(),0);QCOMPARE(f.provider->storage.uploads.load(),0);QCOMPARE(f.provider->storage.checks.load(),0);
  // Turning Encrypt off for a new upload is how it goes out as an ordinary link.
  const auto plain=f.upload(pattern(129),"huge-again.bin",{{"enabled",false}});QCOMPARE(plain.items.first().uploadStatus,QString("uploaded"));QCOMPARE(f.provider->count(),1);
 }
 void twoFilesWhereLinksCantServeThemIsNeverSent() {
  Fixture f({{"mode","on"}});f.provider->encryptSplit=false;f.operations->setEncryptLimitsForTesting(64,1LL<<30);PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload(pattern(65),"big.bin");const auto item=op.items.first();
  QCOMPARE(item.uploadStatus,QString("failed"));QCOMPARE(item.error.code,QString("encrypt_split_unavailable"));QVERIFY(item.encrypt.isEmpty());QCOMPARE(f.provider->count(),0);
  QCOMPARE(f.provider->storage.uploads.load(),0);QCOMPARE(f.provider->storage.checks.load(),0);QVERIFY(op.stateName()!="completed");
  // The same file fits one page where the limit allows it, and encrypts.
  const auto small=f.upload(pattern(64),"fits.bin");QVERIFY(small.items.first().encrypted());QCOMPARE(f.provider->count(),1);
 }
 // Encrypting jobs remove only files they made (#2079). The resident creates both outputs itself and
 // removes a path only while it is still that file, so a file already there, or one that took
 // the path's place while the helper ran, survives a refusal, a crash or a Cancel.
 void foreignFileAtAnOutputPathSurvives() {
  QTemporaryDir dir;const auto marker=dir.filePath("started");
  const auto helper=encryptHelper(dir,"touch '"+marker+"'\necho '{\"ok\":false}'\n");
  const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);QVERIFY(!fixture(dir.path(),id,"not ours").isEmpty());
  EncryptJob job(helper);job.setOutputIdsForTesting(id,{});
  const auto result=job.run(input(dir),dir.path(),"inline","a.html",{},{});
  QCOMPARE(result.error.code,QString("encrypt_failed"));QVERIFY(!QFileInfo::exists(marker));QCOMPARE(read(dir.filePath(id)),QByteArray("not ours"));
 }
 void cancelLeavesAFileThatTookAnOutputPath_data() {QTest::addColumn<bool>("swapped");QTest::newRow("own file")<<false;QTest::newRow("path taken over")<<true;}
 void cancelLeavesAFileThatTookAnOutputPath() {
  QFETCH(bool,swapped);
  QTemporaryDir dir;const auto marker=dir.filePath("started");
  // The helper (or a racer while it runs) replaces the page path with another file, then hangs until killed.
  const QString swap=swapped?"printf 'not ours' > \"$page.new\"\nmv -f \"$page.new\" \"$page\"\n":"";
  const auto helper=encryptHelper(dir,swap+"touch '"+marker+"'\nexec sleep 30\n");
  const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);EncryptJob job(helper);job.setOutputIdsForTesting(id,{});
  auto cancelled=std::make_shared<std::atomic_bool>(false);
  std::thread canceller([&]{for(int i=0;i<2000&&!QFileInfo::exists(marker);++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));cancelled->store(true);});
  const auto result=job.run(input(dir),dir.path(),"inline","a.html",{},{},cancelled);canceller.join();
  QCOMPARE(result.error.code,QString("cancelled"));
  if(swapped)QCOMPARE(read(dir.filePath(id)),QByteArray("not ours"));else QVERIFY(!QFileInfo::exists(dir.filePath(id)));
 }
 // A Cancel while the helper's output is checked is a cancel, not a mismatch, and what it wrote goes.
 void cancelWhileTheOutputIsCheckedIsACancel() {
  QTemporaryDir dir;
  const auto helper=encryptHelper(dir,"printf 'encrypted' >> \"$page\"\necho '{\"ok\":true,\"page\":{\"bytes\":6,\"sha256\":\"mismatch\"}}'\n");
  const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);EncryptJob job(helper);job.setOutputIdsForTesting(id,{});
  auto cancelled=std::make_shared<std::atomic_bool>(false);job.setVerifyingForTesting([cancelled]{cancelled->store(true);});
  const auto result=job.run(input(dir),dir.path(),"inline","a.html",{},{},cancelled);
  QCOMPARE(result.error.code,QString("cancelled"));QVERIFY(result.page.path.isEmpty());QVERIFY(!QFileInfo::exists(dir.filePath(id)));
  // Without the Cancel the same answer is a mismatch.
  EncryptJob plain(helper);plain.setOutputIdsForTesting(QUuid::createUuid().toString(QUuid::WithoutBraces),{});
  // The mismatch itself, not a later check such as the key, is what refuses it.
  const auto mismatch=plain.run(input(dir),dir.path(),"inline","a.html",{},{},std::make_shared<std::atomic_bool>(false)).error;
  QCOMPARE(mismatch.code,QString("encrypt_failed"));QCOMPARE(mismatch.message,QString("The encrypted file didn't match what putgd-cli reported, so the upload didn't start."));
 }
 // Forget key: History keeps no way to open it, and offers no link.
 void forgetKeyLeavesNoLink() {
  Fixture f({{"mode","on"}});PUTGD_TRY_VERIFY(f.operations->ready());
  const auto op=f.upload("forget me","g.txt");const auto id=op.items.first().artifact.id;
  HistoryStore journal(f.operations->databasePath());QVERIFY(!journal.forgetEncryptKey(id));
  const auto record=f.detail(id);QVERIFY(!record["linkAvailable"].toBool());QVERIFY(record["link"].toString().isEmpty());QVERIFY(record["encrypt"].toObject()["keyForgotten"].toBool());
  QVERIFY(journal.operation(op.id).items.first().encrypt["key"].toString().isEmpty());
  // A writer that read the operation before Forget key can't put the key back.
  QVERIFY(!journal.saveOperation(op));
  QVERIFY(journal.operation(op.id).items.first().encrypt["key"].toString().isEmpty());QVERIFY(f.detail(id)["link"].toString().isEmpty());
  QCOMPARE(journal.forgetEncryptKey("missing-id").code,QString("history_not_found"));
  // The mark lives in the operation's journal: removing the History record doesn't lift it, and
  // the other file of the same upload keeps its key.
  const auto pair=f.uploadAll({"one.txt","two.txt"});QCOMPARE(pair.items.size(),2);
  const auto first=pair.items[0].artifact.id,second=pair.items[1].artifact.id;const auto secondKey=pair.items[1].encrypt["key"].toString();QVERIFY(!secondKey.isEmpty());
  QVERIFY(!journal.forgetEncryptKey(first));
  int removed=0;QVERIFY(!journal.forget({first},{},&removed));QCOMPARE(removed,1);
  QVERIFY(!journal.saveOperation(pair));
  const auto saved=journal.operation(pair.id);
  QVERIFY(saved.items[0].encrypt["key"].toString().isEmpty());QVERIFY(saved.items[0].encrypt["keyForgotten"].toBool());
  QCOMPARE(saved.items[1].encrypt["key"].toString(),secondKey);
  const auto plain=f.upload("not encrypted","h.txt",{{"enabled",false}});QCOMPARE(journal.forgetEncryptKey(plain.items.first().artifact.id).code,QString("history_not_encrypted"));
 }
};
QTEST_GUILESS_MAIN(EncryptedUploadsTest)
#include "test_encrypted_uploads.moc"
