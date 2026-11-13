// One smoke test of the real putgd-ocr (#1428): confined, through its
// protocol, it reads large clear text and one QR code. Recognition quality is
// scored on the corpus, never here: Tesseract differs by distribution.
#include "integration/ocr/OcrParent.h"
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QJsonArray>
#include <QPainter>
#include <QTest>

class OcrHelper final:public QObject {
 Q_OBJECT
private slots:
 void readsClearTextAndACode(){
  putgd::test::OcrRequest request;request.helper=qEnvironmentVariable("PUTGD_OCR_HELPER");
  QVERIFY(QFileInfo(request.helper).isExecutable());
  const auto data=putgd::test::defaultDataDirectory();
  if(data.isEmpty())QSKIP("English language data is not installed");
  request.dataDirectories={data};
  QImage image(1000,420,QImage::Format_RGB32);image.fill(Qt::white);
  {
   QPainter painter(&image);QFont font(QStringLiteral("DejaVu Sans"));font.setPixelSize(40);painter.setFont(font);painter.setPen(Qt::black);
   painter.drawText(40,80,QStringLiteral("HELLO WORLD"));
   painter.drawImage(QRect(40,140,300,300),QImage(QStringLiteral(PUTGD_SOURCE_DIR "/tests/fixtures/ocr/qr-url.png")));
  }
  const auto run=putgd::test::runOcr(image,request);
  QVERIFY2(run.error.isEmpty(),qPrintable(run.error));
  QVERIFY2(run.result["text"].toString().contains(QLatin1String("WORLD")),qPrintable(run.result["text"].toString()));
  // ZXing's format names differ between 2.x and 3.x; a mismatch would drop every code.
  const auto codes=run.result["codes"].toArray();QCOMPARE(codes.size(),1);
  QCOMPARE(codes.first().toObject()["format"].toString(),QStringLiteral("QRCode"));
  QCOMPARE(codes.first().toObject()["text"].toString(),QStringLiteral("https://example.test/ocr?id=1428"));
 }
};
QTEST_MAIN(OcrHelper)
#include "test_ocr_helper.moc"
