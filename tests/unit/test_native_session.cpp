#include "platform/NativeSession.h"
#include <QtTest>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
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
 // Budgie (#1275): the Budgie token routes, Budgie beats GNOME and KDE, bare
 // labwc stays unavailable, and startbudgielabwc's "wayland;xcb" is accepted.
 void budgieRouting_data(){
  QTest::addColumn<QByteArray>("type");QTest::addColumn<QByteArray>("wayland");QTest::addColumn<QByteArray>("desktop");QTest::addColumn<QByteArray>("platform");
  QTest::addColumn<bool>("budgie");QTest::addColumn<bool>("gnome");QTest::addColumn<bool>("kde");
  QTest::newRow("Budgie")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("Budgie")<<QByteArray()<<true<<false<<false;
  QTest::newRow("Budgie:GNOME")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("Budgie:GNOME")<<QByteArray()<<true<<false<<false;
  QTest::newRow("budgie-lowercase")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("budgie")<<QByteArray()<<true<<false<<false;
  QTest::newRow("labwc:wlroots")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("labwc:wlroots")<<QByteArray()<<false<<false<<false;
  QTest::newRow("wayland;xcb")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("Budgie")<<QByteArray("wayland;xcb")<<true<<false<<false;
  QTest::newRow("wayland;xcb-gnome")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("Budgie:GNOME")<<QByteArray("wayland;xcb")<<true<<false<<false;
  QTest::newRow("xcb;wayland")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("Budgie")<<QByteArray("xcb;wayland")<<false<<false<<false;
  QTest::newRow("explicit-xcb")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("Budgie")<<QByteArray("xcb")<<false<<false<<false;
  QTest::newRow("x11-session")<<QByteArray("x11")<<QByteArray()<<QByteArray("Budgie:GNOME")<<QByteArray()<<false<<false<<false;
  QTest::newRow("missing-display")<<QByteArray("wayland")<<QByteArray()<<QByteArray("Budgie")<<QByteArray()<<false<<false<<false;
  QTest::newRow("prefix-desktop")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("Budgie-unknown")<<QByteArray()<<false<<false<<false;
  QTest::newRow("Budgie:KDE")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("Budgie:KDE")<<QByteArray()<<false<<false<<false;
  QTest::newRow("GNOME-only")<<QByteArray("wayland")<<QByteArray("wayland-0")<<QByteArray("GNOME")<<QByteArray()<<false<<true<<false;
 }
 void budgieRouting(){
  QFETCH(QByteArray,type);QFETCH(QByteArray,wayland);QFETCH(QByteArray,desktop);QFETCH(QByteArray,platform);QFETCH(bool,budgie);QFETCH(bool,gnome);QFETCH(bool,kde);
  const QList<QByteArray> names{"XDG_SESSION_TYPE","WAYLAND_DISPLAY","XDG_CURRENT_DESKTOP","QT_QPA_PLATFORM"};
  struct Previous{QByteArray name,value;bool present;};QList<Previous> saved;
  for(const auto& name:names)saved.append({name,qgetenv(name),qEnvironmentVariableIsSet(name.constData())});
  const auto restore=qScopeGuard([&]{for(const auto& p:saved){if(p.present)qputenv(p.name,p.value);else qunsetenv(p.name);}});
  qputenv("XDG_SESSION_TYPE",type);qputenv("WAYLAND_DISPLAY",wayland);qputenv("XDG_CURRENT_DESKTOP",desktop);qputenv("QT_QPA_PLATFORM",platform);
  QCOMPARE(putgd::budgieCoreSessionEnvironment(),budgie);
  QCOMPARE(putgd::gnomeCoreSessionEnvironment(),gnome);
  QCOMPARE(putgd::kdeCoreSessionEnvironment(),kde);
  QCOMPARE(putgd::waylandCoreSessionEnvironment(),budgie||kde);
 }
 // Hyprland (#1275): the Hyprland token and an answering request socket route;
 // Omarchy's "wayland;xcb" is accepted; KDE, GNOME, Cinnamon and Budgie tokens
 // keep their own route; no signature or a dead socket stays off the route.
 void hyprlandRouting_data(){
  QTest::addColumn<QByteArray>("desktop");QTest::addColumn<QByteArray>("platform");QTest::addColumn<QByteArray>("signature");QTest::addColumn<bool>("listening");
  QTest::addColumn<bool>("hyprland");QTest::addColumn<bool>("kde");QTest::addColumn<bool>("budgie");
  QTest::newRow("Hyprland")<<QByteArray("Hyprland")<<QByteArray()<<QByteArray("abc_1_2")<<true<<true<<false<<false;
  QTest::newRow("Hyprland+wayland;xcb")<<QByteArray("Hyprland")<<QByteArray("wayland;xcb")<<QByteArray("abc_1_2")<<true<<true<<false<<false;
  QTest::newRow("hyprland-lowercase")<<QByteArray("hyprland")<<QByteArray("wayland")<<QByteArray("abc_1_2")<<true<<true<<false<<false;
  QTest::newRow("KDE:Hyprland")<<QByteArray("KDE:Hyprland")<<QByteArray()<<QByteArray("abc_1_2")<<true<<false<<true<<false;
  QTest::newRow("Budgie:Hyprland")<<QByteArray("Budgie:Hyprland")<<QByteArray()<<QByteArray("abc_1_2")<<true<<false<<false<<true;
  QTest::newRow("GNOME:Hyprland")<<QByteArray("GNOME:Hyprland")<<QByteArray()<<QByteArray("abc_1_2")<<true<<false<<false<<false;
  QTest::newRow("no-signature")<<QByteArray("Hyprland")<<QByteArray()<<QByteArray()<<true<<false<<false<<false;
  QTest::newRow("dead-socket")<<QByteArray("Hyprland")<<QByteArray()<<QByteArray("abc_1_2")<<false<<false<<false<<false;
  QTest::newRow("escaping-signature")<<QByteArray("Hyprland")<<QByteArray()<<QByteArray("../x")<<true<<false<<false<<false;
  QTest::newRow("explicit-xcb")<<QByteArray("Hyprland")<<QByteArray("xcb")<<QByteArray("abc_1_2")<<true<<false<<false<<false;
  QTest::newRow("xcb;wayland")<<QByteArray("Hyprland")<<QByteArray("xcb;wayland")<<QByteArray("abc_1_2")<<true<<false<<false<<false;
  QTest::newRow("prefix-desktop")<<QByteArray("Hyprland-ish")<<QByteArray()<<QByteArray("abc_1_2")<<true<<false<<false<<false;
 }
 void hyprlandRouting(){
  QFETCH(QByteArray,desktop);QFETCH(QByteArray,platform);QFETCH(QByteArray,signature);QFETCH(bool,listening);QFETCH(bool,hyprland);QFETCH(bool,kde);QFETCH(bool,budgie);
  const QList<QByteArray> names{"XDG_SESSION_TYPE","WAYLAND_DISPLAY","XDG_CURRENT_DESKTOP","QT_QPA_PLATFORM","XDG_RUNTIME_DIR","HYPRLAND_INSTANCE_SIGNATURE"};
  struct Previous{QByteArray name,value;bool present;};QList<Previous> saved;
  for(const auto& name:names)saved.append({name,qgetenv(name),qEnvironmentVariableIsSet(name.constData())});
  const auto restore=qScopeGuard([&]{for(const auto& p:saved){if(p.present)qputenv(p.name,p.value);else qunsetenv(p.name);}});
  QTemporaryDir runtime;QVERIFY(runtime.isValid());
  // The listening socket is created under the real signature "abc_1_2" only.
  QVERIFY(QDir(runtime.path()).mkpath("hypr/abc_1_2"));
  const QByteArray path=QFile::encodeName(runtime.path()+"/hypr/abc_1_2/.socket.sock");
  const int server=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);QVERIFY(server>=0);
  const auto closeServer=qScopeGuard([server]{::close(server);});
  sockaddr_un address{};address.sun_family=AF_UNIX;QVERIFY(size_t(path.size())<sizeof address.sun_path);std::memcpy(address.sun_path,path.constData(),size_t(path.size()));
  QCOMPARE(::bind(server,reinterpret_cast<const sockaddr*>(&address),sizeof address),0);
  if(listening)QCOMPARE(::listen(server,4),0);
  qputenv("XDG_SESSION_TYPE","wayland");qputenv("WAYLAND_DISPLAY","wayland-1");qputenv("XDG_CURRENT_DESKTOP",desktop);qputenv("QT_QPA_PLATFORM",platform);
  qputenv("XDG_RUNTIME_DIR",QFile::encodeName(runtime.path()));
  if(signature.isEmpty())qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");else qputenv("HYPRLAND_INSTANCE_SIGNATURE",signature);
  QCOMPARE(putgd::hyprlandCoreSessionEnvironment(),hyprland);
  QCOMPARE(putgd::kdeCoreSessionEnvironment(),kde);
  QCOMPARE(putgd::budgieCoreSessionEnvironment(),budgie);
  QCOMPARE(putgd::waylandCoreSessionEnvironment(),hyprland||kde||budgie);
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
