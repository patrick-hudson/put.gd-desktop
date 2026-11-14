#include "MutterScreenCast.h"
#include "DebugLog.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>
#include <QPointer>

namespace putgd::record {
namespace {
constexpr auto service = "org.gnome.Mutter.ScreenCast";
constexpr auto rootPath = "/org/gnome/Mutter/ScreenCast";
constexpr auto sessionInterface = "org.gnome.Mutter.ScreenCast.Session";
constexpr auto streamInterface = "org.gnome.Mutter.ScreenCast.Stream";
constexpr auto remoteService = "org.gnome.Mutter.RemoteDesktop";
constexpr auto remoteRootPath = "/org/gnome/Mutter/RemoteDesktop";
constexpr auto remoteSessionInterface = "org.gnome.Mutter.RemoteDesktop.Session";
// Mutter's cursor modes: 0 hidden, 1 embedded in the frames, 2 as metadata.
uint cursorMode(bool hidden) { return hidden ? 0u : 1u; }
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

bool MutterScreenCast::remoteDesktopAvailable(QString* reason) {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) { if (reason) *reason = "session_bus_unavailable"; return false; }
    if (!bus.interface()->isServiceRegistered(QString::fromLatin1(remoteService))) { if (reason) *reason = "mutter_remote_desktop_unavailable"; return false; }
    return true;
}

bool MutterScreenCast::linked() const {
    std::lock_guard lock(paths_);
    return !remote_.isEmpty();
}

void MutterScreenCast::create(const char* method, const QList<QVariant>& arguments, const QRect& logical, const Error& refused, const QString& label, bool linked) {
    auto bus = QDBusConnection::sessionBus();
    closed_.store(false);
    QVariantMap options;
    if (linked) {
        // The RemoteDesktop session first; the screen cast names it by its id.
        const QDBusReply<QDBusObjectPath> remote = bus.call(QDBusMessage::createMethodCall(remoteService, remoteRootPath, remoteService, "CreateSession"), QDBus::Block, 10000);
        if (!remote.isValid()) { fail({"scroll_unavailable", "Mutter refused a remote desktop session: " + remote.error().message(), {{"reason", "injection_unavailable"}}}); return; }
        { std::lock_guard lock(paths_); remote_ = remote.value().path(); }
        bus.connect(remoteService, remote.value().path(), remoteSessionInterface, "Closed", this, SLOT(sessionClosed()));
        const QDBusReply<QDBusVariant> id = bus.call(QDBusMessage::createMethodCall(remoteService, remote.value().path(), "org.freedesktop.DBus.Properties", "Get") << QString::fromLatin1(remoteSessionInterface) << QStringLiteral("SessionId"), QDBus::Block, 10000);
        if (!id.isValid() || id.value().variant().toString().isEmpty()) { fail({"scroll_unavailable", "Mutter's remote desktop session has no id: " + id.error().message(), {{"reason", "injection_unavailable"}}}); return; }
        options.insert(QStringLiteral("remote-desktop-session-id"), id.value().variant().toString());
    }
    const QDBusReply<QDBusObjectPath> created = bus.call(QDBusMessage::createMethodCall(service, rootPath, service, "CreateSession") << options, QDBus::Block, 10000);
    if (!created.isValid()) { fail({"recording_source_failed", "Mutter refused a screen cast session: " + created.error().message(), {}}); return; }
    { std::lock_guard lock(paths_); session_ = created.value().path(); }
    // Closed comes when Mutter ends the session itself: a recorded window went away.
    bus.connect(service, session_, sessionInterface, "Closed", this, SLOT(sessionClosed()));
    QDBusMessage record = QDBusMessage::createMethodCall(service, session_, sessionInterface, QString::fromLatin1(method));
    record.setArguments(arguments);
    const QDBusReply<QDBusObjectPath> stream = bus.call(record, QDBus::Block, 10000);
    if (!stream.isValid()) { auto error = refused; error.message += ": " + stream.error().message(); fail(error); return; }
    debugLog(QStringLiteral("mutter screencast: session %1 %2").arg(session_, label));
    start(stream.value().path(), logical);
}

void MutterScreenCast::recordMonitor(const QString& connector, const QRect& logical, Done done, bool cursorHidden) {
    if (done_ || !session_.isEmpty()) { done({"recording_state", "A Mutter screen cast is already open", {}}, {}); return; }
    done_ = std::move(done);
    create("RecordMonitor", {connector, QVariantMap{{"cursor-mode", cursorMode(cursorHidden)}}}, logical, {"recording_target", "Mutter has no monitor with this connector", {{"identity", connector}}}, QStringLiteral("monitor '%1'").arg(connector));
}

void MutterScreenCast::recordArea(const QRect& logical, Done done, bool cursorHidden) {
    if (done_ || !session_.isEmpty()) { done({"recording_state", "A Mutter screen cast is already open", {}}, {}); return; }
    done_ = std::move(done);
    create("RecordArea", {logical.x(), logical.y(), logical.width(), logical.height(), QVariantMap{{"cursor-mode", cursorMode(cursorHidden)}}}, logical, {"recording_target", "Mutter refused the area", {}},
           QStringLiteral("area %1,%2 %3x%4").arg(logical.x()).arg(logical.y()).arg(logical.width()).arg(logical.height()));
}

void MutterScreenCast::recordAreaLinked(const QRect& logical, Done done) {
    if (done_ || !session_.isEmpty() || !remote_.isEmpty()) { done({"recording_state", "A Mutter screen cast is already open", {}}, {}); return; }
    done_ = std::move(done);
    // The cursor is hidden: the stitcher must never see the pointer.
    create("RecordArea", {logical.x(), logical.y(), logical.width(), logical.height(), QVariantMap{{"cursor-mode", cursorMode(true)}}}, logical, {"recording_target", "Mutter refused the area", {}},
           QStringLiteral("linked area %1,%2 %3x%4").arg(logical.x()).arg(logical.y()).arg(logical.width()).arg(logical.height()), true);
}

bool MutterScreenCast::pointerMotion(double streamX, double streamY) {
    QString remote, stream;
    { std::lock_guard lock(paths_); remote = remote_; stream = stream_; }
    if (remote.isEmpty() || stream.isEmpty() || closed_.load()) return false;
    // (sdd): the stream's object path and a position in its pixels.
    const QDBusMessage reply = QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(remoteService, remote, remoteSessionInterface, "NotifyPointerMotionAbsolute") << stream << streamX << streamY, QDBus::Block, 5000);
    if (reply.type() == QDBusMessage::ErrorMessage) { debugLog("mutter remote desktop: motion refused: " + reply.errorMessage()); return false; }
    return true;
}

bool MutterScreenCast::pointerMotionBy(double dx, double dy) {
    QString remote;
    { std::lock_guard lock(paths_); remote = remote_; }
    if (remote.isEmpty() || closed_.load()) return false;
    const QDBusMessage reply = QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(remoteService, remote, remoteSessionInterface, "NotifyPointerMotion") << dx << dy, QDBus::Block, 5000);
    if (reply.type() == QDBusMessage::ErrorMessage) { debugLog("mutter remote desktop: relative motion refused: " + reply.errorMessage()); return false; }
    return true;
}

bool MutterScreenCast::pointerAxis(int steps) {
    QString remote;
    { std::lock_guard lock(paths_); remote = remote_; }
    if (remote.isEmpty() || steps == 0 || closed_.load()) return false;
    // (ui): axis 0 is vertical; a positive step is one wheel notch down.
    const QDBusMessage reply = QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(remoteService, remote, remoteSessionInterface, "NotifyPointerAxisDiscrete") << uint(0) << int(steps), QDBus::Block, 5000);
    if (reply.type() == QDBusMessage::ErrorMessage) { debugLog("mutter remote desktop: axis refused: " + reply.errorMessage()); return false; }
    return true;
}

void MutterScreenCast::recordWindow(quint64 windowId, const QRect& logical, Done done, bool cursorHidden) {
    if (done_ || !session_.isEmpty()) { done({"recording_state", "A Mutter screen cast is already open", {}}, {}); return; }
    done_ = std::move(done);
    create("RecordWindow", {QVariantMap{{"window-id", QVariant::fromValue<qulonglong>(windowId)}, {"cursor-mode", cursorMode(cursorHidden)}}}, logical,
           {"recording_target", "Mutter has no window with this id", {{"identity", QString::number(windowId)}}}, QStringLiteral("window %1").arg(windowId));
}

void MutterScreenCast::sessionClosed() {
    if (session_.isEmpty() && remote_.isEmpty()) return;  // the linked pair reports twice
    debugLog("mutter screencast: session closed by Mutter");
    closed_.store(true);
    const bool streaming = !session_.isEmpty() && !done_;
    if (auto done = std::exchange(done_, {})) done({"recording_source_lost", "Mutter closed the screen cast", {}}, {});
    if (!session_.isEmpty()) QDBusConnection::sessionBus().disconnect(service, session_, sessionInterface, "Closed", this, SLOT(sessionClosed()));
    if (!remote_.isEmpty()) QDBusConnection::sessionBus().disconnect(remoteService, remote_, remoteSessionInterface, "Closed", this, SLOT(sessionClosed()));
    { std::lock_guard lock(paths_); session_.clear(); stream_.clear(); remote_.clear(); }
    if (streaming) emit lost();
}

void MutterScreenCast::start(const QString& stream, const QRect& logical) {
    { std::lock_guard lock(paths_); stream_ = stream; }
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
    // A linked cast is started by its RemoteDesktop session, never on its own.
    const QDBusReply<void> started = remote_.isEmpty()
        ? QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(service, session_, sessionInterface, "Start"), QDBus::Block, 10000)
        : QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(remoteService, remote_, remoteSessionInterface, "Start"), QDBus::Block, 10000);
    if (!started.isValid()) fail({"recording_source_failed", "Mutter could not start the screen cast: " + started.error().message(), {}});
}

void MutterScreenCast::close() {
    if (session_.isEmpty() && remote_.isEmpty()) return;
    if (!session_.isEmpty()) QDBusConnection::sessionBus().disconnect(service, session_, sessionInterface, "Closed", this, SLOT(sessionClosed()));
    if (!remote_.isEmpty()) {
        // Stop on the RemoteDesktop session ends it and its linked cast.
        QDBusConnection::sessionBus().disconnect(remoteService, remote_, remoteSessionInterface, "Closed", this, SLOT(sessionClosed()));
        QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(remoteService, remote_, remoteSessionInterface, "Stop"), 5000);
    } else QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(service, session_, sessionInterface, "Stop"), 5000);
    debugLog("mutter screencast: session stopped");
    { std::lock_guard lock(paths_); session_.clear(); stream_.clear(); remote_.clear(); }
    if (auto done = std::exchange(done_, {})) done({"cancelled", "Screen cast canceled", {}}, {});
}
}
#include "MutterScreenCast.moc"
