#include "app/DiagnosticsBundle.h"
#include "core/Log.h"
#include "fixtures/U2Fixtures.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <thread>
#include "fixtures/Waits.h"
using namespace putgd;
// The optional diagnostics package upload (#123): only to a working
// destination, through the normal pipeline, under an unguessable name, with a
// link; a skipped or failed upload never touches the local copy.
namespace {
// An opaque destination whose uploads can fail like a revoked S3 key, and
// which can make expiring share links.
class FakeDestination final:public StorageProvider {
public:
 test::OpaqueProvider storage;Error failure;bool sharing=false;QList<QJsonObject> shareOptions;QList<UploadRequest> uploads;
 ProviderCapabilities capabilities(const putgd::Destination& d) const override {auto c=storage.capabilities(d);c.sharing=sharing;return c;}
 QString serializationKey(const putgd::Destination& d,const QJsonObject& o,const QString& n) const override {return storage.serializationKey(d,o,n);}
 Error validateSettings(const QJsonObject& s) const override {return storage.validateSettings(s);}
 void checkName(const putgd::Destination& d,const QString& n,std::function<void(NameCheck)> cb, std::shared_ptr<std::atomic_bool> cancellation = {}) override {storage.checkName(d,n,cb,cancellation);}
 void upload(const UploadRequest& r,std::function<void(UploadResult)> cb) override {
  uploads.append(r);
  if(failure){cb({"failed",{},failure});return;}
  storage.upload(r,[this,cb](UploadResult result){if(sharing)result.receipt.directUrl.clear();cb(result);});
 }
 void stat(const StorageReceipt& r,std::function<void(StatResult)> cb) override {storage.stat(r,cb);}
 void reconcile(const UploadRequest& r,std::function<void(StatResult)> cb) override {storage.reconcile(r,cb);}
 void remove(const DeleteRequest& r,DurableCheckpoint d,std::function<void(DeleteResult)> cb) override {storage.remove(r,d,cb);}
 void share(const ShareRequest& r,DurableCheckpoint durable,std::function<void(ShareResult)> cb) override {
  durable({{"schemaVersion",1},{"phase","intent"}},[this,r,cb](Error e){
   if(e){cb({"failed",r.receipt,{},e});return;}
   shareOptions.append(r.options);auto receipt=r.receipt;receipt.directUrl="https://example.invalid/shared#synthetic";cb({"shared",receipt,{},{}});
  });
 }
};
QJsonObject wait(std::function<void(JsonReply)> start,Error* error=nullptr) {
 bool done=false;QJsonObject value;
 start([&](Error e,QJsonObject result){if(error)*error=e;value=result;done=true;});
 if(!putgd::test::waitFor([&]{return done;},20000))qWarning("timed out");
 return value;
}
}
class DiagnosticsUploadContract final:public QObject {
 Q_OBJECT
 QTemporaryDir dir_;
 std::shared_ptr<FakeDestination> provider_=std::make_shared<FakeDestination>();
 std::unique_ptr<ProviderRegistry> registry_;std::unique_ptr<SettingsService> settings_;std::unique_ptr<test::Source> source_;
 std::unique_ptr<OperationService> operations_;std::unique_ptr<HistoryService> history_;std::unique_ptr<DiagnosticsBundles> bundles_;
 int tests_=0;Error testResult_;
 QString folder() const {return dir_.filePath("packages");}
 BundleRequest request(bool upload=true) const {BundleRequest r;r.output=folder();r.upload=upload;return r;}
 QJsonObject create(const BundleRequest& r,Error* error=nullptr) {return wait([&](JsonReply reply){bundles_->create(r,std::move(reply));},error);}
 DiagnosticsBundles::Context context() const {
  DiagnosticsBundles::Context context;
  context.databasePath=operations_->databasePath();context.logsDirectory=dir_.filePath("state/logs");context.defaultFolder=folder();context.home=dir_.path();
  context.settings=settings_->document();context.coredumpctl="-";
  return context;
 }
 // One member of a package, read with Python's zipfile.
 static QByteArray member(const QString& path,const QString& name) {
  QProcess python;python.start("python3",{"-I","-c","import sys,zipfile;sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))",path,name});
  python.waitForFinished(30000);return python.readAllStandardOutput();
 }
private slots:
 void initTestCase() {
  QVERIFY(dir_.isValid());QVERIFY(QDir().mkpath(folder()));
  logging::open(dir_.filePath("state/logs"));
  registry_=std::make_unique<ProviderRegistry>();registry_->add("opaque",provider_);
  auto* registry=registry_.get();
  settings_=std::make_unique<SettingsService>(dir_.filePath("config.json"),[registry](const auto& id,const auto& value){return registry->validate(id,value);});
  QVERIFY(settings_->save(test::configuration(dir_.path()),{}));
  source_=std::make_unique<test::Source>();source_->payload=test::textPayload();
  operations_=std::make_unique<OperationService>(dir_.filePath("data"),*settings_,*registry_,*source_);PUTGD_TRY_VERIFY(operations_->ready());
  history_=std::make_unique<HistoryService>(*operations_);
  bundles_=std::make_unique<DiagnosticsBundles>(*operations_,dir_.filePath("state"));
  // Like the S3 test, answer from another thread.
  bundles_->setDestinationTest([this](const QString& profileId,std::function<void(Error)> done){
   QCOMPARE(profileId,QString("test"));++tests_;std::thread([done,result=testResult_]{done(result);}).detach();
  });
 }
 void cleanupTestCase() {operations_->shutdown();logging::close();}
 void parsesUploadOptions() {
  BundleRequest r;
  QVERIFY(!DiagnosticsBundles::parse({{"upload",true},{"profileId","test"},{"copyLink",true}},r));
  QVERIFY(r.upload&&r.copyLink);QCOMPARE(r.profileId,QString("test"));
  // A preview writes nothing, so it takes no upload options.
  for(const auto& params:{QJsonObject{{"profileId","test"}},QJsonObject{{"copyLink",true}},QJsonObject{{"upload","yes"}},QJsonObject{{"upload",true},{"profileId",""}},
                          QJsonObject{{"preview",true},{"upload",true}},QJsonObject{{"preview",true},{"upload",false}},
                          QJsonObject{{"preview",true},{"upload",true},{"copyLink",true}},QJsonObject{{"preview",true},{"upload",true},{"profileId","test"}}}) {
   BundleRequest rejected;QCOMPARE(DiagnosticsBundles::parse(params,rejected).code,QString("invalid_request"));
  }
  QCOMPARE(DiagnosticsBundles::defaultUploadProfile(settings_->snapshot()),QString("test"));
  QVERIFY(DiagnosticsBundles::credentialOrConfigurationError({"s3_permission_denied",{},{}}));
  QVERIFY(DiagnosticsBundles::credentialOrConfigurationError({"credential_setup_required",{},{}}));
  QVERIFY(DiagnosticsBundles::credentialOrConfigurationError({"proton_setup_required",{},{}}));
  QVERIFY(!DiagnosticsBundles::credentialOrConfigurationError({"s3_transport_unknown",{},{}}));
  QVERIFY(!DiagnosticsBundles::credentialOrConfigurationError({"fixture_failed",{},{}}));
 }
 // A failing Test connection skips the upload with its reason; the package stays.
 void brokenDestinationKeepsTheLocalCopy() {
  testResult_={"s3_authentication_failed","Synthetic authentication failure",{}};
  const auto uploads=provider_->uploads.size();
  Error error;const auto result=create(request(),&error);
  QVERIFY2(!error,qPrintable(error.message));
  QVERIFY(QFileInfo::exists(result["path"].toString()));
  QCOMPARE(result["uploadError"]["code"].toString(),QString("diagnostics_destination_unavailable"));
  QCOMPARE(result["uploadError"]["details"]["error"]["code"].toString(),QString("s3_authentication_failed"));
  QVERIFY(!result.contains("link"));QCOMPARE(provider_->uploads.size(),uploads);
  const auto target=wait([&](JsonReply reply){bundles_->uploadTarget({},std::move(reply));});
  QVERIFY(!target["available"].toBool());QCOMPARE(target["reason"].toString(),QString("diagnostics_destination_unavailable"));
  testResult_={};
 }
 // A working destination: an unguessable name, the normal pipeline, a link,
 // a diagnostics entry in history that deletes like any upload.
 void workingDestinationUploadsAndLinks() {
  tests_=0;
  const auto target=wait([&](JsonReply reply){bundles_->uploadTarget({},std::move(reply));});
  QVERIFY(target["available"].toBool());QCOMPARE(target["profileId"].toString(),QString("test"));QVERIFY(target["warning"].toString().contains("anyone with the link"));
  auto r=request();r.copyLink=true;
  Error error;const auto result=create(r,&error);QVERIFY2(!error,qPrintable(error.message));
  QVERIFY2(!result.contains("uploadError"),QJsonDocument(result).toJson().constData());
  QCOMPARE(tests_,1); // the dialog's check is reused by the upload
  const auto upload=result["upload"].toObject();
  QVERIFY2(QRegularExpression("^putgd-diagnostics-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.zip$").match(upload["name"].toString()).hasMatch(),qPrintable(upload["name"].toString()));
  QVERIFY(result["link"].toString().startsWith("https://example.invalid/direct"));
  QVERIFY(result["linkCopied"].toBool());QCOMPARE(source_->copied.text,result["link"].toString());
  // The uploaded bytes are the saved package.
  const auto sent=provider_->uploads.last();
  QCOMPARE(sent.candidate,upload["name"].toString());QCOMPARE(sent.artifact.bytes,QFileInfo(result["path"].toString()).size());
  const auto op=wait([&](JsonReply reply){operations_->inspect(upload["operationId"].toString(),std::move(reply));});
  QCOMPARE(op["action"].toString(),QString("diagnostics"));QCOMPARE(op["state"].toString(),QString("completed"));
  QCOMPARE(op["copyStatus"].toString(),QString("suppressed"));
  // No second copy in the local archive, whatever keepUploads says.
  QVERIFY(QDir(dir_.filePath("archive")).entryList({"putgd-diagnostics-*"},QDir::Files,QDir::NoSort).isEmpty());
  const auto deletion=wait([&](JsonReply reply){history_->prepareDelete({upload["historyId"].toString()},std::move(reply));});
  QCOMPARE(deletion["targets"].toArray().size(),1);
 }
 // A later package leaves an uploaded package out, operations and log lines.
 void laterPackagesLeaveUploadedPackagesOut() {
  const auto first=create(request());const auto id=first["upload"]["operationId"].toString();QVERIFY(!id.isEmpty());
  logging::flush();
  BundleRequest later;later.output=dir_.filePath("later.zip");Error error;
  const auto result=DiagnosticsBundles::build(context(),later,error);QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(result["operations"].toInt(),0);
  const auto logs=member(result["path"].toString(),"logs/putgd.jsonl");
  QVERIFY(!logs.isEmpty());QVERIFY(!logs.contains(id.toUtf8()));
  // Asked for by its ID, it is packaged like any operation.
  BundleRequest one;one.operationId=id;one.output=dir_.filePath("one.zip");
  const auto explicitPackage=DiagnosticsBundles::build(context(),one,error);QVERIFY(!error);QCOMPARE(explicitPackage["operations"].toInt(),1);
  QVERIFY(member(explicitPackage["path"].toString(),"logs/putgd.jsonl").contains(id.toUtf8()));
 }
 // The last upload to this destination failed with a credential error: skip
 // without even testing, and say why.
 void credentialFailureSkipsTheUpload() {
  provider_->failure={"s3_permission_denied","S3 denied the request (HTTP 403).",{{"httpStatus",403}}};
  bool accepted=false;operations_->invoke("clipboard",[&](Error error,QJsonObject){QVERIFY(!error);accepted=true;});PUTGD_TRY_VERIFY(accepted);
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(provider_->uploads.size()>0&&provider_->uploads.last().candidate.startsWith("Text-"),10000);
  PUTGD_TRY_COMPARE(operations_->diagnostics()["activeOperations"].toInt(),0);
  provider_->failure={};tests_=0;
  const auto result=create(request());
  QCOMPARE(result["uploadError"]["code"].toString(),QString("diagnostics_destination_failing"));
  QCOMPARE(result["uploadError"]["details"]["lastError"].toString(),QString("s3_permission_denied"));
  QCOMPARE(tests_,0);QVERIFY(QFileInfo::exists(result["path"].toString()));
  // A Test connection that passed since (new credentials, same reference) clears it.
  bundles_->destinationTested("test");
  QVERIFY2(!create(request()).contains("uploadError"),"a passing Test connection clears the earlier failure");
  provider_->failure={"s3_permission_denied","S3 denied the request (HTTP 403).",{{"httpStatus",403}}};
  accepted=false;operations_->invoke("clipboard",[&](Error error,QJsonObject){QVERIFY(!error);accepted=true;});PUTGD_TRY_VERIFY(accepted);
  PUTGD_TRY_COMPARE(operations_->diagnostics()["activeOperations"].toInt(),0);provider_->failure={};
  QCOMPARE(QJsonObject(create(request())).value("uploadError").toObject()["code"].toString(),QString("diagnostics_destination_failing"));
  // A destination whose settings changed since that failure is checked afresh.
  auto document=settings_->document();auto profiles=document["profiles"].toArray();auto profile=profiles[0].toObject();
  profile["settings"]=QJsonObject{{"workspace","fixture-workspace-fixed"}};profiles[0]=profile;document["profiles"]=profiles;
  QVERIFY(settings_->save(document,settings_->revision()));
  QVERIFY2(!create(request()).contains("uploadError"),"changed settings clear the earlier failure");
  profile["settings"]=QJsonObject{{"workspace","fixture-workspace"}};profiles[0]=profile;document["profiles"]=profiles;
  QVERIFY(settings_->save(document,settings_->revision()));
  // A passing ordinary upload clears it.
  accepted=false;operations_->invoke("clipboard",[&](Error error,QJsonObject){QVERIFY(!error);accepted=true;});PUTGD_TRY_VERIFY(accepted);
  PUTGD_TRY_COMPARE(operations_->diagnostics()["activeOperations"].toInt(),0);
  QVERIFY(!create(request()).contains("uploadError"));
 }
 // A failed diagnostics upload reports the provider's error, and later
 // packages keep it like any failure.
 void failedUploadIsReportedAndKept() {
  provider_->failure={"s3_transport_unknown","Synthetic transport failure",{}};
  const auto result=create(request());
  provider_->failure={};
  QCOMPARE(result["uploadError"]["code"].toString(),QString("s3_transport_unknown"));
  QVERIFY(QFileInfo::exists(result["path"].toString()));
  const auto id=result["upload"]["operationId"].toString();QVERIFY(!id.isEmpty());
  logging::flush();
  BundleRequest later;later.output=dir_.filePath("after-failure.zip");Error error;
  const auto package=DiagnosticsBundles::build(context(),later,error);QVERIFY(!error);
  QVERIFY(member(package["path"].toString(),"operations.json").contains(id.toUtf8()));
 }
 // Where the provider makes share links, the package's link expires.
 void shareLinksExpire() {
  provider_->sharing=true;
  const auto result=create(request());
  provider_->sharing=false;
  QVERIFY2(!result.contains("uploadError"),QJsonDocument(result).toJson().constData());
  QCOMPARE(provider_->shareOptions.size(),1);
  QCOMPARE(provider_->shareOptions[0]["expirationDays"].toInt(),OperationService::diagnosticsShareDays);
  QCOMPARE(result["link"].toString(),QString("https://example.invalid/shared#synthetic"));
 }
 void noDestinationMeansLocalOnly() {
  QVERIFY(settings_->save(test::configuration(dir_.path(),true),settings_->revision()));
  const auto result=create(request());
  QCOMPARE(result["uploadError"]["code"].toString(),QString("diagnostics_no_destination"));
  QVERIFY(QFileInfo::exists(result["path"].toString()));
  const auto target=wait([&](JsonReply reply){bundles_->uploadTarget({},std::move(reply));});
  QVERIFY(!target["available"].toBool());QCOMPARE(target["reason"].toString(),QString("diagnostics_no_destination"));
 }
};
QTEST_GUILESS_MAIN(DiagnosticsUploadContract)
#include "test_diagnostics_upload.moc"
