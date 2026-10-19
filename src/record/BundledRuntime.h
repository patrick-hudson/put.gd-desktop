#pragma once
#include <QMap>
#include <QString>

namespace putgd::record {
// The AppImage carries putgd-record's own GStreamer plugins. When `executable`
// runs from inside the mounted image `appDir`, GStreamer must load only those
// plugins, scan them with the image's scanner and keep a registry of its own,
// so host plugins built for another GStreamer never load. Returns the variables
// to set, or nothing outside an image. Only this process (and its scanner)
// sees them: nothing the resident starts on the host inherits them.
QMap<QString, QString> bundledGStreamerEnvironment(const QString& appDir, const QString& executable, const QString& cacheDir);
// Applies bundledGStreamerEnvironment for this process; call before GStreamer initialises.
void useBundledGStreamer();
}
