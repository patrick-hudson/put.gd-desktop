#include "KWinWindows.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>

namespace putgd {
namespace {
constexpr auto objectPath = "/org/putgd/KWinWindows";
constexpr auto kwin = "org.kde.KWin";
QDBusMessage scripting(const QString& method, const QVariantList& arguments) {
    auto message = QDBusMessage::createMethodCall(kwin, "/Scripting", "org.kde.kwin.Scripting", method);
    message.setArguments(arguments);
    message.setAutoStartService(false);
    return message;
}
}
struct KWinWindows::Request {
    Own own;
    Reply reply;
    std::function<void(Error, QPointF)> pointer;
    QString plugin;
    std::unique_ptr<QTemporaryFile> script;
    QTimer* timer = nullptr;
};

KWinWindows::KWinWindows(QObject* parent) : QObject(parent) {}
KWinWindows::~KWinWindows() {
    if (registered_) QDBusConnection::sessionBus().unregisterObject(objectPath);
    for (auto it = requests_.begin(); it != requests_.end(); ++it) QDBusConnection::sessionBus().asyncCall(scripting("unloadScript", {(*it)->plugin}));
}

QString kwinWindowListScript(const QString& service, const QString& token) {
    // Normal windows and dialogs only: panels, the desktop, notifications and
    // popups are not windows a recording follows.
    return QStringLiteral(R"JS((function () {
    const out = [];
    const active = workspace.activeWindow;
    for (const w of workspace.windowList()) {
        if (!(w.normalWindow || w.dialog))
            continue;
        const g = w.frameGeometry;
        out.push({identity: w.internalId.toString(), title: String(w.caption ?? ''), desktopFile: String(w.desktopFileName ?? ''),
            resourceClass: String(w.resourceClass ?? ''), pid: Number(w.pid ?? 0),
            rect: [Math.round(g.x), Math.round(g.y), Math.round(g.width), Math.round(g.height)],
            active: w === active, minimized: Boolean(w.minimized)});
    }
    callDBus(%1, "%2", "org.putgd.KWinWindows", "Windows", %3, JSON.stringify(out));
})();
)JS").arg(QString::fromUtf8(QJsonDocument(QJsonArray{service}).toJson(QJsonDocument::Compact)).mid(1).chopped(1), QString::fromLatin1(objectPath),
             QString::fromUtf8(QJsonDocument(QJsonArray{token}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
}

QString kwinPointerScript(const QString& service, const QString& token) {
    return QStringLiteral(R"JS((function () {
    const p = workspace.cursorPos;
    callDBus(%1, "%2", "org.putgd.KWinWindows", "Windows", %3, JSON.stringify([p.x, p.y]));
})();
)JS").arg(QString::fromUtf8(QJsonDocument(QJsonArray{service}).toJson(QJsonDocument::Compact)).mid(1).chopped(1), QString::fromLatin1(objectPath),
             QString::fromUtf8(QJsonDocument(QJsonArray{token}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
}

QJsonObject kwinRecordingWindows(const QJsonArray& raw, const KWinWindows::Own& own) {
    QJsonArray windows;
    bool activeExcluded = false;
    for (const auto& value : raw) {
        const auto w = value.toObject();
        const auto identity = w["identity"].toString();
        const auto rect = w["rect"].toArray();
        if (QUuid(identity).isNull() || rect.size() != 4) continue;
        if (own && own(w)) { activeExcluded = activeExcluded || w["active"].toBool(); continue; }
        const auto desktopFile = w["desktopFile"].toString(), resourceClass = w["resourceClass"].toString();
        windows.append(QJsonObject{{"identity", identity}, {"title", w["title"].toString().left(512)}, {"appId", (desktopFile.isEmpty() ? resourceClass : desktopFile).left(256)},
                                   {"rect", rect}, {"active", w["active"].toBool()}, {"minimized", w["minimized"].toBool()}});
    }
    return {{"windows", windows}, {"activeExcluded", activeExcluded}};
}

void KWinWindows::list(Own own, Reply reply) {
    const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto request = std::make_shared<Request>();
    request->own = std::move(own);
    request->reply = std::move(reply);
    request->plugin = "putgd-recording-windows-" + token;
    run(token, std::move(request), kwinWindowListScript(QDBusConnection::sessionBus().baseService(), token));
}

void KWinWindows::pointer(std::function<void(Error, QPointF)> reply) {
    const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto request = std::make_shared<Request>();
    request->pointer = std::move(reply);
    request->reply = [](Error, QJsonObject) {};
    request->plugin = "putgd-pointer-" + token;
    run(token, std::move(request), kwinPointerScript(QDBusConnection::sessionBus().baseService(), token));
}

void KWinWindows::run(const QString& token, std::shared_ptr<Request> request, const QString& script) {
    auto bus = QDBusConnection::sessionBus();
    const auto refuse = [&request](Error error) { if (request->pointer) request->pointer(std::move(error), {}); else request->reply(std::move(error), {}); };
    if (!bus.isConnected()) { refuse({"helper_unavailable", "The session bus is unavailable", {}}); return; }
    if (!registered_) registered_ = bus.registerObject(objectPath, this, QDBusConnection::ExportAllSlots);
    if (!registered_) { refuse({"helper_unavailable", "Cannot receive KWin's answer", {}}); return; }
    // KWin reads the script from a file; a private one under the runtime directory.
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/putgd";
    QDir().mkpath(directory);
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    request->script = std::make_unique<QTemporaryFile>(directory + "/kwin-windows-XXXXXX.js");
    if (!request->script->open() || request->script->write(script.toUtf8()) < 0 || !request->script->flush()) {
        refuse({"helper_unavailable", "Cannot write the KWin script", {}});
        return;
    }
    request->timer = new QTimer(this);
    request->timer->setSingleShot(true);
    connect(request->timer, &QTimer::timeout, this, [this, token] { finish(token, {"helper_unavailable", "KWin did not list its windows", {}}); });
    requests_.insert(token, request);
    request->timer->start(timeoutMs);
    auto* load = new QDBusPendingCallWatcher(bus.asyncCall(scripting("loadScript", {request->script->fileName(), request->plugin}), timeoutMs), this);
    connect(load, &QDBusPendingCallWatcher::finished, this, [this, load, token] {
        const QDBusPendingReply<int> loaded = *load;
        load->deleteLater();
        if (!requests_.contains(token)) return;
        if (loaded.isError() || loaded.value() < 0) { finish(token, {"helper_unavailable", "KWin refused the window-list script", {{"dbus_error", loaded.error().name()}}}); return; }
        auto run = QDBusMessage::createMethodCall(kwin, QStringLiteral("/Scripting/Script%1").arg(loaded.value()), "org.kde.kwin.Script", "run");
        run.setAutoStartService(false);
        auto* started = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(run, timeoutMs), this);
        connect(started, &QDBusPendingCallWatcher::finished, this, [this, started, token] {
            const QDBusPendingReply<> ran = *started;
            started->deleteLater();
            if (ran.isError() && requests_.contains(token)) finish(token, {"helper_unavailable", "KWin could not run the window-list script", {{"dbus_error", ran.error().name()}}});
        });
    });
}

void KWinWindows::Windows(const QString& token, const QString& json, const QDBusMessage& message) {
    if (!requests_.contains(token)) return;
    // Only KWin itself answers: any other session client could call this object.
    const auto owner = QDBusConnection::sessionBus().interface()->serviceOwner(kwin);
    if (!owner.isValid() || owner.value() != message.service()) return;
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isArray()) { finish(token, {"bridge_protocol", "KWin returned an invalid window list", {}}); return; }
    finish(token, {}, document.array());
}

void KWinWindows::finish(const QString& token, Error error, const QJsonArray& raw) {
    const auto request = requests_.take(token);
    if (!request) return;
    if (request->timer) request->timer->deleteLater();
    QDBusConnection::sessionBus().asyncCall(scripting("unloadScript", {request->plugin}));
    if (request->pointer) {
        // [x, y] in KWin's logical layout.
        if (!error && !(raw.size() == 2 && raw[0].isDouble() && raw[1].isDouble())) error = {"bridge_protocol", "KWin returned an invalid pointer position", {}};
        request->pointer(error, error ? QPointF() : QPointF(raw[0].toDouble(), raw[1].toDouble()));
        return;
    }
    if (error) { request->reply(error, {}); return; }
    request->reply({}, kwinRecordingWindows(raw, request->own));
}
}
