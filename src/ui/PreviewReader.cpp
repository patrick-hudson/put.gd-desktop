#include "PreviewReader.h"
#include <QFile>
#include <QImageReader>
#include <cstring>
namespace putgd {
void preparePreviewReader(QImageReader& reader, const QString& path) {
 static constexpr char pngSignature[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
 QFile file(path);
 char signature[8];
 if (!file.open(QIODevice::ReadOnly) || file.read(signature, 8) != 8 || std::memcmp(signature, pngSignature, 8) != 0) return;
 reader.setAutoDetectImageFormat(false);
 reader.setFormat("png");
}
}
