#include "KWinClickEffect.h"
#include "core/Log.h"
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonArray>
#include <QPointer>

namespace putgd {
namespace {
const QString kwin = QStringLiteral("org.kde.KWin"), effectsPath = QStringLiteral("/Effects"), effectsInterface = QStringLiteral("org.kde.kwin.Effects");
const QString accel = QStringLiteral("org.kde.kglobalaccel"), accelPath = QStringLiteral("/component/kwin"), accelInterface = QStringLiteral("org.kde.kglobalaccel.Component");
constexpr int timeoutMs = 3000;
}

KWinClickEffect::KWinClickEffect(QObject* parent) : QObject(parent), bus_(QDBusConnection::sessionBus()) {}
KWinClickEffect::~KWinClickEffect() {
    // A resident that quits mid-recording still puts the desktop back.
    if (!on_) return;
    bus_.call(QDBusMessage::createMethodCall(accel, accelPath, accelInterface, QStringLiteral("invokeShortcut")) << QString::fromLatin1(shortcut), QDBus::Block, timeoutMs);
    if (!wasLoaded_) bus_.call(QDBusMessage::createMethodCall(kwin, effectsPath, effectsInterface, QStringLiteral("unloadEffect")) << QString::fromLatin1(effect), QDBus::Block, timeoutMs);
}
QJsonObject KWinClickEffect::clickCapability() const {
    // KWin and KGlobalAccel answer on every Plasma session; whether the effect
    // loads is known when a recording asks for it and is reported then.
    const auto* interface = bus_.interface();
    const bool services = interface && interface->isServiceRegistered(kwin) && interface->isServiceRegistered(accel);
    QJsonObject capability{{"available", services}, {"route", "kwin-mouseclick-effect"}, {"drawnBy", "compositor"},
                           {"targets", services ? QJsonArray{"monitor", "desktop", "region"} : QJsonArray{}},
                           {"note", "KWin draws the rings on the screen for the length of the recording; window recordings carry none."}};
    if (!services) { capability["reason"] = "kwin_unavailable"; capability["message"] = "KWin or KGlobalAccel is not on the session bus, so the click effect cannot be switched on."; }
    return capability;
}
void KWinClickEffect::effects(const QString& method, std::function<void(bool, bool)> reply) {
    auto* watcher = new QDBusPendingCallWatcher(bus_.asyncCall(QDBusMessage::createMethodCall(kwin, effectsPath, effectsInterface, method) << QString::fromLatin1(effect), timeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, reply] {
        const QDBusMessage message = watcher->reply();
        watcher->deleteLater();
        const bool ok = message.type() == QDBusMessage::ReplyMessage;
        // unloadEffect answers with nothing; the others with a boolean.
        reply(ok, ok && (message.arguments().isEmpty() || message.arguments().first().toBool()));
    });
}
void KWinClickEffect::toggle(std::function<void(bool)> reply) {
    auto* watcher = new QDBusPendingCallWatcher(bus_.asyncCall(QDBusMessage::createMethodCall(accel, accelPath, accelInterface, QStringLiteral("invokeShortcut")) << QString::fromLatin1(shortcut), timeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, reply] {
        const bool ok = watcher->reply().type() == QDBusMessage::ReplyMessage;
        watcher->deleteLater();
        reply(ok);
    });
}
void KWinClickEffect::watchClicks(const QJsonObject&, RecordingClicks::Sink, RecordingClicks::Reply reply) {
    if (on_) { reply({}, {}); return; }
    const quint64 generation = ++generation_;
    QPointer<KWinClickEffect> guard(this);
    auto failed = [reply](const QString& message) { reply({"click_effect_unavailable", message, {{"effect", QString::fromLatin1(effect)}}}, {}); };
    effects(QStringLiteral("isEffectLoaded"), [guard, generation, reply, failed](bool ok, bool loaded) {
        if (!guard || guard->generation_ != generation) return;
        if (!ok) { failed(QStringLiteral("KWin did not answer, so its click effect could not be switched on.")); return; }
        guard->wasLoaded_ = loaded;
        auto load = [guard, generation, reply, failed] {
            guard->effects(QStringLiteral("loadEffect"), [guard, generation, reply, failed](bool ok, bool loaded) {
                if (!guard || guard->generation_ != generation) return;
                if (!ok || !loaded) { failed(QStringLiteral("KWin could not load its Mouse Click Animation effect.")); return; }
                guard->toggle([guard, generation, reply, failed](bool ok) {
                    if (!guard || guard->generation_ != generation) return;
                    if (!ok) {
                        if (!guard->wasLoaded_) guard->effects(QStringLiteral("unloadEffect"), [](bool, bool) {});
                        failed(QStringLiteral("KGlobalAccel could not switch KWin's click effect on."));
                        return;
                    }
                    guard->on_ = true;
                    logging::info(logRecording(), "KWin click effect switched on for the recording", {{"wasLoaded", guard->wasLoaded_}});
                    reply({}, {{"route", "kwin-mouseclick-effect"}});
                });
            });
        };
        // Loaded by the user: reloading leaves it switched off, the one state KWin lets us know.
        if (loaded) guard->effects(QStringLiteral("unloadEffect"), [guard, generation, load](bool, bool) { if (guard && guard->generation_ == generation) load(); });
        else load();
    });
}
void KWinClickEffect::unwatchClicks() {
    ++generation_;  // a watch still starting stops where it is
    if (!on_) return;
    on_ = false;
    const bool unload = !wasLoaded_;
    QPointer<KWinClickEffect> guard(this);
    toggle([guard, unload](bool) {
        if (guard && unload) guard->effects(QStringLiteral("unloadEffect"), [](bool, bool) {});
    });
    logging::info(logRecording(), "KWin click effect switched off", {{"unloaded", unload}});
}
}
