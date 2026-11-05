#include "ui/PreviewReader.h"
#include <QtTest>
#include <QImageReader>
#include <QTemporaryDir>
#include <QtEndian>
#include <QFile>
using namespace putgd;
namespace {
quint32 crc32(const QByteArray& bytes) {
 quint32 crc = 0xffffffffu;
 for (const unsigned char byte : bytes) {
  crc ^= byte;
  for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
 }
 return ~crc;
}
QByteArray chunk(const char* type, const QByteArray& data) {
 QByteArray out;
 char length[4];
 qToBigEndian<quint32>(quint32(data.size()), length);
 out.append(length, 4);
 const QByteArray body = QByteArray(type, 4) + data;
 out += body;
 char crc[4];
 qToBigEndian<quint32>(crc32(body), crc);
 return out + QByteArray(crc, 4);
}
// A 2x2 RGB PNG written by hand, so this process never runs QImageWriter,
// which on Qt 6.4 to 6.8 would itself load every image-format plugin.
QByteArray pngBytes() {
 QByteArray header;
 char size[8];
 qToBigEndian<quint32>(2, size);
 qToBigEndian<quint32>(2, size + 4);
 header.append(size, 8);
 header += QByteArray("\x08\x02\x00\x00\x00", 5);
 QByteArray raw;
 raw += '\0'; raw += QByteArray("\xff\x00\x00\x00\xff\x00", 6);
 raw += '\0'; raw += QByteArray("\x00\x00\xff\xff\xff\xff", 6);
 const QByteArray idat = qCompress(raw, 9).mid(4); // qCompress prefixes the zlib stream with a length
 return QByteArray("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", header) + chunk("IDAT", idat) + chunk("IEND", {});
}
QStringList mappedImageFormatPlugins() {
 QStringList found;
 QFile maps("/proc/self/maps");
 if (!maps.open(QIODevice::ReadOnly)) return found;
 for (const auto& line : maps.readAll().split('\n'))
  if (line.contains("/imageformats/")) found << QString::fromLocal8Bit(line.mid(line.lastIndexOf('/') + 1));
 found.removeDuplicates();
 return found;
}
}
class PreviewReaderContract : public QObject {
 Q_OBJECT
private slots:
 void pngDecodesThroughTheBuiltInHandlerWithoutPlugins() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto path = dir.filePath("preview.png");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QVERIFY(file.write(pngBytes()) > 0);
  file.close();
  QVERIFY2(mappedImageFormatPlugins().isEmpty(), qPrintable(mappedImageFormatPlugins().join(" ")));
  QImageReader reader(path);
  preparePreviewReader(reader, path);
  QCOMPARE(reader.format(), QByteArray("png"));
  QVERIFY(!reader.autoDetectImageFormat());
  QCOMPARE(reader.size(), QSize(2, 2));
  const auto image = reader.read();
  QVERIFY(!image.isNull());
  QCOMPARE(image.pixelColor(0, 0), QColor(255, 0, 0));
  QCOMPARE(image.pixelColor(1, 0), QColor(0, 255, 0));
  QCOMPARE(image.pixelColor(0, 1), QColor(0, 0, 255));
  QCOMPARE(image.pixelColor(1, 1), QColor(255, 255, 255));
#ifdef Q_OS_LINUX
  // The point of naming the format: no image-format plugin was instantiated.
  QVERIFY2(mappedImageFormatPlugins().isEmpty(), qPrintable(mappedImageFormatPlugins().join(" ")));
#endif
  // The same bytes through Qt's detection decode to the same pixels.
  QImageReader detected(path);
  QCOMPARE(detected.read(), image);
 }
 void otherContentKeepsDetection() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto path = dir.filePath("preview.png");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QVERIFY(file.write("BM not a png at all", 19) > 0);
  file.close();
  QImageReader reader(path);
  const auto format = reader.format(); // Qt's own suffix-derived guess stays
  preparePreviewReader(reader, path);
  QVERIFY(reader.autoDetectImageFormat());
  QCOMPARE(reader.format(), format);
  QImageReader missing(dir.filePath("absent.png"));
  preparePreviewReader(missing, dir.filePath("absent.png"));
  QVERIFY(missing.autoDetectImageFormat());
 }
};
QTEST_GUILESS_MAIN(PreviewReaderContract)
#include "test_preview_reader.moc"
