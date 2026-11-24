// TextJob against a real child process (#1428): the fake putgd-ocr helper
// speaks the real protocol, so this is the parent's side of the contract.
#include "core/TextJob.h"
#include "fixtures/FakeTextHelper.h"
#include <QFileInfo>
#include <QtTest>

using namespace putgd;

class TextJobTest:public QObject {
 Q_OBJECT
private slots:
 // A helper from another build, or one that returns more than the output cap,
 // is refused: its result would otherwise be read by a parent that expects
 // other shapes or other bounds. The first row is the control: the same path
 // succeeds, and the child got the two memfds and nothing else of the parent's
 // (no other descriptor, no environment), with its input sealed.
 void resultIsAcceptedOnlyFromThisBuildWithinTheOutputCap_data() {
  QTest::addColumn<QString>("mode");QTest::addColumn<QString>("code");
  QTest::newRow("this build")<<QString("describe")<<QString();
  QTest::newRow("another build")<<QString("wrong-build")<<QString("text_incompatible");
  QTest::newRow("over the output cap")<<QString("oversize")<<QString("text_incompatible");
 }
 void resultIsAcceptedOnlyFromThisBuildWithinTheOutputCap() {
  QFETCH(QString,mode);QFETCH(QString,code);
  test::FakeTextHelper helper;QVERIFY(helper.directory.isValid());QVERIFY(helper.plan({{"mode",mode}}));
  TextJob job(QStringLiteral(PUTGD_TEXT_JOB_FIXTURE));job.setDataDirectories(helper.dataDirectories());
  QImage image(64,32,QImage::Format_Grayscale8);image.fill(7);
  TextJob::Options options;options.languages={"eng"};Error error;
  const auto result=job.recognize(image,options,error);
  QCOMPARE(error.code,code);
  if(code.isEmpty()) {
   QVERIFY2(result.text.contains("image 64x32 stride 64 sum 14336\n"),qPrintable(result.text));
   // The colour plane too: the same grey 7 as packed RGB (#1748).
   QVERIFY2(result.text.contains("\ncolour stride 192 sum 43008\n"),qPrintable(result.text));
   QVERIFY2(result.text.contains("\ndescriptors 0,1,2,3,4\n"),qPrintable(result.text));
   QVERIFY2(result.text.contains("\nenvironment LC_ALL,OMP_THREAD_LIMIT,PUTGD_TEXT_DATA\n"),qPrintable(result.text));
   QVERIFY2(result.text.endsWith("input sealed"),qPrintable(result.text));
  } else QVERIFY(result.text.isEmpty());
  // The exact child is gone when the job returns.
  const auto pid=helper.pid();QVERIFY(pid>0);QVERIFY(!QFileInfo::exists("/proc/"+QString::number(pid)));
  QVERIFY(!TextJob::busy());
 }
 // A scaled screen's capture carries a device pixel ratio. The helper gets its
 // pixels, every one, across the strips the parent writes: drawn at a point,
 // each strip came out at half size with white beside and below it.
 void aScaledCaptureKeepsItsPixels() {
  test::FakeTextHelper helper;QVERIFY(helper.directory.isValid());QVERIFY(helper.plan({{"mode","describe"}}));
  TextJob job(QStringLiteral(PUTGD_TEXT_JOB_FIXTURE));job.setDataDirectories(helper.dataDirectories());
  QImage image(64,300,QImage::Format_Grayscale8);image.fill(7);image.setDevicePixelRatio(2);
  TextJob::Options options;options.languages={"eng"};Error error;
  const auto result=job.recognize(image,options,error);
  QVERIFY2(error.code.isEmpty(),qPrintable(error.code));
  QVERIFY2(result.text.contains("image 64x300 stride 64 sum 134400\n"),qPrintable(result.text));
  QVERIFY2(result.text.contains("\ncolour stride 192 sum 403200\n"),qPrintable(result.text));
 }
};
QTEST_GUILESS_MAIN(TextJobTest)
#include "test_text_job.moc"
