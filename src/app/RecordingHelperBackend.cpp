#include "RecordingHelperBackend.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>

namespace putgd {
namespace {
QJsonObject unavailable(const QString& reason) {
    return {{"available", false}, {"reason", reason}, {"targets", QJsonArray{}}, {"formats", QJsonArray{}}, {"codecs", QJsonObject{}},
        {"audio", QJsonObject{{"system", false}, {"microphone", false}}}, {"overlayExclusion", QJsonObject{}}};
}
bool validCapabilities(const QJsonObject& o) {
    return o["available"].isBool() && o["targets"].isArray() && o["formats"].isArray() && o["codecs"].isObject() && o["audio"].isObject() && (o["available"].toBool() || o["reason"].isString());
}
constexpr qsizetype probeLimit = 1024 * 1024;
Error recordingError(HelperFault fault, const QString& message, const QJsonObject& details) {
    switch (fault) {
    case HelperFault::Closed: return {"recording_helper_closed", message, details};
    case HelperFault::Protocol: return {"recording_helper_protocol", message, details};
    case HelperFault::Build: return {"recording_incompatible", message, details};
    case HelperFault::StartFailed: return {"recording_helper_start_failed", message, details};
    case HelperFault::StartTimeout: {
        // The step that timed out: the helper's ready frame after bootstrap (#250).
        auto withStep = details;
        withStep["step"] = "ready";
        return {"recording_helper_start_timeout", message, withStep};
    }
    case HelperFault::CommandTimeout: return {"recording_helper_timeout", message, details};
    case HelperFault::Exited: return {"recording_helper_exited", message, details};
    case HelperFault::Crashed: return {"recording_helper_crashed", message, details};
    }
    return {"recording_helper_protocol", message, details};
}
HelperPolicy policy() {
    HelperPolicy p;
    p.noun = "recording helper";
    p.prefix = "recording";
    p.process = "putgd-record";
    p.lost = "uploaded";
    p.discarded = "The recording session was discarded";
    p.active = {"recording_active", "A recording helper is already running", {}};
    p.timeoutEndsHelper = false;
    p.error = recordingError;
    p.probe.argument = "--capabilities";
    p.probe.limit = probeLimit;
    p.probe.unavailableLog = "Screen recording is unavailable";
    p.probe.unavailable = unavailable;
    p.probe.accept = [](const QJsonObject& answer) { return validCapabilities(answer) ? answer : QJsonObject{}; };
    p.probe.invalidReason = "recording_probe_invalid";
    // On the image-copy route the targets follow the outputs connected right now.
    p.probe.stale = [](const QJsonObject& cached) { return cached.value("acquisition").toString() == "wlr-capture"; };
    // A failed re-probe mustn't drop the route that decides how soon the next one runs.
    p.probe.kept = {"acquisition"};
    return p;
}
QString defaultPath(QString path) { return path.isEmpty() ? QCoreApplication::applicationDirPath() + "/putgd-record" : path; }
}

RecordingHelperBackend::RecordingHelperBackend(QString helperPath, QObject* parent) : QObject(parent), helper_(policy(), defaultPath(std::move(helperPath))) {}
void RecordingHelperBackend::setTimeouts(int startupMs, int commandMs, int prepareMs, int exportMs) { helper_.setStartupMs(startupMs); commandMs_ = commandMs; prepareMs_ = prepareMs; exportMs_ = exportMs; }
void RecordingHelperBackend::microphones(const QString& device, Reply reply) {
    const QJsonObject none{{"available", false}, {"reason", "microphone_probe_failed"}, {"message", "The microphone list could not be read."}, {"devices", QJsonArray{}}, {"defaultIsMonitor", false}};
    if (!helper_.executable()) { reply({}, none); return; }
    auto* process = new QProcess(this);
    auto* deadline = new QTimer(process);
    deadline->setSingleShot(true);
    auto answered = std::make_shared<bool>(false);
    auto finish = [process, reply, none, answered](bool ok) {
        if (std::exchange(*answered, true)) return;
        QJsonObject result = none;
        if (ok) {
            const auto document = QJsonDocument::fromJson(process->readAllStandardOutput().left(probeLimit));
            if (document.isObject() && document.object()["available"].isBool() && document.object()["devices"].isArray()) result = document.object();
        }
        process->deleteLater();
        reply({}, result);
    };
    connect(deadline, &QTimer::timeout, process, [process, finish] { process->kill(); finish(false); });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) finish(false); });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [finish](int code, QProcess::ExitStatus status) { finish(status == QProcess::NormalExit && code == 0); });
    QStringList arguments{"--microphones"};
    if (!device.isEmpty()) arguments.append(device);
    deadline->start(helper_.startupMs());
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start(helper_.helperPath(), arguments, QIODevice::ReadOnly);
}
void RecordingHelperBackend::prepare(const QString& id, const RecordingTarget& target, const RecordingConfig& config, const QString& staging, Reply reply, const QJsonObject& reviewOptions) {
    QJsonObject params{{"id", id}, {"target", target.toJson()}, {"config", config.toJson()}, {"staging", staging}, {"reviewOptions", reviewOptions}};
    if (target.kind == "window" && !target.window.isEmpty()) params["window"] = target.window;
    helper_.prepare(id, params, prepareMs_, std::move(reply));
}
void RecordingHelperBackend::control(const QString& id, const QString& command, const QJsonObject& params, Reply reply) {
    // stop finalizes the file; recording::helperStopMs is this doubled deadline.
    helper_.command(id, command, params, command == "stop" ? 2 * commandMs_ : commandMs_, std::move(reply));
}
void RecordingHelperBackend::preview(const QString& id, const RecordingEdit& edit, const RecordingConfig& config, qint64 timeUs, int maxEdge, Reply reply) {
    helper_.command(id, "preview", {{"edit", edit.toJson()}, {"config", config.toJson()}, {"time", double(timeUs)}, {"maxEdge", maxEdge}}, commandMs_, std::move(reply));
}
void RecordingHelperBackend::exportClip(const QString& id, const RecordingEdit& edit, const RecordingConfig& config, const QString& outputPath, Reply reply) {
    helper_.command(id, "export", {{"edit", edit.toJson()}, {"config", config.toJson()}, {"output", outputPath}}, exportMs_, std::move(reply));
}
void RecordingHelperBackend::setEdit(const QString& id, const RecordingEdit& edit, const RecordingConfig& config, const QJsonObject& limits) {
    // Fire and forget: a helper that is not running, or not this session's, ignores it.
    helper_.command(id, "setEdit", {{"edit", edit.toJson()}, {"config", config.toJson()}, {"limits", limits}}, commandMs_, [](Error, QJsonObject) {});
}
}
