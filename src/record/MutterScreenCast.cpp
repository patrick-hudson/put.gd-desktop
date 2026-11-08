#include "MutterScreenCast.h"
#include "DebugLog.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QPointer>

namespace putgd::record {
namespace {
constexpr auto service = "org.gnome.Mutter.ScreenCast";
constexpr auto rootPath = "/org/gnome/Mutter/ScreenCast";
constexpr auto sessionInterface = "org.gnome.Mutter.ScreenCast.Session";
constexpr auto streamInterface = "org.gnome.Mutter.ScreenCast.Stream";
class NodeSink final : public QObject {
    Q_OBJECT
public:
    std::function<void(uint)> handler;
public slots:
    void added(uint node) { if (handler) handler(node); }
};
}

MutterScreenCast::MutterScreenCast(QObject* parent) : QObject(parent) {}
MutterScreenCast::~MutterScreenCast() { close(); }

bool MutterScreenCast::available(QJsonObject& facts, QString* reason) {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) { if (reason) *reason = "session_bus_unavailable"; return false; }
    const auto owned = bus.interface()->isServiceRegistered(QString::fromLatin1(service));
    facts["service"] = bool(owned);
    if (!owned) { if (reason) *reason = "mutter_screencast_unavailable"; return false; }
    return true;
}

void MutterScreenCast::fail(const Error& error) {
    debugLog("mutter screencast: " + error.code + " " + error.message);
    auto done = std::exchange(done_, {});  // taken first: close() reports "cancelled" to a pending caller
    close();
    if (done) done(error, {});
}

void MutterScreenCast::create(const char* method, const QList<QVariant>& arguments, const QRect& logical, const Error& refused, const QString& label) {
    auto bus = QDBusConnection::sessionBus();
    const QDBusReply<QDBusObjectPath> created = bus.call(QDBusMessage::createMethodCall(service, rootPath, service, "CreateSession") << QVariantMap{}, QDBus::Block, 10000);
    if (!created.isValid()) { fail({"recording_source_failed", "Mutter refused a screen cast session: " + created.error().message(), {}}); return; }
    session_ = created.value().path();
    // Closed comes when Mutter ends the session itself: a recorded window went away.
    bus.connect(service, session_, sessionInterface, "Closed", this, SLOT(sessionClosed()));
    QDBusMessage record = QDBusMessage::createMethodCall(service, session_, sessionInterface, QString::fromLatin1(method));
    record.setArguments(arguments);
    const QDBusReply<QDBusObjectPath> stream = bus.call(record, QDBus::Block, 10000);
    if (!stream.isValid()) { auto error = refused; error.message += ": " + stream.error().message(); fail(error); return; }
    debugLog(QStringLiteral("mutter screencast: session %1 %2").arg(session_, label));
    start(stream.value().path(), logical);
}

void MutterScreenCast::recordMonitor(const QString& connector, const QRect& logical, Done done) {
    if (done_ || !session_.isEmpty()) { done({"recording_state", "A Mutter screen cast is already open", {}}, {}); return; }
    done_ = std::move(done);
    create("RecordMonitor", {connector, QVariantMap{{"cursor-mode", uint(1)}}}, logical, {"recording_target", "Mutter has no monitor with this connector", {{"identity", connector}}}, QStringLiteral("monitor '%1'").arg(connector));
}

void MutterScreenCast::recordArea(const QRect& logical, Done done) {
    if (done_ || !session_.isEmpty()) { done({"recording_state", "A Mutter screen cast is already open", {}}, {}); return; }
    done_ = std::move(done);
    create("RecordArea", {logical.x(), logical.y(), logical.width(), logical.height(), QVariantMap{{"cursor-mode", uint(1)}}}, logical, {"recording_target", "Mutter refused the area", {}},
           QStringLiteral("area %1,%2 %3x%4").arg(logical.x()).arg(logical.y()).arg(logical.width()).arg(logical.height()));
}

void MutterScreenCast::recordWindow(quint64 windowId, const QRect& logical, Done done) {
    if (done_ || !session_.isEmpty()) { done({"recording_state", "A Mutter screen cast is already open", {}}, {}); return; }
    done_ = std::move(done);
    create("RecordWindow", {QVariantMap{{"window-id", QVariant::fromValue<qulonglong>(windowId)}, {"cursor-mode", uint(1)}}}, logical,
           {"recording_target", "Mutter has no window with this id", {{"identity", QString::number(windowId)}}}, QStringLiteral("window %1").arg(windowId));
}

void MutterScreenCast::sessionClosed() {
    debugLog("mutter screencast: session closed by Mutter");
    const bool streaming = !session_.isEmpty() && !done_;
    if (auto done = std::exchange(done_, {})) done({"recording_source_lost", "Mutter closed the screen cast", {}}, {});
    if (!session_.isEmpty()) QDBusConnection::sessionBus().disconnect(service, session_, sessionInterface, "Closed", this, SLOT(sessionClosed()));
    session_.clear();
    stream_.clear();
    if (streaming) emit lost();
}

void MutterScreenCast::start(const QString& stream, const QRect& logical) {
    stream_ = stream;
    auto* sink = new NodeSink;
    sink->setParent(this);
    QPointer<MutterScreenCast> guard(this);
    sink->handler = [guard, logical, sink](uint node) {
        sink->deleteLater();
        if (!guard) return;
        debugLog(QStringLiteral("mutter screencast: PipeWire node %1").arg(node));
        if (auto done = std::exchange(guard->done_, {})) done({}, {node, logical});
    };
    QDBusConnection::sessionBus().connect(service, stream_, streamInterface, "PipeWireStreamAdded", sink, SLOT(added(uint)));
    const QDBusReply<void> started = QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(service, session_, sessionInterface, "Start"), QDBus::Block, 10000);
    if (!started.isValid()) fail({"recording_source_failed", "Mutter could not start the screen cast: " + started.error().message(), {}});
}

void MutterScreenCast::close() {
    if (session_.isEmpty()) return;
    QDBusConnection::sessionBus().disconnect(service, session_, sessionInterface, "Closed", this, SLOT(sessionClosed()));
    QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(service, session_, sessionInterface, "Stop"), 5000);
    debugLog("mutter screencast: session stopped");
    session_.clear();
    stream_.clear();
    if (auto done = std::exchange(done_, {})) done({"cancelled", "Screen cast canceled", {}}, {});
}
}
#include "MutterScreenCast.moc"
