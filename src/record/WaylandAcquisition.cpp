#include "WaylandAcquisition.h"
#include <QFileInfo>

namespace putgd::record {
QString pipewireSocket(const QProcessEnvironment& env) {
    const QString remote = env.value("PIPEWIRE_REMOTE", "pipewire-0");
    for (const QString& dir : {env.value("PIPEWIRE_RUNTIME_DIR"), env.value("XDG_RUNTIME_DIR"), env.value("USERPROFILE")})
        if (!dir.isEmpty() && QFileInfo(dir + "/" + remote).exists()) return dir + "/" + remote;
    return {};
}
}
