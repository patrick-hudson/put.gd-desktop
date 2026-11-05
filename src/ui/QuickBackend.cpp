#include "QuickBackend.h"
#include <QCoreApplication>
#include <QDir>
#include <QList>
#include <QString>
#include <dlfcn.h>
namespace putgd {
bool plasmaThemeExpected(const QByteArray& platformTheme, const QByteArray& currentDesktop) {
 if (!platformTheme.isEmpty()) return platformTheme.toLower() == "kde";
 for (const auto& entry : currentDesktop.toUpper().split(':')) if (entry == "KDE") return true;
 return false;
}
bool plasmaThemeInstalled(const QStringList& libraryPaths) {
 for (const auto& path : libraryPaths) {
  const QDir themes(path + QStringLiteral("/platformthemes"));
  if (!themes.entryList({QStringLiteral("KDEPlasmaPlatformTheme*")}, QDir::Files).isEmpty()) return true;
 }
 return false;
}
void nameSoftwareSceneGraphBackend() {
 if (!plasmaThemeExpected(qgetenv("QT_QPA_PLATFORMTHEME"), qgetenv("XDG_CURRENT_DESKTOP"))) return;
 // Only the theme plugin runs the probe, and only it brings Qt Quick along:
 // without it (the AppImage, a desktop without plasma-integration) dlopen
 // would load the host's Qt Quick into this process for nothing.
 if (!plasmaThemeInstalled(QCoreApplication::libraryPaths())) return;
 // static void QQuickWindow::setSceneGraphBackend(const QString&); the
 // mangled name is fixed by the Qt 6 ABI (QString by const reference).
 void* quick = dlopen("libQt6Quick.so.6", RTLD_LAZY | RTLD_LOCAL);
 if (!quick) return;
 using Setter = void (*)(const QString&);
 if (auto setter = reinterpret_cast<Setter>(dlsym(quick, "_ZN12QQuickWindow20setSceneGraphBackendERK7QString"))) setter(QStringLiteral("software"));
}
}
