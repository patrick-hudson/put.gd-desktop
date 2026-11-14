#include "NativeTray.h"
#include "core/NotificationServer.h"
#include "core/ActivationToken.h"
#include "core/Settings.h"
#include <QSet>
#include <QKeySequence>
#include "IconArt.h"
#include "Theme.h"
#include <QCoreApplication>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QProcess>
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
#include <QElapsedTimer>
#include <utility>
#include <array>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtEndian>

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
// A dismissal command (notifications::dismissCommand) that has not ended by then is stopped.
constexpr int dismissTimeoutMs = 900;
// IDs are 1-based table positions and stay stable; rootOrder sets the display
// order. An entry with a parent belongs to that entry's submenu. A retired
// entry keeps its slot, so later IDs don't move, and is unknown on the wire.
struct Entry { const char* label; const char* action; int parent; };
constexpr int retired = -1;
constexpr Entry entries[] {
 {"Capture region", "region", 0}, {"Capture window", "window", 0},
 {"Capture desktop", "desktop", 0}, {"Upload clipboard", "clipboard", 0},
 {"Capture region & edit", "edit", 0},
 {"", "", 0}, {"Settings", "settings", 0}, {"History", "history", 0},
 // 9 was "Recent activity", which opened the same History page.
 {"", "", retired}, {"", "", 0}, {"About put.gd", "about", 0}, {"Quit put.gd", "quit", 0},
 {"Edit image", "", 0}, {"Edit clipboard image", "edit-clipboard", 13}, {"Edit an image file…", "edit-file", 13},
 {"Record screen", "record", 0},
 // Hidden until a stable update is available; its label names the version (#293).
 {"Update available", "update", 0}, {"Destinations", "destinations", 0},
 {"Upload status", "", 0}, {"Cancel upload", "cancel-upload", 0}, {"", "", 0}, {"Failed uploads", "failed-uploads", 0},
 {"Capture output", "", 0}, {"", "", 23}, {"", "", 23}, {"", "", 23}, {"", "", 23}, {"", "", 23}, {"", "", 23}, {"Change output…", "capture-settings", 0},
 // #1363, from 31: Capture monitor, Repeat last region, the capture delay's
 // radio submenu (33, choices 34 to 37) and Capture monitor's output row (38).
 {"Capture monitor", "monitor", 0}, {"Repeat last region", "repeat", 0},
 {"Capture delay", "", 0}, {"None", "delay-0", 33}, {"3 seconds", "delay-3", 33}, {"5 seconds", "delay-5", 33}, {"10 seconds", "delay-10", 33},
 {"", "", 23}, {"Capture scrolling region", "scroll", 0}, {"", "", 23},
 // 41, Copy text (#1428): after Capture scrolling region. It has no output, so no output row.
 {"Copy text from region", "text", 0}
};
constexpr int delayMenuId = 33, firstDelayId = 34, lastDelayId = 37, extraOutputId = 38;
// Capture output rows: 23 is the summary; 24–29, 38 and 40 are action rows.
int outputRow(int id) { return id >= 23 && id <= 29 ? id - 23 : id == extraOutputId ? 7 : id == 40 ? 8 : -1; }
constexpr int updateId = 17, uploadStatusId = 19, cancelUploadId = 20, uploadSeparatorId = 21;
constexpr int entryCount = sizeof(entries) / sizeof(entries[0]);
QList<int> childrenOf(int id) {
 QList<int> result;
 if (id == 0) {
  result.append({uploadStatusId,cancelUploadId,uploadSeparatorId,23,30});
  for(const auto& action:Settings::shortcutOrder())for(int child=1;child<=entryCount;++child)if(QLatin1String(entries[child-1].action)==action){result.append(child);if(action=="edit")result.append(13);if(action=="desktop")result.append(delayMenuId);}
  result.append({6,22,8,18,7,17,10,11,12});
 }
 else for (int i = 1; i <= entryCount; ++i) if (entries[i-1].parent == id) result.append(i);
 return result;
}
bool knownId(int id) { return id == 0 || (id > 0 && id <= entryCount && entries[id-1].parent != retired); }
bool validNames(const QStringList& names) {
 if (names.size() > 32) return false;
 for (const auto& name : names) if (name.size() > 128) return false;
 return true;
}
QVariantMap menuProperties(int id, const QStringList& names = {}, const QString& update = {}, const QHash<QString,QString>& shortcuts = {}, const QString& upload = {},int uploads=1,bool failed=false,const QString& recording={},const QString& attention="Failed uploads",const QStringList& outputs={},int delay=0) {
 QVariantMap props;
 if (id == 0 || !childrenOf(id).isEmpty()) props.insert("children-display", "submenu");
 if (id != 0) {
  const auto& e = entries[id-1];
  if (outputRow(id)>=0){props.insert("label",outputs.value(outputRow(id)));if(id!=23)props.insert("enabled",false);}
  else if (!*e.label) props.insert("type", "separator");
  else props.insert("label", id == updateId && !update.isEmpty() ? update : QString::fromUtf8(e.label));
  // Always explicit: Plasma's and GNOME's hosts update a known item only with the
  // properties a layout carries, so an omitted default would keep it hidden (#304).
  if (id == updateId) props.insert("visible", !update.isEmpty());
  if(id==uploadStatusId||id==cancelUploadId||id==uploadSeparatorId)props.insert("visible",!upload.isEmpty());
  if(id==uploadStatusId){props.insert("label",upload);props.insert("enabled",false);}
  if(id==cancelUploadId){props.insert("enabled",!upload.isEmpty());props.insert("label",uploads>1?"Cancel all uploads…":"Cancel upload");}
  if(id==22){props.insert("visible",failed);props.insert("label",attention);}
  if(id==16&&!recording.isEmpty())props.insert("label","Record screen — "+recording);
  // The capture delay (#1363): the submenu names the choice, its rows are a radio group.
  if(id==delayMenuId)props.insert("label",delay>0?QStringLiteral("Capture delay: %1 seconds").arg(delay):QStringLiteral("Capture delay: none"));
  if(id>=firstDelayId&&id<=lastDelayId){props.insert("toggle-type","radio");props.insert("toggle-state",QString::fromLatin1(e.action).mid(6).toInt()==delay?1:0);}
  const QKeySequence shortcut(shortcuts.value(QString::fromLatin1(e.action)));
  if(Settings::shortcutOrder().contains(QString::fromLatin1(e.action))){
   QList<QStringList> keys;for(int i=0;i<shortcut.count();++i){const auto chord=shortcut[i];const auto modifiers=chord.keyboardModifiers();QStringList parts;if(modifiers&Qt::ControlModifier)parts<<"Control";if(modifiers&Qt::AltModifier)parts<<"Alt";if(modifiers&Qt::ShiftModifier)parts<<"Shift";if(modifiers&Qt::MetaModifier)parts<<"Super";parts<<QKeySequence(chord.key()).toString(QKeySequence::PortableText);keys.append(parts);}
   // An unbound action sends no shortcut: libdbusmenu rejects an empty one ("too many keys", #1294).
   if(!keys.isEmpty())props.insert("shortcut",QVariant::fromValue(keys));
  }
 }
 if (!names.isEmpty()) for (auto it = props.begin(); it != props.end();) {
  if (!names.contains(it.key())) it = props.erase(it); else ++it;
 }
 return props;
}
NativeLayout menuLayout(int id, int depth, const QStringList& names, const QString& update,const QHash<QString,QString>& shortcuts,const QString& upload,int uploads,bool failed,const QString& recording,const QString& attention,const QStringList& outputs,int delay) {
 NativeLayout result{id, menuProperties(id, names, update, shortcuts, upload,uploads,failed,recording,attention,outputs,delay), {}};
 // depth -1 expands every level; 0 returns this item alone.
 if (depth != 0) for (int child : childrenOf(id))
  result.children.append(QDBusVariant(QVariant::fromValue(menuLayout(child, depth < 0 ? depth : depth - 1, names, update, shortcuts, upload,uploads,failed,recording,attention,outputs,delay))));
 return result;
}
// A minimal PNG (RGBA, no filtering) for the named-icon file below. QImageWriter would load every
// image plugin into the resident on Qt 6.8 and older, which costs memory for one small file.
QByteArray pngBytes(const QImage& source) {
 const auto image=source.convertToFormat(QImage::Format_RGBA8888);
 static const auto table=[]{std::array<quint32,256> t{};for(quint32 n=0;n<256;++n){quint32 c=n;for(int k=0;k<8;++k)c=c&1?0xedb88320u^(c>>1):c>>1;t[n]=c;}return t;}();
 const auto crc=[](const QByteArray& data){quint32 c=0xffffffffu;for(const char b:data)c=table[(c^quint8(b))&0xff]^(c>>8);return c^0xffffffffu;};
 const auto chunk=[&](const char* type,const QByteArray& data){QByteArray out(4,'\0');qToBigEndian<quint32>(quint32(data.size()),out.data());QByteArray body=QByteArray(type,4)+data;QByteArray tail(4,'\0');qToBigEndian<quint32>(crc(body),tail.data());return out+body+tail;};
 QByteArray header(13,'\0');qToBigEndian<quint32>(quint32(image.width()),header.data());qToBigEndian<quint32>(quint32(image.height()),header.data()+4);header[8]=8;header[9]=6;
 QByteArray raw;raw.reserve(image.height()*(image.width()*4+1));
 for(int y=0;y<image.height();++y){raw.append('\0');raw.append(reinterpret_cast<const char*>(image.constScanLine(y)),image.width()*4);}
 const auto zlib=qCompress(raw,9).mid(4); // qCompress prefixes the zlib stream with its length
 return QByteArray("\x89PNG\r\n\x1a\n",8)+chunk("IHDR",header)+chunk("IDAT",zlib)+chunk("IEND",{});
}
NativeImages brandIcon(const IconArt::TrayIcon& icon=IconArt::TrayIcon{Theme::trayTints(false).idle}) {
 NativeImages result;
 QList<int> sizes{24,48,72};if(icon.glyph==IconArt::TrayGlyph::Turkey&&!icon.recording&&icon.percent!=-2)sizes.append(16);
 for (int size : sizes) {
  const auto image=IconArt::liftTrayAtSize(icon,size);
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
 void dispatch(const QString& action, const QString& activationToken = {});
 // The xdg-activation token a tray host hands over just before it activates the
 // item or one of its menu entries (#657). The next click takes it, whether or
 // not that click dispatches anything; an old one belonged to an earlier click.
 void provideActivationToken(const QString& token);
 QString takeActivationToken();
 void aboutToShow(){emit q_->menuAboutToShow();}
 void setActivity(const QColor& color,const QString& description,int percent,int recording,const QString& title,IconArt::TrayGlyph glyph);
 void notify(const QString& title, const QString& message, bool warning, const QUrl& link, const QList<NotificationAction>& actions);
 // The update entry's label, empty while it is hidden, and the layout revision
 // that changes with it.
 const QString& updateLabel() const { return updateLabel_; }
 uint menuRevision() const { return menuRevision_; }
 void setUpdateLabel(const QString& label);
 void setCommands(const QHash<QString,QString>& shortcuts,const QString& upload,int uploads,bool failed,const QString& recording,const QString& attention);
 int uploadCount()const{return uploads_;}bool failed()const{return failed_;}const QString& recordingText()const{return recordingText_;}const QString& attentionLabel()const{return attentionLabel_;}
 const QHash<QString,QString>& shortcuts()const{return shortcuts_;}
 const QStringList& outputs()const{return outputs_;}
 int delay()const{return delay_;}
 void setDelay(int seconds);
 void setOutput(const QString& summary,const QStringList& details);
 const QString& uploadStatus()const{return uploadStatus_;}
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
 QHash<uint,QString> openNotifications_; // every one sent and not reported closed, with its summary
 // closeNotifications(): Notify calls made so far and those it covers; ids it
 // asked the server to close, answers and dismissals it still waits for.
 quint64 notifySerial_=0,closeThrough_=0;
 QSet<uint> closing_;
 int arriving_=0,dismissing_=0;
 QList<std::function<void()>> gone_;
 QTimer closeDeadline_;
 void sendClose(uint id);
 void dismissOnScreen(const QStringList& summaries);
 void closedCheck(bool deadline=false);
 int uploads_=0;bool failed_=false;QString updateLabel_,uploadStatus_,attentionLabel_="Failed uploads";QHash<QString,QString> shortcuts_;
 QStringList outputs_;
 int delay_=0;
 uint menuRevision_ = 1;
 QString activationToken_;
 QElapsedTimer activationTokenAge_;
 void closeNotification(uint id);
public:
 int closeNotifications(std::function<void()> gone);
private:
 std::unique_ptr<TrayExport> item_, menu_;
 NativeImages icons_;
 QColor color_ = Theme::trayTints(false).idle;
 int percent_ = -1;
 IconArt::TrayGlyph glyph_ = IconArt::TrayGlyph::Mark;
 int recording_ = 0;
 QString recordingText_;
 QString activityText_,activityTitle_="put.gd";
 QTimer retry_;
 quint64 generation_ = 0, hostQuery_ = 0;
 int attempts_ = 0, notificationsInFlight_ = 0;
 bool exportedItem_ = false, exportedMenu_ = false, registeredService_ = false;
 bool hostPresent_ = false, available_ = false, registrationInFlight_ = false;
 bool ownerKnown_ = false, hostKnown_ = false, registrationSettled_ = false, availabilityKnown_ = false;
 // Ayatana's and Canonical's indicator-application-service ignore IconPixmap (#1294; Ubuntu Budgie 26.04's
 // default tray applet), so for them the icon is also a PNG file, its absolute path in IconName.
 // A new file per change, because those hosts cache an icon by name.
 bool namedIcon_ = false, detecting_ = false;
 QString iconDirectory_, iconName_;
 QStringList iconFiles_;
 quint64 iconSerial_ = 0;
 void detectIconHost(quint64 generation);
 bool writeNamedIcon();
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
  ++notificationGeneration_;notificationLinks_.clear();notificationActions_.clear();openNotifications_.clear();
  // The server that held them is gone, and what it showed with it.
  closing_.clear();arriving_=0;closedCheck();
 });
 closeDeadline_.setSingleShot(true);
 connect(&closeDeadline_,&QTimer::timeout,this,[this]{closedCheck(true);});
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
 for (const auto& file : iconFiles_) QFile::remove(file);
 if (exportedMenu_) bus_.unregisterObject("/Menu");
 if (exportedItem_) bus_.unregisterObject("/StatusNotifierItem");
}
void NativeTrayPrivate::setOwner(const QString& owner) {
 if (ownerKnown_ && owner == owner_) return;
 ownerKnown_ = true; hostKnown_ = owner.isEmpty(); registrationSettled_ = owner.isEmpty(); availabilityKnown_ = false;
 ++generation_; ++hostQuery_; owner_ = owner; registeredOwner_.clear(); hostPresent_ = false;
 attempts_ = 0; retry_.stop(); updateAvailability();
 if (!owner_.isEmpty()) detectIconHost(generation_); else detecting_ = false;
}
void NativeTrayPrivate::detectIconHost(quint64 generation) {
 // Registration waits for this answer, so the host's first GetAll already has the right icon (review #1275).
 detecting_ = true;
 auto call = QDBusMessage::createMethodCall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixProcessID");
 call << owner_;
 auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(call, callTimeoutMs), this);
 connect(pending, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher* p) {
  const QDBusPendingReply<uint> result = *p; p->deleteLater();
  if (generation != generation_) return;
  detecting_ = false;
  // An upgraded package leaves the running service's exe as "... (deleted)".
  auto program = result.isError() ? QString{} : QFileInfo(QFile::symLinkTarget(QString("/proc/%1/exe").arg(result.value()))).fileName();
  if (program.endsWith(" (deleted)")) program.chop(int(qstrlen(" (deleted)")));
  // Without a file to name, keep the pixmap-only item rather than announce an empty name.
  const bool named = program.endsWith("indicator-application-service") && (namedIcon_ || writeNamedIcon());
  if (named != namedIcon_) {
   namedIcon_ = named;
   // Already registered with this host: tell it the icon changed.
   if (!registeredOwner_.isEmpty()) {
    const auto props = properties(false);
    auto signal = QDBusMessage::createSignal("/StatusNotifierItem", "org.freedesktop.DBus.Properties", "PropertiesChanged");
    signal.setArguments({QString::fromLatin1(itemInterface), QVariantMap{{"IconName", props["IconName"]}, {"IconThemePath", props["IconThemePath"]}}, QStringList{}}); bus_.send(signal);
    bus_.send(QDBusMessage::createSignal("/StatusNotifierItem", itemInterface, "NewIcon"));
   }
  }
  queryHost(); registerItem();
 });
}
bool NativeTrayPrivate::writeNamedIcon() {
 if (iconDirectory_.isEmpty()) {
  const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if (runtime.isEmpty()) return false;
  iconDirectory_ = runtime + "/putgd/tray";
  if (!QDir().mkpath(iconDirectory_)) { iconDirectory_.clear(); return false; }
  QFile::setPermissions(iconDirectory_, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
 }
 // No dashes: GTK expands a dashed name into fallbacks (putgd-tray-1 -> putgd) and matches the installed app
 // icon in the themes before it reads this unthemed file.
 const auto name = QString("putgd_tray_%1_%2").arg(QCoreApplication::applicationPid()).arg(++iconSerial_);
 QFile file(iconDirectory_ + "/" + name + ".png");
 if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(pngBytes(IconArt::liftTray({color_, percent_, recording_, glyph_}, 2))) < 0) return false;
 file.close();
 iconName_ = name;
 // Keep the last few: during a burst of changes (upload progress) the host can still be loading a
 // name it was told about, and deleting that file leaves it on the missing-icon glyph (#1294).
 iconFiles_.append(file.fileName());
 while (iconFiles_.size() > 4) QFile::remove(iconFiles_.takeFirst());
 return true;
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
 if (detecting_) return;  // detectIconHost registers when it knows the host
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
void NativeTrayPrivate::dispatch(const QString& action, const QString& token) {
 QTimer::singleShot(0, q_, [guard = QPointer<NativeTray>(q_), action, token] {
  if (guard) emit guard->actionActivated(action, token);
 });
}
void NativeTrayPrivate::provideActivationToken(const QString& token) {
 activationToken_ = token; activationTokenAge_.start();
}
QString NativeTrayPrivate::takeActivationToken() {
 const bool fresh = activationTokenAge_.isValid() && activationTokenAge_.elapsed() < 10000;
 activationTokenAge_.invalidate();
 auto token = std::exchange(activationToken_, {});
 return fresh ? token : QString{};
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
 // Notifications don't need a tray (#1105): GNOME without a StatusNotifier
 // extension has none, and a file-manager upload's result must still show.
 if (notificationsInFlight_ >= 8) return;
 auto call = QDBusMessage::createMethodCall(notificationService, notificationPath, notificationService, "Notify");
 QVariantMap hints{{"urgency", QVariant::fromValue(static_cast<uchar>(warning ? 2 : 1))}, {"desktop-entry", "putgd"}};
 // Critical until seen; transient only where the server would restore it after a restart (#1298).
 if (warning && putgd::notifications::failuresTransient(notificationServer_)) hints.insert("transient", true);
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
 const auto serial=++notifySerial_;const auto summary=title.left(512);
 auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(call, callTimeoutMs), this);
 connect(pending, &QDBusPendingCallWatcher::finished, this, [this,generation,serial,summary,link,values,resident](QDBusPendingCallWatcher* p) {
  const QDBusPendingReply<uint> result=*p;
  --notificationsInFlight_; p->deleteLater();
  // closeNotifications() ran before this id was known: it is closed now.
  const bool late=generation==notificationGeneration_&&serial<=closeThrough_;
  if(late&&arriving_>0)--arriving_;
  if(generation!=notificationGeneration_||result.isError()){if(late)closedCheck();return;}
  if(late){closing_.insert(result.value());sendClose(result.value());dismissOnScreen({summary});closedCheck();return;}
  if(!link.isEmpty())notificationLinks_.insert(result.value(),link);
  if(!values.isEmpty())notificationActions_.insert(result.value(),values);
  if(resident)residentNotifications_.insert(result.value());
  // A server keeps what it showed in a list; the newest few are what can still be on screen.
  if(openNotifications_.size()<64)openNotifications_.insert(result.value(),summary);
 });
}
void NativeTrayPrivate::sendClose(uint id) {
 auto call=QDBusMessage::createMethodCall(notificationService,notificationPath,notificationService,"CloseNotification");
 call<<id;bus_.asyncCall(call,callTimeoutMs);
}
int NativeTrayPrivate::closeNotifications(std::function<void()> gone) {
 const auto open=std::exchange(openNotifications_,{});
 closeThrough_=notifySerial_;arriving_=notificationsInFlight_;
 QStringList summaries;
 for(auto it=open.cbegin();it!=open.cend();++it){
  const uint id=it.key();
  notificationLinks_.remove(id);notificationActions_.remove(id);residentNotifications_.remove(id);
  closing_.insert(id);sendClose(id);
  if(!summaries.contains(it.value()))summaries.append(it.value());
 }
 const int count=int(open.size())+arriving_;
 if(count&&gone){gone_.append(std::move(gone));closeDeadline_.start(NativeTray::closeConfirmMs);}
 dismissOnScreen(summaries);
 closedCheck();
 return count;
}
// A server that keeps a closed notification on screen is asked in its own way
// (notifications::dismissCommand), by the process that owns the bus name.
void NativeTrayPrivate::dismissOnScreen(const QStringList& summaries) {
 if(summaries.isEmpty()||putgd::notifications::dismissCommand(notificationServer_,1,QStringLiteral("x")).isEmpty())return;
 auto call=QDBusMessage::createMethodCall("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","GetConnectionUnixProcessID");
 call<<QString(notificationService);
 ++dismissing_;
 auto* pending=new QDBusPendingCallWatcher(bus_.asyncCall(call,callTimeoutMs),this);
 connect(pending,&QDBusPendingCallWatcher::finished,this,[this,summaries](QDBusPendingCallWatcher* p) {
  const QDBusPendingReply<uint> owner=*p;p->deleteLater();--dismissing_;
  for(const auto& summary:summaries){
   if(owner.isError())break;
   const auto command=putgd::notifications::dismissCommand(notificationServer_,owner.value(),summary);
   if(command.isEmpty())continue;
   auto* process=new QProcess(this);
   // The shell is found through its own runtime directory, which is not this
   // process's when put.gd runs with another one (an isolated profile): there
   // `qs ipc` answered "No instance found" and the cards stayed.
   auto environment=QProcessEnvironment::systemEnvironment();
   QFile theirs(QStringLiteral("/proc/%1/environ").arg(owner.value()));
   if(theirs.open(QIODevice::ReadOnly))
    for(const auto& entry:theirs.readAll().split('\0'))
     if(entry.startsWith("XDG_RUNTIME_DIR="))environment.insert(QStringLiteral("XDG_RUNTIME_DIR"),QString::fromLocal8Bit(entry.mid(16)));
   process->setProcessEnvironment(environment);
   process->setStandardInputFile(QProcess::nullDevice());process->setStandardOutputFile(QProcess::nullDevice());process->setStandardErrorFile(QProcess::nullDevice());
   ++dismissing_;
   // Once, whether it ran, could not be started or was stopped below.
   const auto ended=[this,process]{if(process->property("ended").toBool())return;process->setProperty("ended",true);--dismissing_;process->deleteLater();closedCheck();};
   connect(process,&QProcess::finished,this,ended);
   connect(process,&QProcess::errorOccurred,this,[ended](QProcess::ProcessError error){if(error==QProcess::FailedToStart)ended();});
   QTimer::singleShot(dismissTimeoutMs,process,[process]{process->kill();});
   process->start(command.first(),command.mid(1));
  }
  closedCheck();
 });
}
// Everything asked for has happened, or the deadline says to wait no longer.
void NativeTrayPrivate::closedCheck(bool deadline) {
 if(deadline)closing_.clear();
 if(gone_.isEmpty()||(!deadline&&(!closing_.isEmpty()||arriving_>0||dismissing_>0)))return;
 closeDeadline_.stop();
 for(const auto& gone:std::exchange(gone_,{}))QTimer::singleShot(0,q_,gone);
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
void NativeTrayPrivate::notificationClosed(uint id,uint) {
 notificationLinks_.remove(id);notificationActions_.remove(id);residentNotifications_.remove(id);openNotifications_.remove(id);
 if(closing_.remove(id))closedCheck();
}
void NativeTrayPrivate::setActivity(const QColor& color,const QString& description,int percent,int recording,const QString& title,IconArt::TrayGlyph glyph) {
 percent=qBound(-2,percent,100);
 const bool iconChanged=color!=color_||percent!=percent_||recording!=recording_||glyph!=glyph_;
 const auto tooltipTitle=title.isEmpty()?(percent==-2?QString("Needs attention"):percent>=0?QString("Uploading"):QString("put.gd")):title;
 const bool textChanged=description!=activityText_||tooltipTitle!=activityTitle_;
 if(!iconChanged&&!textChanged)return;
 color_=color;activityText_=description;activityTitle_=tooltipTitle;percent_=percent;recording_=recording;glyph_=glyph;
 if(iconChanged){icons_=brandIcon({color,percent,recording,glyph});if(namedIcon_)writeNamedIcon();}
 const auto props=properties(false);QVariantMap changed;
 if(textChanged)changed.insert("ToolTip",props["ToolTip"]);
 if(iconChanged){changed.insert("IconPixmap",props["IconPixmap"]);changed.insert("IconName",props["IconName"]);changed.insert("IconThemePath",props["IconThemePath"]);}
 auto signal=QDBusMessage::createSignal("/StatusNotifierItem","org.freedesktop.DBus.Properties","PropertiesChanged");
 signal.setArguments({QString::fromLatin1(itemInterface),changed,QStringList{}});bus_.send(signal);
 if(textChanged)bus_.send(QDBusMessage::createSignal("/StatusNotifierItem",itemInterface,"NewToolTip"));
 if(iconChanged)bus_.send(QDBusMessage::createSignal("/StatusNotifierItem",itemInterface,"NewIcon"));
}
QVariantMap NativeTrayPrivate::properties(bool menu) const {
 if (menu) return {{"Version", uint(3)}, {"TextDirection", "ltr"}, {"Status", "normal"}, {"IconThemePath", QStringList{}}};
 return {{"Category", "ApplicationStatus"}, {"Id", "putgd"}, {"Title", "put.gd"}, {"Status", "Active"}, {"WindowId", 0},
  {"IconThemePath", QString{}}, {"Menu", QVariant::fromValue(QDBusObjectPath("/Menu"))}, {"ItemIsMenu", false},
  // The file's absolute path, not a name in a theme path: GTK re-reads a search path only every few
  // seconds, so a host looking up a just-written name draws the missing-icon glyph (#1294).
  {"IconName", namedIcon_ ? iconDirectory_ + "/" + iconName_ + ".png" : QString{}}, {"IconPixmap", QVariant::fromValue(icons_)},
  {"OverlayIconName", ""}, {"OverlayIconPixmap", QVariant::fromValue(NativeImages{})},
  {"AttentionIconName", ""}, {"AttentionIconPixmap", QVariant::fromValue(NativeImages{})}, {"AttentionMovieName", ""},
  {"ToolTip", QVariant::fromValue(NativeTooltip{"", {}, activityTitle_, activityText_})}};
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
  <method name="ProvideXdgActivationToken"><arg type="s" direction="in"/></method>
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
 auto dispatchId = [this](int id, const QString& token) {
  if (id > 0 && knownId(id) && id!=uploadStatusId && (id!=cancelUploadId||!owner_->uploadStatus().isEmpty()) && (id!=22||owner_->failed()) && *entries[id-1].action)
   owner_->dispatch(QString::fromLatin1(entries[id-1].action), token);
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
   const auto token = owner_->takeActivationToken();
   if (member == "Activate") owner_->dispatch("history", token);
   if (member == "ContextMenu") {
    auto show = QDBusMessage::createSignal("/Menu", menuInterface, "ItemActivationRequested");
    show.setArguments({0, uint(0)}); bus.send(show);
   }
   return reply();
  }
  if (member == "Scroll" && sig == "is") return reply();
  if (member == "ProvideXdgActivationToken" && sig == "s") {
   const auto token = args.at(0).toString();
   if (!validActivationToken(token)) return error();
   owner_->provideActivationToken(token); return reply();
  }
  return error("org.freedesktop.DBus.Error.UnknownMethod");
 }
 if (member == "GetLayout" && sig == "iias") {
  const int id = args.at(0).toInt(), depth = args.at(1).toInt();
  const auto names = qdbus_cast<QStringList>(args.at(2));
  if (!knownId(id) || depth < -1 || !validNames(names)) return error();
  return reply({owner_->menuRevision(), QVariant::fromValue(menuLayout(id, depth, names, owner_->updateLabel(),owner_->shortcuts(),owner_->uploadStatus(),owner_->uploadCount(),owner_->failed(),owner_->recordingText(),owner_->attentionLabel(),owner_->outputs(),owner_->delay()))});
 }
 if (member == "GetGroupProperties" && sig == "aias") {
  auto ids = qdbus_cast<QList<int>>(args.at(0)); const auto names = qdbus_cast<QStringList>(args.at(1));
  if (ids.size() > 64 || !validNames(names)) return error();
  if (ids.isEmpty()) for (int id = 0; id <= entryCount; ++id) ids.append(id);
  NativePropertyList values;
  for (int id : std::as_const(ids)) if (knownId(id)) values.append({id, menuProperties(id, names, owner_->updateLabel(),owner_->shortcuts(),owner_->uploadStatus(),owner_->uploadCount(),owner_->failed(),owner_->recordingText(),owner_->attentionLabel(),owner_->outputs(),owner_->delay())});
  return reply({QVariant::fromValue(values)});
 }
 if (member == "GetProperty" && sig == "is") {
  int id = args.at(0).toInt(); if (!knownId(id)) return error();
  const auto name = args.at(1).toString(); if (name.size() > 128) return error(); auto props = menuProperties(id, {}, owner_->updateLabel(),owner_->shortcuts(),owner_->uploadStatus(),owner_->uploadCount(),owner_->failed(),owner_->recordingText(),owner_->attentionLabel(),owner_->outputs(),owner_->delay());
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
  if (args.at(1).toString() == "clicked") dispatchId(id, owner_->takeActivationToken());
  return reply();
 }
 if (member == "EventGroup" && sig == "a(isvu)") {
  const auto events = qdbus_cast<NativeEvents>(args.at(0)); if (events.size() > 64) return error();
  QList<int> missing; bool found = false;
  for (const auto& e : events) {
   if (!knownId(e.id)) missing.append(e.id);
   else { found = true; if (e.event == "clicked") dispatchId(e.id, owner_->takeActivationToken()); }
  }
  if (!events.isEmpty() && !found) return error();
  return reply({QVariant::fromValue(missing)});
 }
 if (member == "AboutToShow" && sig == "i") {
  if (!knownId(args.at(0).toInt())) return error();
  const auto revision=owner_->menuRevision();owner_->aboutToShow();return reply({revision!=owner_->menuRevision()});
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
void NativeTray::setActivity(const QColor& color,const QString& description,int percent,int recording,const QString& title,IconArt::TrayGlyph glyph) {d_->setActivity(color,description,percent,recording,title,glyph);}
void NativeTray::setUpdateLabel(const QString& label) {d_->setUpdateLabel(label);}
void NativeTray::setCommands(const QHash<QString,QString>& shortcuts,const QString& upload,int uploads,bool failed,const QString& recording,const QString& attention) {d_->setCommands(shortcuts,upload,uploads,failed,recording,attention);}
void NativeTrayPrivate::setCommands(const QHash<QString,QString>& shortcuts,const QString& upload,int uploads,bool failed,const QString& recording,const QString& attention) {
 if(shortcuts_==shortcuts&&uploadStatus_==upload&&uploads_==uploads&&failed_==failed&&recordingText_==recording&&attentionLabel_==attention)return;
 // An entry whose shortcut was cleared sends none any more (#1294); hosts that apply this signal must hear it removed.
 QSet<int> hadShortcut;for(int id=1;id<=entryCount;++id)if(knownId(id)&&menuProperties(id,{"shortcut"},updateLabel_,shortcuts_).contains("shortcut"))hadShortcut.insert(id);
 shortcuts_=shortcuts;uploadStatus_=upload;uploads_=uploads;failed_=failed;recordingText_=recording;attentionLabel_=attention;++menuRevision_;
 NativePropertyList changed;NativeRemovedList removed;
 for(int id=1;id<=entryCount;++id)if(knownId(id)){changed.append({id,menuProperties(id,{"label","visible","enabled","shortcut"},updateLabel_,shortcuts_,uploadStatus_,uploads_,failed_,recordingText_,attentionLabel_,outputs_,delay_)});if(hadShortcut.contains(id)&&!changed.last().properties.contains("shortcut"))removed.append({id,{"shortcut"}});}
 auto properties=QDBusMessage::createSignal("/Menu",menuInterface,"ItemsPropertiesUpdated");properties.setArguments({QVariant::fromValue(changed),QVariant::fromValue(removed)});bus_.send(properties);
 auto signal=QDBusMessage::createSignal("/Menu",menuInterface,"LayoutUpdated");signal.setArguments({menuRevision_,0});bus_.send(signal);
}
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
int NativeTray::closeNotifications(std::function<void()> gone) { return d_->closeNotifications(std::move(gone)); }
}
#include "NativeTray.moc"

namespace putgd {
void NativeTray::setOutput(const QString& summary,const QStringList& details){d_->setOutput(summary,details);}
void NativeTray::setCaptureDelay(int seconds){d_->setDelay(seconds);}
void NativeTrayPrivate::setDelay(int seconds){if(seconds==delay_)return;delay_=seconds;++menuRevision_;auto signal=QDBusMessage::createSignal("/Menu",menuInterface,"LayoutUpdated");signal.setArguments({menuRevision_,0});bus_.send(signal);}
void NativeTrayPrivate::setOutput(const QString& summary,const QStringList& details){QStringList values{summary};values.append(details);if(values==outputs_)return;outputs_=values;++menuRevision_;auto signal=QDBusMessage::createSignal("/Menu",menuInterface,"LayoutUpdated");signal.setArguments({menuRevision_,0});bus_.send(signal);}
}
