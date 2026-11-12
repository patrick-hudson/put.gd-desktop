#include "ScrollHelperBackend.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>

namespace putgd {
namespace {
QJsonObject build() { return QJsonDocument::fromJson(buildManifestJson()).object(); }
// The helper died or broke its protocol; `check` says which part.
Error helperError(const QString& check, const QString& message) { return {"scroll_helper_failed", message, {{"check", check}}}; }
QJsonObject unavailable(const QString& reason) { return {{"available", false}, {"reason", reason}}; }
bool validCapabilities(const QJsonObject& o) {
    return o["available"].isBool() && o["injector"].isString() && o["frames"].isString() && o["selection"].isString() && o["maxHeight"].isDouble() && (o["available"].toBool() || o["reason"].isString());
}
constexpr qint64 failedProbeCacheMs = 60000;
constexpr qsizetype probeLimit = 64 * 1024;
}

ScrollHelperBackend::ScrollHelperBackend(QString helperPath, QObject* parent) : QObject(parent), helperPath_(std::move(helperPath)), startup_(this), stopDeadline_(this), probeDeadline_(this) {
    if (helperPath_.isEmpty()) helperPath_ = QCoreApplication::applicationDirPath() + "/putgd-record";
    startup_.setSingleShot(true);
    stopDeadline_.setSingleShot(true);
    probeDeadline_.setSingleShot(true);
    connect(&startup_, &QTimer::timeout, this, [this] {
        if (!child_) return;
        auto error = helperError("start_timeout", "The scroll capture helper did not answer in time");
        error.details["timeoutMs"] = startupMs_;
        failChild(child_->generation, error);
    });
    connect(&stopDeadline_, &QTimer::timeout, this, [this] {
        if (!child_ || !child_->process || child_->process->state() == QProcess::NotRunning) return;
        // A failure kill already said why; a shutdown that ran out of time says so here.
        if (output_.killReason().isEmpty()) output_.killing("scroll_stop_timeout", {{"operationId", child_->id}, {"deadline", "stop"}, {"deadlineMs", stopDeadline_.interval()}, {"ready", child_->ready}});
        child_->process->kill();
    });
    connect(&probeDeadline_, &QTimer::timeout, this, [this] {
        if (!probe_) return;
        probeOutput_.killing("scroll_probe_timeout", {{"deadline", "probe"}, {"deadlineMs", probeDeadline_.interval()}});
        probe_->kill();
    });
}
ScrollHelperBackend::~ScrollHelperBackend() {
    startup_.stop();
    stopDeadline_.stop();
    probeDeadline_.stop();
    if (probe_) { probe_->disconnect(this); probe_->kill(); probe_->waitForFinished(1000); }
    if (child_) {
        if (child_->control) child_->control->stop();
        // The helper's pipe closes with it: nothing more is injected.
        if (child_->process) { child_->process->disconnect(this); child_->process->kill(); child_->process->waitForFinished(1000); }
        for (auto& pending : child_->outstanding) pending(helperError("closed", "The resident is shutting down"), {});
    }
    if (queued_) { auto reply = std::move(queued_->reply); queued_.reset(); reply(helperError("closed", "The resident is shutting down"), {}); }
}
void ScrollHelperBackend::setTimeouts(int startupMs, int commandMs) { startupMs_ = startupMs; commandMs_ = commandMs; }
QJsonObject ScrollHelperBackend::diagnostics() const {
    return {{"helper", QFileInfo(helperPath_).isExecutable()}, {"active", child_ != nullptr}, {"processId", child_ && child_->process ? child_->process->processId() : 0}, {"lastExit", lastExit_}, {"probed", !cached_.isEmpty()}, {"queued", queued_.has_value()}};
}
QJsonObject ScrollHelperBackend::capabilities() const {
    if (!cached_.isEmpty()) return cached_;
    return unavailable(QFileInfo(helperPath_).isExecutable() ? (probe_ ? "scroll_probe_pending" : "scroll_not_probed") : "recording_helper_missing");
}
void ScrollHelperBackend::refreshCapabilities(std::function<void()> done) {
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
    process->start(helperPath_, {"--scroll-capabilities"}, QIODevice::ReadOnly);
}
void ScrollHelperBackend::finishProbe() {
    auto* process = probe_;
    probe_ = nullptr;
    probeDeadline_.stop();
    QJsonObject result = unavailable("scroll_probe_failed");
    if (process) {
        const auto output = process->readAllStandardOutput();
        const auto document = QJsonDocument::fromJson(output.left(probeLimit));
        if (process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0 && output.size() <= probeLimit && document.isObject() && validCapabilities(document.object())) {
            const auto probed = document.object();
            // The contract's keys, and the two the resident acts on besides.
            result = {{"available", probed["available"]}, {"injector", probed["injector"]}, {"frames", probed["frames"]}, {"selection", probed["selection"]}, {"maxHeight", probed["maxHeight"]}};
            if (!probed["available"].toBool()) { result["reason"] = probed["reason"].toString().left(64); result["message"] = probed["message"].toString().left(512); }
        }
        if (probeOutput_.pid() > 0) {
            const bool running = process->state() != QProcess::NotRunning;
            if (running) { process->kill(); process->waitForFinished(1000); }
            probeOutput_.append(process->readAllStandardError());
            probeOutput_.finished(process->exitCode(), process->exitStatus(), running);
        }
        if (result.value("reason").isString()) logging::warning(logRecording(), "Scrolling capture is unavailable", {{"reason", result.value("reason")}});
        process->deleteLater();
    }
    cached_ = result;
    cachedAt_ = QDateTime::currentMSecsSinceEpoch();
    const auto waiters = std::move(probeWaiters_);
    probeWaiters_.clear();
    QPointer<ScrollHelperBackend> guard(this);
    for (const auto& waiter : waiters) { if (!guard) return; waiter(); }
}
QJsonObject ScrollHelperBackend::envelope() const { return {{"role", "record"}, {"nonce", child_ ? child_->nonce : QString{}}, {"generation", QString::number(child_ ? child_->generation : 0)}}; }

void ScrollHelperBackend::prepare(const QString& id, const QJsonObject& params, const QString& staging, Reply reply) {
    if (!QFileInfo(helperPath_).isExecutable()) { reply({"scroll_unavailable", "Scrolling capture is not available on this installation", {{"reason", "recording_helper_missing"}}}, {}); return; }
    if (child_ && !child_->exited) {
        // The previous capture already ended and its helper is still leaving:
        // a prepare right after a cancel waits for that exit instead of failing.
        if (child_->shuttingDown && !queued_) { queued_ = Queued{id, params, staging, std::move(reply)}; return; }
        reply({"scroll_active", "A scroll capture is already running", {}}, {});
        return;
    }
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
        if (child_ && child_->generation == generation && child_->stderrBytes > 1024 * 1024) failChild(generation, helperError("protocol", "The scroll capture helper flooded its diagnostics"));
    });
    connect(child->control, &UiControl::frame, this, [this, generation](const QString& type, const QString&, const QJsonObject& payload) { receive(generation, type, payload); });
    connect(child->control, &UiControl::failed, this, [this, generation](Error) { failChild(generation, helperError("protocol", "The scroll capture helper broke its control protocol")); });
    connect(process, &QProcess::started, this, [this, generation] {
        if (!child_ || child_->generation != generation) return;
        output_.started(child_->process->processId(), {{"generation", QString::number(generation)}, {"operationId", child_->id}});
        auto frame = envelope();
        frame["build"] = build();
        frame["appearance"] = QJsonObject{{"scheme", scheme_}};
        // The helper's second session kind.
        frame["session"] = "scroll";
        if (child_->control->send("bootstrap", frame).isEmpty()) failChild(generation, helperError("protocol", "Cannot bootstrap the scroll capture helper"));
    });
    connect(process, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !child_ || child_->generation != generation || child_->exited) return;
        child_->exited = true;
        output_.failedToStart(child_->process->errorString());
        failChild(generation, helperError("start", "The scroll capture helper could not be started"));
        if (child_ && child_->generation == generation && child_->process) { child_->process->disconnect(this); child_->process->deleteLater(); child_->process = nullptr; child_->control = nullptr; /* parented to the process */ }
        launchQueued();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, generation](int code, QProcess::ExitStatus status) { exited(generation, code, status); });
    child_ = std::move(child);
    // A sentinel request waits for `ready`; the helper's prepare result then answers the caller.
    auto prepared = params;
    prepared["id"] = id;
    prepared["staging"] = staging;
    auto userReply = std::move(reply);
    child_->outstanding.insert("prepare", [this, prepared, userReply](Error error, QJsonObject) { if (error) { userReply(error, {}); return; } send("prepare", prepared, commandMs_, userReply); });
    startup_.start(startupMs_);
    process->start(helperPath_, {});
}
void ScrollHelperBackend::setAppearance(const QString& scheme) {
    const QString value = scheme == "light" ? QStringLiteral("light") : QStringLiteral("dark");
    if (value == scheme_) return;
    scheme_ = value;
    if (!child_ || !child_->ready || child_->shuttingDown || child_->exited) return;
    auto frame = envelope();
    frame["scheme"] = scheme_;
    if (child_->control->send("appearance", frame).isEmpty()) failChild(child_->generation, helperError("protocol", "Cannot send the appearance to the scroll capture helper"));
}
void ScrollHelperBackend::send(const QString& command, const QJsonObject& params, int timeoutMs, Reply reply) {
    if (!child_ || !child_->ready || child_->shuttingDown || child_->exited) { reply(helperError("closed", "The scroll capture helper is not running"), {}); return; }
    const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto frame = envelope();
    frame["requestId"] = requestId;
    frame["command"] = command;
    frame["params"] = params;
    const auto generation = child_->generation;
    child_->outstanding.insert(requestId, std::move(reply));
    if (child_->control->send("command", frame).isEmpty()) { failChild(generation, helperError("protocol", "Cannot send a command to the scroll capture helper")); return; }
    QPointer<ScrollHelperBackend> guard(this);
    QTimer::singleShot(timeoutMs, this, [guard, generation, requestId, command, timeoutMs] {
        if (!guard || !guard->child_ || guard->child_->generation != generation) return;
        auto found = guard->child_->outstanding.find(requestId);
        if (found == guard->child_->outstanding.end()) return;
        logging::warning(logRecording(), "Scroll capture helper command timed out", {{"operationId", guard->child_->id}, {"command", command}, {"timeoutMs", timeoutMs}});
        // A helper that stopped answering may still be turning the wheel: end it.
        auto error = helperError("command_timeout", QStringLiteral("The scroll capture helper did not answer %1 in time").arg(command));
        error.details["step"] = command;
        error.details["timeoutMs"] = timeoutMs;
        guard->failChild(generation, error);
    });
}
void ScrollHelperBackend::receive(quint64 generation, const QString& type, const QJsonObject& payload) {
    if (!child_ || child_->generation != generation) return;
    if (payload["role"] != "record" || payload["nonce"] != child_->nonce || payload["generation"] != QString::number(child_->generation)) { failChild(generation, helperError("protocol", "The scroll capture helper answered with a foreign envelope")); return; }
    if (type == "ready") {
        if (child_->ready || payload.size() != 4) { failChild(generation, helperError("protocol", "Unexpected ready frame")); return; }
        if (payload["build"].toObject() != build()) { failChild(generation, helperError("build", "The scroll capture helper does not match this build; reinstall the package and restart")); return; }
        child_->ready = true;
        startup_.stop();
        auto found = child_->outstanding.find("prepare");
        if (found != child_->outstanding.end()) { auto reply = std::move(*found); child_->outstanding.erase(found); reply({}, {}); }
        return;
    }
    if (!child_->ready) { failChild(generation, helperError("protocol", "The scroll capture helper spoke before ready")); return; }
    if (type == "commandResult") {
        // discard() already settled and cleared these requests; the helper can still answer while it leaves.
        if (child_->shuttingDown) return;
        const auto requestId = payload["requestId"].toString();
        auto found = child_->outstanding.find(requestId);
        if (found == child_->outstanding.end() || !payload["ok"].isBool()) { failChild(generation, helperError("protocol", "The scroll capture helper answered an unknown request")); return; }
        auto reply = std::move(*found);
        child_->outstanding.erase(found);
        if (payload["ok"].toBool()) {
            if (payload.size() != 6 || !payload["result"].isObject()) { failChild(generation, helperError("protocol", "Malformed helper result")); return; }
            reply({}, payload["result"].toObject());
        } else {
            if (payload.size() != 8 || !payload["code"].isString() || !payload["message"].isString() || !payload["details"].isObject()) { failChild(generation, helperError("protocol", "Malformed helper error")); return; }
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
    failChild(generation, helperError("protocol", "Unexpected scroll capture helper frame"));
}
void ScrollHelperBackend::failChild(quint64 generation, const Error& error) {
    if (!child_ || child_->generation != generation) return;
    startup_.stop();
    auto outstanding = std::move(child_->outstanding);
    child_->outstanding.clear();
    const bool notify = !child_->shuttingDown;
    child_->shuttingDown = true;
    if (child_->control) child_->control->stop();
    if (child_->process && child_->process->state() != QProcess::NotRunning) {
        output_.killing(error.details["check"].toString(error.code), {{"operationId", child_->id}, {"error", logging::errorFields(error)}, {"ready", child_->ready}, {"outstanding", int(outstanding.size())}});
        // Killed, not asked: a dead process injects nothing.
        child_->process->kill();
        stopDeadline_.start(2000);
    }
    const auto id = child_->id;
    QPointer<ScrollHelperBackend> guard(this);
    for (auto& pending : outstanding) { if (!guard) return; pending(error, {}); }
    if (guard && notify && sink_) sink_({{"id", id}, {"event", "failed"}, {"error", error.toJson()}});
}
void ScrollHelperBackend::exited(quint64 generation, int code, QProcess::ExitStatus status) {
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
        const auto error = helperError(status == QProcess::NormalExit ? "exited" : "crashed", "The scroll capture helper ended unexpectedly; nothing was captured");
        auto outstanding = std::move(child_->outstanding);
        child_->outstanding.clear();
        const bool notify = !child_->shuttingDown;
        child_->shuttingDown = true;
        const auto id = child_->id;
        QPointer<ScrollHelperBackend> guard(this);
        for (auto& pending : outstanding) { if (!guard) return; pending(error, {}); }
        if (guard && notify && sink_) sink_({{"id", id}, {"event", "failed"}, {"error", error.toJson()}});
        if (!guard) return;
    }
    if (child_ && child_->generation == generation) { if (child_->process) child_->process->deleteLater(); child_.reset(); }
    launchQueued();
}
void ScrollHelperBackend::launchQueued() {
    if (!queued_ || (child_ && !child_->exited)) return;
    auto next = std::move(*queued_);
    queued_.reset();
    prepare(next.id, next.params, next.staging, std::move(next.reply));
}
void ScrollHelperBackend::control(const QString& id, const QString& command, const QJsonObject& params, Reply reply) {
    if (!child_ || child_->id != id) { reply(helperError("closed", "No scroll capture helper owns this session"), {}); return; }
    // stop encodes the PNG; scroll::helperStopMs is this doubled deadline.
    send(command, params, command == "stop" ? 2 * commandMs_ : commandMs_, std::move(reply));
}
qint64 ScrollHelperBackend::processId(const QString& id) const {
    return child_ && child_->id == id && !child_->exited && child_->process ? child_->process->processId() : 0;
}
void ScrollHelperBackend::discard(const QString& id) {
    // Cancelled while it waited for the previous helper: it never starts one.
    if (queued_ && queued_->id == id) { auto reply = std::move(queued_->reply); queued_.reset(); reply(helperError("closed", "The scroll capture was discarded"), {}); return; }
    if (!child_ || child_->id != id || child_->shuttingDown) return;
    child_->shuttingDown = true;
    startup_.stop();
    auto outstanding = std::move(child_->outstanding);
    child_->outstanding.clear();
    QPointer<ScrollHelperBackend> guard(this);
    for (auto& pending : outstanding) { if (!guard) return; pending(helperError("closed", "The scroll capture was discarded"), {}); }
    if (!guard || !child_) return;
    if (child_->process && child_->process->state() != QProcess::NotRunning) {
        // The helper ends its worker before it leaves; nothing is injected after this frame is read.
        if (child_->ready && child_->control) child_->control->send("shutdown", envelope());
        stopDeadline_.start(2000);
    } else { if (child_->process) child_->process->deleteLater(); child_.reset(); }
}
}
