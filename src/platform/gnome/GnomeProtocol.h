#pragma once
#include "core/ClipboardPayload.h"
namespace putgd::gnome {
constexpr int protocolVersion=1;
constexpr int chunkBytes=65536;
// Metadata and bytes are untrusted even when received from the authenticated helper.
PayloadResult decodeTransfer(const QJsonObject& metadata,const QByteArray& bytes,
                             const QString& requestId,const ResourceLimits& limits);
}
