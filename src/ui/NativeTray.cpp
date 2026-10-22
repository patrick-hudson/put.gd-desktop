#include "NativeTray.h"
#include <QSet>
#include "IconArt.h"
#include "Theme.h"
#include <QCoreApplication>
#include <QHash>
#include <QImage>
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
struct NativeItemRemoved { int id = 0; QStringList names; };
using NativeRemovedList = QList<NativeItemRemoved>;
struct NativeMenuEvent { int id = 0; QString event; QDBusVariant data; uint timestamp = 0; };
using NativeEvents = QList<NativeMenuEvent>;
Q_DECLARE_METATYPE(NativeImage)
Q_DECLARE_METATYPE(NativeImages)
Q_DECLARE_METATYPE(NativeTooltip)
Q_DECLARE_METATYPE(NativeLayout)
Q_DECLARE_METATYPE(NativeItemProperties)
Q_DECLARE_METATYPE(NativePropertyList)
Q_DECLARE_METATYPE(NativeItemRemoved)
Q_DECLARE_METATYPE(NativeRemovedList)
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
QDBusArgument& operator<<(QDBusArgument& a, const NativeItemRemoved& x) {
 a.beginStructure(); a << x.id << x.names; a.endStructure(); return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, NativeItemRemoved& x) {
 a.beginStructure(); a >> x.id >> x.names; a.endStructure(); return a;
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
constexpr auto notificationService = "org.freedesktop.Notifications";
constexpr auto notificationPath = "/org/freedesktop/Notifications";
constexpr int callTimeoutMs = 1500;
// IDs are 1-based table positions and stay stable; rootOrder sets the display
// order. An entry with a parent belongs to that entry's submenu. A retired
// entry keeps its slot, so later IDs don't move, and is unknown on the wire.
struct Entry { const char* label; const char* action; int parent; };
constexpr int retired = -1;
constexpr Entry entries[] {
 {"Capture Region", "region", 0}, {"Capture Window", "window", 0},
 {"Capture Desktop", "desktop", 0}, {"Upload Clipboard", "clipboard", 0},
 {"Capture Region & Edit", "edit", 0},
 {"", "", 0}, {"Settings and setup", "settings", 0}, {"Upload history", "history", 0},
 // 9 was "Recent activity", which opened the same History page.
 {"", "", retired}, {"", "", 0}, {"About put.gd", "about", 0}, {"Quit", "quit", 0},
 {"Edit Image", "", 0}, {"From clipboard", "edit-clipboard", 13}, {"From file…", "edit-file", 13},
 {"Capture Screen Recording", "record", 0},
 // Hidden until a stable update is available; its label names the version (#293).
 {"Update available", "update", 0}
};
constexpr int updateId = 17;
constexpr int rootOrder[] {1, 5, 2, 3, 16, 13, 4, 6, 7, 8, 17, 10, 11, 12};
constexpr int entryCount = sizeof(entries) / sizeof(entries[0]);
QList<int> childrenOf(int id) {
 QList<int> result;
 if (id == 0) for (int child : rootOrder) result.append(child);
 else for (int i = 1; i <= entryCount; ++i) if (entries[i-1].parent == id) result.append(i);
 return result;
}
bool knownId(int id) { return id == 0 || (id > 0 && id <= entryCount && entries[id-1].parent != retired); }
bool validNames(const QStringList& names) {
 if (names.size() > 32) return false;
 for (const auto& name : names) if (name.size() > 128) return false;
 return true;
}
QVariantMap menuProperties(int id, const QStringList& names = {}, const QString& update = {}) {
 QVariantMap props;
 if (id == 0 || !childrenOf(id).isEmpty()) props.insert("children-display", "submenu");
 if (id != 0) {
  const auto& e = entries[id-1];
  if (!*e.label) props.insert("type", "separator");
  else props.insert("label", id == updateId && !update.isEmpty() ? update : QString::fromUtf8(e.label));
  // Always explicit: Plasma's and GNOME's hosts update a known item only with the
  // properties a layout carries, so an omitted default would keep it hidden (#304).
  if (id == updateId) props.insert("visible", !update.isEmpty());
 }
 if (!names.isEmpty()) for (auto it = props.begin(); it != props.end();) {
  if (!names.contains(it.key())) it = props.erase(it); else ++it;
 }
 return props;
}
NativeLayout menuLayout(int id, int depth, const QStringList& names, const QString& update) {
 NativeLayout result{id, menuProperties(id, names, update), {}};
 // depth -1 expands every level; 0 returns this item alone.
 if (depth != 0) for (int child : childrenOf(id))
  result.children.append(QDBusVariant(QVariant::fromValue(menuLayout(child, depth < 0 ? depth : depth - 1, names, update))));
 return result;
}
NativeImages brandIcon(const QColor& color=Theme::trayTints(false).idle,int percent=-1) {
 NativeImages result;
 for (int scale : {1, 2, 3}) {
  const auto image=IconArt::liftTray(color,percent,scale);
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
 qDBusRegisterMetaType<NativeItemRemoved>(); qDBusRegisterMetaType<NativeRemovedList>();
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
 void setActivity(const QColor& color,const QString& description,int percent);
 void notify(const QString& title, const QString& message, bool warning, const QUrl& link, const QList<NotificationAction>& actions);
 // The update entry's label, empty while it is hidden, and the layout revision
 // that changes with it.
 const QString& updateLabel() const { return updateLabel_; }
 uint menuRevision() const { return menuRevision_; }
 void setUpdateLabel(const QString& label);
private slots:
 void hostChanged();
 void notificationActivated(uint id, const QString& action);
 void notificationClosed(uint id, uint reason);
private:
 void setOwner(const QString& owner);
 void queryNotificationServer();
 void queryHost();
 void registerItem();
 void updateAvailability();
 NativeTray* q_;
 QDBusConnection bus_;
 QString service_, owner_, registeredOwner_;
 QDBusServiceWatcher watcher_, notificationWatcher_;
 QHash<uint,QUrl> notificationLinks_;
 QHash<uint,QHash<QString,QString>> notificationActions_;
 quint64 notificationGeneration_ = 0;
 QString notificationServer_; // GetServerInformation's name, empty until it answers
 QSet<uint> residentNotifications_; // sent resident (#151): put.gd closes them once used
 QString updateLabel_;
 uint menuRevision_ = 1;
 void closeNotification(uint id);
 std::unique_ptr<TrayExport> item_, menu_;
 NativeImages icons_;
 QColor color_ = Theme::trayTints(false).idle;
 int percent_ = -1;
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
   watcher_(watcherService, bus_, QDBusServiceWatcher::WatchForOwnerChange, this),
   notificationWatcher_(notificationService,bus_,QDBusServiceWatcher::WatchForOwnerChange,this), icons_(brandIcon()) {
 registerWireTypes();
 connect(&notificationWatcher_,&QDBusServiceWatcher::serviceOwnerChanged,this,[this](const QString&,const QString& oldOwner,const QString&) {
  notificationServer_.clear();queryNotificationServer();
  if(oldOwner.isEmpty())return;
  ++notificationGeneration_;notificationLinks_.clear();notificationActions_.clear();
 });
 bus_.connect(notificationService,notificationPath,notificationService,"ActionInvoked",this,SLOT(notificationActivated(uint,QString)));
 bus_.connect(notificationService,notificationPath,notificationService,"NotificationClosed",this,SLOT(notificationClosed(uint,uint)));
 queryNotificationServer();
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
void NativeTrayPrivate::queryNotificationServer() {
 if(!bus_.isConnected())return;
 auto call=QDBusMessage::createMethodCall(notificationService,notificationPath,notificationService,"GetServerInformation");
 auto* pending=new QDBusPendingCallWatcher(bus_.asyncCall(call,callTimeoutMs),this);
 connect(pending,&QDBusPendingCallWatcher::finished,this,[this](QDBusPendingCallWatcher* p) {
  const QDBusPendingReply<QString,QString,QString,QString> result=*p;p->deleteLater();
  if(!result.isError())notificationServer_=result.argumentAt<0>();
 });
}
int NativeTray::notificationTimeoutMs(const QString& server,bool actions) {
 // GNOME Shell and Plasma keep a notification, buttons and all, in their lists once
 // the popup goes. Elsewhere (Cinnamon, #151, and servers not yet known) a
 // notification with buttons stays until it is used or dismissed.
 if(!actions||server=="gnome-shell"||server=="Plasma")return 10000;
 return 0;
}
void NativeTrayPrivate::notify(const QString& title, const QString& message, bool warning, const QUrl& link, const QList<NotificationAction>& actions) {
 if (!available_ || notificationsInFlight_ >= 8) return;
 auto call = QDBusMessage::createMethodCall(notificationService, notificationPath, notificationService, "Notify");
 QVariantMap hints{{"urgency", QVariant::fromValue(static_cast<uchar>(warning ? 2 : 1))}, {"desktop-entry", "putgd"}};
 QStringList names=link.isEmpty()?QStringList{}:QStringList{"default","Open in browser"};
 QHash<QString,QString> values;
 for(const auto& action:actions)if(action.key!="default"){names<<action.key<<action.label;values.insert(action.key,action.value);}
 const int timeout=NativeTray::notificationTimeoutMs(notificationServer_,!values.isEmpty());
 // Cinnamon removes an app's non-resident notifications as soon as one of its
 // windows takes focus, buttons and all; a resident one stays until used (#151).
 const bool resident=timeout==0;
 if(resident)hints.insert("resident",true);
 call.setArguments({QString("put.gd"), uint(0), QString("putgd"), title.left(512), message.left(4096), names, hints, timeout});
 ++notificationsInFlight_;
 const auto generation=notificationGeneration_;
 auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(call, callTimeoutMs), this);
 connect(pending, &QDBusPendingCallWatcher::finished, this, [this,generation,link,values,resident](QDBusPendingCallWatcher* p) {
  const QDBusPendingReply<uint> result=*p;
  --notificationsInFlight_; p->deleteLater();
  if(generation!=notificationGeneration_||result.isError())return;
  if(!link.isEmpty())notificationLinks_.insert(result.value(),link);
  if(!values.isEmpty())notificationActions_.insert(result.value(),values);
  if(resident)residentNotifications_.insert(result.value());
 });
}
// A resident notification isn't removed by its server when used; put.gd closes it.
void NativeTrayPrivate::closeNotification(uint id) {
 if(!residentNotifications_.remove(id))return;
 auto call=QDBusMessage::createMethodCall(notificationService,notificationPath,notificationService,"CloseNotification");
 call<<id;bus_.asyncCall(call,callTimeoutMs);
}
void NativeTrayPrivate::notificationActivated(uint id,const QString& action) {
 // A button or a click on the body uses a resident notification up.
 closeNotification(id);
 if(action!="default"&&notificationActions_.value(id).contains(action)) {
  const auto value=notificationActions_.take(id).value(action);notificationLinks_.remove(id);
  QTimer::singleShot(0,q_,[guard=QPointer<NativeTray>(q_),action,value] {
   if(guard)emit guard->notificationAction(action,value);
  });
  return;
 }
 if(action!="default"||!notificationLinks_.contains(id))return;
 const auto link=notificationLinks_.take(id);notificationActions_.remove(id);
 QTimer::singleShot(0,q_,[guard=QPointer<NativeTray>(q_),link] {
  if(guard)emit guard->notificationActivated(link);
 });
}
void NativeTrayPrivate::notificationClosed(uint id,uint) {notificationLinks_.remove(id);notificationActions_.remove(id);residentNotifications_.remove(id);}
void NativeTrayPrivate::setActivity(const QColor& color,const QString& description,int percent) {
 percent=qBound(-1,percent,100);
 const bool iconChanged=color!=color_||percent!=percent_;
 const bool textChanged=description!=activityText_;
 if(!iconChanged&&!textChanged)return;
 color_=color;activityText_=description;percent_=percent;
 if(iconChanged)icons_=brandIcon(color,percent);
 const auto props=properties(false);QVariantMap changed;
 if(textChanged)changed.insert("ToolTip",props["ToolTip"]);
 if(iconChanged)changed.insert("IconPixmap",props["IconPixmap"]);
 auto signal=QDBusMessage::createSignal("/StatusNotifierItem","org.freedesktop.DBus.Properties","PropertiesChanged");
 signal.setArguments({QString::fromLatin1(itemInterface),changed,QStringList{}});bus_.send(signal);
 if(textChanged)bus_.send(QDBusMessage::createSignal("/StatusNotifierItem",itemInterface,"NewToolTip"));
 if(iconChanged)bus_.send(QDBusMessage::createSignal("/StatusNotifierItem",itemInterface,"NewIcon"));
}
QVariantMap NativeTrayPrivate::properties(bool menu) const {
 if (menu) return {{"Version", uint(3)}, {"TextDirection", "ltr"}, {"Status", "normal"}, {"IconThemePath", QStringList{}}};
 return {{"Category", "ApplicationStatus"}, {"Id", "putgd"}, {"Title", "put.gd"}, {"Status", "Active"}, {"WindowId", 0},
  {"IconThemePath", ""}, {"Menu", QVariant::fromValue(QDBusObjectPath("/Menu"))}, {"ItemIsMenu", false},
  {"IconName", ""}, {"IconPixmap", QVariant::fromValue(icons_)},
  {"OverlayIconName", ""}, {"OverlayIconPixmap", QVariant::fromValue(NativeImages{})},
  {"AttentionIconName", ""}, {"AttentionIconPixmap", QVariant::fromValue(NativeImages{})}, {"AttentionMovieName", ""},
  {"ToolTip", QVariant::fromValue(NativeTooltip{"", {}, activityText_.isEmpty()?QString("put.gd — Share and local capture"):QString("put.gd — Working"), activityText_})}};
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
   if (member == "Activate") owner_->dispatch("history");
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
  return reply({owner_->menuRevision(), QVariant::fromValue(menuLayout(id, depth, names, owner_->updateLabel()))});
 }
 if (member == "GetGroupProperties" && sig == "aias") {
  auto ids = qdbus_cast<QList<int>>(args.at(0)); const auto names = qdbus_cast<QStringList>(args.at(1));
  if (ids.size() > 64 || !validNames(names)) return error();
  if (ids.isEmpty()) for (int id = 0; id <= entryCount; ++id) ids.append(id);
  NativePropertyList values;
  for (int id : ids) if (knownId(id)) values.append({id, menuProperties(id, names, owner_->updateLabel())});
  return reply({QVariant::fromValue(values)});
 }
 if (member == "GetProperty" && sig == "is") {
  int id = args.at(0).toInt(); if (!knownId(id)) return error();
  const auto name = args.at(1).toString(); if (name.size() > 128) return error(); auto props = menuProperties(id, {}, owner_->updateLabel());
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
void NativeTray::setActivity(const QColor& color,const QString& description,int percent) {d_->setActivity(color,description,percent);}
void NativeTray::setUpdateLabel(const QString& label) {d_->setUpdateLabel(label);}
void NativeTrayPrivate::setUpdateLabel(const QString& label) {
 if (label == updateLabel_) return;
 updateLabel_ = label; ++menuRevision_;
 // Hosts that track item properties get the change directly; the others
 // re-read the layout on LayoutUpdated(revision, parent 0).
 auto properties = QDBusMessage::createSignal("/Menu", menuInterface, "ItemsPropertiesUpdated");
 properties.setArguments({QVariant::fromValue(NativePropertyList{{updateId, menuProperties(updateId, {"label", "visible"}, label)}}),
                          QVariant::fromValue(NativeRemovedList{})});
 bus_.send(properties);
 auto signal = QDBusMessage::createSignal("/Menu", menuInterface, "LayoutUpdated");
 signal.setArguments({menuRevision_, 0}); bus_.send(signal);
}
void NativeTray::notify(const QString& title, const QString& message, bool warning, const QUrl& link, const QList<NotificationAction>& actions) { d_->notify(title, message, warning, link, actions); }
}
#include "NativeTray.moc"
