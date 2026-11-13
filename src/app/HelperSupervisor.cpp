#include "HelperSupervisor.h"
#include "core/BuildInfo.h"
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QUuid>

namespace putgd {
namespace {
QJsonObject build() { return QJsonDocument::fromJson(buildManifestJson()).object(); }
constexpr qint64 failedProbeCacheMs = 60000;
}

HelperSupervisor::HelperSupervisor(HelperPolicy policy, QString helperPath)
    : policy_(std::move(policy)), helperPath_(std::move(helperPath)), startup_(this), stopDeadline_(this), probeDeadline_(this),
      output_(policy_.process, logRecording()), probeOutput_(QStringLiteral("putgd-record ") + policy_.probe.argument, logRecording(), true) {
    startup_.setSingleShot(true);
    stopDeadline_.setSingleShot(true);
    probeDeadline_.setSingleShot(true);
    connect(&startup_, &QTimer::timeout, this, [this] {
        if (!child_) return;
        // The helper's ready frame after bootstrap did not come (#250).
        failChild(child_->generation, fault(HelperFault::StartTimeout, the() + " did not answer in time", {{"timeoutMs", startupMs_}}));
    });
    connect(&stopDeadline_, &QTimer::timeout, this, [this] {
        if (!child_ || !child_->process || child_->process->state() == QProcess::NotRunning) return;
        // A failure kill already said why; a shutdown that ran out of time says so here.
        if (output_.killReason().isEmpty()) output_.killing(policy_.prefix + "_stop_timeout", {{"operationId", child_->id}, {"deadline", "stop"}, {"deadlineMs", stopDeadline_.interval()}, {"ready", child_->ready}});
        child_->process->kill();
    });
    connect(&probeDeadline_, &QTimer::timeout, this, [this] {
        if (!probe_) return;
        probeOutput_.killing(policy_.prefix + "_probe_timeout", {{"deadline", "probe"}, {"deadlineMs", probeDeadline_.interval()}});
        probe_->kill();
    });
}
HelperSupervisor::~HelperSupervisor() {
    startup_.stop();
    stopDeadline_.stop();
    probeDeadline_.stop();
    if (probe_) { probe_->disconnect(this); probe_->kill(); probe_->waitForFinished(1000); }
    const auto closing = fault(HelperFault::Closed, "The resident is shutting down");
    if (child_) {
        if (child_->control) child_->control->stop();
        // The helper's pipe closes with it: nothing more is captured or injected.
        if (child_->process) { child_->process->disconnect(this); child_->process->kill(); child_->process->waitForFinished(1000); }
        for (auto& pending : child_->outstanding) pending(closing, {});
    }
    if (queued_) { auto reply = std::move(queued_->reply); queued_.reset(); reply(closing, {}); }
}
Error HelperSupervisor::fault(HelperFault kind, const QString& message, const QJsonObject& details) const { return policy_.error(kind, message, details); }
bool HelperSupervisor::executable() const { return QFileInfo(helperPath_).isExecutable(); }
QJsonObject HelperSupervisor::diagnostics() const {
    return {{"helper", executable()}, {"active", child_ != nullptr}, {"processId", child_ && child_->process ? child_->process->processId() : 0}, {"lastExit", lastExit_}, {"probed", !cached_.isEmpty()}, {"queued", queued_.has_value()}};
}
QJsonObject HelperSupervisor::capabilities() const {
    if (!cached_.isEmpty()) return cached_;
    return policy_.probe.unavailable(executable() ? policy_.prefix + (probe_ ? "_probe_pending" : "_not_probed") : QStringLiteral("recording_helper_missing"));
}
void HelperSupervisor::refreshCapabilities(std::function<void()> done) {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    // value(), not []: [] on an empty cache inserts null members, and capabilities() would return those (#1455).
    const bool stale = policy_.probe.stale && !cached_.isEmpty() && policy_.probe.stale(cached_);
    if (!cached_.isEmpty() && !stale && (cached_.value("available").toBool() || now - cachedAt_ < failedProbeCacheMs)) { done(); return; }
    if (!executable()) { cached_ = policy_.probe.unavailable("recording_helper_missing"); cachedAt_ = now; done(); return; }
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
    process->start(helperPath_, {policy_.probe.argument}, QIODevice::ReadOnly);
}
void HelperSupervisor::finishProbe() {
    auto* process = probe_;
    probe_ = nullptr;
    probeDeadline_.stop();
    QJsonObject result;
    bool clean = false;
    if (process) {
        const auto output = process->readAllStandardOutput();
        const auto document = QJsonDocument::fromJson(output.left(policy_.probe.limit));
        clean = process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0;
        if (clean && output.size() <= policy_.probe.limit && document.isObject()) result = policy_.probe.accept(document.object());
    }
    if (result.isEmpty()) {
        result = policy_.probe.unavailable(clean && !policy_.probe.invalidReason.isEmpty() ? policy_.probe.invalidReason : policy_.prefix + "_probe_failed");
        // A failed re-probe keeps what decides how soon the next one runs.
        for (const auto& key : policy_.probe.kept) if (const auto kept = cached_.value(key); kept.isString()) result[key] = kept;
    }
    if (process) {
        if (probeOutput_.pid() > 0) {
            const bool running = process->state() != QProcess::NotRunning;
            if (running) { process->kill(); process->waitForFinished(1000); }
            probeOutput_.append(process->readAllStandardError());
            probeOutput_.finished(process->exitCode(), process->exitStatus(), running);
        }
        if (result.value("reason").isString()) logging::warning(logRecording(), policy_.probe.unavailableLog, {{"reason", result.value("reason")}});
        process->deleteLater();
    }
    cached_ = result;
    cachedAt_ = QDateTime::currentMSecsSinceEpoch();
    const auto waiters = std::move(probeWaiters_);
    probeWaiters_.clear();
    QPointer<HelperSupervisor> guard(this);
    for (const auto& waiter : waiters) { if (!guard) return; waiter(); }
}
QJsonObject HelperSupervisor::envelope() const { return {{"role", "record"}, {"nonce", child_ ? child_->nonce : QString{}}, {"generation", QString::number(child_ ? child_->generation : 0)}}; }

void HelperSupervisor::prepare(const QString& id, const QJsonObject& params, int prepareMs, Reply reply) {
    if (child_ && !child_->exited) {
        // The previous session already ended (the resident reports none active)
        // and its helper is still shutting down: a prepare right after a cancel
        // or a failure waits for that exit instead of failing (#1296).
        if (child_->shuttingDown && !queued_) { queued_ = Queued{id, params, prepareMs, std::move(reply)}; return; }
        reply(policy_.active, {});
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
        if (child_ && child_->generation == generation && child_->stderrBytes > 1024 * 1024) failChild(generation, fault(HelperFault::Protocol, the() + " flooded its diagnostics"));
    });
    connect(child->control, &UiControl::frame, this, [this, generation](const QString& type, const QString&, const QJsonObject& payload) { receive(generation, type, payload); });
    connect(child->control, &UiControl::failed, this, [this, generation](Error) { failChild(generation, fault(HelperFault::Protocol, the() + " broke its control protocol")); });
    connect(process, &QProcess::started, this, [this, generation] {
        if (!child_ || child_->generation != generation) return;
        output_.started(child_->process->processId(), {{"generation", QString::number(generation)}, {"operationId", child_->id}});
        auto frame = envelope();
        frame["build"] = build();
        frame["appearance"] = QJsonObject{{"scheme", scheme_}};
        // The helper's second session kind.
        if (!policy_.session.isEmpty()) frame["session"] = policy_.session;
        if (child_->control->send("bootstrap", frame).isEmpty()) failChild(generation, fault(HelperFault::Protocol, "Cannot bootstrap the " + policy_.noun));
    });
    connect(process, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !child_ || child_->generation != generation || child_->exited) return;
        child_->exited = true;
        output_.failedToStart(child_->process->errorString());
        failChild(generation, fault(HelperFault::StartFailed, the() + " could not be started"));
        if (child_ && child_->generation == generation && child_->process) { child_->process->disconnect(this); child_->process->deleteLater(); child_->process = nullptr; child_->control = nullptr; /* parented to the process */ }
        launchQueued();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, generation](int code, QProcess::ExitStatus status) { exited(generation, code, status); });
    child_ = std::move(child);
    // A sentinel request waits for `ready`; the helper's prepare result then answers the caller.
    child_->outstanding.insert("prepare", [this, params, prepareMs, reply = std::move(reply)](Error error, QJsonObject) { if (error) { reply(error, {}); return; } send("prepare", params, prepareMs, reply); });
    startup_.start(startupMs_);
    process->start(helperPath_, {});
}
void HelperSupervisor::setAppearance(const QString& scheme) {
    const QString value = scheme == "light" ? QStringLiteral("light") : QStringLiteral("dark");
    if (value == scheme_) return;
    scheme_ = value;
    if (!child_ || !child_->ready || child_->shuttingDown || child_->exited) return;
    auto frame = envelope();
    frame["scheme"] = scheme_;
    if (child_->control->send("appearance", frame).isEmpty()) failChild(child_->generation, fault(HelperFault::Protocol, "Cannot send the appearance to the " + policy_.noun));
}
void HelperSupervisor::command(const QString& id, const QString& command, const QJsonObject& params, int timeoutMs, Reply reply) {
    if (!child_ || child_->id != id) { reply(fault(HelperFault::Closed, "No " + policy_.noun + " owns this session"), {}); return; }
    send(command, params, timeoutMs, std::move(reply));
}
void HelperSupervisor::send(const QString& command, const QJsonObject& params, int timeoutMs, Reply reply) {
    if (!child_ || !child_->ready || child_->shuttingDown || child_->exited) { reply(fault(HelperFault::Closed, the() + " is not running"), {}); return; }
    const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto frame = envelope();
    frame["requestId"] = requestId;
    frame["command"] = command;
    frame["params"] = params;
    const auto generation = child_->generation;
    child_->outstanding.insert(requestId, std::move(reply));
    if (child_->control->send("command", frame).isEmpty()) { failChild(generation, fault(HelperFault::Protocol, "Cannot send a command to the " + policy_.noun)); return; }
    QPointer<HelperSupervisor> guard(this);
    QTimer::singleShot(timeoutMs, this, [guard, generation, requestId, command, timeoutMs] {
        if (!guard || !guard->child_ || guard->child_->generation != generation) return;
        auto& child = *guard->child_;
        auto found = child.outstanding.find(requestId);
        if (found == child.outstanding.end()) return;
        // The step that timed out, for the log and the operation's error (#250).
        logging::warning(logRecording(), guard->policy_.noun.left(1).toUpper() + guard->policy_.noun.mid(1) + " command timed out", {{"operationId", child.id}, {"command", command}, {"timeoutMs", timeoutMs}});
        const auto error = guard->fault(HelperFault::CommandTimeout, guard->the() + QStringLiteral(" did not answer %1 in time").arg(command), {{"step", command}, {"timeoutMs", timeoutMs}});
        if (guard->policy_.timeoutEndsHelper) { guard->failChild(generation, error); return; }
        auto reply = std::move(*found);
        child.outstanding.erase(found);
        child.timedOut.insert(requestId, command);
        reply(error, {});
    });
}
void HelperSupervisor::receive(quint64 generation, const QString& type, const QJsonObject& payload) {
    if (!child_ || child_->generation != generation) return;
    if (payload["role"] != "record" || payload["nonce"] != child_->nonce || payload["generation"] != QString::number(child_->generation)) { failChild(generation, fault(HelperFault::Protocol, the() + " answered with a foreign envelope")); return; }
    if (type == "ready") {
        if (child_->ready || payload.size() != 4) { failChild(generation, fault(HelperFault::Protocol, "Unexpected ready frame")); return; }
        if (payload["build"].toObject() != build()) { failChild(generation, fault(HelperFault::Build, the() + " does not match this build; reinstall the package and restart")); return; }
        child_->ready = true;
        startup_.stop();
        auto found = child_->outstanding.find("prepare");
        if (found != child_->outstanding.end()) { auto reply = std::move(*found); child_->outstanding.erase(found); reply({}, {}); }
        return;
    }
    if (!child_->ready) { failChild(generation, fault(HelperFault::Protocol, the() + " spoke before ready")); return; }
    if (type == "commandResult") {
        // discard() already settled and cleared these requests. The helper can
        // still answer cancel (or an earlier command) while it tears down.
        if (child_->shuttingDown) return;
        const auto requestId = payload["requestId"].toString();
        // The caller already has the timeout for this request (#1443). What the
        // helper did is still the session's business: a late pause or resume
        // changed the clip's state, so the service hears of it as an event.
        if (const auto late = child_->timedOut.constFind(requestId); late != child_->timedOut.cend()) {
            const auto command = late.value();
            child_->timedOut.erase(late);
            logging::info(logRecording(), policy_.noun.left(1).toUpper() + policy_.noun.mid(1) + " answered after its deadline", {{"operationId", child_->id}, {"command", command}});
            if (sink_) sink_({{"id", child_->id}, {"event", "lateResult"}, {"command", command}, {"ok", payload["ok"].toBool()}});
            return;
        }
        auto found = child_->outstanding.find(requestId);
        if (found == child_->outstanding.end() || !payload["ok"].isBool()) { failChild(generation, fault(HelperFault::Protocol, the() + " answered an unknown request")); return; }
        // Checked while the request is still outstanding, so failChild() answers its caller.
        const bool ok = payload["ok"].toBool();
        if (ok && (payload.size() != 6 || !payload["result"].isObject())) { failChild(generation, fault(HelperFault::Protocol, "Malformed helper result")); return; }
        if (!ok && (payload.size() != 8 || !payload["code"].isString() || !payload["message"].isString() || !payload["details"].isObject())) { failChild(generation, fault(HelperFault::Protocol, "Malformed helper error")); return; }
        auto reply = std::move(*found);
        child_->outstanding.erase(found);
        if (ok) reply({}, payload["result"].toObject());
        else reply({payload["code"].toString().left(64), payload["message"].toString().left(512), payload["details"].toObject()}, {});
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
    failChild(generation, fault(HelperFault::Protocol, "Unexpected " + policy_.noun + " frame"));
}
void HelperSupervisor::failChild(quint64 generation, const Error& error) {
    if (!child_ || child_->generation != generation) return;
    startup_.stop();
    auto outstanding = std::move(child_->outstanding);
    child_->outstanding.clear();
    const bool notify = !child_->shuttingDown;
    child_->shuttingDown = true;
    if (child_->control) child_->control->stop();
    if (child_->process && child_->process->state() != QProcess::NotRunning) {
        output_.killing(error.details.value("check").toString(error.code), {{"operationId", child_->id}, {"error", logging::errorFields(error)}, {"ready", child_->ready}, {"outstanding", int(outstanding.size())}});
        // Killed, not asked: a dead process captures and injects nothing.
        child_->process->kill();
        stopDeadline_.start(2000);
    }
    const auto id = child_->id;
    QPointer<HelperSupervisor> guard(this);
    for (auto& pending : outstanding) { if (!guard) return; pending(error, {}); }
    if (guard && notify && sink_) sink_({{"id", id}, {"event", "failed"}, {"error", error.toJson()}});
}
void HelperSupervisor::exited(quint64 generation, int code, QProcess::ExitStatus status) {
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
        const auto error = fault(status == QProcess::NormalExit ? HelperFault::Exited : HelperFault::Crashed, the() + " ended unexpectedly; nothing was " + policy_.lost);
        auto outstanding = std::move(child_->outstanding);
        child_->outstanding.clear();
        const bool notify = !child_->shuttingDown;
        child_->shuttingDown = true;
        const auto id = child_->id;
        QPointer<HelperSupervisor> guard(this);
        for (auto& pending : outstanding) { if (!guard) return; pending(error, {}); }
        if (guard && notify && sink_) sink_({{"id", id}, {"event", "failed"}, {"error", error.toJson()}});
        if (!guard) return;
    }
    if (child_ && child_->generation == generation) { if (child_->process) child_->process->deleteLater(); child_.reset(); }
    launchQueued();
}
void HelperSupervisor::launchQueued() {
    if (!queued_ || (child_ && !child_->exited)) return;
    auto next = std::move(*queued_);
    queued_.reset();
    prepare(next.id, next.params, next.prepareMs, std::move(next.reply));
}
qint64 HelperSupervisor::processId(const QString& id) const {
    return child_ && child_->id == id && !child_->exited && child_->process ? child_->process->processId() : 0;
}
void HelperSupervisor::discard(const QString& id) {
    // Cancelled while it waited for the previous helper: it never starts one.
    if (queued_ && queued_->id == id) { auto reply = std::move(queued_->reply); queued_.reset(); reply(fault(HelperFault::Closed, policy_.discarded), {}); return; }
    if (!child_ || child_->id != id || child_->shuttingDown) return;
    child_->shuttingDown = true;
    startup_.stop();
    auto outstanding = std::move(child_->outstanding);
    child_->outstanding.clear();
    QPointer<HelperSupervisor> guard(this);
    for (auto& pending : outstanding) { if (!guard) return; pending(fault(HelperFault::Closed, policy_.discarded), {}); }
    if (!guard || !child_) return;
    if (child_->process && child_->process->state() != QProcess::NotRunning) {
        // The helper ends its capture or its worker before it leaves; nothing is injected after this frame is read.
        if (child_->ready && child_->control) child_->control->send("shutdown", envelope());
        stopDeadline_.start(2000);
    } else { if (child_->process) child_->process->deleteLater(); child_.reset(); }
}
}
