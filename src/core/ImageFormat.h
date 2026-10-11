#pragma once
#include <QByteArray>
#include <QByteArrayView>
namespace putgd {
// A hint comes from the actual encoded signature, never a claimed MIME type.
// Other bytes retain Qt's existing content detection, including mislabeled data.
inline QByteArray imageFormatHint(QByteArrayView bytes) {
 static constexpr char pngSignature[]="\x89PNG\r\n\x1a\n";
 return bytes.startsWith(QByteArrayView(pngSignature,8))?QByteArray("png"):QByteArray{};
}
}
