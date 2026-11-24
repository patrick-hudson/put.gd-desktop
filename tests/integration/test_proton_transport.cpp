#include <thread>
#include <QJsonDocument>
#include "fixtures/Waits.h"
#include "storage/proton/ProtonProvider.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <unistd.h>
#include <signal.h>
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
 void invalidOrUnboundedProgressIsUnknown_data(){
  QTest::addColumn<QString>("fixture");QTest::addColumn<int>("timeoutMs");QTest::addColumn<QString>("message");
  // An untrusted helper's malformed progress and an unbounded flood both fail closed.
  QTest::newRow("bad-progress-negative")<<"bad-progress-negative"<<1000<<"Unexpected or invalid upload progress";
  // Parsing 40000 lines can outlast a 1 s deadline on a loaded runner, so this row's deadline cannot be reached (#236).
  QTest::newRow("flood")<<"flood"<<120000<<"Helper response exceeded its bound";
 }
 void invalidOrUnboundedProgressIsUnknown(){
  QFETCH(QString,fixture);QFETCH(int,timeoutMs);QFETCH(QString,message);ProtonProvider provider;auto d=destination(fixture);d.settings["requestTimeoutMs"]=timeoutMs;
  const auto r=upload(provider,d);QCOMPARE(r.status,QString("unknown"));QCOMPARE(r.error.code,QString("proton_protocol"));QCOMPARE(r.error.message,message);
 }
 static NameCheck ask(ProtonProvider& provider,Destination d,const QString& fixture="session") {
  d.settings["fixture"]=fixture;NameCheck result;provider.checkName(d,"synthetic.txt",[&](NameCheck value){result=value;});return result;
 }
 static bool running(const QJsonValue& pid) {return ::kill(pid_t(pid.toInteger()),0)==0;}
 void sessionKeepsOneHelperUntilItIsReleased(){
  ProtonProvider provider;const auto d=destination("session");
  QJsonValue pid;
  {
   const auto session=provider.openSession(d);QVERIFY(session);QVERIFY(!provider.openSession(d));
   const auto first=ask(provider,d),second=ask(provider,d);pid=first.objectReference["pid"];
   QCOMPARE(first.status,QString("absent"));QCOMPARE(first.objectReference["served"].toInt(),1);
   QCOMPARE(second.objectReference["pid"],pid);QCOMPARE(second.objectReference["served"].toInt(),2);
   QVERIFY(running(pid));QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
   // The account stays locked for as long as the helper is kept: two helpers must never
   // refresh one rotating session. A request from another thread is refused undispatched.
   NameCheck other;std::jthread([&]{other=ask(provider,d);}).join();
   QCOMPARE(other.status,QString("inconclusive"));QCOMPARE(other.error.code,QString("proton_account_busy"));
  }
  // Releasing the session ends its helper and frees the account: the next request has a
  // helper of its own, which the synthetic peer answers as a disconnected account.
  PUTGD_TRY_VERIFY(!running(pid));
  QCOMPARE(ask(provider,d).error.code,QString("authentication_required"));
 }
 void accountBusyNeverDispatches(){
  ProtonProvider provider;auto d=destination();const auto root=QString("/tmp/putgd-proton-accounts-%1").arg(getuid());QVERIFY(::mkdir(QFile::encodeName(root).constData(),0700)==0||errno==EEXIST);const QFileInfo info(root);QVERIFY(info.isDir()&&!info.isSymLink());QCOMPARE(info.ownerId(),uint(getuid()));QVERIFY(!(info.permissions()&(QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup|QFile::ReadOther|QFile::WriteOther|QFile::ExeOther)));
  const auto key=QString::fromLatin1(QCryptographicHash::hash(d.credentialReference["reference"].toString().toUtf8(),QCryptographicHash::Sha256).toHex());QLockFile lock(QDir(root).filePath(key+".lock"));lock.setStaleLockTime(0);QVERIFY(lock.tryLock());
  const auto r=upload(provider,d);QCOMPARE(r.status,QString("failed"));QCOMPARE(r.error.code,QString("proton_account_busy"));QCOMPARE(provider.diagnostics()["activeHelpers"].toInt(),0);
 }
};
QTEST_GUILESS_MAIN(ProtonTransportTest)
#include "test_proton_transport.moc"
