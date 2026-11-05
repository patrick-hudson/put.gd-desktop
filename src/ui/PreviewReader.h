#pragma once
#include <QString>
class QImageReader;
namespace putgd {
// Points a reader at the built-in PNG handler when the file is a PNG. With
// the format left to detection, Qt 6.4 to 6.8 instantiate every image-format
// plugin (and map their codec libraries) to ask each whether it reads "png"
// before falling back to the handler built into QtGui; naming the format
// looks up only a plugin that advertises png, which none does, so the same
// built-in handler decodes the same pixels without those loads (#1161).
// Any other content keeps Qt's full detection.
void preparePreviewReader(QImageReader& reader, const QString& path);
}
