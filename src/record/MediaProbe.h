#pragma once
#include <QJsonObject>

namespace putgd::record {
// Truthful inventory of what this installation can record and export, built
// from the GStreamer registry and the FFmpeg libraries the helper links.
// It opens no capture source, no audio device and no portal session. The
// resident never runs this code; it reads the JSON from a transient process.
QJsonObject probeMedia();
}
