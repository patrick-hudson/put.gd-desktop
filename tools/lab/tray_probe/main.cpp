// Read the genuine desktop watcher and this resident's SNI export. No mock host.
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

struct Image { int width = 0, height = 0; QByteArray bytes; };
using Images = QList<Image>;
struct Tooltip { QString icon; Images images; QString title, description; };
Q_DECLARE_METATYPE(Image)
Q_DECLARE_METATYPE(Images)
Q_DECLARE_METATYPE(Tooltip)
QDBusArgument& operator<<(QDBusArgument& a, const Image& v) {
 a.beginStructure(); a << v.width << v.height << v.bytes; a.endStructure(); return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, Image& v) {
 a.beginStructure(); a >> v.width >> v.height >> v.bytes; a.endStructure(); return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const Tooltip& v) {
 a.beginStructure(); a << v.icon << v.images << v.title << v.description; a.endStructure(); return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, Tooltip& v) {
 a.beginStructure(); a >> v.icon >> v.images >> v.title >> v.description; a.endStructure(); return a;
}

static int emitJson(const QJsonObject& value, int code = 0) {
 const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
 std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
 return code;
}
static int fail(const char* code) { return emitJson({{"ok", false}, {"error", code}}, 1); }

int main(int argc, char** argv) {
 QCoreApplication app(argc, argv);
 QCommandLineParser parser;
 parser.addHelpOption();
 parser.addOption({"pid", "Exact resident process ID", "pid"});
 parser.process(app);
 bool valid = false;
 const auto pid = parser.value("pid").toUInt(&valid);
 if (!valid || !pid) return fail("invalid_pid");
 qDBusRegisterMetaType<Image>(); qDBusRegisterMetaType<Images>(); qDBusRegisterMetaType<Tooltip>();
 const auto bus = QDBusConnection::sessionBus();
 if (!bus.isConnected() || !bus.interface()) return fail("session_bus_unavailable");
 auto properties = [&](const QString& service, const QString& path, const QString& interface) {
  auto request = QDBusMessage::createMethodCall(service, path, "org.freedesktop.DBus.Properties", "GetAll");
  request << interface;
  return QDBusReply<QVariantMap>(bus.call(request, QDBus::Block, 2000));
 };
 const QString watcher = "org.kde.StatusNotifierWatcher";
 const auto owner = bus.interface()->serviceOwner(watcher);
 if (!owner.isValid()) return fail("native_tray_owner_unavailable");
 // Anchor GetAll to this unique owner; a replacement cannot mix host evidence.
 const auto host = properties(owner.value(), "/StatusNotifierWatcher", watcher);
 if (!host.isValid() || !host.value().value("IsStatusNotifierHostRegistered").toBool())
  return fail("native_tray_host_unavailable");
 const auto entries = qdbus_cast<QStringList>(host.value().value("RegisteredStatusNotifierItems"));
 QString service, path;
 QVariantMap item;
 int matches = 0;
 for (const auto& entry : entries) {
  const auto slash = entry.indexOf('/');
  const auto candidateService = slash < 0 ? entry : entry.left(slash);
  const auto candidatePath = slash < 0 ? QString("/StatusNotifierItem") : entry.mid(slash);
  const auto candidatePid = bus.interface()->servicePid(candidateService);
  if (!candidatePid.isValid() || candidatePid.value() != pid) continue;
  const auto reply = properties(candidateService, candidatePath, "org.kde.StatusNotifierItem");
  if (!reply.isValid() || reply.value().value("Id").toString() != "putgd") continue;
  ++matches; service = candidateService; path = candidatePath; item = reply.value();
 }
 if (matches != 1) return fail("resident_registered_item_not_unique");
 const auto tooltip = qdbus_cast<Tooltip>(item.value("ToolTip"));
 const auto pixmaps = qdbus_cast<Images>(item.value("IconPixmap"));
 QJsonArray icons;
 for (const auto& pixmap : pixmaps) {
  if (pixmap.width < 1 || pixmap.height < 1 || pixmap.width > 512 || pixmap.height > 512 ||
      pixmap.bytes.size() != pixmap.width * pixmap.height * 4) return fail("invalid_icon_pixmap");
  QJsonObject colors;
  for (qsizetype offset = 0; offset < pixmap.bytes.size(); offset += 4) {
   if (static_cast<unsigned char>(pixmap.bytes[offset]) != 255) continue;
   const auto color = QString::fromLatin1("#" + pixmap.bytes.mid(offset + 1, 3).toHex());
   colors[color] = colors.value(color).toInt() + 1;
  }
  icons.append(QJsonObject{{"width", pixmap.width}, {"height", pixmap.height},
   {"sha256", QString::fromLatin1(QCryptographicHash::hash(pixmap.bytes, QCryptographicHash::Sha256).toHex())},
   {"opaqueColors", colors}});
 }
 if (icons.isEmpty()) return fail("native_tray_icon_missing");
 // A host change during collection invalidates this observation.
 const auto ownerAfter = bus.interface()->serviceOwner(watcher);
 const auto pidAfter = bus.interface()->servicePid(service);
 if (!ownerAfter.isValid() || ownerAfter.value() != owner.value() ||
     !pidAfter.isValid() || pidAfter.value() != pid) return fail("tray_identity_changed");
 return emitJson({{"ok", true}, {"pid", qint64(pid)}, {"hostRegistered", true}, {"registered", true},
  {"watcherOwner", owner.value()}, {"service", service}, {"path", path},
  {"status", item.value("Status").toString()}, {"title", tooltip.title},
  {"description", tooltip.description}, {"icons", icons}});
}
