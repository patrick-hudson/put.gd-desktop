#include "platform/NativeSession.h"
#include <QtTest>
#include <QScopeGuard>
class NativeSessionTest final:public QObject {
 Q_OBJECT
private slots:
 void gnomeRouting_data(){
  QTest::addColumn<QByteArray>("type");QTest::addColumn<QByteArray>("wayland");QTest::addColumn<QByteArray>("desktop");QTest::addColumn<QByteArray>("platform");QTest::addColumn<bool>("expected");
  QTest::newRow("native")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray()<<true;
  QTest::newRow("desktop-token")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("ubuntu:gnome")<<QByteArray("wayland:opaque-option")<<true;
  QTest::newRow("wayland-egl")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray("wayland-egl")<<true;
  QTest::newRow("missing-display")<<QByteArray("wayland")<<QByteArray()<<QByteArray("GNOME")<<QByteArray()<<false;
  QTest::newRow("x11")<<QByteArray("x11")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray()<<false;
  QTest::newRow("missing-session")<<QByteArray()<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray()<<false;
  QTest::newRow("prefix-desktop")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME-unknown")<<QByteArray()<<false;
  QTest::newRow("kde")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray()<<false;
  QTest::newRow("mixed-kde")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME:KDE")<<QByteArray()<<false;
  QTest::newRow("mixed-cinnamon")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME:X-Cinnamon")<<QByteArray()<<false;
  QTest::newRow("offscreen")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray("offscreen")<<false;
  QTest::newRow("explicit-xcb")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray("xcb")<<false;
  QTest::newRow("prefix-platform")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray("wayland-unknown")<<false;
  QTest::newRow("missing-plugin")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray(":opaque-option")<<false;
  QTest::newRow("colon-only")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray(":")<<false;
  QTest::newRow("fallback-list")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray("wayland;xcb")<<false;
 }
 void gnomeRouting(){
  QFETCH(QByteArray,type);QFETCH(QByteArray,wayland);QFETCH(QByteArray,desktop);QFETCH(QByteArray,platform);QFETCH(bool,expected);
  const QList<QByteArray> names{"XDG_SESSION_TYPE","WAYLAND_DISPLAY","XDG_CURRENT_DESKTOP","QT_QPA_PLATFORM"};
  struct Previous{QByteArray name,value;bool present;};QList<Previous> saved;
  for(const auto& name:names)saved.append({name,qgetenv(name),qEnvironmentVariableIsSet(name.constData())});
  const auto restore=qScopeGuard([&]{for(const auto& p:saved){if(p.present)qputenv(p.name,p.value);else qunsetenv(p.name);}});
  qputenv("XDG_SESSION_TYPE",type);qputenv("WAYLAND_DISPLAY",wayland);qputenv("XDG_CURRENT_DESKTOP",desktop);qputenv("QT_QPA_PLATFORM",platform);
  QCOMPARE(putgd::gnomeCoreSessionEnvironment(),expected);
 }
 void gnomeArgumentOverrides_data(){
  QTest::addColumn<QStringList>("arguments");QTest::addColumn<bool>("expected");
  QTest::newRow("no-options")<<QStringList{"putgd"}<<true;
  for(const auto* option:{"--background","--history","--activity","-h","--help","--help-all","-v","--version"})
   QTest::newRow(option)<<QStringList{"putgd",option}<<true;
  QTest::newRow("combined-app-flags")<<QStringList{"putgd","--background","--history","--activity"}<<true;
  QTest::newRow("platformtheme-gtk3")<<QStringList{"putgd","-platformtheme","gtk3","--background"}<<false;
  QTest::newRow("style-fusion")<<QStringList{"putgd","-style","fusion","--background"}<<false;
  for(const auto* option:{"-platform","--platform","-platform=offscreen","--platform=wayland",
                         "--platformtheme","--platformpluginpath","-stylesheet","--stylesheet=theme.qss",
                         "-reverse","--reverse","-qwindowgeometry","--qwindowicon","-qwindowtitle",
                         "-plugin","-session","-testability","-widgetcount","-qdebug","-qdevel",
                         "--future-qt-option","--background=unexpected","--","payload","-hv"})
   QTest::newRow(option)<<QStringList{"putgd",option}<<false;
  QTest::newRow("gui-after-terminator")<<QStringList{"putgd","--","-platformtheme","gtk3"}<<false;
 }
 void gnomeArgumentOverrides(){
  QFETCH(QStringList,arguments);QFETCH(bool,expected);
  QList<QByteArray> storage;for(const auto& argument:arguments)storage.append(argument.toLocal8Bit());
  QList<const char*> argv;for(const auto& argument:storage)argv.append(argument.constData());
  QCOMPARE(putgd::gnomeCoreSessionArguments(static_cast<int>(argv.size()),argv.constData()),expected);
 }
 void kdeRouting_data(){
  QTest::addColumn<QByteArray>("type");QTest::addColumn<QByteArray>("wayland");QTest::addColumn<QByteArray>("desktop");QTest::addColumn<QByteArray>("platform");QTest::addColumn<bool>("expected");
  QTest::newRow("native")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray()<<true;
  QTest::newRow("desktop-token")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("plasma:kde")<<QByteArray("wayland:opaque-option")<<true;
  QTest::newRow("wayland-egl")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray("wayland-egl")<<true;
  QTest::newRow("missing-display")<<QByteArray("wayland")<<QByteArray()<<QByteArray("KDE")<<QByteArray()<<false;
  QTest::newRow("x11")<<QByteArray("x11")<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray()<<false;
  QTest::newRow("missing-session")<<QByteArray()<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray()<<false;
  QTest::newRow("prefix-desktop")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE-unknown")<<QByteArray()<<false;
  QTest::newRow("gnome-desktop")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray()<<false;
  QTest::newRow("mixed-kde")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE:GNOME")<<QByteArray()<<false;
  QTest::newRow("mixed-cinnamon")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE:X-Cinnamon")<<QByteArray()<<false;
  QTest::newRow("offscreen")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray("offscreen")<<false;
  QTest::newRow("explicit-xcb")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray("xcb")<<false;
  QTest::newRow("prefix-platform")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray("wayland-unknown")<<false;
  QTest::newRow("missing-plugin")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray(":opaque-option")<<false;
  QTest::newRow("colon-only")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray(":")<<false;
  QTest::newRow("fallback-list")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("KDE")<<QByteArray("wayland;xcb")<<false;
 }
 void kdeRouting(){
  QFETCH(QByteArray,type);QFETCH(QByteArray,wayland);QFETCH(QByteArray,desktop);QFETCH(QByteArray,platform);QFETCH(bool,expected);
  const QList<QByteArray> names{"XDG_SESSION_TYPE","WAYLAND_DISPLAY","XDG_CURRENT_DESKTOP","QT_QPA_PLATFORM"};
  struct Previous{QByteArray name,value;bool present;};QList<Previous> saved;
  for(const auto& name:names)saved.append({name,qgetenv(name),qEnvironmentVariableIsSet(name.constData())});
  const auto restore=qScopeGuard([&]{for(const auto& p:saved){if(p.present)qputenv(p.name,p.value);else qunsetenv(p.name);}});
  qputenv("XDG_SESSION_TYPE",type);qputenv("WAYLAND_DISPLAY",wayland);qputenv("XDG_CURRENT_DESKTOP",desktop);qputenv("QT_QPA_PLATFORM",platform);
  QCOMPARE(putgd::kdeCoreSessionEnvironment(),expected);
 }
 void kdeArgumentOverrides_data(){
  QTest::addColumn<QStringList>("arguments");QTest::addColumn<bool>("expected");
  QTest::newRow("no-options")<<QStringList{"putgd"}<<true;
  for(const auto* option:{"--background","--history","--activity","-h","--help","--help-all","-v","--version"})
   QTest::newRow(option)<<QStringList{"putgd",option}<<true;
  QTest::newRow("combined-app-flags")<<QStringList{"putgd","--background","--history","--activity"}<<true;
  QTest::newRow("platformtheme-gtk3")<<QStringList{"putgd","-platformtheme","gtk3","--background"}<<false;
  QTest::newRow("style-fusion")<<QStringList{"putgd","-style","fusion","--background"}<<false;
  for(const auto* option:{"-platform","--platform","-platform=offscreen","--platform=wayland",
                         "--platformtheme","--platformpluginpath","-stylesheet","--stylesheet=theme.qss",
                         "-reverse","--reverse","-qwindowgeometry","--qwindowicon","-qwindowtitle",
                         "-plugin","-session","-testability","-widgetcount","-qdebug","-qdevel",
                         "--future-qt-option","--background=unexpected","--","payload","-hv"})
   QTest::newRow(option)<<QStringList{"putgd",option}<<false;
  QTest::newRow("gui-after-terminator")<<QStringList{"putgd","--","-platformtheme","gtk3"}<<false;
 }
 void kdeArgumentOverrides(){
  QFETCH(QStringList,arguments);QFETCH(bool,expected);
  QList<QByteArray> storage;for(const auto& argument:arguments)storage.append(argument.toLocal8Bit());
  QList<const char*> argv;for(const auto& argument:storage)argv.append(argument.constData());
  QCOMPARE(putgd::kdeCoreSessionArguments(static_cast<int>(argv.size()),argv.constData()),expected);
 }
 void routingGuards() {
  const QList<QByteArray> names{"DISPLAY","WAYLAND_DISPLAY","XDG_SESSION_TYPE","QT_QPA_PLATFORM","XDG_CURRENT_DESKTOP"};
  struct Previous {QByteArray name,value;bool present;};QList<Previous> saved;
  for(const auto& n:names){saved.append({n,qgetenv(n),qEnvironmentVariableIsSet(n.constData())});qunsetenv(n);}
  const auto restore=qScopeGuard([&]{for(const auto& p:saved){if(p.present)qputenv(p.name,p.value);else qunsetenv(p.name);}});
  QVERIFY(!putgd::nativeX11SessionEnvironment());
  qputenv("DISPLAY",":99");QVERIFY(putgd::nativeX11SessionEnvironment());
  qputenv("XDG_SESSION_TYPE","wayland");QVERIFY(!putgd::nativeX11SessionEnvironment());
  qputenv("XDG_SESSION_TYPE","x11");qputenv("WAYLAND_DISPLAY","wayland-0");QVERIFY(!putgd::nativeX11SessionEnvironment());
  qunsetenv("WAYLAND_DISPLAY");qputenv("QT_QPA_PLATFORM","offscreen");QVERIFY(!putgd::nativeX11SessionEnvironment());
  qputenv("QT_QPA_PLATFORM","xcb:opaque-option");QVERIFY(putgd::nativeX11SessionEnvironment());
  qputenv("XDG_CURRENT_DESKTOP","GNOME");QVERIFY(!putgd::cinnamonCoreSessionEnvironment());
  qputenv("XDG_CURRENT_DESKTOP","X-Cinnamon");QVERIFY(putgd::cinnamonCoreSessionEnvironment());
  qputenv("XDG_CURRENT_DESKTOP","Other:cinnamon");QVERIFY(putgd::cinnamonCoreSessionEnvironment());
  qputenv("XDG_SESSION_TYPE","tty");QVERIFY(!putgd::cinnamonCoreSessionEnvironment());
 }
};
QTEST_GUILESS_MAIN(NativeSessionTest)
#include "test_native_session.moc"
