#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <optional>
#include <unistd.h>
namespace putgd {
struct LabwcSession { qint64 pid = 0; QString configDir, xdgConfigHome; };
// The session's labwc: Budgie starts it with
// --config-dir $XDG_CONFIG_HOME/budgie-desktop/labwc.
inline std::optional<LabwcSession> runningLabwc() {
    const auto uid = getuid();
    for (const auto& entry : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const qint64 pid = entry.toLongLong(&numeric);
        if (!numeric) continue;
        QFile comm("/proc/" + entry + "/comm");
        if (!comm.open(QIODevice::ReadOnly) || comm.readAll().trimmed() != "labwc") continue;
        if (QFileInfo("/proc/" + entry).ownerId() != uid) continue;
        QFile cmdline("/proc/" + entry + "/cmdline"), environ("/proc/" + entry + "/environ");
        if (!cmdline.open(QIODevice::ReadOnly) || !environ.open(QIODevice::ReadOnly)) continue;
        LabwcSession found;
        found.pid = pid;
        const auto args = cmdline.read(65536).split('\0');
        for (qsizetype i = 0; i + 1 < args.size(); ++i) if (args[i] == "--config-dir" || args[i] == "-C") found.configDir = QString::fromLocal8Bit(args[i + 1]);
        QString home;
        for (const auto& line : environ.read(1 << 20).split('\0')) {
            if (line.startsWith("XDG_CONFIG_HOME=")) found.xdgConfigHome = QString::fromLocal8Bit(line.mid(16));
            else if (line.startsWith("HOME=")) home = QString::fromLocal8Bit(line.mid(5));
        }
        if (found.xdgConfigHome.isEmpty() && !home.isEmpty()) found.xdgConfigHome = home + "/.config";
        return found;
    }
    return {};
}
}
