#pragma once
#include "core/ClipboardPayload.h"
namespace putgd::gnome {
constexpr int protocolVersion=1;
// The "version" of the packaged integrations/gnome/metadata.json (read by CMake).
// GNOME Shell keeps running the extension it loaded at login, so an older loaded
// version means the package was upgraded and the user must log out and back in.
constexpr int extensionVersion=PUTGD_GNOME_EXTENSION_VERSION;
constexpr int chunkBytes=65536;
// Metadata and bytes are untrusted even when received from the authenticated helper.
PayloadResult decodeTransfer(const QJsonObject& metadata,const QByteArray& bytes,
                             const QString& requestId,const ResourceLimits& limits);
}
