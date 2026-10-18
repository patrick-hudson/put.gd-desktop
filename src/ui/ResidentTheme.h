#pragma once
#include <QByteArray>
#include <QGuiApplication>

namespace putgd {
// The widget resident draws no desktop-themed UI of its own: the tray menu and
// notifications are drawn by the desktop over D-Bus, the X11 region overlay is
// custom-painted, and views run in putgd-ui. A desktop theme plugin can still
// load a GPU driver into it; on Plasma X11 the KDE plugin probes OpenGL at
// startup, mapping Qt Quick, Mesa and LLVM (about 81 MiB RSS, 15 MiB private).
// Hold this across QApplication construction: Qt then selects only its built-in
// generic theme. The variable is restored afterwards so putgd-ui views and
// helper processes still inherit the desktop's native theme.
class GenericResidentTheme final {
public:
 GenericResidentTheme():wasSet_(qEnvironmentVariableIsSet("QT_QPA_PLATFORMTHEME")),previous_(qgetenv("QT_QPA_PLATFORMTHEME")){
  // Without desktop-settings awareness Qt adds no desktop-derived theme names.
  QGuiApplication::setDesktopSettingsAware(false);
  qputenv("QT_QPA_PLATFORMTHEME","generic");
 }
 ~GenericResidentTheme(){if(wasSet_)qputenv("QT_QPA_PLATFORMTHEME",previous_);else qunsetenv("QT_QPA_PLATFORMTHEME");}
 GenericResidentTheme(const GenericResidentTheme&)=delete;
 GenericResidentTheme& operator=(const GenericResidentTheme&)=delete;
private:
 bool wasSet_;QByteArray previous_;
};
}
