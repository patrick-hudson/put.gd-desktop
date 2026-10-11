#include "platform/Geometry.h"
#include <QtTest>
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
 void rejectsStaleOrUnboundedMetadata() {
  CaptureFrame f{"bad",QRectF(0,0,100,100),QRect(0,0,100,100),1,0,QImage(100,100,QImage::Format_ARGB32)};Error error;
  QVERIFY(composeFrames({f},f.logicalRect,9999,error).isNull());QCOMPARE(error.code,QString("image_limit"));
  error={};f.logicalRect.setWidth(0);QVERIFY(composeFrames({f},QRectF(0,0,10,10),10000,error).isNull());QCOMPARE(error.code,QString("capture_geometry"));
 }
};
QTEST_GUILESS_MAIN(GeometryTest)
#include "test_geometry.moc"
