#include <QElapsedTimer>
#include <thread>
#include <chrono>
#include <QJsonDocument>
#include "storage/proton/OutputBudget.h"
#include "storage/proton/ProtonProvider.h"
#include "core/HostEnvironment.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QScopeGuard>
#include <QTest>
#include <QUuid>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <QFileInfo>
using namespace putgd;
class ProtonTransportTest:public QObject {
 Q_OBJECT
 static Destination destination(const QString& fixture="auth") {
  auto settings=ProtonProvider::defaults();settings["fixture"]=fixture;settings["requestTimeoutMs"]=1000;
  return {"proton-drive","synthetic","revision",settings,{{"mode","keychain"},{"reference","putgd/proton/transport-"+QUuid::createUuid().toString(QUuid::WithoutBraces)}}};
 }
 static UploadResult upload(ProtonProvider& provider,const Destination& d,std::function<void(const UploadProgress&)> progress={}) {
  UploadRequest req;req.destination=d;req.condition="absent";req.candidate="synthetic.txt";req.publicationId="synthetic";
  req.progress=std::move(progress);UploadResult result;provider.upload(req,[&](UploadResult r){result=r;});return result;
 }
private slots:
 void nameCheckPropagatesCancellationToHelper() {
  ProtonProvider provider;auto d=destination("timeout");d.settings["requestTimeoutMs"]=120000;
  auto cancellation=std::make_shared<std::atomic_bool>(false);
  std::jthread cancel([cancellation]{std::this_thread::sleep_for(std::chrono::milliseconds(200));cancellation->store(true);});
  NameCheck result;QElapsedTimer elapsed;elapsed.start();
  provider.checkName(d,"synthetic.txt",[&](NameCheck value){result=value;},cancellation);
  QVERIFY(elapsed.elapsed()<2000);QCOMPARE(result.status,QString("inconclusive"));
  QCOMPARE(result.error.code,QString("proton_result_unknown"));QCOMPARE(result.error.details["reason"].toString(),QString("cancelled"));
  QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
  provider.checkName(d,"synthetic.txt",[&](NameCheck value){result=value;},cancellation);
  QCOMPARE(result.error.code,QString("operation_cancelled"));
 }

 // #518: inside an AppImage the helper got AppRun's library path, so the host libsecret it loads
 // bound to the image's older GLib and every keychain call failed (Debian 13, Ubuntu 26.04).
 void helperGetsTheHostEnvironmentNotTheAppImages(){
  QProcessEnvironment current;const QString mount="/tmp/.mount_putgdSynthetic";
  current.insert("APPDIR",mount);current.insert("APPIMAGE","/home/synthetic/putgd.AppImage");
  current.insert("LD_LIBRARY_PATH",mount+"/usr/lib:"+mount+"/usr/lib/putgd");
  current.insert("QT_PLUGIN_PATH",mount+"/usr/plugins:"+mount+"/usr/lib/qt6/plugins");
  current.insert("PUTGD_HOST_ENVIRONMENT","LD_LIBRARY_PATH QT_PLUGIN_PATH");
  current.insert("DBUS_SESSION_BUS_ADDRESS","unix:path=/run/user/1000/bus");
  current.insert("PROTON_DRIVE_SYNTHETIC","1");current.insert("PUTGD_PROTON_DATA_DIR","/synthetic/elsewhere");
  auto env=ProtonProvider::helperEnvironment(host::environment({current,{},{}}),"/tmp/putgd-proton-synthetic");
  for(const auto* key:{"LD_LIBRARY_PATH","QT_PLUGIN_PATH","APPDIR","APPIMAGE","PUTGD_HOST_ENVIRONMENT","PROTON_DRIVE_SYNTHETIC"})
   QVERIFY2(!env.contains(key),key);
  QCOMPARE(env.value("DBUS_SESSION_BUS_ADDRESS"),QString("unix:path=/run/user/1000/bus"));
  QCOMPARE(env.value("PUTGD_PROTON_DATA_DIR"),QString("/tmp/putgd-proton-synthetic"));
  // The host's own library path, as AppRun saved it, comes back.
  current.insert("PUTGD_HOST_LD_LIBRARY_PATH","/opt/synthetic-host/lib");
  env=ProtonProvider::helperEnvironment(host::environment({current,{},{}}),"/tmp/putgd-proton-synthetic");
  QCOMPARE(env.value("LD_LIBRARY_PATH"),QString("/opt/synthetic-host/lib"));
  // A DEB, RPM or development build passes its environment on unchanged.
  QProcessEnvironment native;native.insert("LD_LIBRARY_PATH","/opt/synthetic-dev/lib");
  QCOMPARE(ProtonProvider::helperEnvironment(host::environment({native,{},{}}),"/d").value("LD_LIBRARY_PATH"),
           QString("/opt/synthetic-dev/lib"));
 }
 void connectionTestValidatesAccountAndSelectedFolder_data(){
  QTest::addColumn<QString>("root");QTest::addColumn<QString>("folder");QTest::addColumn<QString>("fixture");QTest::addColumn<QString>("expected");
  QTest::newRow("top-level-folder")<<"synthetic-root"<<"synthetic-folder"<<"inspect-account"<<"";
  QTest::newRow("my-files-root")<<"synthetic-root"<<"synthetic-root"<<"inspect-account"<<"";
  QTest::newRow("signed-out")<<""<<""<<"inspect-account"<<"proton_setup_required";
  QTest::newRow("no-folder")<<"synthetic-root"<<""<<"inspect-account"<<"proton_setup_required";
  QTest::newRow("changed-account")<<"another-root"<<"synthetic-folder"<<"inspect-account"<<"proton_account_changed";
  QTest::newRow("missing-folder")<<"synthetic-root"<<"missing-folder"<<"inspect-account"<<"proton_folder_unavailable";
  QTest::newRow("denied")<<"synthetic-root"<<"synthetic-folder"<<"auth"<<"authentication_required";
 }
 void connectionTestValidatesAccountAndSelectedFolder(){
  QFETCH(QString,root);QFETCH(QString,folder);QFETCH(QString,fixture);QFETCH(QString,expected);
  ProtonProvider provider;auto d=destination(fixture);d.settings["accountRootUid"]=root;d.settings["folderUid"]=folder;
  Error error;const auto result=provider.setup("testDestination",d,{},error);
  QCOMPARE(error.code,expected);
  if(expected.isEmpty())QCOMPARE(result,QJsonObject({{"testScope","account-folder"}}));else QVERIFY(result.isEmpty());
  QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
 }
 void transportFailureDisposition_data(){
  QTest::addColumn<QString>("fixture");QTest::addColumn<QString>("status");QTest::addColumn<QString>("error");
  QTest::newRow("confirmed-auth-failure")<<QString("auth")<<QString("failed")<<QString("authentication_required");
  QTest::newRow("interrupted-request")<<QString("timeout")<<QString("unknown")<<QString("proton_request_timeout");
  QTest::newRow("malformed-response")<<QString("malformed")<<QString("unknown")<<QString("proton_protocol");
  QTest::newRow("unconfirmed-exit")<<QString("crash")<<QString("unknown")<<QString("proton_result_unknown");
 }
 void transportFailureDisposition(){
  QFETCH(QString,fixture);QFETCH(QString,status);QFETCH(QString,error);ProtonProvider provider;const auto d=destination(fixture);
  const auto r=upload(provider,d);QCOMPARE(r.status,status);QCOMPARE(r.error.code,error);
  ShareRequest req;req.id="synthetic-share";req.action="create";req.receipt.destination=d;ShareResult sharing;
  provider.share(req,[](const QJsonObject&,auto ack){ack({});},[&](ShareResult r){sharing=r;});QCOMPARE(sharing.status,status);QCOMPARE(sharing.error.code,error);QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
 }
 void confirmedLinkExpiryReachesTheShareResult(){
  // The helper's confirmed expiry is kept as UTC ISO 8601; anything unparsable is dropped (#253).
  for(const auto& [fixture,expected]:QList<std::pair<QString,QString>>{{"share-expiry","2026-10-27T17:48:40.000Z"},{"share-expiry-invalid",""}}){
   ProtonProvider provider;ShareRequest req;req.id="synthetic-share";req.action="inspect";req.receipt.destination=destination(fixture);ShareResult sharing;
   provider.share(req,[](const QJsonObject&,auto ack){ack({});},[&](ShareResult r){sharing=r;});
   QCOMPARE(sharing.status,QString("shared"));QCOMPARE(sharing.expiresAt,expected);
  }
 }
 void planFileLimitIsNotAFullDrive(){
  // Proton error 200003 arrives as its own code with the file's size and plan guidance (#147).
  ProtonProvider provider;UploadRequest req;req.destination=destination("plan-limit");req.condition="absent";req.candidate="synthetic.txt";req.publicationId="synthetic";req.artifact.bytes=3LL*1024*1024*1024;
  UploadResult r;provider.upload(req,[&](UploadResult value){r=value;});
  QCOMPARE(r.error.code,QString("destination_file_too_large_for_plan"));QCOMPARE(r.status,QString("unknown"));
  QVERIFY2(r.error.message.contains("3.0 GB")&&r.error.message.contains("Proton plan"),qPrintable(r.error.message));
  QCOMPARE(r.error.details["bytes"].toDouble(),double(3LL*1024*1024*1024));QVERIFY(!r.error.message.contains("capacity"));
 }
 void planFileLimitWithNothingCreatedSettlesAsFailed(){
  // The helper settles 200003 as failed once the name is still free afterwards (#201); the size guidance stays.
  ProtonProvider provider;UploadRequest req;req.destination=destination("plan-limit-settled");req.condition="absent";req.candidate="synthetic.txt";req.publicationId="synthetic";req.artifact.bytes=3LL*1024*1024*1024;
  UploadResult r;provider.upload(req,[&](UploadResult value){r=value;});
  QCOMPARE(r.status,QString("failed"));QCOMPARE(r.error.code,QString("destination_file_too_large_for_plan"));
  QVERIFY2(r.error.message.contains("3.0 GB")&&r.error.message.contains("Proton plan"),qPrintable(r.error.message));
  QCOMPARE(r.error.details["bytes"].toDouble(),double(3LL*1024*1024*1024));QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
 }
 void progressFramesPrecedeCompletion(){
  ProtonProvider provider;QList<UploadProgress> reports;const auto r=upload(provider,destination("progress"),[&](const UploadProgress& value){reports.append(value);});
  QCOMPARE(r.status,QString("failed"));QCOMPARE(r.error.code,QString("authentication_required"));QCOMPARE(reports.size(),4);QCOMPARE(reports[0].phase,QString("preparing"));QVERIFY(!reports[0].bytesTransferred);QCOMPARE(reports[1].bytesTransferred.value(),1048576);QCOMPARE(reports[2].bytesTransferred.value(),524288);QVERIFY(!reports[2].bytesTotal);QCOMPARE(reports[3].phase,QString("finalizing"));
 }
 void invalidOrUnboundedProgressIsUnknown_data(){
  QTest::addColumn<QString>("fixture");QTest::addColumn<int>("timeoutMs");QTest::addColumn<QString>("message");
  for(const auto& name:QStringList{"bad-progress-negative","bad-progress-fraction","bad-progress-too-large","bad-progress-extra","bad-progress-total","bad-progress-phase"})QTest::newRow(qPrintable(name))<<name<<1000<<"Unexpected or invalid upload progress";
  // The flood trips the output budget on byte count alone (2.8 MB against a 2 MB burst
  // and 1 KB/s refill), however slowly it is read. Parsing 40000 lines can outlast a
  // 1 s deadline on a loaded runner, so give this row a deadline it cannot reach (#236).
  QTest::newRow("flood")<<"flood"<<120000<<"Helper response exceeded its bound";
  QTest::newRow("after-result")<<"after-result"<<1000<<"Unexpected or invalid upload progress";
 }
 void invalidOrUnboundedProgressIsUnknown(){
  QFETCH(QString,fixture);QFETCH(int,timeoutMs);QFETCH(QString,message);ProtonProvider provider;auto d=destination(fixture);d.settings["requestTimeoutMs"]=timeoutMs;
  const auto r=upload(provider,d);QCOMPARE(r.status,QString("unknown"));QCOMPARE(r.error.code,QString("proton_protocol"));QCOMPARE(r.error.message,message);
 }
 void cancellationAfterProgressIsDistinctFromDeadline(){
  ProtonProvider provider;UploadRequest request;request.destination=destination("cancel-progress");request.condition="absent";request.cancellation=std::make_shared<std::atomic_bool>(false);request.progress=[flag=request.cancellation](const UploadProgress&){flag->store(true);};UploadResult result;
  provider.upload(request,[&](UploadResult value){result=value;});QCOMPARE(result.status,QString("unknown"));QCOMPARE(result.error.code,QString("proton_result_unknown"));QCOMPARE(result.error.details["reason"].toString(),QString("cancelled"));
  request.cancellation->store(true);provider.upload(request,[&](UploadResult value){result=value;});QCOMPARE(result.status,QString("failed"));QCOMPARE(result.error.code,QString("operation_cancelled"));
 }
 void deadlineReportsConfiguredBudget(){
  ProtonProvider provider;const auto r=upload(provider,destination("timeout"));
  QCOMPARE(r.status,QString("unknown"));QCOMPARE(r.error.code,QString("proton_request_timeout"));
  QCOMPARE(r.error.details["reason"].toString(),QString("deadline"));QCOMPARE(r.error.details["timeoutMs"].toInt(),1000);
  QVERIFY(r.error.message.contains("deadline"));
 }
 void retryBudgetExtendsWatchdogWithoutChangingAcceptedDestination(){
  ProtonProvider provider;UploadRequest request;request.destination=destination("budget-delay");const auto accepted=request.destination.toJson();request.condition="absent";request.requestTimeoutMs=2000;UploadResult result;provider.upload(request,[&](UploadResult value){result=value;});QCOMPARE(result.status,QString("failed"));QCOMPARE(result.error.code,QString("authentication_required"));QCOMPARE(result.error.details["attemptTimeoutMs"].toInt(),2000);QCOMPARE(request.destination.toJson(),accepted);
 }
 void sustainedProgressAcrossHoursRemainsBoundedWithoutExhaustingLifetimeQuota(){
  ProtonOutputBudget budget;qint64 bytes=0;for(int i=0;i<32768;++i){const auto line=QJsonDocument(QJsonObject{{"event","progress"},{"phase","uploading"},{"bytesTransferred",qint64(i+1)*4194304}}).toJson(QJsonDocument::Compact)+"\n";bytes+=line.size();QVERIFY(budget.consume(line.size(),qint64(i)*500));}QVERIFY(bytes>ProtonOutputBudget::MaximumBurstBytes);QVERIFY(budget.consume(1024*1024,24LL*60*60*1000));
  ProtonOutputBudget flood;QVERIFY(flood.consume(ProtonOutputBudget::MaximumBurstBytes,0));QVERIFY(!flood.consume(1,0));ProtonOutputBudget frame;QVERIFY(!frame.consume(ProtonOutputBudget::MaximumBurstBytes+1,24LL*60*60*1000));
 }
 void reconciliationCancellationStopsExtendedRequest(){
  ProtonProvider provider;UploadRequest request;request.destination=destination("timeout");request.requestTimeoutMs=30000;request.cancellation=std::make_shared<std::atomic_bool>(false);std::jthread cancel([flag=request.cancellation]{std::this_thread::sleep_for(std::chrono::milliseconds(200));flag->store(true);});StatResult result;QElapsedTimer elapsed;elapsed.start();provider.reconcile(request,[&](StatResult value){result=value;});QVERIFY(elapsed.elapsed()<2000);QCOMPARE(result.status,QString("unknown"));QCOMPARE(result.error.code,QString("proton_result_unknown"));QCOMPARE(result.error.details["reason"].toString(),QString("cancelled"));QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
 }
 void accountBusyNeverDispatches(){
  ProtonProvider provider;auto d=destination();const auto root=QString("/tmp/putgd-proton-accounts-%1").arg(getuid());QVERIFY(::mkdir(QFile::encodeName(root).constData(),0700)==0||errno==EEXIST);const QFileInfo info(root);QVERIFY(info.isDir()&&!info.isSymLink());QCOMPARE(info.ownerId(),uint(getuid()));QVERIFY(!(info.permissions()&(QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|QFile::ReadOther|QFile::WriteOther|QFile::ExeOther)));
  const auto key=QString::fromLatin1(QCryptographicHash::hash(d.credentialReference["reference"].toString().toUtf8(),QCryptographicHash::Sha256).toHex());QLockFile lock(QDir(root).filePath(key+".lock"));lock.setStaleLockTime(0);QVERIFY(lock.tryLock());
  const auto r=upload(provider,d);QCOMPARE(r.status,QString("failed"));QCOMPARE(r.error.code,QString("proton_account_busy"));QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
 }
 void missingRuntimeNeverDispatches(){
  const auto runtime=QDir(QCoreApplication::applicationDirPath()).filePath("proton-fixture/helper");const auto held=runtime+".held";QVERIFY(QFile::rename(runtime,held));const auto restore=qScopeGuard([&]{QFile::rename(held,runtime);});
  ProtonProvider provider;const auto r=upload(provider,destination());QCOMPARE(r.status,QString("failed"));QCOMPARE(r.error.code,QString("proton_runtime_unavailable"));QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
 }
};
QTEST_GUILESS_MAIN(ProtonTransportTest)
#include "test_proton_transport.moc"
