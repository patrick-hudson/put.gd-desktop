#include "NativeTray.h"
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QUuid>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QTimer>

// Wire structures contain only Core types. ARGB bytes are in network order.
struct NativeImage { int width = 0, height = 0; QByteArray bytes; };
using NativeImages = QList<NativeImage>;
struct NativeTooltip { QString icon; NativeImages images; QString title, description; };
struct NativeLayout { int id = 0; QVariantMap properties; QList<QDBusVariant> children; };
struct NativeItemProperties { int id = 0; QVariantMap properties; };
using NativePropertyList = QList<NativeItemProperties>;
struct NativeMenuEvent { int id = 0; QString event; QDBusVariant data; uint timestamp = 0; };
using NativeEvents = QList<NativeMenuEvent>;
Q_DECLARE_METATYPE(NativeImage)
Q_DECLARE_METATYPE(NativeImages)
Q_DECLARE_METATYPE(NativeTooltip)
Q_DECLARE_METATYPE(NativeLayout)
Q_DECLARE_METATYPE(NativeItemProperties)
Q_DECLARE_METATYPE(NativePropertyList)
Q_DECLARE_METATYPE(NativeMenuEvent)
Q_DECLARE_METATYPE(NativeEvents)

QDBusArgument& operator<<(QDBusArgument& a, const NativeImage& x) {
 a.beginStructure(); a << x.width << x.height << x.bytes; a.endStructure(); return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, NativeImage& x) {
 a.beginStructure(); a >> x.width >> x.height >> x.bytes; a.endStructure(); return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const NativeTooltip& x) {
 a.beginStructure(); a << x.icon << x.images << x.title << x.description; a.endStructure(); return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, NativeTooltip& x) {
 a.beginStructure(); a >> x.icon >> x.images >> x.title >> x.description; a.endStructure(); return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const NativeLayout& x) {
 a.beginStructure(); a << x.id << x.properties << x.children; a.endStructure(); return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, NativeLayout& x) {
 a.beginStructure(); a >> x.id >> x.properties >> x.children; a.endStructure(); return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const NativeItemProperties& x) {
 a.beginStructure(); a << x.id << x.properties; a.endStructure(); return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, NativeItemProperties& x) {
 a.beginStructure(); a >> x.id >> x.properties; a.endStructure(); return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const NativeMenuEvent& x) {
 a.beginStructure(); a << x.id << x.event << x.data << x.timestamp; a.endStructure(); return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, NativeMenuEvent& x) {
 a.beginStructure(); a >> x.id >> x.event >> x.data >> x.timestamp; a.endStructure(); return a;
}

namespace putgd {
namespace {
constexpr auto itemInterface = "org.kde.StatusNotifierItem";
constexpr auto menuInterface = "com.canonical.dbusmenu";
constexpr auto watcherService = "org.kde.StatusNotifierWatcher";
constexpr auto watcherPath = "/StatusNotifierWatcher";
constexpr int callTimeoutMs = 1500;
struct Entry { const char* label; const char* action; };
constexpr Entry entries[] {
 {"Capture region", "region"}, {"Capture window", "window"},
 {"Capture desktop", "desktop"}, {"Capture clipboard", "clipboard"},
 {"", ""}, {"Settings and setup", "settings"}, {"Upload history", "history"},
 {"Recent activity", "activity"}, {"", ""}, {"About put.gd", "about"}, {"Quit", "quit"}
};
constexpr int entryCount = sizeof(entries) / sizeof(entries[0]);
bool knownId(int id) { return id >= 0 && id <= entryCount; }
bool validNames(const QStringList& names) {
 if (names.size() > 32) return false;
 for (const auto& name : names) if (name.size() > 128) return false;
 return true;
}
QVariantMap menuProperties(int id, const QStringList& names = {}) {
 QVariantMap props;
 if (id == 0) props.insert("children-display", "submenu");
 else {
  const auto& e = entries[id-1];
  if (!*e.label) props.insert("type", "separator");
  else props.insert("label", QString::fromLatin1(e.label));
 }
 if (!names.isEmpty()) for (auto it = props.begin(); it != props.end();) {
  if (!names.contains(it.key())) it = props.erase(it); else ++it;
 }
 return props;
}
NativeLayout menuLayout(int id, int depth, const QStringList& names) {
 NativeLayout result{id, menuProperties(id, names), {}};
 if (id == 0 && depth != 0) for (int i = 1; i <= entryCount; ++i)
  result.children.append(QDBusVariant(QVariant::fromValue(menuLayout(i, 0, names))));
 return result;
}
NativeImages brandIcon(bool working=false) {
 NativeImages result;
 for (int scale : {1, 2, 3}) {
  QImage image(24 * scale, 24 * scale, QImage::Format_ARGB32);
  image.setDevicePixelRatio(scale); image.fill(Qt::transparent);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(QColor(working?"#62d69b":"#9bbcff"), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);
  painter.drawRoundedRect(QRectF(3, 3, 18, 18), 5, 5);
  painter.drawLine(12, 17, 12, 7); painter.drawLine(12, 7, 8, 11); painter.drawLine(12, 7, 16, 11);
  painter.end();
  NativeImage wire{image.width(), image.height(), QByteArray(image.width() * image.height() * 4, '\0')};
  for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
   const auto pixel = image.pixel(x, y); const int pos = (y * image.width() + x) * 4;
   wire.bytes[pos] = char(qAlpha(pixel)); wire.bytes[pos+1] = char(qRed(pixel));
   wire.bytes[pos+2] = char(qGreen(pixel)); wire.bytes[pos+3] = char(qBlue(pixel));
  }
  result.append(std::move(wire));
 }
 return result;
}
void registerWireTypes() {
 qDBusRegisterMetaType<NativeImage>(); qDBusRegisterMetaType<NativeImages>();
 qDBusRegisterMetaType<NativeTooltip>(); qDBusRegisterMetaType<NativeLayout>();
 qDBusRegisterMetaType<NativeItemProperties>(); qDBusRegisterMetaType<NativePropertyList>();
 qDBusRegisterMetaType<NativeMenuEvent>(); qDBusRegisterMetaType<NativeEvents>();
 qDBusRegisterMetaType<QList<int>>(); qDBusRegisterMetaType<QList<QStringList>>();
}
class TrayExport;
}

class NativeTrayPrivate final : public QObject {
 Q_OBJECT
public:
 NativeTrayPrivate(NativeTray* owner, QDBusConnection bus);
 ~NativeTrayPrivate() override;
 bool available() const { return available_; }
 bool availabilityKnown() const { return availabilityKnown_; }
 QVariantMap properties(bool menu) const;
 void dispatch(const QString& action);
 void setActivity(bool working,const QString& description);
 void notify(const QString& title, const QString& message, bool warning);
private slots:
 void hostChanged();
private:
 void setOwner(const QString& owner);
 void queryHost();
 void registerItem();
 void updateAvailability();
 NativeTray* q_;
 QDBusConnection bus_;
 QString service_, owner_, registeredOwner_;
 QDBusServiceWatcher watcher_;
 std::unique_ptr<TrayExport> item_, menu_;
 NativeImages icons_;
 bool working_ = false;
 QString activityText_;
 QTimer retry_;
 quint64 generation_ = 0, hostQuery_ = 0;
 int attempts_ = 0, notificationsInFlight_ = 0;
 bool exportedItem_ = false, exportedMenu_ = false, registeredService_ = false;
 bool hostPresent_ = false, available_ = false, registrationInFlight_ = false;
 bool ownerKnown_ = false, hostKnown_ = false, registrationSettled_ = false, availabilityKnown_ = false;
};

namespace {
class TrayExport final : public QDBusVirtualObject {
public:
 TrayExport(bool menu, NativeTrayPrivate* owner): menu_(menu), owner_(owner) {}
 QString introspect(const QString&) const override;
 bool handleMessage(const QDBusMessage&, const QDBusConnection&) override;
private:
 bool menu_;
 NativeTrayPrivate* owner_;
};
}

NativeTrayPrivate::NativeTrayPrivate(NativeTray* owner, QDBusConnection bus)
 : QObject(owner), q_(owner), bus_(std::move(bus)),
   service_("org.kde.StatusNotifierItem.putgd_" + QUuid::createUuid().toString(QUuid::Id128)),
   watcher_(watcherService, bus_, QDBusServiceWatcher::WatchForOwnerChange, this), icons_(brandIcon()) {
 registerWireTypes();
 auto resolveUnavailable = [this] {
  ownerKnown_ = hostKnown_ = registrationSettled_ = true;
  updateAvailability();
 };
 if (!bus_.isConnected()) { resolveUnavailable(); return; }
 item_ = std::make_unique<TrayExport>(false, this);
 menu_ = std::make_unique<TrayExport>(true, this);
 registeredService_ = bus_.registerService(service_);
 if (!registeredService_) { resolveUnavailable(); return; }
 exportedItem_ = bus_.registerVirtualObject("/StatusNotifierItem", item_.get());
 if (!exportedItem_) { resolveUnavailable(); return; }
 exportedMenu_ = bus_.registerVirtualObject("/Menu", menu_.get());
 if (!exportedMenu_) { resolveUnavailable(); return; }
 retry_.setSingleShot(true);
 connect(&retry_, &QTimer::timeout, this, &NativeTrayPrivate::registerItem);
 connect(&watcher_, &QDBusServiceWatcher::serviceOwnerChanged, this,
  [this](const QString&, const QString&, const QString& value) { setOwner(value); });
 bus_.connect(watcherService, watcherPath, watcherService, "StatusNotifierHostRegistered", this, SLOT(hostChanged()));
 bus_.connect(watcherService, watcherPath, watcherService, "StatusNotifierHostUnregistered", this, SLOT(hostChanged()));
 const auto generation = generation_;
 auto call = QDBusMessage::createMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner");
 call << QString::fromLatin1(watcherService);
 auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(call, callTimeoutMs), this);
 connect(pending, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher* p) {
  const QDBusPendingReply<QString> result = *p; p->deleteLater();
  if (generation != generation_) return;
  setOwner(result.isError() ? QString{} : result.value());
 });
}
NativeTrayPrivate::~NativeTrayPrivate() {
 retry_.stop();
 // The watcher tracks the named service, so disappearance removes the item even
 // when other app services keep the shared session-bus connection alive.
 if (registeredService_) bus_.unregisterService(service_);
 if (exportedMenu_) bus_.unregisterObject("/Menu");
 if (exportedItem_) bus_.unregisterObject("/StatusNotifierItem");
}
void NativeTrayPrivate::setOwner(const QString& owner) {
 if (ownerKnown_ && owner == owner_) return;
 ownerKnown_ = true; hostKnown_ = owner.isEmpty(); registrationSettled_ = owner.isEmpty(); availabilityKnown_ = false;
 ++generation_; ++hostQuery_; owner_ = owner; registeredOwner_.clear(); hostPresent_ = false;
 attempts_ = 0; retry_.stop(); updateAvailability();
 if (!owner_.isEmpty()) { queryHost(); registerItem(); }
}
void NativeTrayPrivate::hostChanged() { attempts_ = 0; queryHost(); registerItem(); }
void NativeTrayPrivate::queryHost() {
 if (owner_.isEmpty()) return;
 hostKnown_ = false;
 const auto generation = generation_, query = ++hostQuery_;
 auto call = QDBusMessage::createMethodCall(owner_, watcherPath, "org.freedesktop.DBus.Properties", "Get");
 call << QString::fromLatin1(watcherService) << QString("IsStatusNotifierHostRegistered");
 auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(call, callTimeoutMs), this);
 connect(pending, &QDBusPendingCallWatcher::finished, this, [this, generation, query](QDBusPendingCallWatcher* p) {
  const QDBusPendingReply<QDBusVariant> result = *p; p->deleteLater();
  if (generation != generation_ || query != hostQuery_) return;
  hostKnown_ = true;
  hostPresent_ = !result.isError() && result.value().variant().metaType() == QMetaType::fromType<bool>()
                && result.value().variant().toBool();
  updateAvailability();
 });
}
void NativeTrayPrivate::registerItem() {
 if (owner_.isEmpty() || registrationInFlight_ || registeredOwner_ == owner_ || attempts_ >= 3) return;
 retry_.stop(); ++attempts_; registrationInFlight_ = true;
 const auto generation = generation_; const auto owner = owner_;
 auto call = QDBusMessage::createMethodCall(owner, watcherPath, watcherService, "RegisterStatusNotifierItem");
 call << service_;
 auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(call, callTimeoutMs), this);
 connect(pending, &QDBusPendingCallWatcher::finished, this, [this, generation, owner](QDBusPendingCallWatcher* p) {
  const QDBusPendingReply<> result = *p; p->deleteLater(); registrationInFlight_ = false;
  if (generation != generation_) { registerItem(); return; }
  if (!result.isError()) { registeredOwner_ = owner; registrationSettled_ = true; queryHost(); updateAvailability(); }
  else if (attempts_ < 3) retry_.start(attempts_ * 500);
  else { registrationSettled_ = true; updateAvailability(); }
 });
}
void NativeTrayPrivate::updateAvailability() {
 const bool known = ownerKnown_ && hostKnown_ && registrationSettled_;
 if (known && !availabilityKnown_) {
  availabilityKnown_ = true;
  QTimer::singleShot(0, q_, [guard = QPointer<NativeTray>(q_)] {
   if (guard && guard->availabilityKnown()) emit guard->availabilityResolved();
  });
 }
 const bool value = exportedItem_ && exportedMenu_ && registeredService_ && hostPresent_
                    && !owner_.isEmpty() && registeredOwner_ == owner_;
 if (value == available_) return;
 available_ = value;
 // Defer user callbacks: they may destroy the controller and this transport.
 QTimer::singleShot(0, q_, [guard = QPointer<NativeTray>(q_), value] {
  if (guard && guard->available() == value) emit guard->availabilityChanged(value);
 });
}
void NativeTrayPrivate::dispatch(const QString& action) {
 QTimer::singleShot(0, q_, [guard = QPointer<NativeTray>(q_), action] {
  if (guard) emit guard->actionActivated(action);
 });
}
void NativeTrayPrivate::notify(const QString& title, const QString& message, bool warning) {
 if (!available_ || notificationsInFlight_ >= 8) return;
 auto call = QDBusMessage::createMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify");
 const QVariantMap hints{{"urgency", QVariant::fromValue(static_cast<uchar>(warning ? 2 : 1))}, {"desktop-entry", "putgd"}};
 call.setArguments({QString("put.gd"), uint(0), QString("putgd"), title.left(512), message.left(4096), QStringList{}, hints, 10000});
 ++notificationsInFlight_;
 auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(call, callTimeoutMs), this);
 connect(pending, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* p) {
  --notificationsInFlight_; p->deleteLater();
 });
}
void NativeTrayPrivate::setActivity(bool working,const QString& description) {
 const bool iconChanged=working!=working_;
 if(!iconChanged&&description==activityText_)return;
 working_=working;activityText_=description;
 if(iconChanged)icons_=brandIcon(working);
 const auto props=properties(false);QVariantMap changed{{"ToolTip",props["ToolTip"]}};
 if(iconChanged)changed.insert("IconPixmap",props["IconPixmap"]);
 auto signal=QDBusMessage::createSignal("/StatusNotifierItem","org.freedesktop.DBus.Properties","PropertiesChanged");
 signal.setArguments({QString::fromLatin1(itemInterface),changed,QStringList{}});bus_.send(signal);
 bus_.send(QDBusMessage::createSignal("/StatusNotifierItem",itemInterface,"NewToolTip"));
 if(iconChanged)bus_.send(QDBusMessage::createSignal("/StatusNotifierItem",itemInterface,"NewIcon"));
}
QVariantMap NativeTrayPrivate::properties(bool menu) const {
 if (menu) return {{"Version", uint(3)}, {"TextDirection", "ltr"}, {"Status", "normal"}, {"IconThemePath", QStringList{}}};
 return {{"Category", "ApplicationStatus"}, {"Id", "putgd"}, {"Title", "put.gd"}, {"Status", "Active"}, {"WindowId", 0},
  {"IconThemePath", ""}, {"Menu", QVariant::fromValue(QDBusObjectPath("/Menu"))}, {"ItemIsMenu", false},
  {"IconName", ""}, {"IconPixmap", QVariant::fromValue(icons_)},
  {"OverlayIconName", ""}, {"OverlayIconPixmap", QVariant::fromValue(NativeImages{})},
  {"AttentionIconName", ""}, {"AttentionIconPixmap", QVariant::fromValue(NativeImages{})}, {"AttentionMovieName", ""},
  {"ToolTip", QVariant::fromValue(NativeTooltip{"", {}, working_?QString("put.gd — Working"):QString("put.gd — Share and local capture"), activityText_})}};
}
QString TrayExport::introspect(const QString&) const {
 QString xml = "<interface name=\"org.freedesktop.DBus.Properties\">"
  "<method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
  "<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
  "<method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>"
  "<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal></interface>";
 xml += QString("<interface name=\"%1\">").arg(menu_ ? menuInterface : itemInterface);
 const auto props = owner_->properties(menu_);
 for (auto it = props.begin(); it != props.end(); ++it) {
  const char* signature = QDBusMetaType::typeToSignature(it.value().metaType());
  xml += QString("<property name=\"%1\" type=\"%2\" access=\"read\"/>").arg(it.key(), QString::fromLatin1(signature));
 }
 if (menu_) xml += R"XML(
  <method name="GetLayout"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/><arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/></method>
  <method name="GetGroupProperties"><arg type="ai" direction="in"/><arg type="as" direction="in"/><arg type="a(ia{sv})" direction="out"/></method>
  <method name="GetProperty"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
  <method name="Event"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="u" direction="in"/></method>
  <method name="EventGroup"><arg type="a(isvu)" direction="in"/><arg type="ai" direction="out"/></method>
  <method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
  <method name="AboutToShowGroup"><arg type="ai" direction="in"/><arg type="ai" direction="out"/><arg type="ai" direction="out"/></method>
  <signal name="ItemsPropertiesUpdated"><arg type="a(ia{sv})"/><arg type="a(ias)"/></signal>
  <signal name="LayoutUpdated"><arg type="u"/><arg type="i"/></signal>
  <signal name="ItemActivationRequested"><arg type="i"/><arg type="u"/></signal>
 )XML";
 else xml += R"XML(
  <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
  <signal name="NewTitle"/><signal name="NewIcon"/><signal name="NewAttentionIcon"/>
  <signal name="NewOverlayIcon"/><signal name="NewMenu"/><signal name="NewToolTip"/>
  <signal name="NewStatus"><arg type="s"/></signal>
 )XML";
 return xml + "</interface>";
}

bool TrayExport::handleMessage(const QDBusMessage& m, const QDBusConnection& bus) {
 if (m.type() != QDBusMessage::MethodCallMessage) return false;
 const auto args = m.arguments(); const auto member = m.member(); const auto sig = m.signature();
 auto reply = [&](const QList<QVariant>& values = {}) { return bus.send(m.createReply(values)); };
 auto error = [&](const char* name = "org.freedesktop.DBus.Error.InvalidArgs") {
  return bus.send(m.createErrorReply(QString::fromLatin1(name), "Invalid or unsupported tray request"));
 };
 auto dispatchId = [this](int id) {
  if (id > 0 && knownId(id) && *entries[id-1].action)
   owner_->dispatch(QString::fromLatin1(entries[id-1].action));
 };
 const QString expected = QString::fromLatin1(menu_ ? menuInterface : itemInterface);
 if (m.interface() == "org.freedesktop.DBus.Properties") {
  if ((member == "GetAll" && sig == "s") || (member == "Get" && sig == "ss") || (member == "Set" && sig == "ssv")) {
   if (args.at(0).toString() != expected) return error("org.freedesktop.DBus.Error.UnknownInterface");
   const auto props = owner_->properties(menu_);
   if (member == "GetAll") return reply({props});
   const auto name = args.at(1).toString();
   if (!props.contains(name)) return error("org.freedesktop.DBus.Error.UnknownProperty");
   if (member == "Set") return error("org.freedesktop.DBus.Error.PropertyReadOnly");
   return reply({QVariant::fromValue(QDBusVariant(props.value(name)))});
  }
  return error();
 }
 if (!m.interface().isEmpty() && m.interface() != expected) return error("org.freedesktop.DBus.Error.UnknownInterface");
 if (!menu_) {
  if ((member == "Activate" || member == "SecondaryActivate" || member == "ContextMenu") && sig == "ii") {
   if (member == "Activate") owner_->dispatch("activity");
   if (member == "ContextMenu") {
    auto show = QDBusMessage::createSignal("/Menu", menuInterface, "ItemActivationRequested");
    show.setArguments({0, uint(0)}); bus.send(show);
   }
   return reply();
  }
  if (member == "Scroll" && sig == "is") return reply();
  return error("org.freedesktop.DBus.Error.UnknownMethod");
 }
 if (member == "GetLayout" && sig == "iias") {
  const int id = args.at(0).toInt(), depth = args.at(1).toInt();
  const auto names = qdbus_cast<QStringList>(args.at(2));
  if (!knownId(id) || depth < -1 || !validNames(names)) return error();
  return reply({uint(1), QVariant::fromValue(menuLayout(id, depth, names))});
 }
 if (member == "GetGroupProperties" && sig == "aias") {
  auto ids = qdbus_cast<QList<int>>(args.at(0)); const auto names = qdbus_cast<QStringList>(args.at(1));
  if (ids.size() > 64 || !validNames(names)) return error();
  if (ids.isEmpty()) for (int id = 0; id <= entryCount; ++id) ids.append(id);
  NativePropertyList values;
  for (int id : ids) if (knownId(id)) values.append({id, menuProperties(id, names)});
  return reply({QVariant::fromValue(values)});
 }
 if (member == "GetProperty" && sig == "is") {
  int id = args.at(0).toInt(); if (!knownId(id)) return error();
  const auto name = args.at(1).toString(); if (name.size() > 128) return error(); auto props = menuProperties(id);
  // Defaults are omitted from layouts but must resolve for explicit queries.
  const QVariantMap defaults{{"type", "standard"}, {"label", ""}, {"enabled", true}, {"visible", true},
   {"icon-name", ""}, {"icon-data", QByteArray{}}, {"shortcut", QVariant::fromValue(QList<QStringList>{})},
   {"toggle-type", ""}, {"toggle-state", -1}, {"children-display", ""}, {"disposition", "normal"}};
  if (!props.contains(name) && defaults.contains(name)) props.insert(name, defaults.value(name));
  if (!props.contains(name)) return error("org.freedesktop.DBus.Error.UnknownProperty");
  return reply({QVariant::fromValue(QDBusVariant(props.value(name)))});
 }
 if (member == "Event" && sig == "isvu") {
  int id = args.at(0).toInt(); if (!knownId(id)) return error();
  if (args.at(1).toString() == "clicked") dispatchId(id);
  return reply();
 }
 if (member == "EventGroup" && sig == "a(isvu)") {
  const auto events = qdbus_cast<NativeEvents>(args.at(0)); if (events.size() > 64) return error();
  QList<int> missing; bool found = false;
  for (const auto& e : events) {
   if (!knownId(e.id)) missing.append(e.id);
   else { found = true; if (e.event == "clicked") dispatchId(e.id); }
  }
  if (!events.isEmpty() && !found) return error();
  return reply({QVariant::fromValue(missing)});
 }
 if (member == "AboutToShow" && sig == "i") {
  if (!knownId(args.at(0).toInt())) return error();
  return reply({false});
 }
 if (member == "AboutToShowGroup" && sig == "ai") {
  const auto ids = qdbus_cast<QList<int>>(args.at(0)); if (ids.size() > 64) return error();
  QList<int> missing; bool found = false;
  for (int id : ids) { if (knownId(id)) found = true; else missing.append(id); }
  if (!ids.isEmpty() && !found) return error();
  return reply({QVariant::fromValue(QList<int>{}), QVariant::fromValue(missing)});
 }
 return error("org.freedesktop.DBus.Error.UnknownMethod");
}


NativeTray::NativeTray(QObject* parent): NativeTray(QDBusConnection::sessionBus(), parent) {}
NativeTray::NativeTray(const QDBusConnection& bus, QObject* parent): QObject(parent), d_(std::make_unique<NativeTrayPrivate>(this, bus)) {}
NativeTray::~NativeTray() = default;
bool NativeTray::available() const { return d_->available(); }
bool NativeTray::availabilityKnown() const { return d_->availabilityKnown(); }
void NativeTray::setActivity(bool working,const QString& description) {d_->setActivity(working,description);}
void NativeTray::notify(const QString& title, const QString& message, bool warning) { d_->notify(title, message, warning); }
}
#include "NativeTray.moc"
