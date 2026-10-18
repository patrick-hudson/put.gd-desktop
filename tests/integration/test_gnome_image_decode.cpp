#include "platform/gnome/GnomeProtocol.h"
#include "../fixtures/ImagePluginMappings.h"
#include <QtTest>
#include <QJsonArray>
using namespace putgd;
using test::loadedImagePlugins;
// GNOME transfers decode in the Core resident: PNG in process with format
// detection off, other formats in the transient image job. Qt loads every
// installed image plugin for an unnamed reader, and on Qt 6.4.2 and 6.8.2
// also for a "png"-named one unless detection is off.
class GnomeImageDecodeTest final:public QObject {
 Q_OBJECT
 static QJsonObject meta(const QByteArray& bytes,const QString& mime){return {{"version",1},{"requestId","synthetic-request"},{"mime",mime},{"bytes",bytes.size()}};}
private slots:
 void initTestCase(){
  // A synthetic plugin is always installed, so an in-process plugin load is
  // observable even where the system ships no image plugins.
  QCoreApplication::addLibraryPath(QStringLiteral(PUTGD_IMAGE_PLUGIN_FIXTURE_DIR));
  QCOMPARE(loadedImagePlugins(),QStringList{});
 }
 void cleanup(){QCOMPARE(loadedImagePlugins(),QStringList{});}
 void clipboardPngDecodesInProcessWithoutPlugins(){
  const auto& png=test::syntheticPng;auto result=gnome::decodeTransfer(meta(png,"image/png"),png,"synthetic-request",{});
  QVERIFY2(!result.error,qPrintable(result.error.code));QCOMPARE(result.payload.kind,QString("image"));QVERIFY(test::syntheticPixels(result.payload.image));
  ResourceLimits limits;limits.imagePixels=5;
  QCOMPARE(gnome::decodeTransfer(meta(png,"image/png"),png,"synthetic-request",limits).error.code,QString("input_excessive"));
 }
 void captureFramesDecodeWithoutPlugins(){
  const auto bytes=test::syntheticPng+test::syntheticPng;const auto length=test::syntheticPng.size();
  QJsonArray frames;
  frames.append(QJsonObject{{"offset",0},{"length",length},{"identity","left"},{"logical",QJsonArray{0,0,3,2}},{"native",QJsonArray{0,0,3,2}},{"density",1},{"rotation",0}});
  frames.append(QJsonObject{{"offset",length},{"length",length},{"identity","right"},{"logical",QJsonArray{3,0,3,2}},{"native",QJsonArray{0,0,3,2}},{"density",1},{"rotation",0}});
  auto metadata=meta(bytes,"application/x-putgd-capture");metadata["frames"]=frames;metadata["selection"]=QJsonArray{0,0,6,2};
  auto result=gnome::decodeTransfer(metadata,bytes,"synthetic-request",{});QVERIFY2(!result.error,qPrintable(result.error.code));
  QCOMPARE(result.payload.image.size(),QSize(6,2));QVERIFY(test::syntheticPixels(result.payload.image.copy(0,0,3,2)));QVERIFY(test::syntheticPixels(result.payload.image.copy(3,0,3,2)));
 }
 void otherFormatsDecodeInImageJob(){
  const auto& bmp=test::syntheticBmp;
  auto result=gnome::decodeTransfer(meta(bmp,"image/bmp"),bmp,"synthetic-request",{});
  QVERIFY2(!result.error,qPrintable(result.error.code));QVERIFY(test::syntheticPixels(result.payload.image));
  // A claimed PNG keeps Qt's content detection, out of process.
  result=gnome::decodeTransfer(meta(bmp,"image/png"),bmp,"synthetic-request",{});
  QVERIFY2(!result.error,qPrintable(result.error.code));QVERIFY(test::syntheticPixels(result.payload.image));
  const QByteArray invalid="synthetic invalid image";
  QCOMPARE(gnome::decodeTransfer(meta(invalid,"image/png"),invalid,"synthetic-request",{}).error.code,QString("source_unsupported"));
 }
};
QTEST_GUILESS_MAIN(GnomeImageDecodeTest)
#include "test_gnome_image_decode.moc"
