#include "platform/NativeSession.h"
#include <QtTest>
#include <QScopeGuard>
class NativeSessionTest final:public QObject {
 Q_OBJECT
private slots:
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
