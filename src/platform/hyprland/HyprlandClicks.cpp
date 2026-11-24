#include "HyprlandClicks.h"
#include "core/Log.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUuid>

namespace putgd {
namespace {
// BTN_LEFT, BTN_MIDDLE and BTN_RIGHT as Hyprland names them, with the button number put.gd reports.
struct Button { const char* key; int number; };
constexpr Button buttons[] = {{"mouse:272", 1}, {"mouse:274", 2}, {"mouse:273", 3}};
bool ours(const QJsonObject& bind) { return bind["description"].toString() == QLatin1String(HyprlandClicks::description); }
bool bare(const QJsonObject& bind, const char* key) { return bind["key"].toString().compare(QLatin1String(key), Qt::CaseInsensitive) == 0 && bind["modmask"].toInt() == 0; }
}

HyprlandClicks::HyprlandClicks(QObject* parent) : QObject(parent) {
    connect(&ipc_, &HyprlandIpc::event, this, [this](const QString& name, const QString& data) {
        if (!sink_) return;
        if (name == "custom") {
            int button = 0; double x = 0, y = 0;
            if (parseEvent(data, token_, &button, &x, &y)) press(button, x, y);
        } else if (name == "configreloaded") {
            // A reload drops every runtime bind and the Lua state with them: put ours back.
            arm([](Error, QJsonObject) {});
        }
    });
}
HyprlandClicks::~HyprlandClicks() {
    if (!sink_) return;
    QByteArray reply;
    HyprlandIpc::requestBlocking("eval " + disarmScript(), &reply);
}
QJsonObject HyprlandClicks::clickCapability() const {
    const bool socket = !HyprlandIpc::requestSocket().isEmpty() && !HyprlandIpc::eventSocket().isEmpty();
    const bool available = socket && lua_;
    QJsonObject capability{{"available", available}, {"route", "hyprland-mouse-binds"}, {"drawnBy", "recorder"},
                           {"targets", available ? QJsonArray{"monitor", "desktop", "region", "window"} : QJsonArray{}}};
    if (!socket) { capability["reason"] = "hyprland_ipc_unavailable"; capability["message"] = "Hyprland's sockets are not reachable, so put.gd cannot watch clicks."; }
    else if (!lua_) { capability["reason"] = "hyprland_lua_unavailable"; capability["message"] = luaError_.isEmpty() ? QStringLiteral("This Hyprland session cannot run put.gd's Lua binds, so recordings cannot highlight clicks.") : luaError_; }
    return capability;
}
QString HyprlandClicks::armScript(const QString& token, bool bind) {
    QStringList all;
    for (const auto& button : buttons) all.append(QString::fromLatin1(button.key));
    return armScript(token, bind ? all : QStringList{});
}
QStringList HyprlandClicks::missing(const QJsonArray& binds) {
    QStringList keys;
    for (const auto& button : buttons) {
        bool found = false;
        for (const auto& value : binds) if (ours(value.toObject()) && bare(value.toObject(), button.key)) { found = true; break; }
        if (!found) keys.append(QString::fromLatin1(button.key));
    }
    return keys;
}
QString HyprlandClicks::armScript(const QString& token, const QStringList& keys) {
    QString script = QStringLiteral("_G.putgd_clicks = %1").arg(HyprlandIpc::luaString(token));
    for (const auto& button : buttons) {
        if (!keys.contains(QString::fromLatin1(button.key))) continue;
        script += QStringLiteral("; hl.bind(%1, function() local t = _G.putgd_clicks; if not t then return end; local p = hl.get_cursor_pos(); if p then hl.dispatch(hl.dsp.event(\"putgd-click \" .. t .. \" %2 \" .. p.x .. \" \" .. p.y)) end end, { non_consuming = true, ignore_mods = true, description = %3 })")
                      .arg(HyprlandIpc::luaString(QString::fromLatin1(button.key))).arg(button.number).arg(HyprlandIpc::luaString(QString::fromLatin1(description)));
    }
    return script;
}
QString HyprlandClicks::disarmScript() { return QStringLiteral("_G.putgd_clicks = nil"); }
QStringList HyprlandClicks::removable(const QJsonArray& binds) {
    QStringList keys;
    for (const auto& button : buttons) {
        bool mine = false, shared = false;
        for (const auto& value : binds) {
            const auto bind = value.toObject();
            if (!bare(bind, button.key)) continue;
            if (ours(bind)) mine = true; else shared = true;
        }
        // hl.unbind takes every bind written on the chord, so a shared chord is left alone.
        if (mine && !shared) keys.append(QString::fromLatin1(button.key));
    }
    return keys;
}
bool HyprlandClicks::parseEvent(const QString& data, const QString& token, int* button, double* x, double* y) {
    const auto parts = data.split(' ', Qt::SkipEmptyParts);
    if (parts.size() != 5 || parts[0] != "putgd-click" || token.isEmpty() || parts[1] != token) return false;
    bool okButton = false, okX = false, okY = false;
    const int number = parts[2].toInt(&okButton);
    const double px = parts[3].toDouble(&okX), py = parts[4].toDouble(&okY);
    if (!okButton || !okX || !okY || number < 1 || number > 3) return false;
    *button = number; *x = px; *y = py;
    return true;
}
void HyprlandClicks::arm(RecordingClicks::Reply reply) {
    const quint64 generation = generation_;
    QPointer<HyprlandClicks> guard(this);
    ipc_.json("j/binds", [guard, generation, reply](QJsonDocument document, Error error) {
        if (!guard || guard->generation_ != generation || !guard->sink_) return;
        if (error) { reply(error, {}); return; }
        // A bind a recording left in place (a shared chord, a crash) is reused, not doubled:
        // only the buttons without one of put.gd's binds get one.
        guard->ipc_.request("eval " + armScript(guard->token_, missing(document.array())), [guard, generation, reply](QByteArray answer, Error error) {
            if (!guard || guard->generation_ != generation) return;
            if (!error && !HyprlandIpc::okReply(answer)) {
                const auto text = QString::fromUtf8(answer.left(300)).trimmed();
                // The legacy hyprland.conf parser has no Lua: nothing can be bound at runtime.
                guard->lua_ = false;
                guard->luaError_ = "Hyprland did not run put.gd's Lua bind, so recordings cannot highlight clicks here: " + text;
                error = {"click_route_unavailable", guard->luaError_, {}};
            }
            if (error) { reply(error, {}); return; }
            guard->lua_ = true;
            reply({}, {{"route", "hyprland-mouse-binds"}});
        });
    });
}
void HyprlandClicks::watchClicks(const QJsonObject& target, RecordingClicks::Sink sink, RecordingClicks::Reply reply) {
    ++generation_;
    sink_ = std::move(sink);
    window_ = target["kind"].toString() == "window" ? target["identity"].toString() : QString();
    token_ = QUuid::createUuid().toString(QUuid::Id128);
    ipc_.listen();
    arm(std::move(reply));
}
void HyprlandClicks::unwatchClicks() {
    ++generation_;
    if (!sink_) return;
    sink_ = {};
    token_.clear();
    QPointer<HyprlandClicks> guard(this);
    ipc_.request("eval " + disarmScript(), [guard](QByteArray, Error) {
        if (!guard) return;
        guard->ipc_.json("j/binds", [guard](QJsonDocument document, Error error) {
            if (!guard || error || guard->sink_) return;
            for (const auto& key : removable(document.array()))
                guard->ipc_.request("eval hl.unbind(" + HyprlandIpc::luaString(key) + ")", [](QByteArray, Error) {});
        });
    });
}
void HyprlandClicks::press(int button, double x, double y) {
    QJsonObject click{{"x", x}, {"y", y}, {"button", button}};
    if (window_.isEmpty()) { sink_(click); return; }
    // A window recording shows the window's own pixels: where it is at this press.
    const quint64 generation = generation_;
    const QString identity = window_;
    QPointer<HyprlandClicks> guard(this);
    ipc_.json("j/clients", [guard, generation, identity, click](QJsonDocument clients, Error error) mutable {
        if (!guard || guard->generation_ != generation || !guard->sink_ || error) return;
        for (const auto& value : clients.array()) {
            const auto window = value.toObject();
            if (window["stableId"].toString() != identity) continue;
            const auto at = window["at"].toArray(), size = window["size"].toArray();
            if (at.size() != 2 || size.size() != 2) return;
            click["window"] = QJsonObject{{"identity", identity}, {"rect", QJsonArray{at[0], at[1], size[0], size[1]}}};
            guard->sink_(click);
            return;
        }
    });
}
}
