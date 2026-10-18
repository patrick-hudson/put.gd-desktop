#include "PortalScreenCast.h"
#include "DebugLog.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QPointer>
#include <unistd.h>

namespace putgd::record {
namespace {
constexpr auto portalService = "org.freedesktop.portal.Desktop";
constexpr auto portalPath = "/org/freedesktop/portal/desktop";
constexpr auto screenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr auto requestInterface = "org.freedesktop.portal.Request";
constexpr auto sessionInterface = "org.freedesktop.portal.Session";
QString senderToken() {
    auto name = QDBusConnection::sessionBus().baseService();
    if (name.startsWith(':')) name.remove(0, 1);
    return name.replace('.', '_');
}
QVariant portalProperty(const char* name) {
    QDBusInterface properties(portalService, portalPath, "org.freedesktop.DBus.Properties", QDBusConnection::sessionBus());
    const QDBusReply<QVariant> reply = properties.call("Get", QString::fromLatin1(screenCastInterface), QString::fromLatin1(name));
    return reply.isValid() ? reply.value() : QVariant();
}
// The response handler for one Request object: the portal emits Response on
// the request path, which is known before the method returns.
class ResponseSink final : public QObject {
    Q_OBJECT
public:
    std::function<void(quint32, QVariantMap)> handler;
public slots:
    void response(uint code, const QVariantMap& results) { if (handler) handler(code, results); deleteLater(); }
};
}

PortalScreenCast::PortalScreenCast(QObject* parent) : QObject(parent) {}
PortalScreenCast::~PortalScreenCast() { close(); }

bool PortalScreenCast::available(QJsonObject& facts, QString* reason) {
    if (!QDBusConnection::sessionBus().isConnected()) { if (reason) *reason = "session_bus_unavailable"; return false; }
    const auto version = portalProperty("version");
    if (!version.isValid()) { if (reason) *reason = "portal_unavailable"; return false; }
    facts["portalVersion"] = int(version.toUInt());
    facts["sourceTypes"] = int(portalProperty("AvailableSourceTypes").toUInt());
    facts["cursorModes"] = int(portalProperty("AvailableCursorModes").toUInt());
    if (version.toUInt() < 2) { if (reason) *reason = "portal_too_old"; return false; }
    return true;
}

QString PortalScreenCast::token() { return QStringLiteral("putgd_record_%1_%2").arg(getpid()).arg(++counter_); }

void PortalScreenCast::request(const QString& method, const QList<QVariant>& arguments, QVariantMap options, std::function<void(quint32, QVariantMap)> onResponse) {
    const auto handle = token();
    options["handle_token"] = handle;
    const auto requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(senderToken(), handle);
    auto* sink = new ResponseSink;
    sink->setParent(this);
    sink->handler = std::move(onResponse);
    QDBusConnection::sessionBus().connect(portalService, requestPath, requestInterface, "Response", sink, SLOT(response(uint,QVariantMap)));
    QDBusMessage call = QDBusMessage::createMethodCall(portalService, portalPath, screenCastInterface, method);
    auto all = arguments;
    all.append(options);
    call.setArguments(all);
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 30000), this);
    QPointer<PortalScreenCast> guard(this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [guard, sink, method, watcher] {
        watcher->deleteLater();
        if (!guard) return;
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        if (reply.isError()) {
            sink->deleteLater();
            guard->fail({"recording_portal_failed", QStringLiteral("%1 failed: %2").arg(method, reply.error().message()), {{"method", method}}});
        }
    });
}

void PortalScreenCast::fail(const Error& error) {
    debugLog("portal: " + error.code + " " + error.message);
    if (auto done = std::exchange(done_, {})) done(error, {});
}

void PortalScreenCast::start(quint32 sourceTypes, const QString& restoreToken, Done done) {
    if (done_ || !session_.isEmpty()) { done({"recording_state", "A portal session is already open", {}}, {}); return; }
    done_ = std::move(done);
    sessionToken_ = token();
    QPointer<PortalScreenCast> guard(this);
    debugLog(QStringLiteral("portal: CreateSession types=%1").arg(sourceTypes));
    request("CreateSession", {}, {{"session_handle_token", sessionToken_}}, [guard, sourceTypes, restoreToken](quint32 code, QVariantMap results) {
        if (!guard) return;
        if (code != 0) { guard->fail({"recording_portal_failed", "The portal refused to create a screen cast session", {{"code", int(code)}}}); return; }
        guard->session_ = results.value("session_handle").toString();
        if (guard->session_.isEmpty()) { guard->fail({"recording_portal_failed", "The portal returned no session handle", {}}); return; }
        QJsonObject facts;
        available(facts);
        const quint32 cursorModes = quint32(facts["cursorModes"].toInt());
        QVariantMap options{{"types", sourceTypes}, {"multiple", false}, {"cursor_mode", cursorModes & Embedded ? uint(Embedded) : uint(Hidden)}};
        // Version 4 portals hand back a restore token when asked to persist the
        // grant; sending it next time skips the picker for the same source.
        if (facts["portalVersion"].toInt() >= 4) { options["persist_mode"] = uint(2); if (!restoreToken.isEmpty()) options["restore_token"] = restoreToken; }
        debugLog("portal: SelectSources");
        guard->request("SelectSources", {QVariant::fromValue(QDBusObjectPath(guard->session_))}, options, [guard](quint32 code, QVariantMap) {
            if (!guard) return;
            if (code == 1) { guard->fail({"cancelled", "Screen sharing was cancelled; nothing was recorded", {}}); return; }
            if (code != 0) { guard->fail({"recording_portal_failed", "The portal refused the requested sources", {{"code", int(code)}}}); return; }
            debugLog("portal: Start (the compositor shows its picker)");
            guard->request("Start", {QVariant::fromValue(QDBusObjectPath(guard->session_)), QString()}, {}, [guard](quint32 code, QVariantMap results) {
                if (!guard) return;
                if (code == 1) { guard->fail({"cancelled", "Screen sharing was cancelled; nothing was recorded", {}}); return; }
                if (code != 0) { guard->fail({"recording_portal_failed", "Screen sharing did not start", {{"code", int(code)}}}); return; }
                Result result;
                result.streams = parseStreams(results.value("streams"));
                result.restoreToken = results.value("restore_token").toString();
                if (result.streams.isEmpty()) { guard->fail({"recording_portal_failed", "The portal shared no stream", {}}); return; }
                QDBusMessage call = QDBusMessage::createMethodCall(portalService, portalPath, screenCastInterface, "OpenPipeWireRemote");
                call.setArguments({QVariant::fromValue(QDBusObjectPath(guard->session_)), QVariantMap{}});
                const QDBusReply<QDBusUnixFileDescriptor> remote = QDBusConnection::sessionBus().call(call, QDBus::Block, 15000);
                if (!remote.isValid() || !remote.value().isValid()) { guard->fail({"recording_portal_failed", "The portal gave no PipeWire connection: " + remote.error().message(), {}}); return; }
                result.fd = ::dup(remote.value().fileDescriptor());
                debugLog(QStringLiteral("portal: stream node=%1 size=%2x%3 at %4,%5 type=%6").arg(result.streams[0].node).arg(result.streams[0].size.width()).arg(result.streams[0].size.height()).arg(result.streams[0].position.x()).arg(result.streams[0].position.y()).arg(result.streams[0].sourceType));
                if (auto done = std::exchange(guard->done_, {})) done({}, result);
            });
        });
    });
}

void PortalScreenCast::close() {
    if (session_.isEmpty()) return;
    QDBusMessage call = QDBusMessage::createMethodCall(portalService, session_, sessionInterface, "Close");
    QDBusConnection::sessionBus().asyncCall(call, 5000);
    debugLog("portal: session closed");
    session_.clear();
    if (auto done = std::exchange(done_, {})) done({"cancelled", "Screen sharing was cancelled; nothing was recorded", {}}, {});
}

QList<PortalScreenCast::Stream> PortalScreenCast::parseStreams(const QVariant& value) {
    QList<Stream> streams;
    if (!value.canConvert<QDBusArgument>()) return streams;
    const auto array = value.value<QDBusArgument>();
    array.beginArray();
    while (!array.atEnd()) {
        quint32 node = 0;
        QVariantMap props;
        array.beginStructure();
        array >> node >> props;
        array.endStructure();
        streams.append(parseStream(node, props));
    }
    array.endArray();
    return streams;
}
PortalScreenCast::Stream PortalScreenCast::parseStream(quint32 node, const QVariantMap& props) {
    auto pair = [](const QVariant& v, int& a, int& b) {
        if (v.canConvert<QDBusArgument>()) {
            const auto arg = v.value<QDBusArgument>();
            arg.beginStructure();
            arg >> a >> b;
            arg.endStructure();
        } else if (v.canConvert<QVariantList>()) {
            const auto list = v.toList();
            if (list.size() == 2) { a = list[0].toInt(); b = list[1].toInt(); }
        }
    };
    Stream stream;
    stream.node = node;
    int x = 0, y = 0, w = 0, h = 0;
    pair(props.value("position"), x, y);
    pair(props.value("size"), w, h);
    stream.position = QPoint(x, y);
    stream.size = QSize(w, h);
    stream.sourceType = props.value("source_type").toUInt();
    return stream;
}
}
#include "PortalScreenCast.moc"
