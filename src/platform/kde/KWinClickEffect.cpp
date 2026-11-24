#include "KWinClickEffect.h"
#include "core/Log.h"
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QJsonArray>
#include <QPointer>
#include <QRegularExpression>

namespace putgd {
namespace {
const QString kwin = QStringLiteral("org.kde.KWin"), effectsPath = QStringLiteral("/Effects"), effectsInterface = QStringLiteral("org.kde.kwin.Effects");
const QString accel = QStringLiteral("org.kde.kglobalaccel"), accelPath = QStringLiteral("/component/kwin"), accelInterface = QStringLiteral("org.kde.kglobalaccel.Component");
constexpr int timeoutMs = 3000;
}

int KWinClickEffect::enabledIn(const QString& supportInformation) {
    static const QRegularExpression line(QStringLiteral("(?:^|\\n)\\s*enabled:\\s*(true|false)\\s*(?:\\n|$)"));
    const auto match = line.match(supportInformation);
    return !match.hasMatch() ? -1 : match.captured(1) == QLatin1String("true") ? 1 : 0;
}
KWinClickEffect::KWinClickEffect(QObject* parent) : QObject(parent), bus_(QDBusConnection::sessionBus()) {}
KWinClickEffect::~KWinClickEffect() {
    // A resident that quits mid-recording still puts the desktop back, by the same rule.
    if (!loadedByUs_ && !toggledByUs_) return;
    auto blocking = [this](const QString& service, const QString& path, const QString& interface, const QString& method) {
        return bus_.call(QDBusMessage::createMethodCall(service, path, interface, method) << QString::fromLatin1(method == QLatin1String("invokeShortcut") ? shortcut : effect), QDBus::Block, timeoutMs);
    };
    if (toggledByUs_) {
        const auto information = blocking(kwin, effectsPath, effectsInterface, QStringLiteral("supportInformation"));
        if (information.type() == QDBusMessage::ReplyMessage && enabledIn(information.arguments().value(0).toString()) == 1) blocking(accel, accelPath, accelInterface, QStringLiteral("invokeShortcut"));
    }
    if (loadedByUs_) blocking(kwin, effectsPath, effectsInterface, QStringLiteral("unloadEffect"));
}
QJsonObject KWinClickEffect::clickCapability() const {
    // KWin and KGlobalAccel answer on every Plasma session; whether the effect
    // loads is known when a recording asks for it and is reported then.
    const auto* interface = bus_.interface();
    const bool services = interface && interface->isServiceRegistered(kwin) && interface->isServiceRegistered(accel);
    QJsonObject capability{{"available", services}, {"route", "kwin-mouseclick-effect"}, {"drawnBy", "compositor"},
                           {"targets", services ? QJsonArray{"monitor", "desktop", "region"} : QJsonArray{}},
                           {"targetReasons", QJsonObject{{"window", "KWin draws the click rings on the screen; a window recording's stream carries the window alone, so it has none."}}},
                           {"note", "KWin draws the rings on the screen for the length of the recording; window recordings carry none."}};
    if (!services) { capability["reason"] = "kwin_unavailable"; capability["message"] = "KWin or KGlobalAccel is not on the session bus, so the click effect cannot be switched on."; }
    return capability;
}
void KWinClickEffect::enqueue(Job job) {
    jobs_.append(std::move(job));
    if (!busy_) next();
}
void KWinClickEffect::next() {
    if (jobs_.isEmpty()) { busy_ = false; return; }
    busy_ = true;
    const auto job = jobs_.takeFirst();
    job([guard = QPointer<KWinClickEffect>(this)] { if (guard) guard->next(); });
}
void KWinClickEffect::call(const QString& service, const QString& path, const QString& interface, const QString& method, const QVariantList& arguments, std::function<void(bool, QVariantList)> reply) {
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(arguments);
    auto* watcher = new QDBusPendingCallWatcher(bus_.asyncCall(message, timeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, reply] {
        const QDBusMessage answer = watcher->reply();
        watcher->deleteLater();
        reply(answer.type() == QDBusMessage::ReplyMessage, answer.arguments());
    });
}
void KWinClickEffect::effects(const QString& method, std::function<void(bool, QVariantList)> reply) {
    call(kwin, effectsPath, effectsInterface, method, {QString::fromLatin1(effect)}, std::move(reply));
}
void KWinClickEffect::state(std::function<void(State)> reply) {
    QPointer<KWinClickEffect> guard(this);
    effects(QStringLiteral("isEffectLoaded"), [guard, reply](bool ok, QVariantList values) {
        State now;
        now.answered = ok;
        now.loaded = ok && values.value(0).toBool();
        if (!guard || !now.loaded) { reply(now); return; }
        guard->effects(QStringLiteral("supportInformation"), [reply, now](bool ok, QVariantList values) mutable {
            const int enabled = ok ? enabledIn(values.value(0).toString()) : -1;
            now.known = enabled >= 0;
            now.enabled = enabled == 1;
            reply(now);
        });
    });
}
void KWinClickEffect::toggle(std::function<void(State)> reply) {
    QPointer<KWinClickEffect> guard(this);
    call(accel, accelPath, accelInterface, QStringLiteral("invokeShortcut"), {QString::fromLatin1(shortcut)}, [guard, reply](bool, QVariantList) {
        // Whether it took is what KWin says afterwards, not what KGlobalAccel answered.
        if (guard) guard->state(reply); else reply({});
    });
}
void KWinClickEffect::restore(Done done) {
    QPointer<KWinClickEffect> guard(this);
    auto unload = [guard, done] {
        if (!guard || !std::exchange(guard->loadedByUs_, false)) { done(); return; }
        guard->effects(QStringLiteral("unloadEffect"), [done](bool, QVariantList) { done(); });
    };
    if (!std::exchange(toggledByUs_, false)) { unload(); return; }
    state([guard, unload](State now) {
        // Off already (the user switched it during the recording): nothing to switch back.
        if (!guard || !now.loaded || !now.known || !now.enabled) { unload(); return; }
        guard->toggle([unload](State) { unload(); });
    });
}
void KWinClickEffect::watchClicks(const QJsonObject&, RecordingClicks::Sink, RecordingClicks::Reply reply) {
    enqueue([this, reply](Done done) {
        if (on_) { reply({}, {{"route", "kwin-mouseclick-effect"}}); done(); return; }
        QPointer<KWinClickEffect> guard(this);
        auto failed = [guard, reply, done](const QString& message) {
            const Error error{"click_effect_unavailable", message, {{"effect", QString::fromLatin1(effect)}}};
            if (!guard) { reply(error, {}); done(); return; }
            guard->restore([reply, error, done] { reply(error, {}); done(); });
        };
        auto switchOn = [guard, reply, done, failed](State now) {
            if (!guard) { done(); return; }
            if (!now.loaded) { failed(QStringLiteral("KWin could not load its Mouse Click Animation effect.")); return; }
            if (!now.known) { failed(QStringLiteral("This KWin does not report whether its Mouse Click Animation effect is switched on, so put.gd leaves it as it is.")); return; }
            auto finish = [guard, reply, done](bool wasOn) {
                guard->on_ = true;
                logging::info(logRecording(), "KWin click effect is on for the recording", {{"loadedByPutgd", guard->loadedByUs_}, {"switchedOnByPutgd", guard->toggledByUs_}, {"wasOn", wasOn}});
                reply({}, {{"route", "kwin-mouseclick-effect"}});
                done();
            };
            // On already: the user's own; nothing is changed and nothing will be undone.
            if (now.enabled) { finish(true); return; }
            guard->toggle([guard, finish, failed, done](State after) {
                if (!guard) { done(); return; }
                if (!after.loaded || !after.known || !after.enabled) { failed(QStringLiteral("KWin's Mouse Click Animation effect did not switch on through its shortcut (KGlobalAccel).")); return; }
                guard->toggledByUs_ = true;
                finish(false);
            });
        };
        state([guard, done, failed, switchOn](State now) {
            if (!guard) { done(); return; }
            if (!now.answered) { failed(QStringLiteral("KWin did not answer, so its click effect could not be switched on.")); return; }
            if (now.loaded) { switchOn(now); return; }
            guard->effects(QStringLiteral("loadEffect"), [guard, done, failed, switchOn](bool ok, QVariantList values) {
                if (!guard) { done(); return; }
                if (!ok || !values.value(0).toBool()) { failed(QStringLiteral("KWin could not load its Mouse Click Animation effect.")); return; }
                guard->loadedByUs_ = true;
                guard->state(switchOn);
            });
        });
    });
}
void KWinClickEffect::unwatchClicks() {
    enqueue([this](Done done) {
        if (!on_ && !loadedByUs_ && !toggledByUs_) { done(); return; }
        on_ = false;
        const bool unloaded = loadedByUs_, switched = toggledByUs_;
        restore([done, unloaded, switched] {
            logging::info(logRecording(), "KWin click effect is as it was before the recording", {{"unloaded", unloaded}, {"switchedOff", switched}});
            done();
        });
    });
}
}
