#include "ui/NativeTray.h"
#include "ui/TrayController.h"
#include "fixtures/U2Fixtures.h"
#include <QDBusArgument>
#include <QDBusContext>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QPointer>
#include <QSignalSpy>
#include <QtTest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QFile>
#include <QElapsedTimer>
#include <QTimer>
#include <csignal>
#include <sys/types.h>
#include "fixtures/X11DesktopFixture.h"
#include <X11/keysym.h>
using namespace putgd;
using namespace putgd::test;

struct WireLayout { int id = 0; QVariantMap props; QList<QDBusVariant> children; };
struct WireImage { int width = 0, height = 0; QByteArray bytes; };
using WireImages = QList<WireImage>;
struct WireTooltip {QString icon;WireImages images;QString title,description;};
Q_DECLARE_METATYPE(WireTooltip)
QDBusArgument& operator<<(QDBusArgument& a,const WireTooltip& v){a.beginStructure();a<<v.icon<<v.images<<v.title<<v.description;a.endStructure();return a;}
const QDBusArgument& operator>>(const QDBusArgument& a,WireTooltip& v){a.beginStructure();a>>v.icon>>v.images>>v.title>>v.description;a.endStructure();return a;}
struct WireEvent { int id = 0; QString event; QDBusVariant data; uint time = 0; };
using WireEvents = QList<WireEvent>;
Q_DECLARE_METATYPE(WireLayout)
Q_DECLARE_METATYPE(WireImage)
Q_DECLARE_METATYPE(WireImages)
Q_DECLARE_METATYPE(WireEvent)
Q_DECLARE_METATYPE(WireEvents)
QDBusArgument& operator<<(QDBusArgument& a, const WireLayout& v) { a.beginStructure(); a << v.id << v.props << v.children; a.endStructure(); return a; }
const QDBusArgument& operator>>(const QDBusArgument& a, WireLayout& v) { a.beginStructure(); a >> v.id >> v.props >> v.children; a.endStructure(); return a; }
QDBusArgument& operator<<(QDBusArgument& a, const WireImage& v) { a.beginStructure(); a << v.width << v.height << v.bytes; a.endStructure(); return a; }
const QDBusArgument& operator>>(const QDBusArgument& a, WireImage& v) { a.beginStructure(); a >> v.width >> v.height >> v.bytes; a.endStructure(); return a; }
QDBusArgument& operator<<(QDBusArgument& a, const WireEvent& v) { a.beginStructure(); a << v.id << v.event << v.data << v.time; a.endStructure(); return a; }
const QDBusArgument& operator>>(const QDBusArgument& a, WireEvent& v) { a.beginStructure(); a >> v.id >> v.event >> v.data >> v.time; a.endStructure(); return a; }

// These peers speak the real wire protocol on dbus-run-session's private bus.
// They do not establish compositor support or visible menu/notification proof.
class WatcherPeer final : public QObject, protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierWatcher")
 Q_PROPERTY(bool IsStatusNotifierHostRegistered READ host)
public:
 bool hostPresent = true, failRegistration = false;
 QStringList registrations;
 QSet<QString> activeServices;
 bool host() const { return hostPresent; }
public slots:
 void RegisterStatusNotifierItem(const QString& service) {
  registrations.append(service);
  if (failRegistration) { sendErrorReply("org.freedesktop.DBus.Error.Failed", "Synthetic registration rejection"); return; }
  if (activeServices.contains(service)) return;
  activeServices.insert(service);
  auto* lifetime = new QDBusServiceWatcher(service, connection(), QDBusServiceWatcher::WatchForUnregistration, this);
  connect(lifetime, &QDBusServiceWatcher::serviceUnregistered, this, [this, service, lifetime] {
   activeServices.remove(service); lifetime->deleteLater();
  });
 }
signals:
 void StatusNotifierHostRegistered();
 void StatusNotifierHostUnregistered();
};
class NotificationPeer final : public QObject {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
 QList<QVariantMap> values;
public slots:
 uint Notify(const QString& app, uint replacement, const QString& icon,
             const QString& summary, const QString& body, const QStringList& actions,
             const QVariantMap& hints, int timeout) {
  values.append({{"app", app}, {"replacement", replacement}, {"icon", icon}, {"summary", summary},
   {"body", body}, {"actions", actions}, {"hints", hints}, {"timeout", timeout}});
  return uint(values.size());
 }
};

class ActivitySignalPeer final:public QObject {
 Q_OBJECT
public:int icons=0,tooltips=0;
public slots:void iconChanged(){++icons;}void tooltipChanged(){++tooltips;}
};

class NativeTrayWire final : public QObject {
 Q_OBJECT
 WatcherPeer watcher;
 NotificationPeer notifications;
 QDBusConnection peer = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "native-tray-wire-peer");
 QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "native-tray-wire-client");
 QDBusMessage call(const QString& path, const QString& interface, const QString& method, const QVariantList& args = {}) {
  if (watcher.registrations.isEmpty()) return QDBusMessage::createError("test.no_registration", "No registered item");
  auto m = QDBusMessage::createMethodCall(watcher.registrations.last(), path, interface, method); m.setArguments(args);
  QDBusPendingCallWatcher pending(client.asyncCall(m, 2000));
  QSignalSpy completed(&pending, &QDBusPendingCallWatcher::finished);
  if (!pending.isFinished()) completed.wait(2500);
  return pending.reply();
 }
 QDBusMessage click(int id, const QString& type = "clicked") {
  return call("/Menu", "com.canonical.dbusmenu", "Event", {id, type, QVariant::fromValue(QDBusVariant(0)), uint(0)});
 }
 void changeHost(bool value) {
  watcher.hostPresent = value;
  if (value) emit watcher.StatusNotifierHostRegistered(); else emit watcher.StatusNotifierHostUnregistered();
 }
private slots:
 void initTestCase() {
  QVERIFY(!QCoreApplication::instance()->inherits("QGuiApplication"));
  QVERIFY(peer.isConnected()); QVERIFY(client.isConnected());
  qDBusRegisterMetaType<WireLayout>(); qDBusRegisterMetaType<WireImage>(); qDBusRegisterMetaType<WireImages>();
  qDBusRegisterMetaType<WireTooltip>(); qDBusRegisterMetaType<WireEvent>(); qDBusRegisterMetaType<WireEvents>(); qDBusRegisterMetaType<QList<int>>();
  QVERIFY(peer.registerObject("/StatusNotifierWatcher", &watcher, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSignals));
  QVERIFY(peer.registerObject("/org/freedesktop/Notifications", &notifications, QDBusConnection::ExportAllSlots));
  QVERIFY(peer.registerService("org.freedesktop.Notifications"));
 }
 void init() {
  QTRY_VERIFY(watcher.activeServices.isEmpty());
  watcher.registrations.clear(); watcher.failRegistration = false; watcher.hostPresent = true; notifications.values.clear();
  QVERIFY(peer.registerService("org.kde.StatusNotifierWatcher"));
 }
 void cleanup() { QVERIFY(peer.unregisterService("org.kde.StatusNotifierWatcher")); }
 void cleanupTestCase() {
  peer.unregisterService("org.freedesktop.Notifications");
  QDBusConnection::disconnectFromBus("native-tray-wire-client");
  QDBusConnection::disconnectFromBus("native-tray-wire-peer");
 }
 void backgroundStartupWaitsForLateTray_data() {
  QTest::addColumn<QString>("configuration");
  QTest::addColumn<QString>("trayState");
  QTest::addColumn<bool>("background");
  QTest::newRow("late-watcher") << QString("valid") << QString("late-watcher") << true;
  QTest::newRow("late-host") << QString("valid") << QString("late-host") << true;
  QTest::newRow("ready-tray") << QString("valid") << QString("ready") << true;
  QTest::newRow("absent-tray-recovery") << QString("valid") << QString("absent") << true;
  QTest::newRow("tray-after-recovery-bound") << QString("valid") << QString("after-bound") << true;
  QTest::newRow("explicit-activation-cancels-recovery") << QString("valid") << QString("user-activation") << true;
  QTest::newRow("missing-config-recovery") << QString("missing") << QString("absent") << true;
  QTest::newRow("invalid-config-recovery") << QString("invalid") << QString("absent") << true;
  QTest::newRow("explicit-launch") << QString("valid") << QString("absent") << false;
 }
 void backgroundStartupWaitsForLateTray() {
  if (qEnvironmentVariable("PUTGD_ISOLATED_STARTUP_TEST") != "1")
   QSKIP("Requires its dedicated private-bus/Xvfb invocation");
  QFETCH(QString, configuration); QFETCH(QString, trayState); QFETCH(bool, background);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto runtime = directory.filePath("r"), socket = runtime + "/putgd/session.sock";
  QVERIFY2(socket.toUtf8().size() <= 107, "Resolved test socket exceeds Linux's limit");
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("XDG_CONFIG_HOME", directory.filePath("c"));
  environment.insert("XDG_DATA_HOME", directory.filePath("d"));
  environment.insert("XDG_CACHE_HOME", directory.filePath("k"));
  environment.insert("XDG_RUNTIME_DIR", runtime);
  environment.insert("XDG_CURRENT_DESKTOP", "X-Cinnamon");
  environment.insert("XDG_SESSION_TYPE", "x11");
  environment.insert("WAYLAND_DISPLAY", "");
  QVERIFY(QDir().mkpath(runtime));
  QVERIFY(QFile::setPermissions(runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  const auto configPath = directory.filePath("c/putgd/putgd/config.json");
  if (configuration != "missing") {
   SettingsService settings(configPath);
   QVERIFY(settings.save(Settings::defaults(), {}));
   if (configuration == "invalid") {
    QFile file(configPath); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write("{invalid"), qint64(8));
   }
  }
  const bool late = trayState.startsWith("late-");
  struct RestoreWatcher { QDBusConnection& bus; ~RestoreWatcher() { bus.registerService("org.kde.StatusNotifierWatcher"); } } restore{peer};
  if (trayState != "ready" && trayState != "late-host")
   QVERIFY(peer.unregisterService("org.kde.StatusNotifierWatcher"));
  watcher.hostPresent = trayState != "late-host";
  QTimer appearance;
  appearance.setSingleShot(true);
  connect(&appearance, &QTimer::timeout, this, [&] {
   if (trayState == "late-watcher" || trayState == "after-bound") QVERIFY(peer.registerService("org.kde.StatusNotifierWatcher"));
   else changeHost(true);
  });
  QProcess resident; resident.setProcessEnvironment(environment);
  struct Stop { QProcess& process; ~Stop() { if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(3000);} } } stop{resident};
  QElapsedTimer elapsed; elapsed.start();
  resident.start(PUTGD_PRODUCT_APP_PATH, background ? QStringList{"--background"} : QStringList{});
  QVERIFY(resident.waitForStarted());
  auto request = [&](const QString& method) {
   QProcess cli; cli.start(PUTGD_PRODUCT_CLI_PATH, {"--socket", socket, method});
   if(!cli.waitForFinished(3000))return QJsonObject{};
   const auto response = QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
   return response["ok"].toBool() ? response["result"].toObject() : QJsonObject{};
  };
  auto ui = [&] { return request("ui.snapshot")["diagnostics"].toObject()["ui"].toObject(); };
  QJsonObject snapshot;
  QTRY_VERIFY_WITH_TIMEOUT((snapshot=request("ui.snapshot"))["operationsReady"].toBool(), 5000);
  QElapsedTimer readyElapsed; readyElapsed.start();
  if (late || trayState == "after-bound") appearance.start(trayState == "after-bound" ? 12000 : 1500);
  const bool windowless = background && configuration == "valid";
  const bool recovery = trayState == "absent" || trayState == "after-bound";
  QJsonObject expectedExit;
  if (trayState == "user-activation") {
   QProcess activation; activation.setProcessEnvironment(environment);
   activation.start(PUTGD_PRODUCT_APP_PATH, {"--history"});
   QVERIFY(activation.waitForFinished(5000)); QCOMPARE(activation.exitCode(), 0);
   QTRY_COMPARE(ui()["state"].toString(), QString("ready"));
   const auto helperPid = ui()["pid"].toInteger();
   QVERIFY(helperPid > 0);
   QVERIFY(::kill(static_cast<pid_t>(helperPid), SIGTERM) == 0);
   QTRY_VERIFY(!(expectedExit=ui()["lastExit"].toObject()).isEmpty());
  }
  if (windowless) {
   // Observe the interval, not just the final state: a transient helper is a failure too.
   const int observeUntil = recovery ? 8000 : 11000;
   while (readyElapsed.elapsed() < observeUntil) {
    const auto status = ui();
    QCOMPARE(status["state"].toString(), QString("stopped"));
    QCOMPARE(status["pid"].toInteger(), qint64(0));
    QCOMPARE(status["lastExit"].toObject(), expectedExit);
    QTest::qWait(100);
   }
   if (!recovery && trayState != "user-activation") {
    QTRY_VERIFY(!watcher.registrations.isEmpty());
    QTRY_VERIFY(request("ui.snapshot")["diagnostics"].toObject()["trayAvailable"].toBool());
   }
  }
  if (!windowless || recovery) {
   QTRY_COMPARE_WITH_TIMEOUT(ui()["state"].toString(), QString("ready"), 10000);
   if (windowless) QVERIFY(elapsed.elapsed() >= 10000);
   else QVERIFY(elapsed.elapsed() < 10000);
  }
  if (trayState == "after-bound") {
   QTRY_VERIFY_WITH_TIMEOUT(!watcher.registrations.isEmpty(), 5000);
   QTRY_VERIFY(request("ui.snapshot")["diagnostics"].toObject()["trayAvailable"].toBool());
   QCOMPARE(ui()["state"].toString(), QString("ready"));
  }
  QVERIFY(request("ui.quit")["quitting"].toBool());
  QTRY_COMPARE_WITH_TIMEOUT(resident.state(), QProcess::NotRunning, 5000);
  QCOMPARE(resident.exitCode(), 0);
 }
 void rejectedGlobalShortcutReportsMissingDestination() {
  if (qEnvironmentVariable("PUTGD_ISOLATED_SHORTCUT_TEST") != "1")
   QSKIP("Requires its dedicated private-bus/Xvfb invocation");
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto runtime = directory.filePath("r");
  const auto socket = runtime + "/putgd/session.sock";
  QVERIFY2(socket.toUtf8().size() <= 107, "Resolved test socket exceeds Linux's limit");
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("XDG_CONFIG_HOME", directory.filePath("c"));
  environment.insert("XDG_DATA_HOME", directory.filePath("d"));
  environment.insert("XDG_CACHE_HOME", directory.filePath("k"));
  environment.insert("XDG_RUNTIME_DIR", runtime);
  environment.insert("XDG_CURRENT_DESKTOP", "X-Cinnamon");
  environment.insert("XDG_SESSION_TYPE", "x11");
  environment.insert("WAYLAND_DISPLAY", "");
  QDir().mkpath(runtime);
  QFile::setPermissions(runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  SettingsService settings(directory.filePath("c/putgd/putgd/config.json"));
  auto document = Settings::defaults();
  auto actions = document["actions"].toObject();
  auto region = actions["region"].toObject(); region["shortcut"] = "Ctrl+1";
  actions["region"] = region; document["actions"] = actions;
  QVERIFY(settings.save(document, {}));
  QProcess resident; resident.setProcessEnvironment(environment);
  resident.start(PUTGD_PRODUCT_APP_PATH, {"--background"});
  QVERIFY(resident.waitForStarted());
  // Keep failed attempts bounded and always release child-owned key grabs.
  struct Stop { QProcess& process; ~Stop() { if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(3000);} } } stop{resident};
  auto request = [&](const QString& method) {
   QProcess cli; cli.start(PUTGD_PRODUCT_CLI_PATH, {"--socket", socket, method});
   if(!cli.waitForFinished(3000))return QJsonObject{};
   return QJsonDocument::fromJson(cli.readAllStandardOutput()).object()["result"].toObject();
  };
  QJsonObject snapshot;
  QTRY_VERIFY_WITH_TIMEOUT((snapshot=request("ui.snapshot"))["operationsReady"].toBool(), 5000);
  QTRY_VERIFY(!watcher.registrations.isEmpty());
  QVERIFY(snapshot["diagnostics"].toObject()["shortcuts"].toObject()["bindings"].toObject()["region"].toObject()["bound"].toBool());
  QCOMPARE(snapshot["diagnostics"].toObject()["ui"].toObject()["pid"].toInteger(), qint64(0));
  X11DesktopFixture desktop;
  desktop.key(XK_Control_L,true);desktop.key(XK_1,true);
  desktop.key(XK_1,false);desktop.key(XK_Control_L,false);
  QTRY_COMPARE_WITH_TIMEOUT(notifications.values.size(), 1, 5000);
  const auto body=notifications.values.first()["body"].toString();QVERIFY(body.contains("No output is enabled"));QVERIFY(body.contains("Settings → Capture"));QVERIFY(body.contains("Keep a local copy in Settings → General"));
  for(int repeat=0;repeat<2;++repeat){
   desktop.key(XK_Control_L,true);desktop.key(XK_1,true);
   desktop.key(XK_1,false);desktop.key(XK_Control_L,false);
   QTest::qWait(50);
  }
  QCOMPARE(notifications.values.size(),1);
  QVERIFY(request("operation.recent")["operations"].toArray().isEmpty());
  QCOMPARE(request("ui.snapshot")["diagnostics"].toObject()["ui"].toObject()["pid"].toInteger(), qint64(0));
  QVERIFY(request("ui.quit")["quitting"].toBool());
  QTRY_COMPARE_WITH_TIMEOUT(resident.state(), QProcess::NotRunning, 5000);
  QCOMPARE(resident.exitCode(), 0);
 }
 void coreControllerExportsLiveUploadStatusAndReturnsIdle() {
  QTemporaryDir directory;ProviderRegistry providers;
  auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.defer=true;source.payload.kind="text";source.payload.bytes=QByteArray(4*1024*1024,'x');
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});QTRY_VERIFY(controller.available());
  auto properties=[&]{auto result=call("/StatusNotifierItem","org.freedesktop.DBus.Properties","GetAll",{QString("org.kde.StatusNotifierItem")});return result.type()==QDBusMessage::ReplyMessage?qdbus_cast<QVariantMap>(result.arguments().first()):QVariantMap{};};
  const auto idle=properties();const auto idleIcon=qdbus_cast<WireImages>(idle["IconPixmap"]).first().bytes;
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});
  QTRY_COMPARE(source.acquisitions.size(),1);
  QTRY_VERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("Acquiring"));
  QCOMPARE(qdbus_cast<WireImages>(properties()["IconPixmap"]).first().bytes,idleIcon);
  source.acquisitions.takeFirst()({source.payload,{}});
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  QTRY_COMPARE(provider->requests.size(),1);
  auto pixels=[&]{return qdbus_cast<WireImages>(properties()["IconPixmap"]).first().bytes;};
  auto green=[](const QByteArray& bytes){int count=0;for(int i=0;i<bytes.size();i+=4)if(uchar(bytes[i])==255&&uchar(bytes[i+1])==98&&uchar(bytes[i+2])==214&&uchar(bytes[i+3])==155)++count;return count;};
  provider->requests.first().progress({"uploading",0,4*1024*1024});
  QTRY_VERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("(0%)"));
  const auto emptyIcon=pixels();
  provider->requests.first().progress({"uploading",2*1024*1024,4*1024*1024});
  QTRY_VERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("2 MiB of 4 MiB"));
  const auto activeIcon=qdbus_cast<WireImages>(properties()["IconPixmap"]).first().bytes;QVERIFY(activeIcon!=idleIcon);
  QVERIFY(green(activeIcon)>green(emptyIcon));
  provider->requests.first().progress({"uploading",4*1024*1024,4*1024*1024});
  QTRY_VERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("(100%)"));
  QVERIFY(green(pixels())>green(activeIcon));
  provider->requests.first().progress({"uploading",2*1024*1024,{}});
  QTRY_VERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("Estimated 50%"));
  QCOMPARE(pixels(),activeIcon);
  QVERIFY(views.isEmpty());QVERIFY(!QCoreApplication::instance()->inherits("QGuiApplication"));
  controller.notifyFailure({"fixture_failed","Synthetic error",{}});
  const auto redIcon=pixels();QVERIFY(redIcon!=idleIcon);QVERIFY(redIcon!=activeIcon);
  int opaqueRed=0;for(int index=0;index<redIcon.size();index+=4)
   if(uchar(redIcon[index])==255&&uchar(redIcon[index+1])==255&&uchar(redIcon[index+2])==180&&uchar(redIcon[index+3])==171)++opaqueRed;
  QVERIFY(opaqueRed>10);
  QTRY_COMPARE(pixels(),idleIcon);QTRY_COMPARE(pixels(),redIcon);
  QTest::qWait(3100);QCOMPARE(pixels(),activeIcon);
  provider->completions.takeFirst()();
  QTRY_COMPARE(qdbus_cast<WireTooltip>(properties()["ToolTip"]).title,qdbus_cast<WireTooltip>(idle["ToolTip"]).title);
  QCOMPARE(qdbus_cast<WireImages>(properties()["IconPixmap"]).first().bytes,idleIcon);
 }
 void activitySignalsChangeOnlyTheAffectedPropertiesAndSurviveHostRestart() {
  NativeTray tray;QTRY_VERIFY(tray.available());ActivitySignalPeer observer;
  const auto service=watcher.registrations.last();
  QVERIFY(client.connect(service,"/StatusNotifierItem","org.kde.StatusNotifierItem","NewIcon",&observer,SLOT(iconChanged())));
  QVERIFY(client.connect(service,"/StatusNotifierItem","org.kde.StatusNotifierItem","NewToolTip",&observer,SLOT(tooltipChanged())));
  tray.setActivity(QColor("#62d69b"),"Uploading: 2 MiB of 4 MiB (50%)",50);QTRY_COMPARE(observer.icons,1);QTRY_COMPARE(observer.tooltips,1);
  tray.setActivity(QColor("#62d69b"),"Uploading: 3 MiB of 4 MiB (75%)",75);QTRY_COMPARE(observer.tooltips,2);QCOMPARE(observer.icons,2);
  tray.setActivity(QColor("#62d69b"),"Uploading: 3 MiB of 4 MiB (75%)",75);QTest::qWait(50);QCOMPARE(observer.tooltips,2);QCOMPARE(observer.icons,2);
  changeHost(false);QTRY_VERIFY(!tray.available());tray.setActivity(QColor("#62d69b"),"Finalizing: 4 MiB of 4 MiB (100%)",100);
  changeHost(true);QTRY_VERIFY(tray.available());
  auto result=call("/StatusNotifierItem","org.freedesktop.DBus.Properties","GetAll",{QString("org.kde.StatusNotifierItem")});
  const auto props=qdbus_cast<QVariantMap>(result.arguments().first());
  QCOMPARE(qdbus_cast<WireTooltip>(props["ToolTip"]).description,QString("Finalizing: 4 MiB of 4 MiB (100%)"));
  const auto pixels=qdbus_cast<WireImages>(props["IconPixmap"]).first().bytes;int opaqueGreen=0;
  for(int index=0;index<pixels.size();index+=4)if(uchar(pixels[index])==255&&uchar(pixels[index+1])==98&&uchar(pixels[index+2])==214&&uchar(pixels[index+3])==155)++opaqueGreen;
  QVERIFY(opaqueGreen>10);
  const auto tooltips=observer.tooltips;
  tray.setActivity(QColor("#ffb4ab"),"Finalizing: 4 MiB of 4 MiB (100%)");QTRY_COMPARE(observer.icons,4);
  QCOMPARE(observer.tooltips,tooltips);
  tray.setActivity(QColor("#9bbcff"),{});QTRY_COMPARE(observer.icons,5);
 }
 void coreOnlyBrandPropertiesAndActualLayoutWire() {
  NativeTray tray; QVERIFY(!tray.availabilityKnown()); QTRY_VERIFY(tray.availabilityKnown()); QTRY_VERIFY(tray.available()); QCOMPARE(watcher.registrations.size(), 1);
  auto reply = call("/StatusNotifierItem", "org.freedesktop.DBus.Properties", "GetAll", {QString("org.kde.StatusNotifierItem")});
  QCOMPARE(reply.type(), QDBusMessage::ReplyMessage); QCOMPARE(reply.signature(), QString("a{sv}"));
  auto props = qdbus_cast<QVariantMap>(reply.arguments().at(0));
  QCOMPARE(props["Id"].toString(), QString("putgd")); QCOMPARE(props["Status"].toString(), QString("Active"));
  QCOMPARE(qdbus_cast<QDBusObjectPath>(props["Menu"]).path(), QString("/Menu"));
  QVERIFY(!props["ItemIsMenu"].toBool());
  const auto images = qdbus_cast<WireImages>(props["IconPixmap"]); QCOMPARE(images.size(), 3);
  for (int i = 0; i < images.size(); ++i) {
   QCOMPARE(images[i].width, 24 * (i+1)); QCOMPARE(images[i].height, images[i].width);
   QCOMPARE(images[i].bytes.size(), images[i].width * images[i].height * 4);
   int opaqueBlue = 0;
   for (int j = 0; j < images[i].bytes.size(); j += 4)
    if (uchar(images[i].bytes[j]) == 255 && uchar(images[i].bytes[j+1]) == 155 && uchar(images[i].bytes[j+2]) == 188 && uchar(images[i].bytes[j+3]) == 255) ++opaqueBlue;
   QVERIFY(opaqueBlue > 10);
  }
  reply = call("/Menu", "com.canonical.dbusmenu", "GetLayout", {0, -1, QStringList{}});
  QCOMPARE(reply.signature(), QString("u(ia{sv}av)")); QCOMPARE(reply.arguments().at(0).toUInt(), uint(1));
  auto layout = qdbus_cast<WireLayout>(reply.arguments().at(1)); QCOMPARE(layout.id, 0); QCOMPARE(layout.children.size(), 11);
  QCOMPARE(layout.props["children-display"].toString(), QString("submenu"));
  QCOMPARE(qdbus_cast<WireLayout>(layout.children.at(0).variant()).props["label"].toString(), QString("Capture region"));
  QCOMPARE(qdbus_cast<WireLayout>(layout.children.at(3).variant()).props["label"].toString(), QString("Upload Clipboard"));
  QCOMPARE(qdbus_cast<WireLayout>(layout.children.at(4).variant()).props["type"].toString(), QString("separator"));
  reply = call("/Menu", "com.canonical.dbusmenu", "GetLayout", {0, 0, QStringList{"label"}});
  layout = qdbus_cast<WireLayout>(reply.arguments().at(1)); QVERIFY(layout.children.isEmpty()); QVERIFY(layout.props.isEmpty());
  QCOMPARE(call("/Menu", "com.canonical.dbusmenu", "AboutToShow", {0}).arguments().at(0).toBool(), false);
  QCOMPARE(call("/Menu", "com.canonical.dbusmenu", "GetLayout", {-1, -1, QStringList{}}).type(), QDBusMessage::ErrorMessage);
  QCOMPARE(call("/Menu", "com.canonical.dbusmenu", "GetLayout", {0, -2, QStringList{}}).type(), QDBusMessage::ErrorMessage);
  QCOMPARE(call("/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Set", {QString("org.kde.StatusNotifierItem"), QString("Status"), QVariant::fromValue(QDBusVariant("Passive"))}).errorName(), QString("org.freedesktop.DBus.Error.PropertyReadOnly"));
  QVERIFY(!QCoreApplication::instance()->inherits("QGuiApplication"));
 }
 void everyActionAndActivationUsesFixedIds() {
  NativeTray tray; QTRY_VERIFY(tray.available()); QSignalSpy actions(&tray, &NativeTray::actionActivated);
  const QList<int> ids{1,2,3,4,6,7,8,10,11};
  const QStringList expected{"region","window","desktop","clipboard","settings","history","activity","about","quit"};
  for (int i = 0; i < ids.size(); ++i) { QCOMPARE(click(ids[i]).type(), QDBusMessage::ReplyMessage); QTRY_COMPARE(actions.size(), i+1); QCOMPARE(actions.at(i).at(0).toString(), expected[i]); }
  QCOMPARE(click(5).type(), QDBusMessage::ReplyMessage); QCOMPARE(click(9).type(), QDBusMessage::ReplyMessage);
  QCOMPARE(click(1, "hovered").type(), QDBusMessage::ReplyMessage); QCOMPARE(click(99).type(), QDBusMessage::ErrorMessage);
  QCOMPARE(actions.size(), 9);
  QCOMPARE(call("/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate", {0,0}).type(), QDBusMessage::ReplyMessage);
  QTRY_COMPARE(actions.size(), 10); QCOMPARE(actions.last().at(0).toString(), QString("activity"));
  WireEvents events{{1,"clicked",QDBusVariant(0),0},{99,"clicked",QDBusVariant(0),0}};
  auto reply = call("/Menu", "com.canonical.dbusmenu", "EventGroup", {QVariant::fromValue(events)});
  QCOMPARE(reply.type(), QDBusMessage::ReplyMessage); QCOMPARE(qdbus_cast<QList<int>>(reply.arguments().at(0)), QList<int>{99});
  QTRY_COMPARE(actions.size(), 11); QCOMPARE(actions.last().at(0).toString(), QString("region"));
 }
 void lateHostAndWatcherRestartDoNotDuplicateRegistration() {
  watcher.hostPresent = false; NativeTray tray;
  QTRY_COMPARE(watcher.registrations.size(), 1); QTRY_VERIFY(tray.availabilityKnown()); QVERIFY(!tray.available());
  changeHost(true); QTRY_VERIFY(tray.available());
  for (int i = 0; i < 3; ++i) emit watcher.StatusNotifierHostRegistered();
  QTest::qWait(100); QCOMPARE(watcher.registrations.size(), 1);
  changeHost(false); QTRY_VERIFY(!tray.available()); changeHost(true); QTRY_VERIFY(tray.available()); QCOMPARE(watcher.registrations.size(), 1);
  QVERIFY(peer.unregisterService("org.kde.StatusNotifierWatcher")); QTRY_VERIFY(!tray.available());
  QVERIFY(peer.registerService("org.kde.StatusNotifierWatcher")); QTRY_VERIFY(tray.available()); QCOMPARE(watcher.registrations.size(), 2);
 }
 void initiallyAbsentWatcherResolvesThenRecoversOnAppearance() {
  QVERIFY(peer.unregisterService("org.kde.StatusNotifierWatcher"));
  NativeTray tray; QSignalSpy resolved(&tray, &NativeTray::availabilityResolved);
  QVERIFY(!tray.availabilityKnown()); QTRY_VERIFY(tray.availabilityKnown());
  QVERIFY(!tray.available()); QTRY_COMPARE(resolved.size(), 1); QVERIFY(watcher.registrations.isEmpty());
  QVERIFY(peer.registerService("org.kde.StatusNotifierWatcher")); QTRY_VERIFY(tray.available());
  QTRY_VERIFY(tray.availabilityKnown()); QCOMPARE(watcher.registrations.size(), 1);
 }
 void watcherNameBeforeObjectExportRecoversWithoutFalseAbsence() {
  peer.unregisterObject("/StatusNotifierWatcher");
  NativeTray tray; QSignalSpy resolved(&tray, &NativeTray::availabilityResolved);
  QTest::qWait(100); QVERIFY(!tray.availabilityKnown()); QVERIFY(!tray.available());
  QVERIFY(peer.registerObject("/StatusNotifierWatcher", &watcher, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSignals));
  QTRY_VERIFY_WITH_TIMEOUT(tray.available(), 3000); QVERIFY(tray.availabilityKnown());
  QTRY_COMPARE(resolved.size(), 1); QCOMPARE(watcher.registrations.size(), 1);
 }
 void registrationFailuresAreBoundedAndNewHostCanRecover() {
  watcher.failRegistration = true; NativeTray tray;
  QTRY_COMPARE_WITH_TIMEOUT(watcher.registrations.size(), 3, 4000); QVERIFY(!tray.available());
  QTRY_VERIFY(tray.availabilityKnown());
  QTest::qWait(1700); QCOMPARE(watcher.registrations.size(), 3);
  watcher.failRegistration = false; emit watcher.StatusNotifierHostRegistered();
  QTRY_VERIFY(tray.available()); QCOMPARE(watcher.registrations.size(), 4);
 }
 void destructiveCallbackRemovesServiceAndExports() {
  auto tray = std::make_unique<NativeTray>(); QTRY_VERIFY(tray->available());
  const auto service = watcher.registrations.last();
  QPointer<NativeTray> guard(tray.get()); bool invoked = false;
  connect(tray.get(), &NativeTray::actionActivated, this, [&](const QString&) { invoked = true; tray.reset(); });
  QCOMPARE(click(11).type(), QDBusMessage::ReplyMessage); QTRY_VERIFY(invoked); QVERIFY(guard.isNull());
  QVERIFY(!client.interface()->isServiceRegistered(service).value());
  QTRY_VERIFY(!watcher.activeServices.contains(service));
  QVERIFY(QDBusConnection::sessionBus().isConnected());
  QCOMPARE(call("/Menu", "com.canonical.dbusmenu", "AboutToShow", {0}).type(), QDBusMessage::ErrorMessage);
  NativeTray replacement; QTRY_VERIFY(replacement.available()); QCOMPARE(watcher.registrations.size(), 2);
 }
 void notificationWireAndNoHostSuppression() {
  NativeTray tray; QTRY_VERIFY(tray.available());
  tray.notify("put.gd — failed", "Synthetic failure", true); QTRY_COMPARE(notifications.values.size(), 1);
  const auto value = notifications.values.first(); QCOMPARE(value["app"].toString(), QString("put.gd"));
  QCOMPARE(value["summary"].toString(), QString("put.gd — failed")); QCOMPARE(value["timeout"].toInt(), 10000);
  QCOMPARE(value["hints"].toMap()["urgency"].toUInt(), uint(2)); QVERIFY(value["actions"].toStringList().isEmpty());
  changeHost(false); QTRY_VERIFY(!tray.available()); tray.notify("Hidden", "Unavailable"); QTest::qWait(50); QCOMPARE(notifications.values.size(), 1);
 }
 void coreControllerRoutesAllFourActionsWithoutViews() {
  QTemporaryDir directory; QVERIFY(directory.isValid()); ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json")); QVERIFY(settings.save(configuration(directory.path(), true), {}));
  Source source; source.defer = true;
  OperationService operations(directory.filePath("data"), settings, providers, source); QTRY_VERIFY(operations.ready());
  QStringList views; QSet<QString> cancelled;
  TrayController controller(operations, [&](const QString& view) { views.append(view); }, [] {});
  QTRY_VERIFY(controller.available());
  connect(&operations, &OperationService::changed, this, [&](const QString& id) {
   operations.inspect(id, [&](Error e, QJsonObject op) { QVERIFY(!e); if (op["state"] == "cancelled") cancelled.insert(op["action"].toString()); });
  });
  const QStringList names{"region","window","desktop","clipboard"};
  for (int i = 0; i < names.size(); ++i) {
   QCOMPARE(click(i+1).type(), QDBusMessage::ReplyMessage); QTRY_COMPARE(source.reads, i+1);
   QTRY_COMPARE(source.acquisitions.size(), 1);
   auto acquired = source.acquisitions.takeFirst(); acquired({{}, {"source_cancelled", "Synthetic source cancellation", {}}});
   QTRY_VERIFY(cancelled.contains(names[i]));
  }
  QCOMPARE(cancelled.size(), 4); QCOMPARE(source.copies, 0); QVERIFY(views.isEmpty());
 }
 void progressChangesDoNotRepeatTerminalToastAndExplicitCopyStillNotifies(){
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);QTRY_VERIFY(operations.ready());QStringList views;TrayController controller(operations,[&](const QString& value){views.append(value);},[]{});QTRY_VERIFY(controller.available());QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);id=result["operationId"].toString();});QTRY_COMPARE(source.copies,1);QTRY_VERIFY(!notifications.values.isEmpty());
  for(int i=0;i<50;++i)emit operations.changed(id);
  QTest::qWait(400);QCOMPARE(notifications.values.size(),1);QVERIFY(views.isEmpty());bool copied=false;operations.copyResult(id,[&](Error e,QJsonObject){QVERIFY(!e);copied=true;});QTRY_VERIFY(copied);QTRY_COMPARE(notifications.values.size(),2);QCOMPARE(source.copies,2);
 }
 void coreControllerDelegatesViewsAndPreservesCopyPendingAndNotificationSetting() {
  QTemporaryDir directory; QVERIFY(directory.isValid()); ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json")); auto config = configuration(directory.path(), true);
  QVERIFY(settings.save(config, {})); Source source; source.payload = textPayload();
  OperationService operations(directory.filePath("data"), settings, providers, source); QTRY_VERIFY(operations.ready());
  QStringList views; int quits = 0;
  TrayController controller(operations, [&](const QString& view) { views.append(view); }, [&] { ++quits; });
  auto* native = controller.findChild<NativeTray*>(); QVERIFY(native); QTRY_VERIFY(native->available());
  for (int id : {6,7,8,10}) QCOMPARE(click(id).type(), QDBusMessage::ReplyMessage);
  QTRY_COMPARE(views, QStringList({"settings","history","activity","about"}));
  QCOMPARE(click(4).type(), QDBusMessage::ReplyMessage); QTRY_COMPARE(source.copies, 1); QTRY_COMPARE(notifications.values.size(), 1);
  QVERIFY(notifications.values.first()["summary"].toString().contains("completed"));
  QVERIFY(notifications.values.first()["body"].toString().contains("copied"));
  QTest::qWait(100); QCOMPARE(notifications.values.size(), 1);
  auto updated = settings.document();
  auto preferences = updated["preferences"].toObject(); preferences["notifications"] = false; updated["preferences"] = preferences;
  const auto saved = settings.save(updated, settings.revision());
  QVERIFY2(saved, qPrintable(saved.error.code)); QVERIFY(!settings.snapshot().notifications);
  QCOMPARE(click(4).type(), QDBusMessage::ReplyMessage); QTRY_COMPARE(source.copies, 2);
  QTest::qWait(100); QCOMPARE(notifications.values.size(), 1);
  QCOMPARE(click(11).type(), QDBusMessage::ReplyMessage); QTRY_COMPARE(quits, 1);
  QVERIFY(!QCoreApplication::instance()->inherits("QGuiApplication"));
 }
};
QTEST_GUILESS_MAIN(NativeTrayWire)
#include "test_native_tray.moc"
