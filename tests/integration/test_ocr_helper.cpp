// One smoke test of the real putgd-ocr (#1428), driven by the contract's
// TextJob as the resident and the editor drive it: confined, it reads large
// clear text and one QR code. Recognition quality is scored on the corpus,
// never here: Tesseract differs by distribution. TextJob's refusals (wrong
// build, a result over the cap) are tested against the contract's fake helper.
#include "core/TextJob.h"
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QPainter>
#include <QTest>

class OcrHelper final:public QObject {
 Q_OBJECT
private slots:
 void readsClearTextAndACode(){
  const auto helper=qEnvironmentVariable("PUTGD_OCR_HELPER");
  QVERIFY(QFileInfo(helper).isExecutable());
  putgd::TextJob job(helper);
  QStringList data;
  for(const auto& d:{QStringLiteral("/usr/share/tesseract-ocr/5/tessdata"),QStringLiteral("/usr/share/tesseract/tessdata"),QStringLiteral("/usr/share/tessdata")})
   if(QFileInfo::exists(d+QStringLiteral("/eng.traineddata")))data.append(d);
  if(data.isEmpty())QSKIP("English language data is not installed");
  job.setDataDirectories(data);
  QImage image(1000,420,QImage::Format_RGB32);image.fill(Qt::white);
  {
   QPainter painter(&image);QFont font(QStringLiteral("DejaVu Sans"));font.setPixelSize(40);painter.setFont(font);painter.setPen(Qt::black);
   painter.drawText(40,80,QStringLiteral("HELLO WORLD"));
   painter.drawImage(QRect(40,140,300,300),QImage(QStringLiteral(PUTGD_SOURCE_DIR "/tests/fixtures/ocr/qr-url.png")));
  }
  putgd::TextJob::Options options;options.languages={QStringLiteral("eng")};options.languagesRequired=true;
  putgd::Error error;
  const auto result=job.recognize(putgd::TextJob::grey(image),options,error);
  QVERIFY2(!error,qPrintable(error.code));
  QVERIFY2(result.text.contains(QLatin1String("WORLD")),qPrintable(result.text));
  // ZXing's format names differ between 2.x and 3.x; a mismatch would drop every code.
  QCOMPARE(result.codes.size(),1);
  QCOMPARE(result.codes.first().format,QStringLiteral("QRCode"));
  QCOMPARE(result.codes.first().text,QStringLiteral("https://example.test/ocr?id=1428"));
 }
};
QTEST_MAIN(OcrHelper)
#include "test_ocr_helper.moc"
