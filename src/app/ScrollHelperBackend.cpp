#include "ScrollHelperBackend.h"
#include <QCoreApplication>

namespace putgd {
namespace {
bool validCapabilities(const QJsonObject& o) {
    return o["available"].isBool() && o["injector"].isString() && o["frames"].isString() && o["selection"].isString() && o["maxHeight"].isDouble() && (o["available"].toBool() || o["reason"].isString());
}
// The helper died, broke its protocol or missed a deadline; `check` says which part.
Error scrollError(HelperFault fault, const QString& message, const QJsonObject& details) {
    const char* check = "protocol";
    switch (fault) {
    case HelperFault::Closed: check = "closed"; break;
    case HelperFault::Protocol: check = "protocol"; break;
    case HelperFault::Build: check = "build"; break;
    case HelperFault::StartFailed: check = "start"; break;
    case HelperFault::StartTimeout: check = "start_timeout"; break;
    case HelperFault::CommandTimeout: check = "command_timeout"; break;
    case HelperFault::Exited: check = "exited"; break;
    case HelperFault::Crashed: check = "crashed"; break;
    }
    auto withCheck = details;
    withCheck["check"] = QString::fromLatin1(check);
    return {"scroll_helper_failed", message, withCheck};
}
HelperPolicy policy() {
    HelperPolicy p;
    p.noun = "scroll capture helper";
    p.prefix = "scroll";
    p.process = "putgd-record (scroll)";
    p.session = "scroll";
    p.lost = "captured";
    p.discarded = "The scroll capture was discarded";
    p.active = {"scroll_active", "A scroll capture is already running", {}};
    // A helper that stopped answering may still be turning the wheel: end it.
    p.timeoutEndsHelper = true;
    p.error = scrollError;
    p.probe.argument = "--scroll-capabilities";
    p.probe.limit = 64 * 1024;
    p.probe.unavailableLog = "Scrolling capture is unavailable";
    p.probe.unavailable = [](const QString& reason) { return QJsonObject{{"available", false}, {"reason", reason}}; };
    p.probe.accept = [](const QJsonObject& probed) {
        if (!validCapabilities(probed)) return QJsonObject{};
        // The contract's keys, and the two the resident acts on besides.
        QJsonObject result{{"available", probed["available"]}, {"injector", probed["injector"]}, {"frames", probed["frames"]}, {"selection", probed["selection"]}, {"maxHeight", probed["maxHeight"]}};
        if (!probed["available"].toBool()) { result["reason"] = probed["reason"].toString().left(64); result["message"] = probed["message"].toString().left(512); }
        return result;
    };
    return p;
}
QString defaultPath(QString path) { return path.isEmpty() ? QCoreApplication::applicationDirPath() + "/putgd-record" : path; }
}

ScrollHelperBackend::ScrollHelperBackend(QString helperPath, QObject* parent) : QObject(parent), helper_(policy(), defaultPath(std::move(helperPath))) {}
void ScrollHelperBackend::setTimeouts(int startupMs, int commandMs) { helper_.setStartupMs(startupMs); commandMs_ = commandMs; }
void ScrollHelperBackend::prepare(const QString& id, const QJsonObject& params, const QString& staging, Reply reply) {
    if (!helper_.executable()) { reply({"scroll_unavailable", "Scrolling capture is not available on this installation", {{"reason", "recording_helper_missing"}}}, {}); return; }
    auto prepared = params;
    prepared["id"] = id;
    prepared["staging"] = staging;
    helper_.prepare(id, prepared, commandMs_, std::move(reply));
}
void ScrollHelperBackend::control(const QString& id, const QString& command, const QJsonObject& params, Reply reply) {
    // stop encodes the PNG; scroll::helperStopMs is this doubled deadline.
    helper_.command(id, command, params, command == "stop" ? 2 * commandMs_ : commandMs_, std::move(reply));
}
}
