#include "platform/kde/KWinTransfer.h"
#include <QtTest>
#include <QSignalSpy>
#include <unistd.h>
#include <cstring>
#include "fixtures/Waits.h"
using namespace putgd;
class KWinTransferTest:public QObject {
 Q_OBJECT
private slots:
 void metadataRejectsInvalidAndExcessive(){
  ResourceLimits limits;limits.imagePixels=16;limits.imageBytes=64;
  QVariantMap m{{"type","raw"},{"width",uint(2)},{"height",uint(2)},{"stride",uint(8)},{"format",uint(QImage::Format_ARGB32)},{"scale",1.0}};
  Error error;QVERIFY(!decodeKWinImage(m,QByteArray(16,'\0'),limits,error).isNull());QVERIFY(!error);
  m["stride"]=uint(7);QVERIFY(decodeKWinImage(m,QByteArray(16,'\0'),limits,error).isNull());QCOMPARE(error.code,QString("capture_metadata"));
  m["stride"]=uint(8);m["width"]=uint(1000000000);QVERIFY(decodeKWinImage(m,{},limits,error).isNull());QCOMPARE(error.code,QString("input_excessive"));
  m["width"]=uint(2);m["format"]=uint(99999);QVERIFY(decodeKWinImage(m,{},limits,error).isNull());QCOMPARE(error.code,QString("capture_metadata"));
  m["format"]=uint(QImage::Format_RGBA64);m["stride"]=uint(20);QVERIFY(decodeKWinImage(m,QByteArray(40,'\0'),limits,error).isNull());QCOMPARE(error.code,QString("capture_metadata"));
  m["stride"]=uint(8);m["format"]=uint(QImage::Format_ARGB32);m["scale"]=0.0;QVERIFY(decodeKWinImage(m,QByteArray(16,'\0'),limits,error).isNull());
 }
 void paddedRowsDetachNativeBytes(){ResourceLimits l;QVariantMap m{{"type","raw"},{"width",uint(2)},{"height",uint(2)},{"stride",uint(12)},{"format",uint(QImage::Format_ARGB32)},{"scale",1.25}};QByteArray b(24,'\0');QRgb red=qRgb(255,0,0);memcpy(b.data(),&red,sizeof red);Error e;auto image=decodeKWinImage(m,b,l,e);QVERIFY(!e);QCOMPARE(image.pixel(0,0),red);QCOMPARE(image.devicePixelRatio(),1.25);b.fill('\0');QCOMPARE(image.pixel(0,0),red);}
 void truncatedAndSurplusFail(){ResourceLimits l;QVariantMap m{{"type","raw"},{"width",uint(2)},{"height",uint(2)},{"stride",uint(8)},{"format",uint(QImage::Format_ARGB32)},{"scale",1.0}};for(int size:{15,17}){Error e;QVERIFY(decodeKWinImage(m,QByteArray(size,'\0'),l,e).isNull());QCOMPARE(e.code,QString("capture_transfer"));}}
 void excessiveTransferIsBounded(){int p[2];QVERIFY(pipe(p)==0);AsyncFdReader reader;bool done=false;Error error;reader.start(p[0],4,1000,[&](QByteArray b,Error e){QVERIFY(b.isEmpty());error=e;done=true;});QCOMPARE(write(p[1],"12345",5),ssize_t(5));close(p[1]);PUTGD_TRY_VERIFY(done);QCOMPARE(error.code,QString("input_excessive"));}
};
QTEST_GUILESS_MAIN(KWinTransferTest)
#include "test_kwin_transfer.moc"
