#include "app/DiagnosticsBundle.h"
#include "app/LocalServer.h"
#include "core/Log.h"
#include "core/ZipWriter.h"
#include "fixtures/U2Fixtures.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include "fixtures/Waits.h"
using namespace putgd;
namespace {
const QStringList canaries{"synthetic-private-secret","synthetic-private-token","synthetic-private-clipboard","putgd/synthetic-private-reference"};
const QStringList names{"synthetic-private-bucket","synthetic-private-endpoint.example.invalid","synthetic-private-cdn.example.invalid","Synthetic Private Label"};
class DeniedProvider final:public StorageProvider {
public:
 ProviderCapabilities capabilities(const Destination&) const override {return {true,true,true,true,true,true,true,255,"Remove the recorded object"};}
 QString serializationKey(const Destination&,const QJsonObject& reference,const QString& name) const override {return reference.isEmpty()?name:reference["key"].toString();}
 Error validateSettings(const QJsonObject& settings) const override {return settings["workspace"].isString()?Error{}:Error{"config_invalid","A workspace is required",{}};}
 void checkName(const Destination&,const QString& name,std::function<void(NameCheck)> reply, std::shared_ptr<std::atomic_bool> cancellation = {}) override {reply({"absent",{{"key","uploads/"+name}},{},{}});}
 void upload(const UploadRequest&,std::function<void(UploadResult)> reply) override {
  reply({"failed",{},{"s3_permission_denied","S3 denied the request (HTTP 403).",{{"httpStatus",403},{"serviceCode","AccessDenied"},{"requestStage","upload_put"},
   {"requestId","SYNTHREQ0123ABCD"},{"extendedRequestId","synthetic+Extended/Id2="}}}});
 }
 void stat(const StorageReceipt&,std::function<void(StatResult)> reply) override {reply({"missing",{},{}});}
 void reconcile(const UploadRequest&,std::function<void(StatResult)> reply) override {reply({"missing",{},{}});}
 void remove(const DeleteRequest&,DurableCheckpoint,std::function<void(DeleteResult)> reply) override {reply({"removed",{},{}});}
};
// Lists and extracts a package with Python's zipfile, which checks every CRC.
QJsonObject unzip(const QString& path) {
 QProcess python;python.start("python3",{"-I","-c",
  "import base64,json,sys,zipfile\n"
  "z=zipfile.ZipFile(sys.argv[1])\n"
  "assert z.testzip() is None\n"
  "print(json.dumps({i.filename:base64.b64encode(z.read(i)).decode() for i in z.infolist()}))",path});
 if(!python.waitForFinished(30000)||python.exitCode()!=0)return {};
 QJsonObject result;const auto files=QJsonDocument::fromJson(python.readAllStandardOutput()).object();
 for(auto it=files.begin();it!=files.end();++it)result[it.key()]=QString::fromUtf8(QByteArray::fromBase64(it.value().toString().toLatin1()));
 return result;
}
}
class DiagnosticsBundleContract final:public QObject {
 Q_OBJECT
 QTemporaryDir dir_;
 std::unique_ptr<ProviderRegistry> registry_;std::unique_ptr<SettingsService> settings_;std::unique_ptr<test::Source> source_;std::unique_ptr<OperationService> operations_;
 QString failedId_;
 DiagnosticsBundles::Context context() const {
  DiagnosticsBundles::Context context;
  context.databasePath=operations_->databasePath();context.logsDirectory=dir_.filePath("state/logs");context.defaultFolder=dir_.filePath("archive/diagnostics");
  context.home=dir_.path();context.roots.insert(dir_.filePath("data"),"<data>");context.roots.insert(dir_.filePath("state"),"<state>");context.roots.insert(dir_.filePath("archive"),"<archive>");
  auto settings=settings_->document();
  auto profiles=settings["profiles"].toArray();
  profiles.append(QJsonObject{{"id","synthetic-s3"},{"label","Synthetic Private Label"},{"provider","s3"},
   {"settings",QJsonObject{{"bucket","synthetic-private-bucket"},{"endpoint","https://synthetic-private-endpoint.example.invalid"},{"publicUrlBase","https://synthetic-private-cdn.example.invalid"},{"region","us-east-2"},{"storageClass","STANDARD"}}},
   {"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-private-reference"}}}});
  settings["profiles"]=profiles;context.settings=settings;
  context.diagnostics=operations_->diagnostics();
  context.environment={{"platform","offscreen"},{"trayAvailable",false},{"screens",QJsonArray{QJsonObject{{"scale",1.5}}}}};
  context.coredumpctl="-";
  return context;
 }
 QJsonObject build(DiagnosticsBundles::Context context,BundleRequest request,Error& error) {return DiagnosticsBundles::build(context,request,error);}
private slots:
 void initTestCase() {
  QVERIFY(dir_.isValid());
  logging::open(dir_.filePath("state/logs"));
  registry_=std::make_unique<ProviderRegistry>();registry_->add("opaque",std::make_shared<DeniedProvider>());
  auto* registry=registry_.get();
  settings_=std::make_unique<SettingsService>(dir_.filePath("config.json"),[registry](const auto& id,const auto& value){return registry->validate(id,value);});
  QVERIFY(settings_->save(test::configuration(dir_.path()),{}));
  source_=std::make_unique<test::Source>();source_->payload.kind="text";source_->payload.bytes="synthetic-private-clipboard text";
  operations_=std::make_unique<OperationService>(dir_.filePath("data"),*settings_,*registry_,*source_);PUTGD_TRY_VERIFY(operations_->ready());
  bool accepted=false;operations_->invoke("clipboard",[&](Error error,QJsonObject result){QVERIFY(!error);failedId_=result["operationId"].toString();accepted=true;});
  PUTGD_TRY_VERIFY(accepted);
  PUTGD_TRY_VERIFY_WITH_TIMEOUT((logging::flush(),QDir(dir_.filePath("state/logs")).entryList({"putgd-*.jsonl"}).size()>0&&[&]{
   QByteArray bytes;
   for(const auto& name:QDir(dir_.filePath("state/logs")).entryList({"putgd-*.jsonl"})){QFile file(dir_.filePath("state/logs/"+name));if(file.open(QIODevice::ReadOnly))bytes+=file.readAll();}
   return bytes.contains("Operation finished");}()),10000);
  // Records a package must never carry, and names it must hash.
  logging::warning(logS3(),"PUT https://synthetic-private-bucket.s3.example.invalid/uploads/x.png?X-Amz-Signature=synthetic-private-token failed",
   {{"operationId",failedId_},{"secretKey","synthetic-private-secret"},{"bucket","synthetic-private-bucket"}});
  logging::flush();
 }
 void cleanupTestCase() {operations_->shutdown();logging::close();}
 void zipWriterProducesValidArchives() {
  ZipWriter zip;zip.add("a.txt","hello hello hello hello");zip.add("dir/ünicode.json",QByteArray(100000,'x'));zip.add("empty","");zip.add("random",QCryptographicHash::hash("seed",QCryptographicHash::Sha512).repeated(10));
  const auto bytes=zip.finish();QFile file(dir_.filePath("test.zip"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(bytes);file.close();
  const auto files=unzip(file.fileName());
  QCOMPARE(files["a.txt"].toString(),QString("hello hello hello hello"));QCOMPARE(files["dir/ünicode.json"].toString().size(),100000);QCOMPARE(files["empty"].toString(),QString());
  QVERIFY(bytes.size()<60000);
  QCOMPARE(ZipWriter::crc32("123456789"),0xcbf43926u);
 }
 // systemd-coredump's text traces for put.gd executables (#124 option B):
 // modules and frames only, never the command line, host or core file.
 void coredumpTracesWithoutCommandLines() {
  const auto script=dir_.filePath("fake-coredumpctl");
  QFile file(script);QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("#!/bin/sh\nfor a in \"$@\"; do case \"$a\" in\n"
   "list) echo '[{\"time\":1760000000000000,\"pid\":4242,\"uid\":1000,\"gid\":1000,\"sig\":11,\"corefile\":\"present\",\"exe\":\"/usr/bin/putgd-ui\",\"size\":1},"
   "{\"time\":1760000001000000,\"pid\":7,\"sig\":6,\"corefile\":\"present\",\"exe\":\"/usr/bin/firefox\"}]'; exit 0;;\n"
   "info) printf '%s\\n' '           PID: 4242 (putgd-ui)' '  Command Line: putgd-ui --edit-image /tmp/synthetic-private-name.png' "
   "'      Hostname: synthetic-private-host' '       Message: Process 4242 (putgd-ui) dumped core.' '' "
   "'                Module libQt6Core.so.6 with build-id 0123456789abcdef0123456789abcdef01234567' "
   "'                Stack trace of thread 4242:' '                #0  0x00007f0000001234 raise (libc.so.6 + 0x1234)' "
   "'                #1  0x000055d000005678 n/a (/usr/bin/putgd-ui + 0x5678)'; exit 0;;\n"
   "esac; done\nexit 2\n");
  file.close();QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  PackageRedactor redactor(false);
  const auto report=DiagnosticsBundles::coredumps(script,QDateTime::currentDateTimeUtc().addDays(-7),redactor);
  QVERIFY(report["available"].toBool());
  const auto entries=report["entries"].toArray();QCOMPARE(entries.size(),1);
  const auto entry=entries[0].toObject();
  QCOMPARE(entry["pid"].toInteger(),4242);QCOMPARE(entry["signal"].toInt(),11);QCOMPARE(entry["executable"].toString(),QString("/usr/bin/putgd-ui"));
  const auto stack=entry["stack"].toString();
  QVERIFY2(stack.contains("#0  0x00007f0000001234 raise")&&stack.contains("build-id 0123456789abcdef0123456789abcdef01234567"),qPrintable(stack));
  for(const auto* hidden:{"Command Line","synthetic-private-host","synthetic-private-name","Hostname"})QVERIFY2(!QJsonDocument(report).toJson().contains(hidden),hidden);
  // In a package; and no coredumpctl at all is reported, not an error.
  auto context=this->context();context.coredumpctl=script;
  Error error;BundleRequest request;request.output=dir_.filePath("coredump-package.zip");
  const auto result=build(context,request,error);QVERIFY2(!error,qPrintable(error.message));
  const auto packaged=QJsonDocument::fromJson(unzip(result["path"].toString())["coredumps.json"].toString().toUtf8()).object();
  QCOMPARE(packaged["entries"].toArray().size(),1);QVERIFY(packaged.contains("since"));
  QCOMPARE(DiagnosticsBundles::coredumps(dir_.filePath("missing-coredumpctl"),QDateTime::currentDateTimeUtc(),redactor)["available"].toBool(),false);
 }
 void packageHoldsTheFailedOperationWithoutPrivateData() {
  Error error;BundleRequest request;request.operationId=failedId_;
  const auto result=build(context(),request,error);QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(result["redaction"].toString(),QString("hashed"));QVERIFY(result["defaultFolder"].toBool());
  QVERIFY(result["path"].toString().startsWith(dir_.filePath("archive/diagnostics/putgd-diagnostics-")));
  QCOMPARE(QFileInfo(result["path"].toString()).permissions()&(QFile::ReadGroup|QFile::ReadOther),QFile::Permissions{});
  const auto files=unzip(result["path"].toString());
  QCOMPARE(QStringList(files.keys()),(QStringList{"config.redacted.json","coredumps.json","diagnostics.json","environment.json","logs/putgd.jsonl","manifest.json","operations.json"}));
  QByteArray everything;for(auto it=files.begin();it!=files.end();++it)everything+=it.value().toString().toUtf8();
  for(const auto& canary:canaries)QVERIFY2(!everything.contains(canary.toUtf8()),qPrintable(canary));
  for(const auto& name:names)QVERIFY2(!everything.contains(name.toUtf8()),qPrintable(name));
  QVERIFY(!everything.contains(dir_.path().toUtf8()));
  // The manifest's hashes match what was packed.
  const auto manifest=QJsonDocument::fromJson(files["manifest.json"].toString().toUtf8()).object();
  QCOMPARE(manifest["schemaVersion"].toInt(),1);QCOMPARE(manifest["redaction"].toString(),QString("hashed"));QCOMPARE(manifest["scope"].toObject()["operationId"].toString(),failedId_);
  for(const auto& value:manifest["files"].toArray()) {
   const auto entry=value.toObject();const auto bytes=files[entry["path"].toString()].toString().toUtf8();
   QCOMPARE(entry["sha256"].toString(),QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()));
   QCOMPARE(entry["bytes"].toInteger(),qint64(bytes.size()));
  }
  // The operation with its error, HTTP status, request IDs and attempts.
  const auto operations=QJsonDocument::fromJson(files["operations.json"].toString().toUtf8()).array();QCOMPARE(operations.size(),1);
  const auto op=operations[0].toObject();QCOMPARE(op["id"].toString(),failedId_);
  const auto item=op["items"].toArray()[0].toObject();
  QCOMPARE(item["error"].toObject()["code"].toString(),QString("s3_permission_denied"));
  QCOMPARE(item["error"].toObject()["details"].toObject()["httpStatus"].toInt(),403);
  QCOMPARE(item["error"].toObject()["details"].toObject()["requestId"].toString(),QString("SYNTHREQ0123ABCD"));
  QCOMPARE(item["attempts"].toInt(),1);QCOMPARE(item["mime"].toString().left(5),QString("text/"));
  QVERIFY(item["name"].toString().startsWith("<name:"));QVERIFY(item["objectKey"].toString().startsWith("<key:"));
  // The same name gets the same token everywhere in the package.
  QVERIFY(files["logs/putgd.jsonl"].toString().contains(item["candidate"].toString()));
  // Its log lines, with the provider error; the environment; redacted config.
  const auto logs=files["logs/putgd.jsonl"].toString();
  QVERIFY(logs.contains("Upload failed"));QVERIFY(logs.contains("SYNTHREQ0123ABCD"));QVERIFY(logs.contains(failedId_));
  const auto config=QJsonDocument::fromJson(files["config.redacted.json"].toString().toUtf8()).object();
  QCOMPARE(config["archive"].toObject()["root"].toString(),QString("<home>/archive"));
  const auto profile=config["profiles"].toArray().last().toObject();
  QCOMPARE(profile["credentials"].toObject(),(QJsonObject{{"mode","keychain"}}));
  QVERIFY(profile["settings"].toObject()["bucket"].toString().startsWith("<bucket:"));QCOMPARE(profile["settings"].toObject()["region"].toString(),QString("us-east-2"));
  const auto environment=QJsonDocument::fromJson(files["environment.json"].toString().toUtf8()).object();
  QVERIFY(environment.contains("osRelease"));QVERIFY(environment.contains("memoryTotalBytes"));QVERIFY(environment.contains("dataFreeBytes"));QCOMPARE(environment["platform"].toString(),QString("offscreen"));
  QVERIFY(QJsonDocument::fromJson(files["diagnostics.json"].toString().toUtf8()).object().contains("logging"));
 }
 void includeNamesKeepsNamesButNeverSecrets() {
  Error error;BundleRequest request;request.includeNames=true;request.output=dir_.filePath("named.zip");
  const auto result=build(context(),request,error);QVERIFY(!error);QVERIFY(!result["defaultFolder"].toBool());QCOMPARE(result["path"].toString(),dir_.filePath("named.zip"));
  QByteArray everything;const auto files=unzip(result["path"].toString());for(auto it=files.begin();it!=files.end();++it)everything+=it.value().toString().toUtf8();
  for(const auto& canary:canaries)QVERIFY2(!everything.contains(canary.toUtf8()),qPrintable(canary));
  QVERIFY(everything.contains("synthetic-private-bucket"));
  // An existing file is never replaced.
  QCOMPARE(build(context(),request,error)["path"].toString(),QString());QCOMPARE(error.code,QString("diagnostics_output_exists"));
 }
 // #473: a completed upload whose link never reached the clipboard belongs in a
 // window package, with the failures and the same redaction; a copied one does not.
 void windowPackagesCarryUploadsWhoseLinkWasNotCopied() {
  HistoryStore store(operations_->databasePath());QVERIFY(!store.error());
  const auto save=[&](const QString& id,const QString& copyStatus){Operation op;op.id=id;op.action="clipboard";op.state="completed";op.phase="completed";op.copyStatus=copyStatus;op.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);OperationItem item;item.artifact.id=id+"-item";item.artifact.mime="text/plain";item.artifact.name="synthetic-private-name.txt";item.uploadStatus="uploaded";item.shareStatus="shared";item.receipt.directUrl="https://synthetic-private-cdn.example.invalid/"+id+".txt";op.items={item};return store.saveOperation(op);};
  QVERIFY(!save("suppressed-copy-upload","suppressed"));QVERIFY(!save("copied-upload","copied"));
  Error error;BundleRequest request;request.output=dir_.filePath("suppressed-copy.zip");
  const auto result=build(context(),request,error);QVERIFY2(!error,qPrintable(error.message));
  const auto operations=QJsonDocument::fromJson(unzip(result["path"].toString())["operations.json"].toString().toUtf8()).array();
  QStringList ids;QJsonObject suppressed;for(const auto& value:operations){ids.append(value.toObject()["id"].toString());if(value.toObject()["id"]=="suppressed-copy-upload")suppressed=value.toObject();}
  QVERIFY2(ids.contains("suppressed-copy-upload")&&ids.contains(failedId_)&&!ids.contains("copied-upload"),qPrintable(ids.join(',')));
  QCOMPARE(suppressed["copyStatus"].toString(),QString("suppressed"));QCOMPARE(suppressed["state"].toString(),QString("completed"));
  const auto item=suppressed["items"].toArray()[0].toObject();QVERIFY(item["name"].toString().startsWith("<name:"));QVERIFY(!item["directUrl"].toString().contains("synthetic-private-cdn"));
 }
 void sizeLimitDropsTheOldestRecords() {
  const auto filler=QString("lorem ipsum ").repeated(125);
  // The writer queue holds 4096 records; let it drain as a real resident would.
  for(int index=0;index<20000;++index){logging::info(logApp(),filler,{{"index",index}});if(index%2000==1999)logging::flush();}
  logging::flush();
  Error error;BundleRequest request;request.output=dir_.path();
  const auto result=build(context(),request,error);QVERIFY(!error);
  QVERIFY(result["bytes"].toInteger()<=DiagnosticsBundles::maximumBytes);QVERIFY(result["droppedLogRecords"].toInteger()>0);
  const auto logs=unzip(result["path"].toString())["logs/putgd.jsonl"].toString().split('\n',Qt::SkipEmptyParts);
  QCOMPARE(QJsonDocument::fromJson(logs.last().toUtf8()).object()["fields"].toObject()["index"].toInt(),19999);
  QVERIFY(QJsonDocument::fromJson(logs.first().toUtf8()).object()["fields"].toObject()["index"].toInt()>0);
 }
 void defaultFolderKeepsTenRecentPackages() {
  QTemporaryDir folder;const auto now=QDateTime::currentDateTimeUtc();
  for(int index=0;index<14;++index) {
   QFile file(folder.filePath(QString("putgd-diagnostics-202610%1-000000Z.zip").arg(10+index,2,10,QChar('0'))));QVERIFY(file.open(QIODevice::WriteOnly));file.write("x");file.flush();
   file.setFileTime(now.addDays(index==0?-40:-index),QFileDevice::FileModificationTime);file.close();
  }
  QFile mine(folder.filePath("my-notes.zip"));QVERIFY(mine.open(QIODevice::WriteOnly));mine.close();
  QCOMPARE(DiagnosticsBundles::prune(folder.path(),now),4);
  QCOMPARE(QDir(folder.path()).entryList({"putgd-diagnostics-*.zip"}).size(),10);QVERIFY(QFile::exists(folder.filePath("my-notes.zip")));
  QVERIFY(!QFile::exists(folder.filePath("putgd-diagnostics-20261010-000000Z.zip")));
 }
 void requestsAreValidated() {
  BundleRequest request;QVERIFY(!DiagnosticsBundles::parse({},request));QCOMPARE(request.sinceMs,7LL*86400000);
  QVERIFY(!DiagnosticsBundles::parse({{"operation","x"},{"since","24h"},{"includeNames",true},{"output","/tmp/a.zip"}},request));
  QCOMPARE(request.sinceMs,24LL*3600000);QVERIFY(request.includeNames);
  for(const auto& params:QList<QJsonObject>{{{"since","0d"}},{{"since","366d"}},{{"since","7"}},{{"since",7}},{{"output","relative.zip"}},{{"includeNames","yes"}},{{"operation",""}},{{"extra",1}},{{"profileId","x"}},{{"copyLink",true}},{{"upload",1}}})
   QCOMPARE(DiagnosticsBundles::parse(params,request).code,QString("invalid_request"));
 }
 void cliFlagsBecomeParameters() {
  QTemporaryDir d;QLocalServer server;QVERIFY(server.listen(d.filePath("s.sock")));QJsonObject received;
  connect(&server,&QLocalServer::newConnection,this,[&]{
   auto* socket=server.nextPendingConnection();socket->setParent(&server);
   connect(socket,&QLocalSocket::readyRead,socket,[&,socket]{
    if(!socket->canReadLine())return;
    received=QJsonDocument::fromJson(socket->readLine()).object();
    socket->write(QJsonDocument(LocalServer::response(received["requestId"].toString(),{},{{"path","/synthetic.zip"}})).toJson(QJsonDocument::Compact)+'\n');socket->flush();
   });
  });
  QProcess client;client.setWorkingDirectory(d.path());
  client.start(PUTGD_CLI_PATH,{"--socket",server.fullServerName(),"diagnostics.bundle","--operation","synthetic-op","--since","24h","--include-names","--output","relative.zip"});
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,10000);QCOMPARE(client.exitCode(),0);
  QCOMPARE(received["method"].toString(),QString("diagnostics.bundle"));
  QCOMPARE(received["params"].toObject(),(QJsonObject{{"operation","synthetic-op"},{"since","24h"},{"includeNames",true},{"output",QDir(d.path()).absoluteFilePath("relative.zip")}}));
  // --upload [--profile ID] [--copy-link] (#123).
  received={};client.start(PUTGD_CLI_PATH,{"--socket",server.fullServerName(),"diagnostics.bundle","--upload","--profile","synthetic-s3","--copy-link"});
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,10000);QCOMPARE(client.exitCode(),0);
  QCOMPARE(received["params"].toObject(),(QJsonObject{{"upload",true},{"profileId","synthetic-s3"},{"copyLink",true}}));
  // The flags belong to diagnostics.bundle alone, and never with a JSON object.
  for(const auto& args:QList<QStringList>{{"diagnostics","--since","1d"},{"diagnostics.bundle","{}","--since","1d"},{"diagnostics.bundle","--profile","x"},{"diagnostics.bundle","--copy-link"}}) {
   received={};client.start(PUTGD_CLI_PATH,QStringList{"--socket",server.fullServerName()}+args);
   PUTGD_TRY_COMPARE_WITH_TIMEOUT(client.state(),QProcess::NotRunning,10000);QCOMPARE(client.exitCode(),1);
   QCOMPARE(QJsonDocument::fromJson(client.readAllStandardOutput()).object()["error"].toObject()["code"].toString(),QString("invalid_request"));QVERIFY(received.isEmpty());
  }
 }
 void sessionMethodSavesAPackage() {
  HistoryService history(*operations_);LocalServer server(*settings_,*operations_,history);QVERIFY(server.listen(dir_.filePath("runtime")));
  DiagnosticsBundles bundles(*operations_,dir_.filePath("state"));server.setDiagnosticsBundles(&bundles);
  QTemporaryDir out;bool done=false;Error error;QJsonObject result;
  server.dispatch({{"schemaVersion",1},{"requestId","bundle"},{"session",server.socketPath()},{"method","diagnostics.bundle"},{"params",QJsonObject{{"since","1d"},{"output",out.path()}}}},
   [&](Error e,QJsonObject r){error=e;result=r;done=true;});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(done,30000);QVERIFY2(!error,qPrintable(error.message));QVERIFY(QFile::exists(result["path"].toString()));QVERIFY(result["operations"].toInt()>=1);
  // #223: the resident remembers what it saved, so Show in folder takes a package
  // under any name it wrote, and still nothing else.
  QVERIFY(bundles.saved(result["path"].toString()));const auto custom=out.filePath("putgd-diagnostics-r9.zip");done=false;
  server.dispatch({{"schemaVersion",1},{"requestId","custom"},{"session",server.socketPath()},{"method","diagnostics.bundle"},{"params",QJsonObject{{"since","1d"},{"output",custom}}}},
   [&](Error e,QJsonObject r){error=e;result=r;done=true;});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(done,30000);QVERIFY2(!error,qPrintable(error.message));QCOMPARE(result["path"].toString(),custom);
  QVERIFY(bundles.saved(custom));QVERIFY(bundles.saved(out.path()+"/./putgd-diagnostics-r9.zip"));QVERIFY(!bundles.saved(out.filePath("other.zip")));
  done=false;server.dispatch({{"schemaVersion",1},{"requestId","bad"},{"session",server.socketPath()},{"method","diagnostics.bundle"},{"params",QJsonObject{{"since","forever"}}}},[&](Error e,QJsonObject){error=e;done=true;});
  PUTGD_TRY_VERIFY(done);QCOMPARE(error.code,QString("invalid_request"));
 }
};
QTEST_GUILESS_MAIN(DiagnosticsBundleContract)
#include "test_diagnostics_bundle.moc"
