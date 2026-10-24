#include "RecordingHelperBackend.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace putgd {
namespace {
QJsonObject build() { return QJsonDocument::fromJson(buildManifestJson()).object(); }
Error helperError(const QString& code, const QString& message) { return {code, message, {}}; }
QJsonObject unavailable(const QString& reason) {
    return {{"available", false}, {"reason", reason}, {"targets", QJsonArray{}}, {"formats", QJsonArray{}}, {"codecs", QJsonObject{}},
        {"audio", QJsonObject{{"system", false}, {"microphone", false}}}, {"overlayExclusion", QJsonObject{}}};
}
bool validCapabilities(const QJsonObject& o) {
    return o["available"].isBool() && o["targets"].isArray() && o["formats"].isArray() && o["codecs"].isObject() && o["audio"].isObject() && (o["available"].toBool() || o["reason"].isString());
}
constexpr qint64 failedProbeCacheMs = 60000;
constexpr qsizetype probeLimit = 1024 * 1024;
}

RecordingHelperBackend::RecordingHelperBackend(QString helperPath, QObject* parent) : QObject(parent), helperPath_(std::move(helperPath)), startup_(this), stopDeadline_(this), probeDeadline_(this) {
    if (helperPath_.isEmpty()) helperPath_ = QCoreApplication::applicationDirPath() + "/putgd-record";
    startup_.setSingleShot(true);
    stopDeadline_.setSingleShot(true);
    probeDeadline_.setSingleShot(true);
    connect(&startup_, &QTimer::timeout, this, [this] {
        if (!child_) return;
        // The step that timed out: the helper's ready frame after bootstrap (#250).
        auto error = helperError(child_->ready ? "recording_helper_timeout" : "recording_helper_start_timeout", "The recording helper did not answer in time");
        error.details = {{"step", child_->ready ? "command" : "ready"}, {"timeoutMs", startupMs_}};
        failChild(child_->generation, error);
    });
    connect(&stopDeadline_, &QTimer::timeout, this, [this] {
        if (!child_ || !child_->process || child_->process->state() == QProcess::NotRunning) return;
        // A failure kill already said why; a shutdown that ran out of time says so here.
        if (output_.killReason().isEmpty()) output_.killing("recording_stop_timeout", {{"operationId", child_->id}, {"deadline", "stop"}, {"deadlineMs", stopDeadline_.interval()}, {"ready", child_->ready}});
        child_->process->kill();
    });
    connect(&probeDeadline_, &QTimer::timeout, this, [this] {
        if (!probe_) return;
        probeOutput_.killing("recording_probe_timeout", {{"deadline", "probe"}, {"deadlineMs", probeDeadline_.interval()}});
        probe_->kill();
    });
}
RecordingHelperBackend::~RecordingHelperBackend() {
    startup_.stop();
    stopDeadline_.stop();
    probeDeadline_.stop();
    if (probe_) { probe_->disconnect(this); probe_->kill(); probe_->waitForFinished(1000); }
    if (child_) {
        if (child_->control) child_->control->stop();
        if (child_->process) { child_->process->disconnect(this); child_->process->kill(); child_->process->waitForFinished(1000); }
        for (auto& pending : child_->outstanding) pending.reply(helperError("recording_helper_closed", "The resident is shutting down"), {});
    }
}
void RecordingHelperBackend::setTimeouts(int startupMs, int commandMs, int prepareMs, int exportMs) { startupMs_ = startupMs; commandMs_ = commandMs; prepareMs_ = prepareMs; exportMs_ = exportMs; }
QJsonObject RecordingHelperBackend::diagnostics() const {
    return {{"helper", QFileInfo(helperPath_).isExecutable()}, {"active", child_ != nullptr}, {"processId", child_ && child_->process ? child_->process->processId() : 0}, {"lastExit", lastExit_}, {"probed", !cached_.isEmpty()}};
}
QJsonObject RecordingHelperBackend::capabilities() const {
    if (!cached_.isEmpty()) return cached_;
    return unavailable(QFileInfo(helperPath_).isExecutable() ? (probe_ ? "recording_probe_pending" : "recording_not_probed") : "recording_helper_missing");
}
void RecordingHelperBackend::refreshCapabilities(std::function<void()> done) {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (!cached_.isEmpty() && (cached_["available"].toBool() || now - cachedAt_ < failedProbeCacheMs)) { done(); return; }
    if (!QFileInfo(helperPath_).isExecutable()) { cached_ = unavailable("recording_helper_missing"); cachedAt_ = now; done(); return; }
    probeWaiters_.append(std::move(done));
    if (probe_) return;
    auto* process = new QProcess(this);
    probe_ = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] { const auto bytes = process->readAllStandardError(); if (probe_ == process) probeOutput_.append(bytes); });
    connect(process, &QProcess::started, this, [this, process] { if (probe_ == process) probeOutput_.started(process->processId()); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) { if (probe_ == process && error == QProcess::FailedToStart) { probeOutput_.failedToStart(process->errorString()); finishProbe(); } });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process](int, QProcess::ExitStatus) { if (probe_ == process) finishProbe(); });
    probeDeadline_.start(startupMs_);
    process->start(helperPath_, {"--capabilities"}, QIODevice::ReadOnly);
}
void RecordingHelperBackend::finishProbe() {
    auto* process = probe_;
    probe_ = nullptr;
    probeDeadline_.stop();
    QJsonObject result = unavailable("recording_probe_failed");
    if (process) {
        const auto output = process->readAllStandardOutput();
        const auto document = QJsonDocument::fromJson(output.left(probeLimit));
        if (process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0 && output.size() <= probeLimit && document.isObject() && validCapabilities(document.object())) result = document.object();
        else if (process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0) result = unavailable("recording_probe_invalid");
        if (probeOutput_.pid() > 0) {
            const bool running = process->state() != QProcess::NotRunning;
            if (running) { process->kill(); process->waitForFinished(1000); }
            probeOutput_.append(process->readAllStandardError());
            probeOutput_.finished(process->exitCode(), process->exitStatus(), running);
        }
        if (result["reason"].isString()) logging::warning(logRecording(), "Screen recording is unavailable", {{"reason", result["reason"]}});
        process->deleteLater();
    }
    cached_ = result;
    cachedAt_ = QDateTime::currentMSecsSinceEpoch();
    const auto waiters = std::move(probeWaiters_);
    probeWaiters_.clear();
    QPointer<RecordingHelperBackend> guard(this);
    for (const auto& waiter : waiters) { if (!guard) return; waiter(); }
}
QJsonObject RecordingHelperBackend::envelope() const { return {{"role", "record"}, {"nonce", child_ ? child_->nonce : QString{}}, {"generation", QString::number(child_ ? child_->generation : 0)}}; }

void RecordingHelperBackend::prepare(const QString& id, const RecordingTarget& target, const RecordingConfig& config, const QString& staging, Reply reply) {
    if (child_ && !child_->exited) { reply(helperError("recording_active", "A recording helper is already running"), {}); return; }
    if (child_) child_.reset();
    auto child = std::make_unique<Child>();
    child->id = id;
    child->nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    child->generation = ++generation_;
    auto* process = new QProcess(this);
    child->process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setReadChannel(QProcess::StandardOutput);
    child->control = new UiControl(*process, process);
    const auto generation = child->generation;
    connect(process, &QProcess::readyReadStandardError, this, [this, process, generation] {
        process->setReadChannel(QProcess::StandardError);
        while (process->bytesAvailable() > 0) { const auto bytes = process->read(16384); if (child_ && child_->generation == generation) { child_->stderrBytes += bytes.size(); output_.append(bytes); } }
        process->setReadChannel(QProcess::StandardOutput);
        if (child_ && child_->generation == generation && child_->stderrBytes > 1024 * 1024) failChild(generation, helperError("recording_helper_protocol", "The recording helper flooded its diagnostics"));
    });
    connect(child->control, &UiControl::frame, this, [this, generation](const QString& type, const QString&, const QJsonObject& payload) { receive(generation, type, payload); });
    connect(child->control, &UiControl::failed, this, [this, generation](Error) { failChild(generation, helperError("recording_helper_protocol", "The recording helper broke its control protocol")); });
    connect(process, &QProcess::started, this, [this, generation] {
        if (!child_ || child_->generation != generation) return;
        output_.started(child_->process->processId(), {{"generation", QString::number(generation)}, {"operationId", child_->id}});
        auto frame = envelope();
        frame["build"] = build();
        frame["appearance"] = QJsonObject{{"scheme", scheme_}};
        if (child_->control->send("bootstrap", frame).isEmpty()) failChild(generation, helperError("recording_helper_protocol", "Cannot bootstrap the recording helper"));
    });
    connect(process, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) { if (child_ && child_->generation == generation && !child_->exited) { child_->exited = true; output_.failedToStart(child_->process->errorString()); failChild(generation, helperError("recording_helper_start_failed", "The recording helper could not be started")); } } });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, generation](int code, QProcess::ExitStatus status) { exited(generation, code, status); });
    child_ = std::move(child);
    // A sentinel request waits for `ready`; the helper's prepare result then answers the caller.
    const QJsonObject params{{"id", id}, {"target", target.toJson()}, {"config", config.toJson()}, {"staging", staging}};
    auto userReply = std::move(reply);
    child_->outstanding.insert("prepare", {[this, params, userReply](Error error, QJsonObject) { if (error) { userReply(error, {}); return; } send("prepare", params, prepareMs_, userReply); }});
    startup_.start(startupMs_);
    process->start(helperPath_, {});
}
void RecordingHelperBackend::setAppearance(const QString& scheme) {
    const QString value = scheme == "light" ? QStringLiteral("light") : QStringLiteral("dark");
    if (value == scheme_) return;
    scheme_ = value;
    if (!child_ || !child_->ready || child_->shuttingDown || child_->exited) return;
    auto frame = envelope();
    frame["scheme"] = scheme_;
    if (child_->control->send("appearance", frame).isEmpty()) failChild(child_->generation, helperError("recording_helper_protocol", "Cannot send the appearance to the recording helper"));
}
void RecordingHelperBackend::send(const QString& command, const QJsonObject& params, int timeoutMs, Reply reply) {
    if (!child_ || !child_->ready || child_->shuttingDown || child_->exited) { reply(helperError("recording_helper_closed", "The recording helper is not running"), {}); return; }
    const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto frame = envelope();
    frame["requestId"] = requestId;
    frame["command"] = command;
    frame["params"] = params;
    const auto generation = child_->generation;
    child_->outstanding.insert(requestId, {std::move(reply)});
    if (child_->control->send("command", frame).isEmpty()) { failChild(generation, helperError("recording_helper_protocol", "Cannot send a command to the recording helper")); return; }
    QPointer<RecordingHelperBackend> guard(this);
    QTimer::singleShot(timeoutMs, this, [guard, generation, requestId, command, timeoutMs] {
        if (!guard || !guard->child_ || guard->child_->generation != generation) return;
        auto found = guard->child_->outstanding.find(requestId);
        if (found == guard->child_->outstanding.end()) return;
        auto reply = std::move(found->reply);
        guard->child_->outstanding.erase(found);
        // The step that timed out, for the log and the operation's error (#250).
        logging::warning(logRecording(), "Recording helper command timed out", {{"operationId", guard->child_->id}, {"command", command}, {"timeoutMs", timeoutMs}});
        auto error = helperError("recording_helper_timeout", QStringLiteral("The recording helper did not answer %1 in time").arg(command));
        error.details = {{"step", command}, {"timeoutMs", timeoutMs}};
        reply(error, {});
    });
}
void RecordingHelperBackend::receive(quint64 generation, const QString& type, const QJsonObject& payload) {
    if (!child_ || child_->generation != generation) return;
    if (payload["role"] != "record" || payload["nonce"] != child_->nonce || payload["generation"] != QString::number(child_->generation)) { failChild(generation, helperError("recording_helper_protocol", "The recording helper answered with a foreign envelope")); return; }
    if (type == "ready") {
        if (child_->ready || payload.size() != 4) { failChild(generation, helperError("recording_helper_protocol", "Unexpected ready frame")); return; }
        if (payload["build"].toObject() != build()) { failChild(generation, helperError("recording_incompatible", "The recording helper does not match this build; reinstall the package and restart")); return; }
        child_->ready = true;
        startup_.stop();
        auto found = child_->outstanding.find("prepare");
        if (found != child_->outstanding.end()) { auto reply = std::move(found->reply); child_->outstanding.erase(found); reply({}, {}); }
        return;
    }
    if (!child_->ready) { failChild(generation, helperError("recording_helper_protocol", "The recording helper spoke before ready")); return; }
    if (type == "commandResult") {
        // discard() already settled and cleared these requests. The helper can
        // still answer cancel (or an earlier command) while it tears down.
        if (child_->shuttingDown) return;
        const auto requestId = payload["requestId"].toString();
        auto found = child_->outstanding.find(requestId);
        if (found == child_->outstanding.end() || !payload["ok"].isBool()) { failChild(generation, helperError("recording_helper_protocol", "The recording helper answered an unknown request")); return; }
        auto reply = std::move(found->reply);
        child_->outstanding.erase(found);
        if (payload["ok"].toBool()) {
            if (payload.size() != 6 || !payload["result"].isObject()) { failChild(generation, helperError("recording_helper_protocol", "Malformed helper result")); return; }
            reply({}, payload["result"].toObject());
        } else {
            if (payload.size() != 8 || !payload["code"].isString() || !payload["message"].isString() || !payload["details"].isObject()) { failChild(generation, helperError("recording_helper_protocol", "Malformed helper error")); return; }
            reply({payload["code"].toString().left(64), payload["message"].toString().left(512), payload["details"].toObject()}, {});
        }
        return;
    }
    if (type == "event") {
        if (!payload["event"].isString() || child_->shuttingDown) return;
        auto event = payload;
        for (const auto* key : {"role", "nonce", "generation"}) event.remove(key);
        event["id"] = child_->id;
        if (sink_) sink_(event);
        return;
    }
    failChild(generation, helperError("recording_helper_protocol", "Unexpected recording helper frame"));
}
void RecordingHelperBackend::failChild(quint64 generation, const Error& error) {
    if (!child_ || child_->generation != generation) return;
    startup_.stop();
    auto outstanding = std::move(child_->outstanding);
    child_->outstanding.clear();
    const bool notify = !child_->shuttingDown;
    child_->shuttingDown = true;
    if (child_->control) child_->control->stop();
    if (child_->process && child_->process->state() != QProcess::NotRunning) {
        output_.killing(error.code, {{"operationId", child_->id}, {"error", logging::errorFields(error)}, {"ready", child_->ready}, {"outstanding", int(outstanding.size())}});
        child_->process->kill();
        stopDeadline_.start(2000);
    }
    const auto id = child_->id;
    QPointer<RecordingHelperBackend> guard(this);
    for (auto& pending : outstanding) { if (!guard) return; pending.reply(error, {}); }
    if (guard && notify && sink_) sink_({{"id", id}, {"event", "failed"}, {"error", error.toJson()}});
}
void RecordingHelperBackend::exited(quint64 generation, int code, QProcess::ExitStatus status) {
    if (!child_ || child_->generation != generation || child_->exited) return;
    child_->exited = true;
    startup_.stop();
    stopDeadline_.stop();
    lastExit_ = {{"generation", QString::number(generation)}, {"processId", child_->process ? child_->process->processId() : 0}, {"code", code}, {"status", status == QProcess::NormalExit ? "normal" : "crash"}};
    if (child_->process) output_.append(child_->process->readAllStandardError());
    output_.finished(code, status, child_->shuttingDown);
    if (child_->control) child_->control->stop();
    const bool clean = child_->shuttingDown && status == QProcess::NormalExit && code == 0;
    if (!clean) {
        const auto error = helperError(status == QProcess::NormalExit ? "recording_helper_exited" : "recording_helper_crashed", "The recording helper ended unexpectedly; nothing was uploaded");
        auto outstanding = std::move(child_->outstanding);
        child_->outstanding.clear();
        const bool notify = !child_->shuttingDown;
        child_->shuttingDown = true;
        const auto id = child_->id;
        QPointer<RecordingHelperBackend> guard(this);
        for (auto& pending : outstanding) { if (!guard) return; pending.reply(error, {}); }
        if (guard && notify && sink_) sink_({{"id", id}, {"event", "failed"}, {"error", error.toJson()}});
        if (!guard) return;
    }
    if (child_ && child_->generation == generation) { if (child_->process) child_->process->deleteLater(); child_.reset(); }
}
void RecordingHelperBackend::control(const QString& id, const QString& command, const QJsonObject& params, Reply reply) {
    if (!child_ || child_->id != id) { reply(helperError("recording_helper_closed", "No recording helper owns this session"), {}); return; }
    // stop finalizes the file; recording::helperStopMs is this doubled deadline.
    send(command, params, command == "stop" ? 2 * commandMs_ : commandMs_, std::move(reply));
}
void RecordingHelperBackend::preview(const QString& id, const RecordingEdit& edit, const RecordingConfig& config, qint64 timeUs, int maxEdge, Reply reply) {
    if (!child_ || child_->id != id) { reply(helperError("recording_helper_closed", "No recording helper owns this session"), {}); return; }
    send("preview", {{"edit", edit.toJson()}, {"config", config.toJson()}, {"time", double(timeUs)}, {"maxEdge", maxEdge}}, commandMs_, std::move(reply));
}
void RecordingHelperBackend::exportClip(const QString& id, const RecordingEdit& edit, const RecordingConfig& config, const QString& outputPath, Reply reply) {
    if (!child_ || child_->id != id) { reply(helperError("recording_helper_closed", "No recording helper owns this session"), {}); return; }
    send("export", {{"edit", edit.toJson()}, {"config", config.toJson()}, {"output", outputPath}}, exportMs_, std::move(reply));
}
void RecordingHelperBackend::setEdit(const QString& id, const RecordingEdit& edit, const RecordingConfig& config, const QJsonObject& limits) {
    if (!child_ || child_->id != id || !child_->ready || child_->shuttingDown) return;
    send("setEdit", {{"edit", edit.toJson()}, {"config", config.toJson()}, {"limits", limits}}, commandMs_, [](Error, QJsonObject) {});
}
qint64 RecordingHelperBackend::processId(const QString& id) const {
    return child_ && child_->id == id && !child_->exited && child_->process ? child_->process->processId() : 0;
}
void RecordingHelperBackend::discard(const QString& id) {
    if (!child_ || child_->id != id || child_->shuttingDown) return;
    child_->shuttingDown = true;
    startup_.stop();
    auto outstanding = std::move(child_->outstanding);
    child_->outstanding.clear();
    QPointer<RecordingHelperBackend> guard(this);
    for (auto& pending : outstanding) { if (!guard) return; pending.reply(helperError("recording_helper_closed", "The recording session was discarded"), {}); }
    if (!guard || !child_) return;
    if (child_->process && child_->process->state() != QProcess::NotRunning) {
        if (child_->ready && child_->control) child_->control->send("shutdown", envelope());
        stopDeadline_.start(2000);
    } else { if (child_->process) child_->process->deleteLater(); child_.reset(); }
}
}
