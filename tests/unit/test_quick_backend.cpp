#include "ui/QuickBackend.h"
#include <QtTest>
#include <QTemporaryDir>
#include <dlfcn.h>
using namespace putgd;
class QuickBackendContract : public QObject {
 Q_OBJECT
private slots:
 void onlyPlasmaIsExpected() {
  QVERIFY(plasmaThemeExpected("kde", ""));
  QVERIFY(plasmaThemeExpected("KDE", "GNOME"));
  QVERIFY(plasmaThemeExpected("", "KDE"));
  QVERIFY(plasmaThemeExpected("", "plasma:KDE"));
  QVERIFY(!plasmaThemeExpected("gtk3", "KDE"));
  QVERIFY(!plasmaThemeExpected("generic", "KDE"));
  QVERIFY(!plasmaThemeExpected("", "ubuntu:GNOME"));
  QVERIFY(!plasmaThemeExpected("", "X-Cinnamon"));
  QVERIFY(!plasmaThemeExpected("", ""));
 }
 void onlyAnInstalledThemePluginCounts() {
  QTemporaryDir plugins;
  QVERIFY(plugins.isValid());
  QVERIFY(!plasmaThemeInstalled({}));
  QVERIFY(!plasmaThemeInstalled({plugins.path()}));
  QVERIFY(QDir(plugins.path()).mkpath("platformthemes"));
  QFile other(plugins.filePath("platformthemes/libqgtk3.so"));
  QVERIFY(other.open(QIODevice::WriteOnly));
  other.close();
  QVERIFY(!plasmaThemeInstalled({plugins.path()}));
  QFile kde(plugins.filePath("platformthemes/KDEPlasmaPlatformTheme6.so"));
  QVERIFY(kde.open(QIODevice::WriteOnly));
  kde.close();
  QVERIFY(plasmaThemeInstalled({plugins.path()}));
  QVERIFY(plasmaThemeInstalled({QDir::tempPath(), plugins.path()}));
 }
 void namesSoftwareOnlyForPlasmaWithItsTheme() {
  // static QString QQuickWindow::sceneGraphBackend()
  using Getter = QString (*)();
  void* quick = dlopen("libQt6Quick.so.6", RTLD_LAZY | RTLD_LOCAL);
  if (!quick) QSKIP("Qt Quick is not installed here; the setter has nothing to resolve");
  auto getter = reinterpret_cast<Getter>(dlsym(quick, "_ZN12QQuickWindow17sceneGraphBackendEv"));
  QVERIFY(getter);
  const bool hostHasTheme = plasmaThemeInstalled(QCoreApplication::libraryPaths());
  qputenv("QT_QPA_PLATFORMTHEME", "gtk3");
  qputenv("XDG_CURRENT_DESKTOP", "KDE");
  nameSoftwareSceneGraphBackend();
  QVERIFY(getter().isEmpty());
  qunsetenv("QT_QPA_PLATFORMTHEME");
  if (!hostHasTheme) {
   // Plasma expected, but no theme plugin on the path (the AppImage case):
   // nothing is named.
   nameSoftwareSceneGraphBackend();
   QVERIFY(getter().isEmpty());
  }
  QTemporaryDir plugins;
  QVERIFY(plugins.isValid());
  QVERIFY(QDir(plugins.path()).mkpath("platformthemes"));
  QFile kde(plugins.filePath("platformthemes/KDEPlasmaPlatformTheme6.so"));
  QVERIFY(kde.open(QIODevice::WriteOnly));
  kde.close();
  QCoreApplication::addLibraryPath(plugins.path());
  nameSoftwareSceneGraphBackend();
  QCOMPARE(getter(), QString("software"));
  QCoreApplication::removeLibraryPath(plugins.path());
  qunsetenv("XDG_CURRENT_DESKTOP");
 }
};
QTEST_GUILESS_MAIN(QuickBackendContract)
#include "test_quick_backend.moc"
