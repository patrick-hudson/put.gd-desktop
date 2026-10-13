#include "storage/proton/ProtonProvider.h"
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
 static UploadResult upload(ProtonProvider& provider,const Destination& d) {
  UploadRequest req;req.destination=d;req.condition="absent";req.candidate="synthetic.txt";req.publicationId="synthetic";
  UploadResult result;provider.upload(req,[&](UploadResult r){result=r;});return result;
 }
private slots:
 void transportFailureDisposition_data(){
  QTest::addColumn<QString>("fixture");QTest::addColumn<QString>("status");QTest::addColumn<QString>("error");
  QTest::newRow("confirmed-auth-failure")<<QString("auth")<<QString("failed")<<QString("authentication_required");
  QTest::newRow("interrupted-request")<<QString("timeout")<<QString("unknown")<<QString("proton_result_unknown");
  QTest::newRow("malformed-response")<<QString("malformed")<<QString("unknown")<<QString("proton_protocol");
  QTest::newRow("unconfirmed-exit")<<QString("crash")<<QString("unknown")<<QString("proton_result_unknown");
 }
 void transportFailureDisposition(){
  QFETCH(QString,fixture);QFETCH(QString,status);QFETCH(QString,error);ProtonProvider provider;const auto d=destination(fixture);
  const auto r=upload(provider,d);QCOMPARE(r.status,status);QCOMPARE(r.error.code,error);
  ShareRequest req;req.id="synthetic-share";req.action="create";req.receipt.destination=d;ShareResult sharing;
  provider.share(req,[](const QJsonObject&,auto ack){ack({});},[&](ShareResult r){sharing=r;});QCOMPARE(sharing.status,status);QCOMPARE(sharing.error.code,error);QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
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
