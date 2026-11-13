// History's Show QR code (#1428, decision 10): the code put.gd writes must read
// back. tests/fixtures/qr/codes.json holds known-good module matrices made by an
// independent encoder (segno, level M, byte mode, no boost) and read back by
// zbarimg and ZXingReader; see tests/fixtures/qr/README.md.
#include "core/QrImage.h"
#include "core/SanitizedPng.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
using namespace putgd;
namespace {
QByteArray longestLink() {
 QByteArray link="https://bucket.s3.example.com/captures/2026-11-12%20screen.png?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=EXAMPLE&X-Amz-Signature=";
 while(link.size()<qr::maximumBytes)link+="0123456789abcdef";
 return link.left(qr::maximumBytes);
}
}
class QrCodeTest final:public QObject {
 Q_OBJECT
private slots:
 void writtenCodeReadsBack_data() {
  QTest::addColumn<QByteArray>("link");QTest::addColumn<QString>("fixture");QTest::addColumn<QString>("error");
  QTest::newRow("short link")<<QByteArray("https://example.com/i/Kx7Qm2Zp.png")<<"short"<<"";
  QTest::newRow("longest link at level M")<<longestLink()<<"longest"<<"";
  QTest::newRow("one byte too long")<<longestLink()+"0"<<""<<"qr_too_long";
 }
 // The exported PNG, read back as modules at its own scale, is the known-good
 // matrix inside a white 4-module quiet zone, at the smallest whole scale
 // reaching 1,024 px. A link too long for level M is refused, not written.
 void writtenCodeReadsBack() {
  QFETCH(QByteArray,link);QFETCH(QString,fixture);QFETCH(QString,error);
  qr::Matrix matrix;const auto encoded=qr::encode(link,matrix);
  QCOMPARE(encoded.code,error);
  if(!error.isEmpty()){QVERIFY(matrix.isNull());return;}
  QFile file(QStringLiteral(PUTGD_TEST_FIXTURES "/qr/codes.json"));QVERIFY(file.open(QIODevice::ReadOnly));
  const auto expected=QJsonDocument::fromJson(file.readAll()).object()[fixture].toObject();
  QCOMPARE(expected["bytes"].toInt(),int(link.size()));
  const auto rows=expected["rows"].toArray();QCOMPARE(matrix.size,int(rows.size()));
  const auto png=qr::png(matrix);QSize size;QVERIFY(!inspectSanitizedPng(png,&size));
  // The PNGs the fixture's decoders read back (tests/fixtures/qr/README.md).
  if(const auto directory=qEnvironmentVariable("PUTGD_QR_WRITE_DIR");!directory.isEmpty()){QFile out(directory+"/"+fixture+".png");QVERIFY(out.open(QIODevice::WriteOnly)&&out.write(png)==png.size());}
  QImage image;QVERIFY(image.loadFromData(png,"PNG"));image=image.convertToFormat(QImage::Format_Grayscale8);
  const int edge=matrix.edge(),scale=image.width()/edge;
  QCOMPARE(image.width(),edge*scale);QCOMPARE(image.height(),image.width());QCOMPARE(size,image.size());
  QVERIFY(scale*edge>=qr::exportEdge);QVERIFY((scale-1)*edge<qr::exportEdge);
  for(int y=0;y<edge;++y)for(int x=0;x<edge;++x) {
   // Every pixel of a module has its colour; sample its centre and corners.
   const bool inside=x>=qr::quietZone&&y>=qr::quietZone&&x<edge-qr::quietZone&&y<edge-qr::quietZone;
   const bool dark=inside&&rows.at(y-qr::quietZone).toString().at(x-qr::quietZone)==u'1';
   for(const auto& point:{QPoint(scale/2,scale/2),QPoint(0,0),QPoint(scale-1,scale-1)}) {
    const int grey=qGray(image.pixel(x*scale+point.x(),y*scale+point.y()));
    if(grey!=(dark?0:255))QFAIL(qPrintable(QString("module %1,%2 of %3 is wrong").arg(x).arg(y).arg(fixture)));
   }
  }
 }
};
QTEST_APPLESS_MAIN(QrCodeTest)
#include "test_qr_code.moc"
