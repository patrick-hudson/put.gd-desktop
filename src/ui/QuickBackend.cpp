#include "QuickBackend.h"
#include <QList>
#include <QString>
#include <dlfcn.h>
namespace putgd {
bool plasmaThemeExpected(const QByteArray& platformTheme, const QByteArray& currentDesktop) {
 if (!platformTheme.isEmpty()) return platformTheme.toLower() == "kde";
 for (const auto& entry : currentDesktop.toUpper().split(':')) if (entry == "KDE") return true;
 return false;
}
void nameSoftwareSceneGraphBackend() {
 if (!plasmaThemeExpected(qgetenv("QT_QPA_PLATFORMTHEME"), qgetenv("XDG_CURRENT_DESKTOP"))) return;
 // static void QQuickWindow::setSceneGraphBackend(const QString&)
 void* quick = dlopen("libQt6Quick.so.6", RTLD_LAZY | RTLD_LOCAL);
 if (!quick) return;
 using Setter = void (*)(const QString&);
 if (auto setter = reinterpret_cast<Setter>(dlsym(quick, "_ZN12QQuickWindow20setSceneGraphBackendERK7QString"))) setter(QStringLiteral("software"));
}
}
