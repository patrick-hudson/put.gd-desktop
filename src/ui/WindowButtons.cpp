#include "WindowButtons.h"
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace putgd {
namespace {
WindowButtonLayout decode(const QStringList& left, const QStringList& right, bool kwin) {
    WindowButtonLayout result{{}, {}};
    QStringList seen;
    auto append = [&](const QStringList& source, QStringList& target) {
        for (const auto& entry : source) {
            const auto token = entry.trimmed();
            const auto name = kwin ? token == "I" ? QStringLiteral("minimize") : token == "A" ? QStringLiteral("maximize") : token == "X" ? QStringLiteral("close") : QString() : token;
            if ((name == "minimize" || name == "maximize" || name == "close") && !seen.contains(name)) {
                target.append(name);
                seen.append(name);
            }
        }
    };
    append(left, result.left);
    append(right, result.right);
    return result;
}
QStringList characters(const QString& text) {
    QStringList result;
    for (const auto ch : text) result.append(QString(ch));
    return result;
}
}
WindowButtonLayout WindowButtons::fromGSettings(QString value) {
    value = value.trimmed();
    if (value.startsWith('\'') && value.endsWith('\'')) value = value.mid(1, value.size() - 2);
    const auto sides = value.split(':');
    if (sides.size() != 2) return {};
    return decode(sides[0].split(','), sides[1].split(','), false);
}
WindowButtonLayout WindowButtons::fromKWin(const QString& path) {
    QSettings settings(path, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("org.kde.kdecoration2"));
    if (!settings.contains(QStringLiteral("ButtonsOnLeft")) && !settings.contains(QStringLiteral("ButtonsOnRight"))) return {};
    return decode(characters(settings.value(QStringLiteral("ButtonsOnLeft"), "M").toString()),
                  characters(settings.value(QStringLiteral("ButtonsOnRight"), "IAX").toString()), true);
}
WindowButtons& WindowButtons::instance() {
    static auto* source = new WindowButtons(qEnvironmentVariable("XDG_CURRENT_DESKTOP"), QStandardPaths::writableLocation(QStandardPaths::ConfigLocation), QCoreApplication::instance());
    return *source;
}
WindowButtons::WindowButtons(const QString& desktop, const QString& configDirectory, QObject* parent) : QObject(parent) {
#ifdef Q_OS_LINUX
    const auto desktops = desktop.toLower().split(':');
    if (desktops.contains("kde")) {
        kwinPath_ = QDir(configDirectory).filePath(QStringLiteral("kwinrc"));
        watcher_.addPath(configDirectory);
        connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { readKWin(); });
        connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { readKWin(); });
        readKWin();
    } else {
        if (desktops.contains("x-cinnamon") || desktops.contains("cinnamon")) schema_ = QStringLiteral("org.cinnamon.desktop.wm.preferences");
        else if (desktops.contains("mate")) schema_ = QStringLiteral("org.mate.Marco.general");
        else if (desktops.contains("gnome") || desktops.contains("unity")) schema_ = QStringLiteral("org.gnome.desktop.wm.preferences");
        if (!schema_.isEmpty()) QTimer::singleShot(0, this, &WindowButtons::monitorSettings);
    }
#else
    Q_UNUSED(desktop);
    Q_UNUSED(configDirectory);
#endif
}
WindowButtons::~WindowButtons() {
    for (auto* process : {&query_, &monitor_}) {
        if (process->state() == QProcess::NotRunning) continue;
        process->kill();
        process->waitForFinished(1000);
    }
}
void WindowButtons::setLayout(const WindowButtonLayout& value) {
    if (layout_ == value) return;
    layout_ = value;
    emit changed();
}
void WindowButtons::readKWin() {
    setLayout(fromKWin(kwinPath_));
    // Editors commonly replace kwinrc atomically; keep the directory watch too.
    if (!watcher_.files().contains(kwinPath_)) watcher_.addPath(kwinPath_);
}
void WindowButtons::monitorSettings() {
    const auto program = QStandardPaths::findExecutable(QStringLiteral("gsettings"));
    if (program.isEmpty()) return;
    connect(&query_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!monitorChanged_ && code == 0 && status == QProcess::NormalExit) setLayout(fromGSettings(QString::fromUtf8(query_.readAllStandardOutput())));
    });
    connect(&monitor_, &QProcess::readyReadStandardOutput, this, [this] {
        monitorOutput_ += monitor_.readAllStandardOutput();
        while (monitorOutput_.contains('\n')) {
            const auto end = monitorOutput_.indexOf('\n');
            const auto line = monitorOutput_.left(end);
            monitorOutput_.remove(0, end + 1);
            if (line.startsWith("button-layout: ")) {
                monitorChanged_ = true;
                setLayout(fromGSettings(QString::fromUtf8(line.mid(15))));
            }
        }
    });
    monitor_.start(program, {QStringLiteral("monitor"), schema_, QStringLiteral("button-layout")});
    query_.start(program, {QStringLiteral("get"), schema_, QStringLiteral("button-layout")});
    // A broken settings service must never stall window construction.
    QTimer::singleShot(2000, &query_, [this] { if (query_.state() != QProcess::NotRunning) query_.kill(); });
}
}
