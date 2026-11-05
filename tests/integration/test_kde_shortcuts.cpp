#include "platform/kde/KdeShortcuts.h"
#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest>
#include <memory>
#include "fixtures/Waits.h"
using namespace putgd;

// These synthetic peers use the real D-Bus transport on dbus-run-session's
// private bus. They prove registration/layout lifetime, never KWin acceptance.
class KeyboardLayoutPeer final : public QObject, protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface", "org.kde.KeyboardLayouts")
public:
 explicit KeyboardLayoutPeer(const QDBusConnection& peer) : peer_(peer) {}
 uint layout = 0;
 int requests = 0;
 bool holdNext = false;
 bool failQueries = false;
 QList<QDBusMessage> held;
 void replyHeld(uint oldLayout) {
  const auto request = held.takeFirst();
  peer_.send(request.createReply(QVariantList{QVariant::fromValue(oldLayout)}));
 }
public slots:
 uint getLayout() {
  ++requests;
  if (failQueries) { sendErrorReply(QDBusError::Failed, "Synthetic layout query failure"); return 0; }
  if (holdNext) { holdNext = false; setDelayedReply(true); held.append(message()); }
  return layout;
 }
signals:
 void layoutChanged(uint index);
 void layoutListChanged();
private:
 QDBusConnection peer_;
};

class GlobalAccelPeer final : public QObject, protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface", "org.kde.KGlobalAccel")
public:
 explicit GlobalAccelPeer(const QDBusConnection& peer) : peer_(peer) {}
 QHash<QString, QList<KdeKeySequence>> active;
 QList<QStringList> inactive;
 QList<QStringList> registered;
 QList<KdeKeySequence> forcedActual;
 bool forceActual = false;
 int setters = 0, componentRequests = 0;
 bool holdComponents = false;
 QList<QDBusMessage> heldComponents;
 bool replyHeldComponent() {
  const auto request = heldComponents.takeFirst();
  return peer_.send(request.createReply(QVariantList{
   QVariant::fromValue(QDBusObjectPath("/component/putgd"))}));
 }
public slots:
 void doRegister(const QStringList& id) { registered.append(id); }
 void setInactive(const QStringList& id) { inactive.append(id); active.remove(id.value(1)); }
 QList<putgd::KdeKeySequence> setShortcutKeys(const QStringList& id,
                                           const QList<putgd::KdeKeySequence>& desired,
                                           uint flags) {
  ++setters;
  if (flags != 6) return {};
  const auto actual = forceActual ? forcedActual : desired;
  active[id.value(1)] = actual;
  return actual;
 }
 QDBusObjectPath getComponent(const QString& component) {
  ++componentRequests;
  if (holdComponents) { setDelayedReply(true); heldComponents.append(message()); }
  return QDBusObjectPath(component == "putgd" ? "/component/putgd" : "/component/unknown");
 }
signals:
 void yourShortcutsChanged(const QStringList& id, const QList<putgd::KdeKeySequence>& keys);
private:
 QDBusConnection peer_;
};

class ShortcutComponentPeer final : public QObject {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface", "org.kde.kglobalaccel.Component")
signals:
 void globalShortcutPressed(const QString& component, const QString& action, qlonglong timestamp);
};

class KdeShortcutsTransportTest final : public QObject {
 Q_OBJECT
 QDBusConnection peer_ = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                      "kde-shortcuts-layout-peer");
 KeyboardLayoutPeer keyboard_{peer_};
 GlobalAccelPeer accel_{peer_};
 ShortcutComponentPeer component_;
 xkb_context* context_ = nullptr;
 using Keymap = std::unique_ptr<xkb_keymap, decltype(&xkb_keymap_unref)>;
 Keymap keymap(const char* layouts) {
  xkb_rule_names names{}; names.layout = layouts;
  return Keymap(xkb_keymap_new_from_names(context_, &names, XKB_KEYMAP_COMPILE_NO_FLAGS),
                &xkb_keymap_unref);
 }
 static QList<KdeKeySequence> keys(int combined) { return {{QList<int>{combined, 0, 0, 0}}}; }
 static QJsonObject region(const KdeShortcuts& source) { return source.status()["region"].toObject(); }
 bool roundTrip(bool allowError = false) {
  auto message = QDBusMessage::createMethodCall("org.kde.keyboard", "/Layouts",
                                               "org.kde.KeyboardLayouts", "getLayout");
  QDBusPendingCallWatcher reply(QDBusConnection::sessionBus().asyncCall(message, 2000));
  QSignalSpy completed(&reply, &QDBusPendingCallWatcher::finished);
  if (!reply.isFinished() && !completed.wait(2500)) return false;
  return reply.reply().type() == QDBusMessage::ReplyMessage ||
         (allowError && reply.reply().type() == QDBusMessage::ErrorMessage);
 }
private slots:
 void initTestCase() {
  QVERIFY(!QCoreApplication::instance()->inherits("QGuiApplication"));
  QVERIFY(QDBusConnection::sessionBus().isConnected()); QVERIFY(peer_.isConnected());
  QVERIFY(peer_.baseService() != QDBusConnection::sessionBus().baseService());
  qDBusRegisterMetaType<KdeKeySequence>(); qDBusRegisterMetaType<QList<KdeKeySequence>>();
  constexpr auto exports = QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals;
  QVERIFY(peer_.registerObject("/Layouts", &keyboard_, exports));
  QVERIFY(peer_.registerObject("/kglobalaccel", &accel_, exports));
  QVERIFY(peer_.registerObject("/component/putgd", &component_, exports));
  context_ = xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES); QVERIFY(context_);
  xkb_context_set_log_level(context_, XKB_LOG_LEVEL_CRITICAL);
 }
 void init() {
  keyboard_.layout = 0; keyboard_.requests = 0; keyboard_.holdNext = false; keyboard_.failQueries = false;
  QVERIFY(keyboard_.held.isEmpty());
  accel_.active.clear(); accel_.inactive.clear(); accel_.registered.clear();
  accel_.forcedActual.clear(); accel_.forceActual = false;
  accel_.setters = accel_.componentRequests = 0; accel_.holdComponents = false;
  QVERIFY(accel_.heldComponents.isEmpty());
  QVERIFY(peer_.registerService("org.kde.keyboard"));
  QVERIFY(peer_.registerService("org.kde.kglobalaccel"));
 }
 void cleanup() {
  // Every source was destroyed before cleanup; drain its inactive calls first.
  keyboard_.failQueries = false;
  QVERIFY(roundTrip());
  QVERIFY(peer_.unregisterService("org.kde.keyboard"));
  QVERIFY(peer_.unregisterService("org.kde.kglobalaccel"));
  keyboard_.held.clear(); accel_.heldComponents.clear();
 }
 void cleanupTestCase() {
  peer_.unregisterObject("/Layouts"); peer_.unregisterObject("/kglobalaccel");
  peer_.unregisterObject("/component/putgd");
  QDBusConnection::disconnectFromBus("kde-shortcuts-layout-peer");
  xkb_context_unref(context_);
 }
 void soleCompositorGroupBindsWithoutLayoutService() {
  // A single French compositor group is known, not a guessed US fallback.
  auto map = keymap("fr"); QVERIFY(map);
  QCOMPARE(xkb_keymap_num_layouts(map.get()), xkb_layout_index_t(1));
  QVERIFY(peer_.unregisterService("org.kde.keyboard"));
  struct Restore { QDBusConnection bus; ~Restore() { bus.registerService("org.kde.keyboard"); } } restore{peer_};
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", "Ctrl+Shift+1"}});
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  QVERIFY(source.available());
  QCOMPARE(region(source)["layout"].toInt(-1), 0);
  QCOMPARE(region(source)["requested"].toString(), QString("Ctrl+Shift+1"));
  QCOMPARE(region(source)["effective"].toString(), QString("Ctrl+1"));
  QCOMPARE(accel_.active.value("region").first().keys,
           keys(int(Qt::ControlModifier) | int(Qt::Key_1)).first().keys);
  QCOMPARE(keyboard_.requests, 0);
 }
 void shiftedDefaultUsesNativeWireAndRetainsRequestedChord() {
  auto map = keymap("us"); QVERIFY(map);
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", "Ctrl+Shift+1"}});
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  QVERIFY(source.available());
  QCOMPARE(region(source)["requested"].toString(), QString("Ctrl+Shift+1"));
  QCOMPARE(region(source)["effective"].toString(), QString("Ctrl+!"));
  QCOMPARE(region(source)["actual"].toArray(), (QJsonArray{"Ctrl+!"}));
  QCOMPARE(accel_.active.value("region").size(), 1);
  QCOMPARE(accel_.active.value("region").first().keys,
           keys(int(Qt::ControlModifier) | int(Qt::Key_Exclam)).first().keys);
  QCOMPARE(accel_.registered.last(), (QStringList{"putgd", "region", "put.gd", "region"}));
 }
 void digitEditReplacesActiveKeysAndRoutesOnlyMatchingAction() {
  auto map = keymap("us"); QVERIFY(map);
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", "Ctrl+Shift+1"}});
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  const auto old = accel_.active.value("region").first().keys;
  source.bind({{"region", "Ctrl+1"}});
  PUTGD_TRY_COMPARE(region(source)["actual"].toArray(), (QJsonArray{"Ctrl+1"}));
  QVERIFY(region(source)["bound"].toBool());
  QCOMPARE(region(source)["requested"].toString(), QString("Ctrl+1"));
  QCOMPARE(accel_.active.size(), 1);
  QCOMPARE(accel_.active.value("region").first().keys,
           keys(int(Qt::ControlModifier) | int(Qt::Key_1)).first().keys);
  QVERIFY(accel_.active.value("region").first().keys != old);
  QVERIFY(accel_.inactive.contains({"putgd", "region", "put.gd", "region"}));
  PUTGD_TRY_VERIFY(accel_.componentRequests > 0); QVERIFY(roundTrip());
  QSignalSpy activated(&source, &KdeShortcuts::activated);
  emit component_.globalShortcutPressed("foreign", "region", 1);
  emit component_.globalShortcutPressed("putgd", "unknown", 2);
  QVERIFY(roundTrip()); QCOMPARE(activated.size(), 0);
  emit component_.globalShortcutPressed("putgd", "region", 3);
  PUTGD_TRY_COMPARE(activated.size(), 1);
  QCOMPARE(activated.first().first().toString(), QString("region"));
 }
 void normalizationFailureReportsItsCodeAndValidRebindRecovers() {
  auto map = keymap("us"); QVERIFY(map);
  const auto unsupported = QString("Ctrl+1, Ctrl+2");
  QCOMPARE(QKeySequence::fromString(unsupported, QKeySequence::PortableText).count(), 2);
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", unsupported}});
  PUTGD_TRY_COMPARE(region(source)["state"].toString(), QString("shortcut_unsupported"));
  QCOMPARE(region(source)["error"].toString(), QString("shortcut_unsupported"));
  QCOMPARE(region(source)["requested"].toString(), unsupported);
  QVERIFY(!region(source)["bound"].toBool());
  QVERIFY(accel_.active.isEmpty());
  source.bind({{"region", "Ctrl+1"}});
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  QCOMPARE(region(source)["requested"].toString(), QString("Ctrl+1"));
  QCOMPARE(region(source)["effective"].toString(), QString("Ctrl+1"));
  QCOMPARE(region(source)["actual"].toArray(), (QJsonArray{"Ctrl+1"}));
  QCOMPARE(accel_.active.value("region").first().keys,
           keys(int(Qt::ControlModifier) | int(Qt::Key_1)).first().keys);
 }
 void conflictingActualKeysCannotClaimBoundOrActivate() {
  auto map = keymap("us"); QVERIFY(map);
  accel_.forceActual = true; accel_.forcedActual = keys(int(Qt::AltModifier) | int(Qt::Key_F12));
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", "Ctrl+1"}});
  PUTGD_TRY_COMPARE(region(source)["state"].toString(), QString("shortcut_conflict"));
  QVERIFY(!region(source)["bound"].toBool());
  QCOMPARE(region(source)["actual"].toArray(), (QJsonArray{"Alt+F12"}));
  QSignalSpy activated(&source, &KdeShortcuts::activated);
  PUTGD_TRY_VERIFY(accel_.componentRequests > 0); QVERIFY(roundTrip());
  emit component_.globalShortcutPressed("putgd", "region", 1);
  QVERIFY(roundTrip()); QCOMPARE(activated.size(), 0);
  accel_.forceActual = false; source.bind({{"region", "Ctrl+1"}});
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  emit accel_.yourShortcutsChanged({"putgd", "region", "put.gd", "region"},
                                 keys(int(Qt::AltModifier) | int(Qt::Key_F12)));
  PUTGD_TRY_COMPARE(region(source)["state"].toString(), QString("shortcut_conflict"));
  QVERIFY(!region(source)["bound"].toBool());
  emit component_.globalShortcutPressed("putgd", "region", 2);
  QVERIFY(roundTrip()); QCOMPARE(activated.size(), 0);
 }
 void unavailableMapAndInvalidGroupClearActiveBindings() {
  KdeShortcuts source; source.bind({{"region", "Ctrl+Shift+1"}});
  QCOMPARE(region(source)["state"].toString(), QString("shortcut_unavailable"));
  QVERIFY(!source.available()); QCOMPARE(accel_.setters, 0);
  auto map = keymap("us,fr"); QVERIFY(map); source.setKeyboardMap(map.get());
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  source.setKeyboardMap(nullptr);
  QVERIFY(!source.available()); QVERIFY(!region(source)["bound"].toBool());
  PUTGD_TRY_VERIFY(accel_.active.isEmpty());
  source.setKeyboardMap(map.get()); PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  keyboard_.layout = 2; emit keyboard_.layoutChanged(2);
  PUTGD_TRY_COMPARE(region(source)["state"].toString(), QString("shortcut_unavailable"));
  PUTGD_TRY_VERIFY(!source.available()); PUTGD_TRY_VERIFY(accel_.active.isEmpty());
  QVERIFY(roundTrip()); QVERIFY(!region(source)["bound"].toBool());
 }
 void layoutOwnerDisappearanceClosesBindingsAndRecoveryRebinds() {
  auto map = keymap("us,fr"); QVERIFY(map);
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", "Ctrl+Shift+1"}});
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  QVERIFY(peer_.unregisterService("org.kde.keyboard"));
  PUTGD_TRY_VERIFY(!source.available());
  PUTGD_TRY_COMPARE(region(source)["state"].toString(), QString("shortcut_unavailable"));
  PUTGD_TRY_VERIFY(accel_.active.isEmpty());
  QVERIFY(peer_.registerService("org.kde.keyboard"));
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  QCOMPARE(region(source)["actual"].toArray(), (QJsonArray{"Ctrl+!"}));
 }
 void layoutSignalsRecomputeWithoutApplicationFocus() {
  auto map = keymap("us,fr"); QVERIFY(map);
  QCOMPARE(xkb_keymap_num_layouts(map.get()), xkb_layout_index_t(2));
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", "Ctrl+Shift+1"}});
  PUTGD_TRY_COMPARE(region(source)["actual"].toArray(), (QJsonArray{"Ctrl+!"}));
  keyboard_.layout = 1; emit keyboard_.layoutChanged(1);
  PUTGD_TRY_COMPARE(region(source)["layout"].toInt(-1), 1);
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  QCOMPARE(region(source)["requested"].toString(), QString("Ctrl+Shift+1"));
  QCOMPARE(accel_.active.value("region").first().keys,
           keys(int(Qt::ControlModifier) | int(Qt::Key_1)).first().keys);
  QCOMPARE(region(source)["actual"].toArray(), (QJsonArray{"Ctrl+1"}));
  keyboard_.layout = 0; emit keyboard_.layoutListChanged();
  PUTGD_TRY_COMPARE(region(source)["actual"].toArray(), (QJsonArray{"Ctrl+!"}));
  QCOMPARE(region(source)["layout"].toInt(-1), 0);
 }
 void equivalentLayoutSignalKeepsBindingAndImmediateDelivery() {
  auto map = keymap("us,fr"); QVERIFY(map);
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", "Ctrl+1"}});
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  PUTGD_TRY_VERIFY(accel_.componentRequests > 0); QVERIFY(roundTrip());
  const auto setters = accel_.setters;
  const auto inactive = accel_.inactive.size();
  bool becameUnbound = false;
  connect(&source, &KdeShortcuts::changed, this, [&] {
   if (!region(source)["bound"].toBool()) becameUnbound = true;
  });
  QSignalSpy activated(&source, &KdeShortcuts::activated);
  // Ordered consecutive peer signals exercise the delivery window itself,
  // rather than waiting for an asynchronous rebind before pressing.
  keyboard_.layout = 1; emit keyboard_.layoutChanged(1);
  emit component_.globalShortcutPressed("putgd", "region", 1);
  QVERIFY(roundTrip());
  QCOMPARE(activated.size(), 1);
  QCOMPARE(activated.first().first().toString(), QString("region"));
  QVERIFY(!becameUnbound);
  QVERIFY(region(source)["bound"].toBool());
  QCOMPARE(region(source)["layout"].toInt(-1), 1);
  QCOMPARE(region(source)["effective"].toString(), QString("Ctrl+1"));
  QCOMPARE(accel_.setters, setters);
  QCOMPARE(accel_.inactive.size(), inactive);
 }
 void equivalentLayoutRestartsPendingComponentSubscription() {
  auto map = keymap("us,fr"); QVERIFY(map);
  accel_.holdComponents = true;
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", "Ctrl+1"}});
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  PUTGD_TRY_COMPARE(accel_.heldComponents.size(), 1);
  const auto firstQuery = accel_.componentRequests;
  const auto setters = accel_.setters;
  const auto inactive = accel_.inactive.size();
  QSignalSpy activated(&source, &KdeShortcuts::activated);
  keyboard_.layout = 1; emit keyboard_.layoutChanged(1);
  QVERIFY(roundTrip());
  QVERIFY(region(source)["bound"].toBool());
  QCOMPARE(region(source)["effective"].toString(), QString("Ctrl+1"));
  // Release the superseded query first. It cannot establish a subscription
  // for the new registration generation, even though the wire stayed equal.
  QVERIFY(accel_.replyHeldComponent()); QVERIFY(roundTrip());
  emit component_.globalShortcutPressed("putgd", "region", 1);
  QVERIFY(roundTrip()); QCOMPARE(activated.size(), 0);
  // A replacement query is required; accepting the stale reply would hide
  // this race, while preserving only the bound status would lose delivery.
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(accel_.componentRequests, firstQuery + 1, 1500);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(accel_.heldComponents.size(), 1, 1500);
  QVERIFY(accel_.replyHeldComponent()); QVERIFY(roundTrip());
  emit component_.globalShortcutPressed("putgd", "region", 2);
  PUTGD_TRY_COMPARE(activated.size(), 1);
  QCOMPARE(activated.first().first().toString(), QString("region"));
  QVERIFY(roundTrip()); QCOMPARE(activated.size(), 1);
  QCOMPARE(accel_.setters, setters);
  QCOMPARE(accel_.inactive.size(), inactive);
  QCOMPARE(region(source)["layout"].toInt(-1), 1);
 }
 void failedLayoutRetryDoesNotRepeatBindingInvalidation() {
  auto map = keymap("us,fr"); QVERIFY(map);
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", "Ctrl+Shift+1"}});
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool()); QVERIFY(roundTrip());
  keyboard_.failQueries = true; emit keyboard_.layoutListChanged();
  PUTGD_TRY_COMPARE(region(source)["state"].toString(), QString("shortcut_unavailable"));
  PUTGD_TRY_VERIFY(accel_.active.isEmpty());
  QVERIFY(roundTrip(true)); // Drain the first failed query and inactive calls.
  const auto requests = keyboard_.requests;
  const auto setters = accel_.setters;
  const auto inactive = accel_.inactive.size();
  QSignalSpy changed(&source, &KdeShortcuts::changed);
  // One three-second retry is sufficient; no long exponential-backoff wait.
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(keyboard_.requests > requests, 4500);
  QVERIFY(roundTrip(true));
  QCOMPARE(accel_.setters, setters);
  QCOMPARE(accel_.inactive.size(), inactive);
  QCOMPARE(changed.size(), 0);
  QVERIFY(!source.available());
  QVERIFY(!region(source)["bound"].toBool());
 }
 void delayedOldLayoutReplyCannotRestoreStaleBinding() {
  auto map = keymap("us,fr"); QVERIFY(map);
  KdeShortcuts source; source.setKeyboardMap(map.get()); source.bind({{"region", "Ctrl+Shift+1"}});
  PUTGD_TRY_COMPARE(region(source)["actual"].toArray(), (QJsonArray{"Ctrl+!"}));
  keyboard_.holdNext = true; emit keyboard_.layoutListChanged();
  PUTGD_TRY_COMPARE(keyboard_.held.size(), 1);
  QVERIFY(!region(source)["bound"].toBool());
  keyboard_.layout = 1; emit keyboard_.layoutChanged(1);
  PUTGD_TRY_COMPARE(region(source)["layout"].toInt(-1), 1);
  PUTGD_TRY_VERIFY(region(source)["bound"].toBool());
  const auto setters = accel_.setters;
  keyboard_.replyHeld(0); QVERIFY(roundTrip());
  QCOMPARE(region(source)["layout"].toInt(-1), 1);
  QCOMPARE(region(source)["actual"].toArray(), (QJsonArray{"Ctrl+1"}));
  QCOMPARE(accel_.setters, setters);
  QCOMPARE(accel_.active.value("region").first().keys,
           keys(int(Qt::ControlModifier) | int(Qt::Key_1)).first().keys);
 }
};

int main(int argc, char** argv) {
 // The private bus is the only Unix socket used by this Core-only transport test.
 // Check its resolved bytes before either Qt connection is opened.
 const auto address = qgetenv("DBUS_SESSION_BUS_ADDRESS").split(';').value(0);
 for (const auto& field : address.split(',')) {
  const auto path = field.startsWith("unix:path=") ? field.mid(10)
                  : field.startsWith("unix:abstract=") ? field.mid(14) : QByteArray{};
  if (!path.isEmpty() && QByteArray::fromPercentEncoding(path).size() > 107) return 2;
 }
 QCoreApplication app(argc, argv);
 KdeShortcutsTransportTest test;
 return QTest::qExec(&test, argc, argv);
}
#include "test_kde_shortcuts.moc"
