#pragma once
#include "ui/Ui.h"
#include <QApplication>

namespace putgd::record {
// The helper's windows, the review window above all, carry put.gd's own icon
// in the title bar, taskbar and Alt-Tab (#264). The desktop file name maps
// them (WM_CLASS on X11, the app ID on Wayland) to the putgd-record.desktop
// entry the packages install. The brand icon is drawn from code, so this
// loads no platform theme, icon theme or image plugin.
inline constexpr char desktopFileName[] = "putgd-record";
inline void applyIdentity(QApplication& app) {
    app.setDesktopFileName(QString::fromLatin1(desktopFileName));
    app.setWindowIcon(Ui::icon("brand"));
}
}
