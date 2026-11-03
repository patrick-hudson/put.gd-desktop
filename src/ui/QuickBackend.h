#pragma once
#include <QByteArray>
namespace putgd {
// Whether Qt will load Plasma's platform theme for this process: the theme
// named by QT_QPA_PLATFORMTHEME, else a KDE entry in XDG_CURRENT_DESKTOP.
bool plasmaThemeExpected(const QByteArray& platformTheme, const QByteArray& currentDesktop);
// Plasma's platform theme (plasma-integration 6.3 to 6.6) runs a startup
// hook that creates a probe OpenGL context to decide the Qt Quick scene-graph
// backend; the probe maps the Wayland EGL integration, Mesa and LLVM into
// every Qt process on Plasma. It skips the probe when a backend is already
// named through QQuickWindow::setSceneGraphBackend(), the only value Qt
// reports before a Qt Quick window exists. A process that draws no Qt Quick
// names the software backend before its QApplication exists, resolving that
// setter from the Qt Quick library the theme is about to load anyway; off
// Plasma, or without the library, nothing changes (#1161).
void nameSoftwareSceneGraphBackend();
}
