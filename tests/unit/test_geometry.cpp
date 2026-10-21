#include "platform/Geometry.h"
#include <QtTest>
#include <QBuffer>
#include <QImageReader>
using namespace putgd;
class GeometryTest: public QObject {
 Q_OBJECT
private slots:
 void singleFramePreservesPixels() {
  CaptureFrame frame{"one",QRectF(-100,20,100,50),QRect(-200,40,200,100),2,0,QImage(200,100,QImage::Format_ARGB32)};
  frame.pixels.fill(Qt::red);Error error;
  auto result=composeFrames({frame},QRectF(-75,30,50,20),100000,error);
  QVERIFY(!error);QCOMPARE(result.size(),QSize(100,40));QCOMPARE(result.pixelColor(0,0),QColor(Qt::red));
 }
 void mixedDensityAndTransparentGap() {
  CaptureFrame a{"left",QRectF(-100,0,100,100),QRect(-100,0,100,100),1,0,QImage(100,100,QImage::Format_ARGB32)};
  CaptureFrame b{"right",QRectF(20,0,50,100),QRect(40,0,100,200),2,90,QImage(100,200,QImage::Format_ARGB32)};
  a.pixels.fill(Qt::red);b.pixels.fill(Qt::blue);Error error;
  auto result=composeFrames({a,b},QRectF(-100,0,170,100),100000,error);
  QVERIFY(!error);QCOMPARE(result.size(),QSize(340,200));QCOMPARE(result.pixelColor(10,10),QColor(Qt::red));QCOMPARE(result.pixelColor(210,10).alpha(),0);QCOMPARE(result.pixelColor(250,10),QColor(Qt::blue));
 }
 void fractionalSingleFramePreservesBoundaryPixels() {
  CaptureFrame frame{"fractional",QRectF(0,0,3.2,3.2),QRect(0,0,4,4),1.25,0,QImage(4,4,QImage::Format_ARGB32)};
  frame.pixels.fill(Qt::black);frame.pixels.setPixelColor(0,0,Qt::red);frame.pixels.setPixelColor(1,0,Qt::blue);Error error;
  auto result=composeFrames({frame},QRectF(.4,0,.8,.8),100,error);
  QVERIFY(!error);QCOMPARE(result.size(),QSize(2,1));QCOMPARE(result.pixelColor(0,0),QColor(Qt::red));QCOMPARE(result.pixelColor(1,0),QColor(Qt::blue));
 }
 // #331: three monitors with a shorter middle one, so the union is not a
 // rectangle. Each case is a selection the native region overlay allows.
 static QList<CaptureFrame> mixedThreeMonitorLayout() {
  QList<CaptureFrame> frames{{"DP-1-2",QRectF(0,0,2560,1440),QRect(0,0,2560,1440),1,0,QImage(2560,1440,QImage::Format_RGB32)},
   {"eDP-1",QRectF(2560,0,1920,1080),QRect(2560,0,1920,1080),1,0,QImage(1920,1080,QImage::Format_RGB32)},
   {"DP-3",QRectF(4480,0,2560,1440),QRect(4480,0,2560,1440),1,0,QImage(2560,1440,QImage::Format_RGB32)}};
  frames[0].pixels.fill(Qt::red);frames[1].pixels.fill(Qt::green);frames[2].pixels.fill(Qt::blue);
  return frames;
 }
 void nonRectangularUnionSelections_data() {
  QTest::addColumn<QRectF>("selection");QTest::addColumn<QSize>("size");QTest::addColumn<QList<QPoint>>("points");QTest::addColumn<QList<QColor>>("colors");
  const QColor gap(0,0,0,0);
  QTest::newRow("inside-one")<<QRectF(3000,200,300,200)<<QSize(300,200)<<QList<QPoint>{{0,0},{299,199}}<<QList<QColor>{Qt::green,Qt::green};
  QTest::newRow("spanning-two")<<QRectF(2400,500,400,200)<<QSize(400,200)<<QList<QPoint>{{159,0},{160,0},{399,199}}<<QList<QColor>{Qt::red,Qt::green,Qt::green};
  QTest::newRow("gap-below-edp")<<QRectF(2400,1000,400,300)<<QSize(400,300)<<QList<QPoint>{{10,10},{200,79},{200,80},{10,299}}<<QList<QColor>{Qt::red,Qt::green,gap,Qt::red};
  QTest::newRow("all-three-and-gap")<<QRectF(2000,900,3000,500)<<QSize(3000,500)<<QList<QPoint>{{0,0},{600,179},{600,180},{2999,499}}<<QList<QColor>{Qt::red,Qt::green,gap,Qt::blue};
  QTest::newRow("full-union")<<QRectF(0,0,7040,1440)<<QSize(7040,1440)<<QList<QPoint>{{0,0},{2560,1079},{2560,1080},{4479,1439},{4480,1439},{7039,0}}<<QList<QColor>{Qt::red,Qt::green,gap,gap,Qt::blue,Qt::blue};
 }
 void nonRectangularUnionSelections() {
  QFETCH(QRectF,selection);QFETCH(QSize,size);QFETCH(QList<QPoint>,points);QFETCH(QList<QColor>,colors);
  Error error;const auto result=composeFrames(mixedThreeMonitorLayout(),selection,7040LL*1440,error);
  QVERIFY2(!error,qPrintable(error.code+": "+error.message));QCOMPARE(result.size(),size);
  for(int i=0;i<points.size();++i)QVERIFY2(result.pixelColor(points[i])==colors[i],qPrintable(QStringLiteral("pixel %1,%2").arg(points[i].x()).arg(points[i].y())));
  // The capture helper's PNG round trip keeps the size and a format the
  // resident accepts (8-bit RGB or RGBA), gap transparency included.
  QBuffer png;QVERIFY(png.open(QIODevice::ReadWrite));QVERIFY(result.save(&png,"PNG"));png.seek(0);
  QImageReader reader(&png,"PNG");reader.setAutoDetectImageFormat(false);QCOMPARE(reader.size(),size);
  QVERIFY(reader.imageFormat()==QImage::Format_RGB32||reader.imageFormat()==QImage::Format_ARGB32);
  const auto decoded=reader.read();QCOMPARE(decoded.size(),size);
  for(int i=0;i<points.size();++i)QCOMPARE(decoded.pixelColor(points[i]).alpha(),colors[i].alpha());
 }
 void rejectsStaleOrUnboundedMetadata() {
  CaptureFrame f{"bad",QRectF(0,0,100,100),QRect(0,0,100,100),1,0,QImage(100,100,QImage::Format_ARGB32)};Error error;
  QVERIFY(composeFrames({f},f.logicalRect,9999,error).isNull());QCOMPARE(error.code,QString("image_limit"));
  error={};f.logicalRect.setWidth(0);QVERIFY(composeFrames({f},QRectF(0,0,10,10),10000,error).isNull());QCOMPARE(error.code,QString("capture_geometry"));
 }
};
QTEST_GUILESS_MAIN(GeometryTest)
#include "test_geometry.moc"
