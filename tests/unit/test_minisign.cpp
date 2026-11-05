#include "cli/Minisign.h"
#include "fixtures/MinisignFixture.h"
#include <QFile>
#include <QTest>
using namespace putgd;
namespace {
QByteArray read(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
}
// SHA256SUMS signatures (#293 phase 3): what the install child accepts.
class MinisignTest:public QObject {
 Q_OBJECT
private slots:
 // The published nightly 847 SHA256SUMS and the signer's .minisig, checked
 // with the key this build trusts for nightly.
 void verifiesTheSignersRealSignatureWithThePinnedKey() {
  const auto sums=read("tests/fixtures/minisign/SHA256SUMS-v0.1.1-nightly.847"),signature=read("tests/fixtures/minisign/SHA256SUMS-v0.1.1-nightly.847.minisig");
  QVERIFY(!sums.isEmpty()&&!signature.isEmpty());QVERIFY(minisign::available());
  QString trusted;const auto error=minisign::verify(sums,signature,minisign::releaseKey("nightly"),&trusted);
  QVERIFY2(!error,qPrintable(error.message));QCOMPARE(trusted,QString("put.gd v0.1.1-nightly.847 nightly"));
  QCOMPARE(minisign::releaseKey("nightly"),read("packaging/linux/repo/keys/putgd-nightly.minisign.pub").split('\n').value(1));
  // The stable key didn't sign it; a changed byte isn't what was signed.
  QCOMPARE(minisign::verify(sums,signature,minisign::releaseKey("stable")).details["check"].toString(),QString("keyId"));
  auto tampered=sums;tampered[10]=tampered[10]=='0'?'1':'0';
  QCOMPARE(minisign::verify(tampered,signature,minisign::releaseKey("nightly")).details["check"].toString(),QString("signature"));
  QVERIFY(minisign::releaseKey("beta").isEmpty());
 }
 void checksTheSignatureAndItsTrustedComment() {
  MinisignKey key;const QByteArray sums="abc  putgd.AppImage\n";
  QString trusted;
  QVERIFY(!minisign::verify(sums,key.signature(sums,"put.gd v0.1.1-nightly.801 nightly"),key.publicFile(),&trusted));
  QCOMPARE(trusted,QString("put.gd v0.1.1-nightly.801 nightly"));
  // Legacy, not prehashed.
  QVERIFY(!minisign::verify(sums,key.signature(sums,"legacy",false),key.publicFile()));
  // A swapped trusted comment breaks the global signature.
  auto signature=key.signature(sums,"put.gd v0.1.1-nightly.801 nightly");
  signature.replace("nightly.801 nightly","nightly.999 nightly");
  QCOMPARE(minisign::verify(sums,signature,key.publicFile()).details["check"].toString(),QString("trustedComment"));
  // Another key with the same id.
  MinisignKey impostor;
  QCOMPARE(minisign::verify(sums,impostor.signature(sums,"x"),key.publicFile()).details["check"].toString(),QString("signature"));
  QCOMPARE(minisign::verify(sums,"",key.publicFile()).details["check"].toString(),QString("format"));
  QCOMPARE(minisign::verify(sums,"untrusted comment: x\n!!!\ntrusted comment: y\nzz\n",key.publicFile()).details["check"].toString(),QString("format"));
  QCOMPARE(minisign::verify(sums,key.signature(sums,"x"),"not a key").details["check"].toString(),QString("key"));
  QCOMPARE(minisign::verify(sums,key.signature(sums,"x"),key.publicFile()).code,QString());
 }
};
QTEST_GUILESS_MAIN(MinisignTest)
#include "test_minisign.moc"
