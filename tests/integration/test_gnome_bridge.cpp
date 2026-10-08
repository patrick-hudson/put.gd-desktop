#include "platform/gnome/GnomeProtocol.h"
#include <QTest>
#include <QBuffer>
#include <QImage>
#include <QTemporaryDir>
#include <QFile>
#include <QUrl>
#include <QJsonArray>
using namespace putgd;
class GnomeBridgeTest final:public QObject {
 Q_OBJECT
 QJsonObject meta(const QByteArray& bytes,const QString& mime="text/plain;charset=utf-8") {
  return {{"version",1},{"requestId","synthetic-request"},{"mime",mime},{"bytes",bytes.size()}};
 }
private slots:
 void plainTextUrlsRemainPayloadBytes() {
  const QByteArray bytes="https://example.invalid/synthetic";
  auto result=gnome::decodeTransfer(meta(bytes),bytes,"synthetic-request",{});
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QCOMPARE(result.payload.kind,QString("text"));QCOMPARE(result.payload.bytes,bytes);
 }
 void forgedStaleAndLengthMismatchAreRejected() {
  const QByteArray bytes="synthetic";
  QCOMPARE(gnome::decodeTransfer(meta(bytes),bytes,"another-request",{}).error.code,QString("bridge_protocol"));
  auto metadata=meta(bytes);metadata["bytes"]=bytes.size()+1;
  QCOMPARE(gnome::decodeTransfer(metadata,bytes,"synthetic-request",{}).error.code,QString("bridge_protocol"));
  metadata=meta(bytes);metadata["version"]=2;
  QCOMPARE(gnome::decodeTransfer(metadata,bytes,"synthetic-request",{}).error.code,QString("bridge_protocol"));
 }
 void boundedTextAndImageBeforeDecode() {
  ResourceLimits limits;limits.clipboardTextBytes=4;const QByteArray text="oversized";
  QCOMPARE(gnome::decodeTransfer(meta(text),text,"synthetic-request",limits).error.code,QString("input_excessive"));
  QImage image(16,8,QImage::Format_ARGB32);image.fill(Qt::green);QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);QVERIFY(image.save(&buffer,"PNG"));
  limits.imagePixels=100;
  QCOMPARE(gnome::decodeTransfer(meta(png,"image/png"),png,"synthetic-request",limits).error.code,QString("input_excessive"));
  limits.imagePixels=200;auto result=gnome::decodeTransfer(meta(png,"image/png"),png,"synthetic-request",limits);
  QVERIFY(!result.error);QCOMPARE(result.payload.image.size(),image.size());QCOMPARE(result.payload.image.pixelColor(4,3),QColor(Qt::green));
 }
 void orderedLocalUrisAndForeignRejection() {
  QTemporaryDir dir;QStringList expected{dir.filePath("a synthetic.txt"),dir.filePath("b synthetic.txt")};
  QByteArray bytes;for(const auto& path:expected){QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic");bytes+=QUrl::fromLocalFile(path).toEncoded()+"\r\n";}
  auto result=gnome::decodeTransfer(meta(bytes,"text/uri-list"),bytes,"synthetic-request",{});
  QVERIFY(!result.error);QCOMPARE(result.payload.kind,QString("files"));QCOMPARE(result.payload.files,expected);
  bytes+="https://example.invalid/remote\r\n";
  QCOMPARE(gnome::decodeTransfer(meta(bytes,"text/uri-list"),bytes,"synthetic-request",{}).error.code,QString("source_unsupported"));
 }
 void captureFramesComposeGapsAndRejectMalformedGeometry() {
  QImage image(8,4,QImage::Format_ARGB32);image.fill(Qt::red);QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);QVERIFY(image.save(&buffer,"PNG"));
  const QByteArray bytes=png+png;
  QJsonArray frames;
  frames.append(QJsonObject{{"offset",0},{"length",png.size()},{"identity","left"},{"logical",QJsonArray{-4,0,4,2}},{"native",QJsonArray{0,0,8,4}},{"density",2},{"rotation",0}});
  frames.append(QJsonObject{{"offset",png.size()},{"length",png.size()},{"identity","right"},{"logical",QJsonArray{2,0,4,2}},{"native",QJsonArray{0,0,8,4}},{"density",2},{"rotation",0}});
  auto metadata=meta(bytes,"application/x-putgd-capture");metadata["frames"]=frames;metadata["selection"]=QJsonArray{-4,0,10,2};
  auto result=gnome::decodeTransfer(metadata,bytes,"synthetic-request",{});
  QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.image.size(),QSize(20,4));
  QCOMPARE(result.payload.image.pixelColor(2,2),QColor(Qt::red));QCOMPARE(result.payload.image.pixelColor(9,2).alpha(),0);
  auto frame=frames[0].toObject();frame["length"]=bytes.size()+1;frames[0]=frame;metadata["frames"]=frames;
  QCOMPARE(gnome::decodeTransfer(metadata,bytes,"synthetic-request",{}).error.code,QString("bridge_protocol"));
 }
 void malformedImageAndUnsupportedMimeAreRejected() {
  const QByteArray bytes="synthetic invalid image";
  QCOMPARE(gnome::decodeTransfer(meta(bytes,"image/png"),bytes,"synthetic-request",{}).error.code,QString("source_unsupported"));
  QCOMPARE(gnome::decodeTransfer(meta(bytes,"application/octet-stream"),bytes,"synthetic-request",{}).error.code,QString("source_unsupported"));
 }
};
QTEST_GUILESS_MAIN(GnomeBridgeTest)
#include "test_gnome_bridge.moc"
