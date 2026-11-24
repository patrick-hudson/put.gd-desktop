// What the GNOME Shell extension sends back (platform/gnome/GnomeProtocol.cpp): a
// metadata object and the bytes it describes, for images, captures made of several
// frames, file lists and text. The extension is another process, so both are untrusted.
// Image segments are given a PNG header so the decoder never starts the image helper
// process; the PNG reader itself still sees whatever follows.
#include "FuzzSupport.h"
#include "platform/gnome/GnomeProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
using namespace putgd;

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    fuzz::application(argc, argv);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    const auto mode = in.byte();
    auto metadata = QJsonDocument::fromJson(in.part()).object();
    auto bytes = in.rest();
    static const QByteArray png("\x89PNG\r\n\x1a\n", 8);
    const auto mime = metadata["mime"].toString();
    if (mode & 1) { metadata["version"] = 1; metadata["requestId"] = "r"; metadata["bytes"] = double(bytes.size()); }
    if (mime.startsWith("image/") && !bytes.startsWith(png) && bytes.size() >= png.size()) bytes.replace(0, png.size(), png);
    if (mime == "application/x-putgd-capture") {
        for (const auto& frame : metadata["frames"].toArray()) {
            const auto offset = frame.toObject()["offset"].toDouble(-1);
            if (offset >= 0 && offset + png.size() <= bytes.size()) bytes.replace(qsizetype(offset), png.size(), png);
        }
    }
    ResourceLimits limits;
    limits.imagePixels = 4 * 1024 * 1024;
    limits.imageBytes = 16 * 1024 * 1024;
    gnome::decodeTransfer(metadata, bytes, (mode & 1) ? "r" : metadata["requestId"].toString(), limits);
    return 0;
}
