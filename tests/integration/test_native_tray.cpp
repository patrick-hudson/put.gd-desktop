#include "ui/NativeTray.h"
#include "ui/Theme.h"
#include "ui/TrayController.h"
#include "fixtures/U2Fixtures.h"
#include "core/Log.h"
#include <QJsonDocument>
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
#include <QDir>
#include <tuple>
#include <memory>
#include <QFile>
#include <QElapsedTimer>
#include <QTimer>
#include <QScopeGuard>
#include <QRegularExpression>
#include <csignal>
#include <sys/types.h>
#include "fixtures/X11DesktopFixture.h"
#include <X11/keysym.h>
#include "fixtures/Waits.h"
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
 QString server; int serverQueries = 0; QList<uint> closed;
public slots:
 void CloseNotification(uint id) { closed.append(id); }
 QString GetServerInformation(QString& vendor, QString& version, QString& spec) {
  ++serverQueries; vendor = "Synthetic"; version = "1"; spec = "1.2"; return server;
 }
 uint Notify(const QString& app, uint replacement, const QString& icon,
             const QString& summary, const QString& body, const QStringList& actions,
             const QVariantMap& hints, int timeout) {
  values.append({{"app", app}, {"replacement", replacement}, {"icon", icon}, {"summary", summary},
   {"body", body}, {"actions", actions}, {"hints", hints}, {"timeout", timeout}});
  return uint(values.size());
 }
signals:
 void ActionInvoked(uint id, const QString& action);
 void NotificationClosed(uint id, uint reason);
};

class FileManagerPeer final : public QObject, protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")
public:
 QList<QStringList> shown;
 // Answers like a bus with no file manager. Registering it also keeps the private
 // bus from activating an installed one (Dolphin on Plasma hosts).
 bool refuse = false;
public slots:
 void ShowItems(const QStringList& uris, const QString&) {
  if (refuse) { sendErrorReply("org.freedesktop.DBus.Error.ServiceUnknown", "Synthetic missing file manager"); return; }
  shown.append(uris);
 }
};
class ActivitySignalPeer final:public QObject {
 Q_OBJECT
public:int icons=0,tooltips=0;
public slots:void iconChanged(){++icons;}void tooltipChanged(){++tooltips;}
};
class LayoutSignalPeer final:public QObject {
 Q_OBJECT
public:int updates=0;uint revision=0;int parent=-1;int itemUpdates=0;QList<std::pair<int,QVariantMap>> items;
public slots:void layoutUpdated(uint r,int p){++updates;revision=r;parent=p;}
 void itemsUpdated(const QDBusMessage& m){
  ++itemUpdates;items.clear();const auto arg=m.arguments().at(0).value<QDBusArgument>();
  arg.beginArray();while(!arg.atEnd()){int id=0;QVariantMap props;arg.beginStructure();arg>>id>>props;arg.endStructure();items.append({id,props});}arg.endArray();
 }
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
  QVERIFY(peer.registerObject("/org/freedesktop/Notifications", &notifications, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
  QVERIFY(peer.registerService("org.freedesktop.Notifications"));
 }
 void init() {
  PUTGD_TRY_VERIFY(watcher.activeServices.isEmpty());
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
  const auto configPath = directory.filePath("c/putgd/config.json");
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
  PUTGD_TRY_VERIFY_WITH_TIMEOUT((snapshot=request("ui.snapshot"))["operationsReady"].toBool(), 5000);
  QElapsedTimer readyElapsed; readyElapsed.start();
  if (late || trayState == "after-bound") appearance.start(trayState == "after-bound" ? 12000 : 1500);
  const bool windowless = background && configuration == "valid";
  const bool recovery = trayState == "absent" || trayState == "after-bound";
  QJsonObject expectedExit;
  if (trayState == "user-activation") {
   QProcess activation; activation.setProcessEnvironment(environment);
   activation.start(PUTGD_PRODUCT_APP_PATH, {"--history"});
   QVERIFY(activation.waitForFinished(5000)); QCOMPARE(activation.exitCode(), 0);
   PUTGD_TRY_COMPARE(ui()["state"].toString(), QString("ready"));
   const auto helperPid = ui()["pid"].toInteger();
   QVERIFY(helperPid > 0);
   QVERIFY(::kill(static_cast<pid_t>(helperPid), SIGTERM) == 0);
   PUTGD_TRY_VERIFY(!(expectedExit=ui()["lastExit"].toObject()).isEmpty());
  }
  if (windowless) {
   // Observe the interval, not just the final state: a transient helper is a failure too.
   const int observeUntil = recovery ? 8000 : 11000;
   while (readyElapsed.elapsed() < observeUntil) {
    const auto status = ui();
    QCOMPARE(status["state"].toString(), QString("stopped"));
    QCOMPARE(status["pid"].toInteger(), qint64(0));
    QCOMPARE(status["lastExit"].toObject(), expectedExit);
    putgd::test::settle(100);
   }
   if (!recovery && trayState != "user-activation") {
    PUTGD_TRY_VERIFY(!watcher.registrations.isEmpty());
    PUTGD_TRY_VERIFY(request("ui.snapshot")["diagnostics"].toObject()["trayAvailable"].toBool());
   }
  }
  if (!windowless || recovery) {
   PUTGD_TRY_COMPARE_WITH_TIMEOUT(ui()["state"].toString(), QString("ready"), 10000);
   if (windowless) QVERIFY(elapsed.elapsed() >= 10000);
   else QVERIFY(elapsed.elapsed() < 10000);
  }
  if (trayState == "after-bound") {
   PUTGD_TRY_VERIFY_WITH_TIMEOUT(!watcher.registrations.isEmpty(), 5000);
   PUTGD_TRY_VERIFY(request("ui.snapshot")["diagnostics"].toObject()["trayAvailable"].toBool());
   QCOMPARE(ui()["state"].toString(), QString("ready"));
  }
  QVERIFY(request("ui.quit")["quitting"].toBool());
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(resident.state(), QProcess::NotRunning, 5000);
  QCOMPARE(resident.exitCode(), 0);
 }
 void rejectedGlobalShortcutReportsMissingDestination_data() {
  QTest::addColumn<bool>("menuOpen");
  QTest::newRow("passive grab") << false;
  // #341: another client's open menu holds the keyboard; XInput2 raw keys fire it.
  QTest::newRow("menu holds the keyboard") << true;
 }
 void rejectedGlobalShortcutReportsMissingDestination() {
  if (qEnvironmentVariable("PUTGD_ISOLATED_SHORTCUT_TEST") != "1")
   QSKIP("Requires its dedicated private-bus/Xvfb invocation");
  QFETCH(bool, menuOpen);
  notifications.values.clear();
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
  SettingsService settings(directory.filePath("c/putgd/config.json"));
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
  PUTGD_TRY_VERIFY_WITH_TIMEOUT((snapshot=request("ui.snapshot"))["operationsReady"].toBool(), 5000);
  PUTGD_TRY_VERIFY(!watcher.registrations.isEmpty());
  // Notifications wait for the resident's registration reply, not just the watcher's view of the call.
  PUTGD_TRY_VERIFY(request("ui.snapshot")["diagnostics"].toObject()["trayAvailable"].toBool());
  QVERIFY(snapshot["diagnostics"].toObject()["shortcuts"].toObject()["bindings"].toObject()["region"].toObject()["bound"].toBool());
  QCOMPARE(snapshot["diagnostics"].toObject()["ui"].toObject()["pid"].toInteger(), qint64(0));
  QVERIFY(snapshot["diagnostics"].toObject()["shortcuts"].toObject()["keyPaths"].toObject()["raw"].toObject()["available"].toBool());
  X11DesktopFixture desktop;
  std::unique_ptr<X11GrabbingClient> menu;
  if (menuOpen) { menu = std::make_unique<X11GrabbingClient>(); QVERIFY(menu->grabbed); }
  desktop.key(XK_Control_L,true);desktop.key(XK_1,true);
  desktop.key(XK_1,false);desktop.key(XK_Control_L,false);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(notifications.values.size(), 1, 5000);
  // The CLI (and the MCP helper's inspect, which forwards to it) names the path that fired.
  const auto fired = request("ui.snapshot")["diagnostics"].toObject()["shortcuts"].toObject()["keyPaths"].toObject()["lastActivation"].toObject();
  QCOMPARE(fired["action"].toString(), QString("region"));
  QCOMPARE(fired["path"].toString(), QString(menuOpen ? "raw" : "passive"));
  const auto body=notifications.values.first()["body"].toString();QVERIFY(body.contains("No output is enabled"));QVERIFY(body.contains("Settings → Capture"));QVERIFY(body.contains("Keep a local copy in Settings → Capture"));
  for(int repeat=0;repeat<2;++repeat){
   desktop.key(XK_Control_L,true);desktop.key(XK_1,true);
   desktop.key(XK_1,false);desktop.key(XK_Control_L,false);
   putgd::test::settle(50);
  }
  QCOMPARE(notifications.values.size(),1);
  QVERIFY(request("operation.recent")["operations"].toArray().isEmpty());
  QCOMPARE(request("ui.snapshot")["diagnostics"].toObject()["ui"].toObject()["pid"].toInteger(), qint64(0));
  QVERIFY(request("ui.quit")["quitting"].toBool());
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(resident.state(), QProcess::NotRunning, 5000);
  QCOMPARE(resident.exitCode(), 0);
 }
 void coreControllerExportsLiveUploadStatusAndReturnsIdle() {
  QTemporaryDir directory;ProviderRegistry providers;
  // #250: toasts at info and tooltip changes at debug reach the log.
  logging::open(directory.filePath("logs"));logging::setDebug(true);
  auto closeLog=qScopeGuard([]{logging::setDebug(false);logging::close();});
  auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.defer=true;source.payload.kind="text";source.payload.bytes=QByteArray(4*1024*1024,'x');
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  QStringList views;TrayController controller(operations,[&](const QString& view){views.append(view);},[]{});PUTGD_TRY_VERIFY(controller.available());
  auto properties=[&]{auto result=call("/StatusNotifierItem","org.freedesktop.DBus.Properties","GetAll",{QString("org.kde.StatusNotifierItem")});return result.type()==QDBusMessage::ReplyMessage?qdbus_cast<QVariantMap>(result.arguments().first()):QVariantMap{};};
  const auto idle=properties();const auto idleIcon=qdbus_cast<WireImages>(idle["IconPixmap"]).first().bytes;
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});
  PUTGD_TRY_COMPARE(source.acquisitions.size(),1);
  PUTGD_TRY_VERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("Acquiring"));
  QCOMPARE(qdbus_cast<WireImages>(properties()["IconPixmap"]).first().bytes,idleIcon);
  source.acquisitions.takeFirst()({source.payload,{}});
  struct Drain {std::shared_ptr<OpaqueProvider> provider;~Drain(){for(auto complete:std::exchange(provider->completions,{}))complete();}} drain{provider};
  PUTGD_TRY_COMPARE(provider->requests.size(),1);
  auto pixels=[&]{return qdbus_cast<WireImages>(properties()["IconPixmap"]).first().bytes;};
  auto amber=[](const QByteArray& bytes){const auto color=Theme::trayTints(false).encoding;int count=0;for(int i=0;i<bytes.size();i+=4)if(uchar(bytes[i])==255&&uchar(bytes[i+1])==color.red()&&uchar(bytes[i+2])==color.green()&&uchar(bytes[i+3])==color.blue())++count;return count;};
  provider->requests.first().progress({"uploading",0,4*1024*1024});
  PUTGD_TRY_VERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("(0%)"));
  const auto emptyIcon=pixels();
  provider->requests.first().progress({"uploading",2*1024*1024,4*1024*1024});
  PUTGD_TRY_VERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("2 MB of 4 MB"));
  PUTGD_TRY_VERIFY(amber(pixels())>amber(emptyIcon));const auto activeIcon=pixels();QVERIFY(activeIcon!=idleIcon);
  provider->requests.first().progress({"uploading",4*1024*1024,4*1024*1024});
  PUTGD_TRY_VERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("(100%)"));
  QVERIFY(amber(pixels())>amber(activeIcon));
  provider->requests.first().progress({"uploading",2*1024*1024,{}});
  PUTGD_TRY_VERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("Estimated 50%"));
  QCOMPARE(pixels(),activeIcon);
  QVERIFY(views.isEmpty());QVERIFY(!QCoreApplication::instance()->inherits("QGuiApplication"));
  controller.notifyFailure({"fixture_failed","Synthetic error",{}});
  const auto redIcon=pixels();QVERIFY(redIcon!=idleIcon);QVERIFY(redIcon!=activeIcon);
  const auto error=Theme::trayTints(false).error;int opaqueRed=0;for(int index=0;index<redIcon.size();index+=4)
   if(uchar(redIcon[index])==255&&uchar(redIcon[index+1])==error.red()&&uchar(redIcon[index+2])==error.green()&&uchar(redIcon[index+3])==error.blue())++opaqueRed;
  QVERIFY(opaqueRed>10);
  PUTGD_COMPARE_STEADY(pixels(),redIcon,3400);click(22);PUTGD_TRY_VERIFY(views.contains("history"));PUTGD_TRY_COMPARE(pixels(),activeIcon);
  provider->completions.takeFirst()();
  PUTGD_TRY_COMPARE(qdbus_cast<WireTooltip>(properties()["ToolTip"]).title,qdbus_cast<WireTooltip>(idle["ToolTip"]).title);
  QCOMPARE(qdbus_cast<WireImages>(properties()["IconPixmap"]).first().bytes,idleIcon);
  logging::flush();QList<QJsonObject> records;
  const QDir logs(directory.filePath("logs"));
  for(const auto& name:logs.entryList({"putgd-*.jsonl"},QDir::Files,QDir::Name)){QFile file(logs.filePath(name));QVERIFY(file.open(QIODevice::ReadOnly));for(const auto& line:file.readAll().split('\n'))if(!line.isEmpty())records.append(QJsonDocument::fromJson(line).object());}
  bool toast=false,progress=false,idleAgain=false;
  for(const auto& record:records) {
   const auto fields=record["fields"].toObject();
   if(record["message"]=="Notification shown"&&fields["summary"]=="fixture_failed") {
    toast=true;QCOMPARE(record["level"].toString(),QString("info"));QCOMPARE(record["category"].toString(),QString("putgd.ui"));
    QCOMPARE(fields["notificationText"].toString(),QString("Synthetic error"));QVERIFY(fields["notificationId"].toInteger()>0);QVERIFY(fields["warning"].toBool());
   }
   if(record["message"]=="Tray tooltip changed") {
    QCOMPARE(record["level"].toString(),QString("debug"));
    progress=progress||fields["activity"].toString().contains("2 MB of 4 MB");
    idleAgain=idleAgain||(fields["activity"].toString().isEmpty()&&fields["title"]=="put.gd");
   }
  }
  QVERIFY(toast);QVERIFY(progress);QVERIFY(idleAgain);
 }
 void tooltipTitlesFollowActivityState(){
  NativeTray tray;PUTGD_TRY_VERIFY(tray.available());
  const auto title=[&]{const auto result=call("/StatusNotifierItem","org.freedesktop.DBus.Properties","GetAll",{QString("org.kde.StatusNotifierItem")});return qdbus_cast<WireTooltip>(qdbus_cast<QVariantMap>(result.arguments().first())["ToolTip"]).title;};
  QCOMPARE(title(),QString("put.gd"));tray.setActivity(Theme::trayTints(false).idle,"Transfer progress",50);QCOMPARE(title(),QString("Uploading"));tray.setActivity(Theme::trayTints(false).upload,"Finished",-1,0,"Upload complete");QCOMPARE(title(),QString("Upload complete"));tray.setActivity(Theme::trayTints(false).error,"Failure",-2);QCOMPARE(title(),QString("Needs attention"));tray.setActivity(Theme::trayTints(false).idle,{});QCOMPARE(title(),QString("put.gd"));
 }
 void activitySignalsChangeOnlyTheAffectedPropertiesAndSurviveHostRestart() {
  NativeTray tray;PUTGD_TRY_VERIFY(tray.available());ActivitySignalPeer observer;
  const auto service=watcher.registrations.last();
  QVERIFY(client.connect(service,"/StatusNotifierItem","org.kde.StatusNotifierItem","NewIcon",&observer,SLOT(iconChanged())));
  QVERIFY(client.connect(service,"/StatusNotifierItem","org.kde.StatusNotifierItem","NewToolTip",&observer,SLOT(tooltipChanged())));
  tray.setActivity(Theme::trayTints(false).idle,"Uploading: 2 MB of 4 MB (50%)",50);PUTGD_TRY_COMPARE(observer.icons,1);PUTGD_TRY_COMPARE(observer.tooltips,1);
  tray.setActivity(Theme::trayTints(false).idle,"Uploading: 3 MB of 4 MB (75%)",75);PUTGD_TRY_COMPARE(observer.tooltips,2);PUTGD_TRY_COMPARE(observer.icons,2);
  tray.setActivity(Theme::trayTints(false).idle,"Uploading: 3 MB of 4 MB (75%)",75);putgd::test::settle(50);PUTGD_TRY_COMPARE(observer.tooltips,2);PUTGD_TRY_COMPARE(observer.icons,2);
  changeHost(false);PUTGD_TRY_VERIFY(!tray.available());tray.setActivity(Theme::trayTints(false).idle,"Finalizing: 4 MB of 4 MB (100%)",100);
  changeHost(true);PUTGD_TRY_VERIFY(tray.available());
  auto result=call("/StatusNotifierItem","org.freedesktop.DBus.Properties","GetAll",{QString("org.kde.StatusNotifierItem")});
  const auto props=qdbus_cast<QVariantMap>(result.arguments().first());
  QCOMPARE(qdbus_cast<WireTooltip>(props["ToolTip"]).description,QString("Finalizing: 4 MB of 4 MB (100%)"));
  const auto pixels=qdbus_cast<WireImages>(props["IconPixmap"]).first().bytes;const auto amber=Theme::trayTints(false).encoding;int opaqueAmber=0;
  for(int index=0;index<pixels.size();index+=4)if(uchar(pixels[index])==255&&uchar(pixels[index+1])==amber.red()&&uchar(pixels[index+2])==amber.green()&&uchar(pixels[index+3])==amber.blue())++opaqueAmber;
  QVERIFY(opaqueAmber>10);
  const auto tooltips=observer.tooltips;
  tray.setActivity(Theme::trayTints(false).error,"Finalizing: 4 MB of 4 MB (100%)",100);PUTGD_TRY_COMPARE(observer.icons,4);
  QCOMPARE(observer.tooltips,tooltips);
  tray.setActivity(QColor("#9bbcff"),{});PUTGD_TRY_COMPARE(observer.icons,5);
 }
 void recordingOverlayKeepsUploadProgressAndFailure() {
  NativeTray tray;PUTGD_TRY_VERIFY(tray.available());
  auto properties=[&]{return qdbus_cast<QVariantMap>(call("/StatusNotifierItem","org.freedesktop.DBus.Properties","GetAll",{QString("org.kde.StatusNotifierItem")}).arguments().first());};
  auto pixels=[&]{return qdbus_cast<WireImages>(properties()["IconPixmap"]).first().bytes;};
  tray.setActivity(Theme::trayTints(false).idle,"Recording",50);const auto progress=pixels();tray.setActivity(Theme::trayTints(false).idle,"Recording",50,1);const auto recording=pixels();QVERIFY(progress!=recording);
  tray.setActivity(Theme::trayTints(false).idle,"Recording paused",50,2);QVERIFY(recording!=pixels());const auto paused=pixels();tray.setActivity(Theme::trayTints(false).idle,"Recording paused",75,2);QVERIFY(paused!=pixels());
  tray.setActivity(Theme::trayTints(false).error,"Failed uploads\nRecording paused",-2,2);const auto failed=pixels();// Paused stays in the badge; failure tints the glyph.
  tray.setActivity(Theme::trayTints(false).error,"Failed uploads\nRecording paused",-2);QVERIFY(pixels()!=failed);QVERIFY(qdbus_cast<WireTooltip>(properties()["ToolTip"]).description.contains("Recording paused"));
  tray.setCommands({},"Uploading a file",1,true,"Recording paused");
  auto property=[&](int id,const QString& name){return qdbus_cast<QDBusVariant>(call("/Menu","com.canonical.dbusmenu","GetProperty",{id,name}).arguments().first()).variant();};
  tray.setCommands({},"Uploading a file",1,true,"Recording paused","Local save needs attention");QCOMPARE(property(22,"label").toString(),QString("Local save needs attention"));QVERIFY(property(16,"label").toString().contains("Recording paused"));QVERIFY(property(22,"visible").toBool());QVERIFY(property(19,"visible").toBool());
  const auto reply=call("/Menu","com.canonical.dbusmenu","GetLayout",{0,-1,QStringList{}});const auto layout=qdbus_cast<WireLayout>(reply.arguments().at(1));bool found=false;for(const auto& child:layout.children){const auto item=qdbus_cast<WireLayout>(child.variant());if(item.id==16){found=true;QVERIFY(item.props["label"].toString().contains("Recording paused"));}}QVERIFY(found);
 }
 void outputSummaryExportsPerActionDetailsAndRecovery(){
  NativeTray tray;PUTGD_TRY_VERIFY(tray.available());QSignalSpy actions(&tray,&NativeTray::actionActivated);QStringList details;for(const auto& action:Settings::presentationOrder())details<<action+": synthetic output";tray.setOutput("Mixed outputs",details);auto reply=call("/Menu","com.canonical.dbusmenu","GetLayout",{23,-1,QStringList{}});const auto output=qdbus_cast<WireLayout>(reply.arguments().at(1));QCOMPARE(output.props["label"].toString(),QString("Mixed outputs"));QCOMPARE(output.children.size(),6);for(int i=0;i<6;++i){const auto row=qdbus_cast<WireLayout>(output.children[i].variant());QCOMPARE(row.props["label"].toString(),details[i]);QVERIFY(!row.props["enabled"].toBool());click(row.id);}QVERIFY(actions.isEmpty());click(30);PUTGD_TRY_COMPARE(actions.size(),1);QCOMPARE(actions.first().first().toString(),QString("capture-settings"));
 }
 void coreOnlyBrandPropertiesAndActualLayoutWire() {
  NativeTray tray; QVERIFY(!tray.availabilityKnown()); PUTGD_TRY_VERIFY(tray.availabilityKnown()); PUTGD_TRY_VERIFY(tray.available()); QCOMPARE(watcher.registrations.size(), 1);
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
   // The idle tray tint on a dark panel (#D6D6D2), as ARGB bytes.
   int opaqueIdle = 0;
   for (int j = 0; j < images[i].bytes.size(); j += 4)
    if (uchar(images[i].bytes[j]) == 255 && uchar(images[i].bytes[j+1]) == 0xD6 && uchar(images[i].bytes[j+2]) == 0xD6 && uchar(images[i].bytes[j+3]) == 0xD2) ++opaqueIdle;
   QVERIFY(opaqueIdle > 10);
  }
  reply = call("/Menu", "com.canonical.dbusmenu", "GetLayout", {0, -1, QStringList{}});
  QCOMPARE(reply.signature(), QString("u(ia{sv}av)")); QCOMPARE(reply.arguments().at(0).toUInt(), uint(1));
  auto layout = qdbus_cast<WireLayout>(reply.arguments().at(1)); QCOMPARE(layout.id, 0); QCOMPARE(layout.children.size(), 21);for(int i=0;i<3;++i){QVERIFY(!qdbus_cast<WireLayout>(layout.children[i].variant()).props["visible"].toBool());}layout.children.erase(layout.children.begin(),layout.children.begin()+5);
  QCOMPARE(layout.props["children-display"].toString(), QString("submenu"));
  bool quitLabel=false;for(const auto& child:layout.children){const auto item=qdbus_cast<WireLayout>(child.variant());if(item.id==12){QCOMPARE(item.props["label"].toString(),QString("Quit put.gd"));quitLabel=true;}}QVERIFY(quitLabel);
  // Capture entries first: region, region & edit, window, desktop, screen recording, then the editor sources and the clipboard upload.
  const QStringList order{"Capture region", "Capture window", "Capture desktop", "Capture region & edit", "Edit image", "Record screen", "Upload clipboard"};
  for (int i = 0; i < order.size(); ++i) QCOMPARE(qdbus_cast<WireLayout>(layout.children.at(i).variant()).props["label"].toString(), order[i]);
  QCOMPARE(qdbus_cast<WireLayout>(layout.children.at(5).variant()).id, 16);
  for(int i=0;i<3;++i)QCOMPARE(qdbus_cast<WireLayout>(layout.children.at(9+i).variant()).props["label"].toString(),QStringList({"History","Destinations","Settings"})[i]);
  // Other editor sources sit in one submenu.
  const auto editImage = qdbus_cast<WireLayout>(layout.children.at(4).variant());
  QCOMPARE(editImage.id, 13); QCOMPARE(editImage.props["children-display"].toString(), QString("submenu"));
  QCOMPARE(editImage.children.size(), 2);
  QCOMPARE(qdbus_cast<WireLayout>(editImage.children.at(0).variant()).props["label"].toString(), QString("Edit clipboard image"));
  QCOMPARE(qdbus_cast<WireLayout>(editImage.children.at(1).variant()).props["label"].toString(), QString("Edit an image file…"));
  QCOMPARE(qdbus_cast<WireLayout>(layout.children.at(7).variant()).props["type"].toString(), QString("separator"));
  // Depth 1 lists the submenu header without expanding it.
  layout = qdbus_cast<WireLayout>(call("/Menu", "com.canonical.dbusmenu", "GetLayout", {0, 1, QStringList{}}).arguments().at(1));
  QVERIFY(qdbus_cast<WireLayout>(layout.children.at(9).variant()).children.isEmpty());
  reply = call("/Menu", "com.canonical.dbusmenu", "GetLayout", {0, 0, QStringList{"label"}});
  layout = qdbus_cast<WireLayout>(reply.arguments().at(1)); QVERIFY(layout.children.isEmpty()); QVERIFY(layout.props.isEmpty());
  QCOMPARE(call("/Menu", "com.canonical.dbusmenu", "AboutToShow", {0}).arguments().at(0).toBool(), false);
  QCOMPARE(call("/Menu", "com.canonical.dbusmenu", "GetLayout", {-1, -1, QStringList{}}).type(), QDBusMessage::ErrorMessage);
  QCOMPARE(call("/Menu", "com.canonical.dbusmenu", "GetLayout", {0, -2, QStringList{}}).type(), QDBusMessage::ErrorMessage);
  QCOMPARE(call("/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Set", {QString("org.kde.StatusNotifierItem"), QString("Status"), QVariant::fromValue(QDBusVariant("Passive"))}).errorName(), QString("org.freedesktop.DBus.Error.PropertyReadOnly"));
  QVERIFY(!QCoreApplication::instance()->inherits("QGuiApplication"));
 }
 void everyActionAndActivationUsesFixedIds() {
  NativeTray tray; PUTGD_TRY_VERIFY(tray.available()); QSignalSpy actions(&tray, &NativeTray::actionActivated);
  const QList<int> ids{1,2,3,4,5,7,8,11,12,14,15,18};
  const QStringList expected{"region","window","desktop","clipboard","edit","settings","history","about","quit","edit-clipboard","edit-file","destinations"};
  for (int i = 0; i < ids.size(); ++i) { QCOMPARE(click(ids[i]).type(), QDBusMessage::ReplyMessage); PUTGD_TRY_COMPARE(actions.size(), i+1); QCOMPARE(actions.at(i).at(0).toString(), expected[i]); }
  // Separators and the submenu header dispatch nothing.
  QCOMPARE(click(6).type(), QDBusMessage::ReplyMessage); QCOMPARE(click(10).type(), QDBusMessage::ReplyMessage); QCOMPARE(click(13).type(), QDBusMessage::ReplyMessage);
  QCOMPARE(click(1, "hovered").type(), QDBusMessage::ReplyMessage); QCOMPARE(click(99).type(), QDBusMessage::ErrorMessage);
  // 9 was "Recent activity": its ID stays retired rather than shifting the others.
  QCOMPARE(click(9).type(), QDBusMessage::ErrorMessage);
  QCOMPARE(call("/Menu", "com.canonical.dbusmenu", "GetProperty", {9, QString("label")}).type(), QDBusMessage::ErrorMessage);
  QCOMPARE(actions.size(), ids.size());
  // Left-click opens History.
  QCOMPARE(call("/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate", {0,0}).type(), QDBusMessage::ReplyMessage);
  PUTGD_TRY_COMPARE(actions.size(), ids.size()+1); QCOMPARE(actions.last().at(0).toString(), QString("history"));
  WireEvents events{{1,"clicked",QDBusVariant(0),0},{99,"clicked",QDBusVariant(0),0}};
  auto reply = call("/Menu", "com.canonical.dbusmenu", "EventGroup", {QVariant::fromValue(events)});
  QCOMPARE(reply.type(), QDBusMessage::ReplyMessage); QCOMPARE(qdbus_cast<QList<int>>(reply.arguments().at(0)), QList<int>{99});
  PUTGD_TRY_COMPARE(actions.size(), ids.size()+2); QCOMPARE(actions.last().at(0).toString(), QString("region"));
 }
 void shortcutsAndLiveUploadEntriesUseStableIds(){
  NativeTray tray;PUTGD_TRY_VERIFY(tray.available());QSignalSpy actions(&tray,&NativeTray::actionActivated);tray.setCommands({{"region","Ctrl+Shift+R"}},"Uploading 2 files…",2,true);auto reply=call("/Menu","com.canonical.dbusmenu","GetLayout",{0,-1,QStringList{}});auto root=qdbus_cast<WireLayout>(reply.arguments().at(1));auto status=qdbus_cast<WireLayout>(root.children[0].variant());QCOMPARE(status.id,19);QCOMPARE(status.props["label"].toString(),QString("Uploading 2 files…"));QVERIFY(status.props["visible"].toBool());QVERIFY(!status.props["enabled"].toBool());auto cancel=qdbus_cast<WireLayout>(root.children[1].variant());QCOMPARE(cancel.id,20);QVERIFY(cancel.props["enabled"].toBool());QCOMPARE(cancel.props["label"].toString(),QString("Cancel all uploads…"));QCOMPARE(call("/Menu","com.canonical.dbusmenu","GetProperty",{22,QString("visible")}).arguments().first().value<QDBusVariant>().variant().toBool(),true);auto region=qdbus_cast<WireLayout>(root.children[5].variant());QCOMPARE(region.id,1);QCOMPARE(qdbus_cast<QList<QStringList>>(region.props["shortcut"]),QList<QStringList>({{"Control","Shift","R"}}));click(19);PUTGD_VERIFY_STEADY(actions.isEmpty(),30);click(20);PUTGD_TRY_COMPARE(actions.size(),1);QCOMPARE(actions.first().first().toString(),QString("cancel-upload"));tray.setCommands({},{});click(20);PUTGD_COMPARE_STEADY(actions.size(),1,30);
 }
 void updateEntryShowsItsVersionAndBumpsTheLayout() {
  // #293: hidden until a stable update is available, then labelled with it.
  NativeTray tray; PUTGD_TRY_VERIFY(tray.available()); QSignalSpy actions(&tray, &NativeTray::actionActivated); LayoutSignalPeer observer;
  QVERIFY(client.connect(watcher.registrations.last(), "/Menu", "com.canonical.dbusmenu", "LayoutUpdated", &observer, SLOT(layoutUpdated(uint,int))));
  QVERIFY(client.connect(watcher.registrations.last(), "/Menu", "com.canonical.dbusmenu", "ItemsPropertiesUpdated", "a(ia{sv})a(ias)", &observer, SLOT(itemsUpdated(QDBusMessage))));
  auto entry = [&] {
   const auto reply = call("/Menu", "com.canonical.dbusmenu", "GetLayout", {0, -1, QStringList{}});
   const auto root = qdbus_cast<WireLayout>(reply.arguments().at(1));
   for(const auto& child:root.children){const auto entry=qdbus_cast<WireLayout>(child.variant());if(entry.id==17)return std::pair(reply.arguments().at(0).toUInt(),entry);}return std::pair(reply.arguments().at(0).toUInt(),WireLayout{});
  };
  auto [revision, update] = entry();
  QCOMPARE(revision, uint(1)); QCOMPARE(update.id, 17); QCOMPARE(update.props["visible"].toBool(), false);
  QCOMPARE(qdbus_cast<QDBusVariant>(call("/Menu", "com.canonical.dbusmenu", "GetProperty", {17, QString("visible")}).arguments().at(0)).variant().toBool(), false);
  tray.setUpdateLabel("Update available: 0.1.2"); PUTGD_TRY_COMPARE(observer.updates, 1); QCOMPARE(observer.revision, uint(2)); QCOMPARE(observer.parent, 0);
  std::tie(revision, update) = entry();
  // #304: visible is sent explicitly, in the layout and as a property change, because
  // Plasma's and GNOME's hosts apply only the properties they are sent to a known item.
  QCOMPARE(revision, uint(2)); QCOMPARE(update.props["label"].toString(), QString("Update available: 0.1.2")); QCOMPARE(update.props["visible"].toBool(), true);
  PUTGD_TRY_COMPARE(observer.itemUpdates, 1); QCOMPARE(observer.items.size(), 1); QCOMPARE(observer.items.at(0).first, 17);
  QCOMPARE(observer.items.at(0).second["visible"].toBool(), true); QCOMPARE(observer.items.at(0).second["label"].toString(), QString("Update available: 0.1.2"));
  QCOMPARE(qdbus_cast<QDBusVariant>(call("/Menu", "com.canonical.dbusmenu", "GetProperty", {17, QString("visible")}).arguments().at(0)).variant().toBool(), true);
  QCOMPARE(click(17).type(), QDBusMessage::ReplyMessage); PUTGD_TRY_COMPARE(actions.size(), 1); QCOMPARE(actions.at(0).at(0).toString(), QString("update"));
  tray.setUpdateLabel("Update available: 0.1.2"); putgd::test::settle(50); PUTGD_TRY_COMPARE(observer.updates, 1);
  tray.setUpdateLabel({}); PUTGD_TRY_COMPARE(observer.updates, 2); QCOMPARE(entry().second.props["visible"].toBool(), false);
  PUTGD_TRY_COMPARE(observer.itemUpdates, 2); QCOMPARE(observer.items.at(0).second["visible"].toBool(), false);
 }
 void lateHostAndWatcherRestartDoNotDuplicateRegistration() {
  watcher.hostPresent = false; NativeTray tray;
  PUTGD_TRY_COMPARE(watcher.registrations.size(), 1); PUTGD_TRY_VERIFY(tray.availabilityKnown()); QVERIFY(!tray.available());
  changeHost(true); PUTGD_TRY_VERIFY(tray.available());
  for (int i = 0; i < 3; ++i) emit watcher.StatusNotifierHostRegistered();
  putgd::test::settle(100); PUTGD_TRY_COMPARE(watcher.registrations.size(), 1);
  changeHost(false); PUTGD_TRY_VERIFY(!tray.available()); changeHost(true); PUTGD_TRY_VERIFY(tray.available()); QCOMPARE(watcher.registrations.size(), 1);
  QVERIFY(peer.unregisterService("org.kde.StatusNotifierWatcher")); PUTGD_TRY_VERIFY(!tray.available());
  QVERIFY(peer.registerService("org.kde.StatusNotifierWatcher")); PUTGD_TRY_VERIFY(tray.available()); QCOMPARE(watcher.registrations.size(), 2);
 }
 void initiallyAbsentWatcherResolvesThenRecoversOnAppearance() {
  QVERIFY(peer.unregisterService("org.kde.StatusNotifierWatcher"));
  NativeTray tray; QSignalSpy resolved(&tray, &NativeTray::availabilityResolved);
  QVERIFY(!tray.availabilityKnown()); PUTGD_TRY_VERIFY(tray.availabilityKnown());
  QVERIFY(!tray.available()); PUTGD_TRY_COMPARE(resolved.size(), 1); QVERIFY(watcher.registrations.isEmpty());
  QVERIFY(peer.registerService("org.kde.StatusNotifierWatcher")); PUTGD_TRY_VERIFY(tray.available());
  PUTGD_TRY_VERIFY(tray.availabilityKnown()); QCOMPARE(watcher.registrations.size(), 1);
 }
 void watcherNameBeforeObjectExportRecoversWithoutFalseAbsence() {
  peer.unregisterObject("/StatusNotifierWatcher");
  NativeTray tray; QSignalSpy resolved(&tray, &NativeTray::availabilityResolved);
  putgd::test::settle(100); PUTGD_TRY_VERIFY(!tray.availabilityKnown()); PUTGD_TRY_VERIFY(!tray.available());
  QVERIFY(peer.registerObject("/StatusNotifierWatcher", &watcher, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSignals));
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(tray.available(), 3000); QVERIFY(tray.availabilityKnown());
  PUTGD_TRY_COMPARE(resolved.size(), 1); QCOMPARE(watcher.registrations.size(), 1);
 }
 void registrationFailuresAreBoundedAndNewHostCanRecover() {
  watcher.failRegistration = true; NativeTray tray;
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(watcher.registrations.size(), 3, 4000); QVERIFY(!tray.available());
  PUTGD_TRY_VERIFY(tray.availabilityKnown());
  putgd::test::settle(1700); PUTGD_TRY_COMPARE(watcher.registrations.size(), 3);
  watcher.failRegistration = false; emit watcher.StatusNotifierHostRegistered();
  PUTGD_TRY_VERIFY(tray.available()); QCOMPARE(watcher.registrations.size(), 4);
 }
 void destructiveCallbackRemovesServiceAndExports() {
  auto tray = std::make_unique<NativeTray>(); PUTGD_TRY_VERIFY(tray->available());
  const auto service = watcher.registrations.last();
  QPointer<NativeTray> guard(tray.get()); bool invoked = false;
  connect(tray.get(), &NativeTray::actionActivated, this, [&](const QString&) { invoked = true; tray.reset(); });
  QCOMPARE(click(12).type(), QDBusMessage::ReplyMessage); PUTGD_TRY_VERIFY(invoked); QVERIFY(guard.isNull());
  QVERIFY(!client.interface()->isServiceRegistered(service).value());
  PUTGD_TRY_VERIFY(!watcher.activeServices.contains(service));
  QVERIFY(QDBusConnection::sessionBus().isConnected());
  QCOMPARE(call("/Menu", "com.canonical.dbusmenu", "AboutToShow", {0}).type(), QDBusMessage::ErrorMessage);
  NativeTray replacement; PUTGD_TRY_VERIFY(replacement.available()); QCOMPARE(watcher.registrations.size(), 2);
 }
 void notificationWireAndNoHostSuppression() {
  NativeTray tray; PUTGD_TRY_VERIFY(tray.available());
  tray.notify("put.gd — failed", "Synthetic failure", true); PUTGD_TRY_COMPARE(notifications.values.size(), 1);
  const auto value = notifications.values.first(); QCOMPARE(value["app"].toString(), QString("put.gd"));
  QCOMPARE(value["summary"].toString(), QString("put.gd — failed")); QCOMPARE(value["timeout"].toInt(), 10000);
  QCOMPARE(value["hints"].toMap()["urgency"].toUInt(), uint(2)); QVERIFY(value["actions"].toStringList().isEmpty());
  // #151: a notification with buttons stays until used or dismissed, except where the
  // server keeps it, buttons and all, in its list (GNOME Shell, Plasma).
  tray.notify("put.gd — partial", "Synthetic failure", true, {}, {{"save-diagnostics", "Save diagnostics for this upload", "op"}});
  PUTGD_TRY_COMPARE(notifications.values.size(), 2); QCOMPARE(notifications.values[1]["timeout"].toInt(), 0);
  // Resident, so Cinnamon keeps it when a put.gd window takes focus; put.gd closes it once used.
  QCOMPARE(notifications.values[1]["hints"].toMap().value("resident").toBool(), true);
  QVERIFY(!notifications.values.first()["hints"].toMap().contains("resident"));
  notifications.closed.clear(); emit notifications.ActionInvoked(2, "save-diagnostics"); PUTGD_TRY_COMPARE(notifications.closed, QList<uint>{2});
  emit notifications.ActionInvoked(2, "save-diagnostics"); putgd::test::settle(50); PUTGD_TRY_COMPARE(notifications.closed, QList<uint>{2});
  tray.notify("put.gd — completed", "Synthetic link", false, QUrl("https://example.invalid/a")); PUTGD_TRY_COMPARE(notifications.values.size(), 3);
  QCOMPARE(notifications.values[2]["timeout"].toInt(), 10000);
  for (const auto& [server, timeout] : std::initializer_list<std::pair<QString, int>>{{"Plasma", 10000}, {"gnome-shell", 10000}, {"cinnamon", 0}}) {
   // A new notification server owner is asked its name again.
   notifications.server = server; const int queries = notifications.serverQueries;
   QVERIFY(peer.unregisterService("org.freedesktop.Notifications")); QVERIFY(peer.registerService("org.freedesktop.Notifications"));
   PUTGD_TRY_VERIFY(notifications.serverQueries > queries); putgd::test::settle(50);
   const auto sent = notifications.values.size();
   tray.notify("put.gd — diagnostics saved", "Synthetic package", false, {}, {{"show-in-folder", "Show in folder", "/synthetic.zip"}});
   PUTGD_TRY_COMPARE(notifications.values.size(), sent + 1); QCOMPARE(notifications.values.last()["timeout"].toInt(), timeout);
   QCOMPARE(notifications.values.last()["hints"].toMap().contains("resident"), timeout == 0);
  }
  notifications.server.clear();
  QCOMPARE(NativeTray::notificationTimeoutMs("cinnamon", false), 10000); QCOMPARE(NativeTray::notificationTimeoutMs({}, true), 0);
  changeHost(false); PUTGD_TRY_VERIFY(!tray.available()); tray.notify("Hidden", "Unavailable"); putgd::test::settle(50); PUTGD_TRY_COMPARE(notifications.values.size(), 3 + 3);
 }
 void firstNotificationOwnerAppearancePreservesClickTarget() {
  QVERIFY(peer.unregisterService("org.freedesktop.Notifications"));
  NativeTray tray;PUTGD_TRY_VERIFY(tray.available());QSignalSpy activated(&tray,&NativeTray::notificationActivated);
  QVERIFY(peer.registerService("org.freedesktop.Notifications"));
  const QUrl link("https://example.invalid/first-image.png");
  tray.notify("put.gd — completed","Click to open in browser",false,link);
  PUTGD_TRY_COMPARE(notifications.values.size(),1);putgd::test::settle(50);
  emit notifications.ActionInvoked(1,"default");
  PUTGD_TRY_COMPARE(activated.count(),1);QCOMPARE(activated.first().first().toUrl(),link);
 }
 void uploadToastOpensItsOwnLink() {
  QTemporaryDir directory; QVERIFY(directory.isValid());
  const auto output=directory.filePath("opened-links");
  QFile opener(directory.filePath("gio")); QVERIFY(opener.open(QIODevice::WriteOnly));
  opener.write(("#!/bin/sh\n[ \"$#\" -eq 2 ] && [ \"$1\" = open ] || exit 1\nprintf '%s\\n' \"$2\" >> '"+output+"'\n").toUtf8()); opener.close();
  QVERIFY(opener.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  QFile genericOpener(directory.filePath("xdg-open"));QVERIFY(genericOpener.open(QIODevice::WriteOnly));
  genericOpener.write("#!/bin/sh\nexit 1\n");genericOpener.close();
  QVERIFY(genericOpener.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  const auto previousPath=qgetenv("PATH");auto restorePath=qScopeGuard([&]{qputenv("PATH",previousPath);});
  qputenv("PATH",directory.path().toUtf8()+":"+previousPath);
  auto opened=[&]{QFile file(output);if(!file.open(QIODevice::ReadOnly))return QStringList{};return QString::fromUtf8(file.readAll()).split('\n',Qt::SkipEmptyParts);};
  ProviderRegistry providers; auto provider=std::make_shared<OpaqueProvider>();providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});
  auto config=configuration(directory.path());auto naming=config["naming"].toObject();naming["mode"]="generated";config["naming"]=naming;QVERIFY(settings.save(config,{}));
  Source source;source.payload=textPayload();OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){QFAIL("Clicking a single upload must open its link, not a view");},[]{});PUTGD_TRY_VERIFY(controller.available());
  QStringList links;
  for(int index=0;index<3;++index) {
   operations.invoke("clipboard",[](Error error,QJsonObject){QVERIFY(!error);});
   PUTGD_TRY_COMPARE(source.copies,index+1);PUTGD_TRY_COMPARE(notifications.values.size(),index+1);links.append(source.copied.text);
   QCOMPARE(notifications.values[index]["actions"].toStringList(),QStringList({"default","Open in browser"}));
  }
  QVERIFY(links[0]!=links[1]);QVERIFY(opened().isEmpty());
  emit notifications.ActionInvoked(999,"default");emit notifications.ActionInvoked(1,"unrecognized");putgd::test::settle(50);PUTGD_TRY_VERIFY(opened().isEmpty());
  emit notifications.ActionInvoked(1,"default");PUTGD_TRY_COMPARE(opened(),QStringList{links[0]});
  emit notifications.ActionInvoked(2,"default");PUTGD_TRY_COMPARE(opened(),links.mid(0,2));
  emit notifications.NotificationClosed(3,2);emit notifications.ActionInvoked(3,"default");putgd::test::settle(50);PUTGD_TRY_COMPARE(opened(),links.mid(0,2));
  PUTGD_TRY_VERIFY(!QCoreApplication::instance()->inherits("QGuiApplication"));
 }
 void failedUploadOffersDiagnosticsAndShowInFolder() {
  // #122: the failure toast saves a package for that operation; the saved toast shows it in the file manager.
  QTemporaryDir directory;QVERIFY(directory.isValid());const auto output=directory.filePath("opened-folders");
  QFile opener(directory.filePath("gio"));QVERIFY(opener.open(QIODevice::WriteOnly));
  opener.write(("#!/bin/sh\n[ \"$#\" -eq 2 ] && [ \"$1\" = open ] || exit 1\nprintf '%s\\n' \"$2\" >> '"+output+"'\n").toUtf8());opener.close();
  QVERIFY(opener.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  const auto previousPath=qgetenv("PATH");auto restorePath=qScopeGuard([&]{qputenv("PATH",previousPath);});qputenv("PATH",directory.path().toUtf8()+":"+previousPath);
  auto opened=[&]{QFile file(output);if(!file.open(QIODevice::ReadOnly))return QStringList{};return QString::fromUtf8(file.readAll()).split('\n',Qt::SkipEmptyParts);};
  ProviderRegistry providers;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;providers.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.payload=textPayload();OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});PUTGD_TRY_VERIFY(controller.available());
  const auto package=directory.filePath("putgd-diagnostics-20261020-010203Z.zip");{QFile file(package);QVERIFY(file.open(QIODevice::WriteOnly));file.write("PK");}
  QStringList requested;controller.setDiagnosticsSaver([&](const QString& id,JsonReply reply){requested.append(id);reply({},{{"path",package},{"bytes",2048}});});
  QString id;operations.invoke("clipboard",[&](Error error,QJsonObject value){QVERIFY(!error);id=value["operationId"].toString();});
  PUTGD_TRY_COMPARE(notifications.values.size(),1);QVERIFY2(notifications.values[0]["summary"].toString()=="Completed with a problem",qPrintable(notifications.values[0]["summary"].toString()+" / "+notifications.values[0]["body"].toString()));
  QCOMPARE(notifications.values[0]["actions"].toStringList(),QStringList({"save-diagnostics","Save diagnostics for this upload"}));
  // #152: the body names the failure, not a clipboard code; #151: it stays until used.
  QVERIFY2(notifications.values[0]["body"].toString().startsWith("Upload failed: "),qPrintable(notifications.values[0]["body"].toString()));
  QVERIFY(!notifications.values[0]["body"].toString().contains("Clipboard:"));QCOMPARE(notifications.values[0]["timeout"].toInt(),0);
  // A click on the body uses the resident notification up too: put.gd closes it.
  notifications.closed.clear();emit notifications.ActionInvoked(1,"default");putgd::test::settle(50);PUTGD_TRY_VERIFY(requested.isEmpty());PUTGD_TRY_COMPARE(notifications.closed,QList<uint>{1});
  emit notifications.ActionInvoked(1,"save-diagnostics");PUTGD_TRY_COMPARE(requested,QStringList{id});QCOMPARE(notifications.closed,QList<uint>{1});
  PUTGD_TRY_COMPARE(notifications.values.size(),2);QCOMPARE(notifications.values[1]["summary"].toString(),QString("put.gd — diagnostics saved"));
  QVERIFY(notifications.values[1]["body"].toString().contains("putgd-diagnostics-20261020-010203Z.zip"));QVERIFY(notifications.values[1]["body"].toString().contains("Nothing was uploaded"));
  QCOMPARE(notifications.values[1]["actions"].toStringList(),QStringList({"show-in-folder","Show in folder"}));
  // The action is spent once used.
  emit notifications.ActionInvoked(1,"save-diagnostics");putgd::test::settle(50);PUTGD_TRY_COMPARE(requested.size(),1);
  // Without a file manager on the bus, Show in folder opens the package's folder.
  FileManagerPeer files;files.refuse=true;QVERIFY(peer.registerObject("/org/freedesktop/FileManager1",&files,QDBusConnection::ExportAllSlots));QVERIFY(peer.registerService("org.freedesktop.FileManager1"));
  auto unregister=qScopeGuard([&]{peer.unregisterService("org.freedesktop.FileManager1");peer.unregisterObject("/org/freedesktop/FileManager1");});
  emit notifications.ActionInvoked(2,"show-in-folder");PUTGD_TRY_COMPARE(opened(),QStringList{QUrl::fromLocalFile(directory.path()).toString(QUrl::FullyEncoded)});
  // With one, the package is selected.
  files.refuse=false;
  QVERIFY(!TrayController::revealPackage(package));PUTGD_TRY_COMPARE(files.shown.size(),1);QCOMPARE(files.shown[0],QStringList{QUrl::fromLocalFile(package).toString(QUrl::FullyEncoded)});putgd::test::settle(50);PUTGD_TRY_COMPARE(opened().size(),1);
  QCOMPARE(TrayController::revealPackage(directory.filePath("other.zip")).code,QString("invalid_request"));
  QCOMPARE(TrayController::revealPackage("relative/putgd-diagnostics-20261020-010203Z.zip").code,QString("invalid_request"));
  QCOMPARE(TrayController::revealPackage(directory.filePath("putgd-diagnostics-20261020-010204Z.zip")).code,QString("diagnostics_package_missing"));
  // A failed save says so instead.
  controller.setDiagnosticsSaver([&](const QString&,JsonReply reply){reply({"diagnostics_output_unavailable","Synthetic folder failure",{}},{});});
  controller.saveDiagnostics(id);PUTGD_TRY_COMPARE(notifications.values.size(),3);QCOMPARE(notifications.values[2]["body"].toString(),QString("Synthetic folder failure"));QVERIFY(notifications.values[2]["actions"].toStringList().isEmpty());
  // A package the dialog also uploaded (#158) says where it went, or why it didn't.
  controller.notifyDiagnosticsSaved({{"path",package},{"bytes",2048},{"link","https://example.invalid/p.zip"},{"linkCopied",true},{"upload",QJsonObject{{"label","Synthetic S3"}}}});
  PUTGD_TRY_COMPARE(notifications.values.size(),4);QVERIFY2(notifications.values[3]["body"].toString().endsWith(". Uploaded to Synthetic S3; link copied."),qPrintable(notifications.values[3]["body"].toString()));
  controller.notifyDiagnosticsSaved({{"path",package},{"bytes",2048},{"uploadError",QJsonObject{{"code","s3_permission_denied"},{"message","S3 denied the request (HTTP 403)."}}}});
  PUTGD_TRY_COMPARE(notifications.values.size(),5);QVERIFY2(notifications.values[4]["body"].toString().endsWith(". Not uploaded: access denied by the destination."),qPrintable(notifications.values[4]["body"].toString()));
  // #223: a package saved under a name the caller chose opens from its button too;
  // diagnostics.reveal keeps the name check for paths this resident did not save.
  QVERIFY(QDir().mkpath(directory.filePath("out")));const auto custom=directory.filePath("out/putgd-diagnostics-r9.zip");{QFile file(custom);QVERIFY(file.open(QIODevice::WriteOnly));file.write("PK");}
  QCOMPARE(TrayController::revealPackage(custom).code,QString("invalid_request"));
  controller.notifyDiagnosticsSaved({{"path",custom},{"bytes",2048}});PUTGD_TRY_COMPARE(notifications.values.size(),6);
  QCOMPARE(notifications.values[5]["actions"].toStringList(),QStringList({"show-in-folder","Show in folder"}));
  emit notifications.ActionInvoked(6,"show-in-folder");PUTGD_TRY_COMPARE(files.shown.size(),2);QCOMPARE(files.shown[1],QStringList{QUrl::fromLocalFile(custom).toString(QUrl::FullyEncoded)});
  QVERIFY(!TrayController::revealPackage(custom,true));PUTGD_TRY_COMPARE(files.shown.size(),3);
  // A package removed before the click says so instead of doing nothing.
  controller.notifyDiagnosticsSaved({{"path",custom},{"bytes",2048}});PUTGD_TRY_COMPARE(notifications.values.size(),7);QVERIFY(QFile::remove(custom));
  emit notifications.ActionInvoked(7,"show-in-folder");PUTGD_TRY_COMPARE(notifications.values.size(),8);
  QCOMPARE(notifications.values[7]["summary"].toString(),QString("put.gd — Show in folder"));QCOMPARE(notifications.values[7]["body"].toString(),QString("The diagnostics package is no longer there"));
  putgd::test::settle(50);PUTGD_TRY_COMPARE(files.shown.size(),3);PUTGD_TRY_COMPARE(opened().size(),1);
 }
 void selfStoppedRecordingAndKeptUploadsAreAnnounced() {
  // #225 and #226: what ended a recording, and uploads cleanup left behind, reach the user.
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));Source source;
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());
  TrayController controller(operations,[](const QString&){},[]{});PUTGD_TRY_VERIFY(controller.available());const auto before=notifications.values.size();
  emit operations.recording().budgetStopped("synthetic",{{"reason","disk_space"},{"message","Recording stopped at 0:03: the disk ran out of room."}});
  PUTGD_TRY_COMPARE(notifications.values.size(),before+1);QCOMPARE(notifications.values.last()["summary"].toString(),QString("put.gd — recording stopped"));
  QCOMPARE(notifications.values.last()["body"].toString(),QString("Recording stopped at 0:03: the disk ran out of room."));
  emit operations.incompleteUploadsKept({{"count",2},{"message","Automatic cleanup could not cancel 2 incomplete uploads in bucket synthetic."}});
  PUTGD_TRY_COMPARE(notifications.values.size(),before+2);QCOMPARE(notifications.values.last()["summary"].toString(),QString("put.gd — incomplete uploads left"));
  QCOMPARE(notifications.values.last()["body"].toString(),QString("Automatic cleanup could not cancel 2 incomplete uploads in bucket synthetic."));
 }
 void coreControllerRoutesAllFourActionsWithoutViews() {
  QTemporaryDir directory; QVERIFY(directory.isValid()); ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json")); QVERIFY(settings.save(configuration(directory.path(), true), {}));
  Source source; source.defer = true;
  OperationService operations(directory.filePath("data"), settings, providers, source); PUTGD_TRY_VERIFY(operations.ready());
  QStringList views; QSet<QString> cancelled;
  TrayController controller(operations, [&](const QString& view) { views.append(view); }, [] {});
  PUTGD_TRY_VERIFY(controller.available());
  connect(&operations, &OperationService::changed, this, [&](const QString& id) {
   operations.inspect(id, [&](Error e, QJsonObject op) { QVERIFY(!e); if (op["state"] == "cancelled") cancelled.insert(op["action"].toString()); });
  });
  const QStringList names{"region","window","desktop","clipboard"};
  for (int i = 0; i < names.size(); ++i) {
   QCOMPARE(click(i+1).type(), QDBusMessage::ReplyMessage); PUTGD_TRY_COMPARE(source.reads, i+1);
   PUTGD_TRY_COMPARE(source.acquisitions.size(), 1);
   auto acquired = source.acquisitions.takeFirst(); acquired({{}, {"source_cancelled", "Synthetic source cancellation", {}}});
   PUTGD_TRY_VERIFY(cancelled.contains(names[i]));
  }
  QCOMPARE(cancelled.size(), 4); QCOMPARE(source.copies, 0); QVERIFY(views.isEmpty());
 }
 void progressChangesDoNotRepeatTerminalToastAndExplicitCopyStillNotifies(){
  QTemporaryDir directory;ProviderRegistry providers;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));Source source;source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,providers,source);PUTGD_TRY_VERIFY(operations.ready());QStringList views;TrayController controller(operations,[&](const QString& value){views.append(value);},[]{});PUTGD_TRY_VERIFY(controller.available());QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);id=result["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);PUTGD_TRY_VERIFY(!notifications.values.isEmpty());
  for(int i=0;i<50;++i)emit operations.changed(id);
  putgd::test::settle(400);PUTGD_TRY_COMPARE(notifications.values.size(),1);PUTGD_TRY_VERIFY(views.isEmpty());PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);bool copied=false;operations.copyResult(id,[&](Error e,QJsonObject){QVERIFY(!e);copied=true;});PUTGD_TRY_VERIFY(copied);PUTGD_TRY_COMPARE(notifications.values.size(),2);QCOMPARE(source.copies,2);
 }
 void coreControllerDelegatesViewsAndPreservesCopyPendingAndNotificationSetting() {
  QTemporaryDir directory; QVERIFY(directory.isValid()); ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json")); auto config = configuration(directory.path(), true);
  QVERIFY(settings.save(config, {})); Source source; source.payload = textPayload();
  OperationService operations(directory.filePath("data"), settings, providers, source); PUTGD_TRY_VERIFY(operations.ready());
  QStringList views; int quits = 0;
  TrayController controller(operations, [&](const QString& view) { views.append(view); }, [&] { ++quits; });
  auto* native = controller.findChild<NativeTray*>(); QVERIFY(native); PUTGD_TRY_VERIFY(native->available());
  for (int id : {7,8,11}) QCOMPARE(click(id).type(), QDBusMessage::ReplyMessage);
  PUTGD_TRY_COMPARE(views, QStringList({"settings","history","about"}));
  QCOMPARE(click(4).type(), QDBusMessage::ReplyMessage); PUTGD_TRY_COMPARE(source.copies, 1); PUTGD_TRY_COMPARE(notifications.values.size(), 1);
  QCOMPARE(notifications.values.first()["summary"].toString(),QString("Saved locally"));
  QVERIFY(notifications.values.first()["body"].toString().contains("copied"));
  putgd::test::settle(100); PUTGD_TRY_COMPARE(notifications.values.size(), 1);
  auto updated = settings.document();
  auto preferences = updated["preferences"].toObject(); preferences["notifications"] = false; updated["preferences"] = preferences;
  const auto saved = settings.save(updated, settings.revision());
  QVERIFY2(saved, qPrintable(saved.error.code)); QVERIFY(!settings.snapshot().notifications);
  QCOMPARE(click(4).type(), QDBusMessage::ReplyMessage); PUTGD_TRY_COMPARE(source.copies, 2);
  putgd::test::settle(100); PUTGD_TRY_COMPARE(notifications.values.size(), 1);
  QCOMPARE(click(12).type(), QDBusMessage::ReplyMessage); PUTGD_TRY_COMPARE(quits, 1);
  QVERIFY(!QCoreApplication::instance()->inherits("QGuiApplication"));
 }
};
QTEST_GUILESS_MAIN(NativeTrayWire)
#include "test_native_tray.moc"
