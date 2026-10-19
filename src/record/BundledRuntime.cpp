#include "BundledRuntime.h"
#include <QFileInfo>
#include <QStandardPaths>

namespace putgd::record {
QMap<QString, QString> bundledGStreamerEnvironment(const QString& appDir, const QString& executable, const QString& cacheDir) {
    if (appDir.isEmpty() || executable.isEmpty() || cacheDir.isEmpty()) return {};
    const auto root = QFileInfo(appDir).canonicalFilePath();
    const auto self = QFileInfo(executable).canonicalFilePath();
    if (root.isEmpty() || !self.startsWith(root + '/')) return {};
    const auto plugins = root + "/usr/lib/gstreamer-1.0";
    const auto scanner = root + "/usr/libexec/gstreamer-1.0/gst-plugin-scanner";
    if (!QFileInfo(plugins).isDir() || !QFileInfo(scanner).isExecutable()) return {};
    // Each mount has a new path, so GStreamer rescans the image's plugins once
    // per mount; a shared host registry would be rewritten for every launch.
    const auto registry = cacheDir + "/putgd/gstreamer-1.0-appimage-registry.bin";
    return {{"GST_PLUGIN_SYSTEM_PATH_1_0", plugins}, {"GST_PLUGIN_SYSTEM_PATH", plugins},
            {"GST_PLUGIN_PATH_1_0", QString()}, {"GST_PLUGIN_PATH", QString()},
            {"GST_PLUGIN_SCANNER_1_0", scanner}, {"GST_PLUGIN_SCANNER", scanner},
            {"GST_REGISTRY_1_0", registry}, {"GST_REGISTRY", registry}};
}
void useBundledGStreamer() {
    const auto values = bundledGStreamerEnvironment(qEnvironmentVariable("APPDIR"), QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath(),
                                                    QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation));
    for (auto it = values.cbegin(); it != values.cend(); ++it) qputenv(it.key().toUtf8().constData(), it.value().toUtf8());
}
}
