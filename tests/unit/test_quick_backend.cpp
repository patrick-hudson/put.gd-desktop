#include "ui/QuickBackend.h"
#include <QtTest>
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
 void namesSoftwareOnlyForPlasma() {
  // static QString QQuickWindow::sceneGraphBackend()
  using Getter = QString (*)();
  void* quick = dlopen("libQt6Quick.so.6", RTLD_LAZY | RTLD_LOCAL);
  if (!quick) QSKIP("Qt Quick is not installed here; the setter has nothing to resolve");
  auto getter = reinterpret_cast<Getter>(dlsym(quick, "_ZN12QQuickWindow17sceneGraphBackendEv"));
  QVERIFY(getter);
  qputenv("QT_QPA_PLATFORMTHEME", "gtk3");
  qputenv("XDG_CURRENT_DESKTOP", "KDE");
  nameSoftwareSceneGraphBackend();
  QVERIFY(getter().isEmpty());
  qunsetenv("QT_QPA_PLATFORMTHEME");
  nameSoftwareSceneGraphBackend();
  QCOMPARE(getter(), QString("software"));
  qunsetenv("XDG_CURRENT_DESKTOP");
 }
};
QTEST_GUILESS_MAIN(QuickBackendContract)
#include "test_quick_backend.moc"
