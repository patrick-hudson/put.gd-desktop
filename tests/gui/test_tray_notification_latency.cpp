#include "fixtures/U2Fixtures.h"
#include "fixtures/Waits.h"
#include "ui/TrayController.h"
#include <QApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <QtTest>
#include <atomic>

using namespace putgd;
using namespace putgd::test;

// #597: the graphical (X11) resident's toast goes through Qt's own tray, and Qt
// 6.11.2+ waits on the calling thread for Notify's reply. On Plasma X11 the
// server, plasmashell, answers only after its Klipper has read the link the
// resident just copied, which the resident serves from that same thread. These
// peers reproduce that dependency on dbus-run-session's private bus: they run on
// their own thread, so Qt's blocking tray calls are answered, but Notify's reply
// is sent only once the resident's event loop runs again.
class WatcherPeer final : public QObject {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierWatcher")
 Q_PROPERTY(bool IsStatusNotifierHostRegistered READ host)
 Q_PROPERTY(int ProtocolVersion READ protocol)
public:
 bool host() const { return true; }
 int protocol() const { return 0; }
public slots:
 void RegisterStatusNotifierItem(const QString&) {}
signals:
 void StatusNotifierHostRegistered();
};
class NotificationPeer final : public QObject, protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
 QMutex mutex;
 QList<QVariantMap> received;
 std::atomic_int answered{0};
public slots:
 uint Notify(const QString& app, uint, const QString& icon, const QString& summary, const QString& body,
             const QStringList&, const QVariantMap&, int) {
  int id = 0;
  {QMutexLocker lock(&mutex);received.append({{"app",app},{"icon",icon},{"summary",summary},{"body",body}});id=received.size();}
  setDelayedReply(true);
  // Like Klipper waiting for the copied link: the resident's thread must run first.
  QMetaObject::invokeMethod(qApp,[this,reply=message().createReply(uint(id)),bus=connection()]{bus.send(reply);++answered;},Qt::QueuedConnection);
  return 0;
 }
 QStringList GetCapabilities() { return {"body"}; }
 QString GetServerInformation(QString& vendor, QString& version, QString& spec) { vendor="Synthetic";version="1";spec="1.2";return "Synthetic"; }
signals:
 void ActionInvoked(uint id, const QString& action);
 void NotificationClosed(uint id, uint reason);
};

class TrayNotificationLatency final : public QObject {
 Q_OBJECT
 QThread peers;
 WatcherPeer* watcher = nullptr;
 NotificationPeer* notifications = nullptr;
 QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "tray-latency-peer");
private slots:
 void initTestCase() {
  if (qEnvironmentVariable("PUTGD_ISOLATED_TRAY_LATENCY_TEST") != "1")
   QSKIP("Requires its dedicated private-bus/Xvfb invocation");
  QVERIFY(QCoreApplication::instance()->inherits("QApplication"));
  QVERIFY(bus.isConnected());
  watcher = new WatcherPeer; notifications = new NotificationPeer;
  peers.start(); watcher->moveToThread(&peers); notifications->moveToThread(&peers);
  QVERIFY(bus.registerObject("/StatusNotifierWatcher", watcher, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSignals));
  QVERIFY(bus.registerService("org.kde.StatusNotifierWatcher"));
  QVERIFY(bus.registerObject("/org/freedesktop/Notifications", notifications, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
  QVERIFY(bus.registerService("org.freedesktop.Notifications"));
 }
 void cleanupTestCase() {
  if (!watcher) return;
  bus.unregisterService("org.freedesktop.Notifications"); bus.unregisterService("org.kde.StatusNotifierWatcher");
  bus.unregisterObject("/org/freedesktop/Notifications"); bus.unregisterObject("/StatusNotifierWatcher");
  QMetaObject::invokeMethod(watcher, &QObject::deleteLater); QMetaObject::invokeMethod(notifications, &QObject::deleteLater);
  peers.quit(); peers.wait();
  QDBusConnection::disconnectFromBus("tray-latency-peer");
 }
 void firstUploadToastKeepsTheEventLoopRunning() {
  QTemporaryDir directory; QVERIFY(directory.isValid());
  ProviderRegistry providers; providers.add("opaque", std::make_shared<OpaqueProvider>());
  SettingsService settings(directory.filePath("config.json"), [&](auto id, auto value) { return providers.validate(id, value); });
  QVERIFY(settings.save(configuration(directory.path()), {}));
  Source source; source.payload = textPayload();
  OperationService operations(directory.filePath("data"), settings, providers, source); PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations, [](const QString&) {}, [] {});
  PUTGD_TRY_VERIFY(controller.available());
  // The longest gap between 10 ms ticks while the first upload runs and its toast is shown.
  QElapsedTimer sinceTick; sinceTick.start(); qint64 longest = 0;
  QTimer ticks; ticks.setInterval(10); ticks.setTimerType(Qt::PreciseTimer);
  connect(&ticks, &QTimer::timeout, this, [&] { longest = std::max(longest, sinceTick.restart()); });
  ticks.start();
  operations.invoke("clipboard", [](Error error, QJsonObject) { QVERIFY(!error); });
  PUTGD_TRY_COMPARE(source.copies, 1);
  PUTGD_TRY_COMPARE(notifications->answered.load(), 1);
  settle(200); ticks.stop();
  {
   QMutexLocker lock(&notifications->mutex);
   QCOMPARE(notifications->received.size(), 1);
   const auto toast = notifications->received.first();
   QCOMPARE(toast["summary"].toString(), QString("Upload complete"));
   QCOMPARE(toast["icon"].toString(), QString("dialog-information"));
  }
  QVERIFY2(longest < scaled(1000), qPrintable(QString("The resident's event loop stalled for %1 ms during the first upload").arg(longest)));
 }
};

QTEST_MAIN(TrayNotificationLatency)
#include "test_tray_notification_latency.moc"
