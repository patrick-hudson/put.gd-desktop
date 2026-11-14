#include "RecordingService.h"
#include "core/Log.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMetaObject>
#include <QTimer>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace putgd {
namespace {
Error invalid(const QString& message) { return {"invalid_request", message, {}}; }
bool integer(const QJsonValue& value, qint64 min, qint64 max, qint64* out) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < double(min) || number > double(max)) return false;
    *out = qint64(number);
    return true;
}
QJsonValue sizeJson(const QSize& size) { return size.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonArray{size.width(), size.height()}); }
// Keeps the previous canvas when the helper reports none or a malformed one.
void updateCanvas(QSize& canvas, const QJsonValue& value) {
    const auto array = value.toArray();
    if (value.isArray() && array.size() == 2) canvas = QSize(array[0].toInt(), array[1].toInt());
}
// The recording, its export and its upload copy each take a third of the space
// above the reserve; one minute of a busy 1080p screen fits this budget.
Error diskSpaceError(const ArtifactStore::DiskBudget& disk) {
    const qint64 needed = disk.reserveBytes + disk.reservedBytes + 3 * RecordingService::minimumBudgetBytes;
    const qint64 shortfall = std::max<qint64>(1, needed - disk.freeBytes);
    return {"disk_space_insufficient",
            QStringLiteral("Not enough free disk space to record: %1 is free, put.gd keeps %2 free for the system, and a recording needs %3 more. Free up at least %4 and try again.")
                .arg(ArtifactStore::sizeText(disk.freeBytes), ArtifactStore::sizeText(disk.reserveBytes + disk.reservedBytes),
                     ArtifactStore::sizeText(3 * RecordingService::minimumBudgetBytes), ArtifactStore::sizeText(shortfall)),
            {{"freeBytes", double(disk.freeBytes)}, {"reserveBytes", double(disk.reserveBytes)}, {"reservedBytes", double(disk.reservedBytes)},
             {"availableBytes", double(disk.available())}, {"requiredBytes", double(3 * RecordingService::minimumBudgetBytes)},
             {"shortfallBytes", double(shortfall)}, {"hint", "Free up space in put.gd's data folder, then record again"}}};
}
QString clockText(qint64 us) {
    const qint64 seconds = us / 1000000;
    return seconds >= 3600 ? QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'))
                           : QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}
QJsonArray rectJson(const QRect& r) { return {r.x(), r.y(), r.width(), r.height()}; }
// [x, y, width, height] in integers with a positive size.
bool rectFrom(const QJsonValue& value, QRect& out) {
    const auto array = value.toArray();
    if (!value.isArray() || array.size() != 4) return false;
    qint64 v[4];
    for (int i = 0; i < 4; ++i) if (!integer(array[i], i < 2 ? -(1LL << 20) : 1, 1LL << 20, &v[i])) return false;
    out = QRect(int(v[0]), int(v[1]), int(v[2]), int(v[3]));
    return true;
}
}

QJsonObject UnavailableRecordingBackend::capabilities() const {
    return {{"available", false}, {"reason", reason_}, {"targets", QJsonArray{}}, {"formats", QJsonArray{}}, {"codecs", QJsonObject{}},
        {"audio", QJsonObject{{"system", false}, {"microphone", false}}}, {"overlayExclusion", QJsonObject{}}};
}

RecordingService::RecordingService(QString stagingRoot, Host host, QObject* parent)
    : QObject(parent), stagingRoot_(std::move(stagingRoot)), host_(std::move(host)), backend_(std::make_unique<UnavailableRecordingBackend>()) {}
RecordingService::~RecordingService() { shutdown(); }
void RecordingService::setBackend(std::unique_ptr<RecordingBackend> backend) {
    backend_ = backend ? std::move(backend) : std::make_unique<UnavailableRecordingBackend>();
    QPointer<RecordingService> guard(this);
    backend_->setEventSink([guard](QJsonObject event) {
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, event] { if (guard && !guard->stopping_) guard->handleEvent(event); }, Qt::QueuedConnection);
    });
}
QJsonObject RecordingService::limitsJson(const Session& s) const {
    const auto limit = limits(s.op.settings);
    return {{"animationSeconds", double(limit.animationMaxUs / 1000000)}, {"animationEdge", limit.animationMaxEdge}};
}
RecordingLimits RecordingService::limits(const Settings& settings) {
    return {settings.limits.recordingAnimationSeconds * 1000000LL, settings.limits.recordingAnimationEdge};
}
void RecordingService::discardStaging(const QString& root) {
    QDir dir(root);
    if (!dir.exists()) return;
    for (const auto& name : dir.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) QDir(dir.filePath(name)).removeRecursively();
}
void RecordingService::shutdown() {
    if (stopping_) return;
    stopping_ = true;
    unwatchClicks();
    if (session_ && backend_) { closeShell(*session_); backend_->control(session_->op.id, "cancel", {}, [](Error, QJsonObject) {}); backend_->discard(session_->op.id); }
    session_.reset();
}
QJsonObject RecordingService::capabilities() const {
    auto result = backend_->capabilities();
    const auto settings = host_.settings();
    const auto limit = limits(settings);
    result["active"] = session_ ? QJsonValue(session_->op.id) : QJsonValue(QJsonValue::Null);
    result["states"] = QJsonArray::fromStringList(recording::states());
    result["timeUnit"] = "microseconds";
    result["maskKinds"] = QJsonArray{"blur", "solid"};
    result["editCommands"] = QJsonArray{"setTrim", "resetTrim", "addMask", "updateMask", "removeMask"};
    result["limits"] = QJsonObject{{"animationMaxUs", double(limit.animationMaxUs)}, {"animationMaxEdge", limit.animationMaxEdge},
        {"maxFileBytes", double(settings.limits.maxFileBytes)}, {"maxMasks", recording::maximumMasks}};
    result["commands"] = QJsonArray{"windows", "microphones", "prepare", "start", "pause", "resume", "stop", "cancel", "inspect", "select", "raise", "overlay", "audio", "edit", "preview", "review", "saveCopy", "upload"};
    // recording.windows lists the identities a window target takes (KDE, GNOME Wayland).
    result["windowList"] = windows_ != nullptr;
    // #1364: whether this desktop can highlight clicks, by which route, and who draws the ring.
    result["clickHighlights"] = clickCapability();
    return result;
}
QJsonObject RecordingService::publicJson(const Session& s) const {
    const auto retained = s.state == RecordingState::Reviewing || s.state == RecordingState::Exporting ? s.edit.exportDuration(s.duration) : 0;
    QJsonObject o{{"recordingId", s.op.id}, {"operationId", s.op.id}, {"state", recording::stateName(s.state)}, {"pending", s.pending.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s.pending)},
        {"target", s.target.toJson()}, {"config", s.config.toJson()}, {"canvas", sizeJson(s.canvas)}, {"durationUs", double(s.duration)},
        {"retainedUs", double(retained)}, {"edit", s.edit.toJson()}, {"progress", s.progress}, {"exports", s.exports},
        {"lastExport", s.lastExport.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s.lastExport)}, {"error", s.op.error.toJson()}};
    if (!s.canvas.isEmpty()) o["outputSize"] = sizeJson(recording::outputSize(s.canvas, s.config.maxEdge));
    if (s.overlayExcluded) o["overlayExcluded"] = *s.overlayExcluded;
    if (!s.overlayPlacement.isEmpty()) o["overlayPlacement"] = s.overlayPlacement;
    o["selecting"] = !s.selector.isEmpty();
    if (!s.target.window.isEmpty()) o["window"] = s.target.window;
    if (!s.stopReason.isEmpty()) o["stopReason"] = s.stopReason;
    if (!s.stopDetails.isEmpty()) o["stopDetails"] = s.stopDetails;
    return o;
}
// The helper ended the capture on its byte budget (#225): say which bound ran
// out, with the figures and what to do, in the review window, the tray and
// recording.inspect. The clip recorded so far stays for review.
void RecordingService::stoppedOnBudget(Session& s, const QJsonObject& observed) {
    const auto maxFile = s.op.settings.limits.maxFileBytes;
    auto disk = ArtifactStore::diskBudget(stagingRoot_);
    // A disk that filled up during the capture ends it on the disk, whatever the prepare-time bound was.
    const bool liveDiskStop = observed.value("reason").toString() == "disk_space";
    const bool diskFull = liveDiskStop || s.budgetLimit == "disk_space" || (disk.freeBytes >= 0 && disk.available() < 2 * minimumBudgetBytes);
    QJsonObject details{{"reason", diskFull ? "disk_space" : "max_file_bytes"}, {"budgetBytes", double(s.config.budgetBytes)}, {"maxFileBytes", double(maxFile)},
        {"durationUs", double(s.duration)}};
    const auto at = clockText(s.duration), budget = ArtifactStore::sizeText(s.config.budgetBytes);
    if (diskFull) {
        if (disk.freeBytes >= 0) {
            details["freeBytes"] = double(disk.freeBytes);
            details["reserveBytes"] = double(disk.reserveBytes + disk.reservedBytes);
            details["availableBytes"] = double(disk.available());
        }
        details["hint"] = "Free up space in put.gd's data folder before the next recording";
        details["message"] = disk.freeBytes >= 0
            ? QStringLiteral("Recording stopped at %1: the disk ran out of room. %2 is free and put.gd keeps %3 free for the system, so the recording stopped at %4 to leave room for its export and upload. The clip so far is kept for review; free up space before recording again.")
                  .arg(at, ArtifactStore::sizeText(disk.freeBytes), ArtifactStore::sizeText(disk.reserveBytes + disk.reservedBytes), budget)
            : QStringLiteral("Recording stopped at %1: the disk ran out of room at %2. The clip so far is kept for review; free up space before recording again.").arg(at, budget);
        if (liveDiskStop) {
            // Keep the triggering sample, even if another writer releases space
            // while EOS drains. It is the reason we stopped, not the byte cap.
            for (const auto* key : {"freeBytes", "reserveBytes", "availableBytes", "requiredBytes", "finalizationReserveBytes", "clipBytes"})
                if (observed.value(key).isDouble()) details[key] = observed.value(key);
            details["hint"] = "Free up space in put.gd's data folder before exporting or recording again";
            details["message"] = QStringLiteral("Recording stopped at %1 because free disk space fell: %2 was free, with %3 kept for the system and %4 needed for finalization and copies. The clip so far is kept for private review. Free up space before exporting or recording again; nothing was uploaded.")
                .arg(at, ArtifactStore::sizeText(details.value("freeBytes").toInteger()), ArtifactStore::sizeText(details.value("reserveBytes").toInteger()), ArtifactStore::sizeText(details.value("requiredBytes").toInteger()));
        }
    } else {
        details["hint"] = "Raise Largest file in Settings > Uploads to record longer";
        details["message"] = QStringLiteral("Recording stopped at %1: it reached the Largest file limit (%2; recording stops at %3 to leave room to finish the file). The clip so far is kept for review; raise Largest file in Settings > Uploads to record longer.")
                                 .arg(at, ArtifactStore::sizeText(maxFile), budget);
    }
    s.stopDetails = details;
    logging::warning(logRecording(), "Recording stopped on its budget", {{"operationId", s.op.id}, {"stop", details}});
    backend_->control(s.op.id, "notice", {{"text", details["message"]}}, [](Error, QJsonObject) {});
    emit budgetStopped(s.op.id, details);
}
void RecordingService::noteCaptured(Session& s, const QJsonObject& result, bool required) {
    const auto sources = result["audioSources"].toObject();
    if (!sources.isEmpty()) {
        s.capturedSystemAudio = sources["system"].toBool();
        s.capturedMicrophone = sources["microphone"].toBool();
    } else if (required || result["audioEncoder"].isString()) {
        const bool captured = !result["audioEncoder"].toString().isEmpty();
        s.capturedMicrophone = captured && s.config.audio.microphone && !s.config.audio.system;
        s.capturedSystemAudio = captured && !s.capturedMicrophone;
    }
}
QString RecordingService::microphoneDevice(const QJsonObject& params) const {
    const auto requested = params["audio"].toObject()["microphoneDevice"].toString();
    return requested.isEmpty() ? host_.settings().recordingMicrophoneDevice : requested;
}
QJsonObject RecordingService::withMicrophone(QJsonObject capabilities, const QJsonObject& microphones) const {
    auto audio = capabilities["audio"].toObject();
    // The helper's probe says whether this installation can record an input at
    // all; the sound server says whether one is connected right now.
    const bool supported = audio["microphone"].toBool();
    const bool available = supported && microphones["available"].toBool();
    audio["microphone"] = available;
    audio.remove("microphoneReason");
    audio.remove("microphoneMessage");
    audio.remove("microphoneDevice");
    if (available) audio["microphoneDevice"] = microphones["device"];
    else {
        audio["microphoneReason"] = supported ? microphones["reason"].toString("no_microphone") : QStringLiteral("microphone_elements_missing");
        audio["microphoneMessage"] = supported ? microphones["message"].toString("No microphone is available.") : QStringLiteral("Microphone recording is not available on this installation.");
    }
    capabilities["audio"] = audio;
    return capabilities;
}
void RecordingService::invoke(const QString& mode, JsonReply reply) {
    QString step = "raise";
    if (!session_) step = "prepare";
    else if (mode == "toggle" && session_->state == RecordingState::Ready) step = "start";
    else if (mode == "toggle" && (session_->state == RecordingState::Recording || session_->state == RecordingState::Paused)) step = "stop";
    QJsonObject prepare{{"target", QJsonObject{{"kind", "region"}}}, {"format", "mp4"}};
    if (step == "prepare") {
        // The bar's microphone toggle and the click highlights are remembered (#1364).
        const auto settings = host_.settings();
        if (settings.recordingMicrophone) prepare["audio"] = QJsonObject{{"microphone", true}};
        if (settings.recordingClickHighlights) prepare["clickHighlights"] = true;
        rememberedPreferences_ = true;
    }
    const auto params = step == "prepare" ? prepare : QJsonObject{{"id", session_->op.id}};
    command(step, params, [step, reply](Error error, QJsonObject result) {
        result["step"] = step;
        reply(error, result);
    });
}
void RecordingService::checkStep(Session& s, RecordingState to) {
    if (recording::allowedStep(s.state, to) || std::exchange(s.offTableStep, true)) return;
    logging::warning(logRecording(), "Recording step outside the state table", {{"operationId", s.op.id}, {"from", recording::stateName(s.state)}, {"state", recording::stateName(to)}});
}
void RecordingService::transition(Session& s, RecordingState state, const QString& phase) {
    // A step the table does not list is still taken: refusing it would leave the
    // session where it is with nothing to move it on, holding the clip, the bar
    // or the dimming. checkStep() makes it visible in the log instead.
    checkStep(s, state);
    const auto name = recording::stateName(state);
    logging::info(logRecording(), "Recording step", {{"operationId", s.op.id}, {"from", recording::stateName(s.state)}, {"state", name}, {"elapsedMs", s.clock.isValid() ? s.clock.elapsed() : -1}});
    s.state = state;
    s.pending.clear();
    s.op.phase = phase.isEmpty() ? name : phase;
    // Click highlights (#1364) follow the capture: on at Record, kept across a pause, off once it stops.
    if (state == RecordingState::Recording) watchClicks(s);
    else if (state != RecordingState::Paused) unwatchClicks();
    // The shell's ring stays through start; dimming covers recording, pause and the stop.
    if (s.shellFrame && shell_ && (state == RecordingState::Recording || state == RecordingState::Paused)) shell_->showFrame(shellFrame(s, state), [](Error, QJsonObject) {});
    else if (s.shellFrame && state != RecordingState::Ready && state != RecordingState::Starting && state != RecordingState::Stopping) closeShell(s);
    host_.observe(s.op);
    emit changed(s.op.id);
}
RecordingBackend::Reply RecordingService::marshal(quint64 generation, std::function<void(Error, QJsonObject)> fn) {
    QPointer<RecordingService> guard(this);
    return [guard, generation, fn = std::move(fn)](Error error, QJsonObject result) {
        if (!guard) return;
        auto deliver = [guard, generation, fn, error, result] {
            // A reply for a session that already ended is ignored, never applied to its successor.
            if (!guard || guard->stopping_ || !guard->session_ || guard->session_->generation != generation) return;
            fn(error, result);
        };
        // Events use this same queue. Keep their wire order even when adjacent
        // event/result frames arrive together on the service's own thread.
        QMetaObject::invokeMethod(guard, std::move(deliver), Qt::QueuedConnection);
    };
}
QJsonObject RecordingService::shellFrame(const Session& s, RecordingState state) const {
    return {{"rect", rectJson(s.recorded)}, {"kind", s.target.kind}, {"state", recording::stateName(state)}, {"pid", double(backend_->processId(s.op.id))}};
}
void RecordingService::closeShell(Session& s) {
    if (!shell_) return;
    if (s.selector == "shell") { ++s.selection; s.selector.clear(); shell_->cancelSelection(); }
    if (s.shellFrame) { s.shellFrame = false; shell_->hideFrame(); }
}
void RecordingService::fail(Error error) {
    if (!session_) return;
    // A user cancellation reported by the helper (Escape during selection,
    // Cancel on the bar) ends as cancelled, not as a failure.
    const bool cancelled = error.code == "cancelled" || error.code == "source_cancelled";
    checkStep(*session_, cancelled ? RecordingState::Cancelled : RecordingState::Failed);
    unwatchClicks();
    closeShell(*session_);
    auto s = std::move(*session_);
    session_.reset();
    backend_->discard(s.op.id);
    QDir(s.staging).removeRecursively();
    // Which step ended it and how long in (#250): a timeout names its step,
    // otherwise the state the session was in.
    logging::write(logRecording(), cancelled ? QtInfoMsg : QtWarningMsg, cancelled ? "Recording cancelled" : "Recording failed",
        {{"operationId", s.op.id}, {"error", logging::errorFields(error)}, {"step", error.details["step"].toString(recording::stateName(s.state))}, {"state", recording::stateName(s.state)}, {"phase", s.op.phase},
         {"selector", s.selector}, {"elapsedMs", s.clock.isValid() ? s.clock.elapsed() : -1}});
    s.op.error = error;
    s.op.cancelled = cancelled;
    s.op.advance(cancelled ? OperationState::Cancelled : OperationState::Failed);
    s.op.phase = s.op.stateName();
    host_.finish(s.op);
    emit changed(s.op.id);
}
void RecordingService::cancelActive(JsonReply reply) {
    checkStep(*session_, RecordingState::Cancelled);
    unwatchClicks();
    closeShell(*session_);
    auto s = std::move(*session_);
    session_.reset();
    backend_->control(s.op.id, "cancel", {}, [](Error, QJsonObject) {});
    backend_->discard(s.op.id);
    QDir(s.staging).removeRecursively();
    s.op.cancelled = true;
    s.op.advance(OperationState::Cancelled);
    s.op.phase = "cancelled";
    s.op.error = {"cancelled", "Recording cancelled; nothing was uploaded", {}};
    host_.finish(s.op);
    emit changed(s.op.id);
    if (reply) reply({}, {{"recordingId", s.op.id}, {"operationId", s.op.id}, {"state", "cancelled"}});
}
void RecordingService::handleEvent(const QJsonObject& event) {
    if (!session_ || event["id"].toString() != session_->op.id) return;
    auto& s = *session_;
    const auto kind = event["event"].toString();
    if (kind == "progress") {
        auto progress = event;
        progress.remove("id");
        progress.remove("event");
        // The helper reports several times a second and each report replaces the last. What the service
        // itself learned about this recording (why its clicks are not highlighted) is not the helper's to
        // report, and stays.
        const auto clickError = s.progress.value("clickHighlightsError");
        s.progress = progress;
        if (!clickError.isUndefined()) s.progress["clickHighlightsError"] = clickError;
        qint64 duration = 0;
        if (integer(event["durationUs"], 0, recording::maximumClipUs, &duration)) s.duration = duration;
        host_.observe(s.op);
    } else if (kind == "stopped" && (s.state == RecordingState::Recording || s.state == RecordingState::Paused || s.state == RecordingState::Stopping)) {
        // The helper stopped on its own (budget, source loss) with a reviewable clip.
        qint64 duration = 0;
        if (!integer(event["durationUs"], 1, recording::maximumClipUs, &duration)) { fail({"recording_failed", "The recording ended without a usable clip", {}}); return; }
        s.duration = duration;
        s.edit = {};
        s.captureStopped = true;
        noteCaptured(s, event, false);
        s.config.audio.system = s.capturedSystemAudio && !s.config.silent();
        s.config.audio.microphone = s.capturedMicrophone && !s.config.silent();
        s.stopReason = event["reason"].toString();
        updateCanvas(s.canvas, event["canvas"]);
        recording::clampAnimationSize(s.config, s.canvas, limits(s.op.settings));
        if (s.stopReason == "budget") stoppedOnBudget(s, event["stopDetails"].toObject());
        transition(s, RecordingState::Reviewing);
    } else if (kind == "selecting" && s.state == RecordingState::Preparing) {
        // The helper's own selector is open (X11, KDE): recording.select can drive it.
        s.selector = "helper";
        logging::info(logRecording(), "Recording selector open", {{"operationId", s.op.id}, {"selector", s.selector}, {"elapsedMs", s.clock.elapsed()}});
        host_.observe(s.op);
        emit changed(s.op.id);
    } else if (kind == "failed") {
        auto error = event["error"].toObject();
        fail({error["code"].toString("recording_failed"), error["message"].toString("The recording helper failed"), error["details"].toObject()});
    } else if (kind == "started" && (s.state == RecordingState::Ready || s.state == RecordingState::Starting)) {
        // The user pressed Record on the helper's control bar.
        noteCaptured(s, event, true);
        for (auto it = event.begin(); it != event.end(); ++it) if (it.key() != "id" && it.key() != "event") s.progress[it.key()] = it.value();
        transition(s, RecordingState::Recording);
    } else if (kind == "paused" && s.state == RecordingState::Recording) transition(s, RecordingState::Paused);
    else if (kind == "resumed" && s.state == RecordingState::Paused) transition(s, RecordingState::Recording);
    else if (kind == "lateResult" && event["ok"].toBool()) {
        // The helper finished a pause or resume after its deadline (#1443): its
        // caller had recording_helper_timeout, and the clip is now where the
        // helper took it. Followed like the bar's own events, from the state the
        // command was sent in; a command still in flight keeps its place.
        const auto command = event["command"].toString();
        std::optional<RecordingState> to;
        if (command == "pause" && s.state == RecordingState::Recording) to = RecordingState::Paused;
        else if (command == "resume" && s.state == RecordingState::Paused) to = RecordingState::Recording;
        if (to) {
            const auto pending = s.pending;
            transition(s, *to);
            s.pending = pending;
        }
    }
    else if (kind == "cancelled" && !recording::terminal(s.state)) cancelActive({});
    else if (kind == "configChanged" && (s.state == RecordingState::Ready || s.state == RecordingState::Reviewing)) {
        auto json = event["config"].toObject();
        for (const auto* key : {"mime", "budgetBytes"}) json.remove(key);
        if (s.state == RecordingState::Reviewing) {
            // After the clip exists only the output can change: the capture rate is
            // fixed and audio cannot appear (and is dropped for silent formats).
            const auto format = json["format"].toString();
            json.remove("fps");
            const bool sound = format != "gif" && format != "webp";
            json["audio"] = QJsonObject{{"system", s.capturedSystemAudio && sound}, {"microphone", s.capturedMicrophone && sound}};
        }
        // The bar's microphone toggle is remembered for the next recording (#1364).
        if (s.state == RecordingState::Ready && event["microphoneRequested"].isBool() && host_.remember && event["microphoneRequested"].toBool() != host_.settings().recordingMicrophone)
            host_.remember({{"recordingMicrophone", event["microphoneRequested"].toBool()}});
        RecordingConfig config;
        if (!recording::parseConfig(json, config)) {
            config.budgetBytes = s.config.budgetBytes;
            if (s.state == RecordingState::Reviewing) config.fps = s.config.fps;
            // Choosing GIF or WebP is a format choice: fit the output to the animation limit.
            const bool clamped = recording::clampAnimationSize(config, s.canvas, limits(s.op.settings));
            s.config = config;
            if (clamped && s.state == RecordingState::Reviewing) backend_->setEdit(s.op.id, s.edit, s.config, limitsJson(s));
            host_.observe(s.op);
            emit changed(s.op.id);
        }
    } else if (kind == "edit" && s.state == RecordingState::Reviewing) {
        // The review window proposes commands at the revision it showed; the
        // resident's answer (setEdit) is what it displays, accepted or not.
        qint64 revision = 0;
        if (integer(event["revision"], 1, 1LL << 52, &revision) && quint64(revision) == s.edit.revision && event["commands"].isArray())
            if (!recording::applyEditCommands(s.edit, event["commands"].toArray(), s.duration, s.canvas, s.config.fps)) { host_.observe(s.op); emit changed(s.op.id); }
        backend_->setEdit(s.op.id, s.edit, s.config, limitsJson(s));
    } else if ((kind == "uploadRequested" || kind == "saveCopyRequested" || kind == "retryExportRequested") && s.state == RecordingState::Reviewing) {
        QJsonObject params{{"id", s.op.id}, {"revision", event["revision"]}};
        if (kind == "saveCopyRequested") { params["path"] = event["path"]; params["overwrite"] = true; }
        // The helper learns the outcome from its own export command; a refused
        // request (stale revision, budget) or a failed export is pushed back as the
        // current edit and the review window, closed by Upload, is reopened with the reason.
        auto refused = [this](Error error, QJsonObject) {
            if (!error || !session_) return;
            backend_->setEdit(session_->op.id, session_->edit, session_->config, limitsJson(*session_));
            backend_->control(session_->op.id, "raise", {{"error", error.message}}, [](Error, QJsonObject) {});
        };
        if (kind == "retryExportRequested") retryExport(refused, refused);
        else exportTo(kind == "uploadRequested" ? "upload" : "saveCopy", params, refused, refused);
    }
}
void RecordingService::command(const QString& command, const QJsonObject& params, JsonReply reply) {
    if (stopping_) { reply({"session_unavailable", "The resident is shutting down", {}}, {}); return; }
    QPointer<RecordingService> self(this);
    if (command == "capabilities") {
        backend_->refreshCapabilities([self, reply] {
            if (!self || self->stopping_) return;
            self->backend_->microphones(self->microphoneDevice({}), [self, reply](Error, QJsonObject microphones) {
                if (!self) return;
                QMetaObject::invokeMethod(self, [self, reply, microphones] { if (self && !self->stopping_) reply({}, self->withMicrophone(self->capabilities(), microphones)); }, Qt::QueuedConnection);
            });
        });
        return;
    }
    if (command == "microphones") {
        // #1364: the inputs to choose from; `device` asks about one by name.
        if (params.contains("device") && !recording::validMicrophoneDevice(params["device"])) { reply(invalid("device is the name of an input device"), {}); return; }
        const QString device = params.contains("device") ? params["device"].toString() : host_.settings().recordingMicrophoneDevice;
        backend_->refreshCapabilities([self, reply, device] {
            if (!self || self->stopping_) return;
            const bool supported = self->backend_->capabilities()["audio"].toObject()["microphone"].toBool();
            self->backend_->microphones(device, [self, reply, supported](Error error, QJsonObject microphones) {
                if (!self) return;
                QMetaObject::invokeMethod(self, [self, reply, error, microphones, supported]() mutable {
                    if (!self || self->stopping_) return;
                    if (error) { reply(error, {}); return; }
                    if (!supported) { microphones["available"] = false; microphones["reason"] = "microphone_elements_missing"; microphones["message"] = "Microphone recording is not available on this installation."; microphones.remove("device"); }
                    reply({}, microphones);
                }, Qt::QueuedConnection);
            });
        });
        return;
    }
    if (command == "windows") {
        if (!windows_) { reply({"recording_unsupported", "This desktop does not list windows for recording; a window target without an identity records the focused window", {}}, {}); return; }
        windows_->listWindows([self, reply](Error error, QJsonObject result) {
            if (!self || self->stopping_) return;
            if (error) { reply(error, {}); return; }
            reply({}, {{"windows", result["windows"].toArray()}, {"activeExcluded", result["activeExcluded"].toBool()}});
        });
        return;
    }
    if (command == "prepare") {
        // The helper probe is lazy; make it current before judging support.
        auto probed = std::make_shared<bool>(false);
        const bool remembered = std::exchange(rememberedPreferences_, false);
        backend_->refreshCapabilities([self, reply, params, probed, remembered] {
            if (!self || self->stopping_ || std::exchange(*probed, true)) return;
            {
                // A malformed request is refused as that, before anything is asked of the sound server.
                RecordingTarget target;
                RecordingConfig config;
                auto fields = params;
                fields.remove("target");
                if (auto error = recording::parseTarget(params["target"], target)) { reply(error, {}); return; }
                if (auto error = recording::parseConfig(fields, config)) { reply(error, {}); return; }
            }
            // A recording that asks for the microphone needs one now. The remembered
            // toggle is not a request: without a microphone the bar shows it
            // unavailable and the recording goes on without narration.
            if (!params["audio"].toObject()["microphone"].toBool() || remembered) { self->prepareSession(params, reply, remembered); return; }
            if (!self->backend_->capabilities()["audio"].toObject()["microphone"].toBool() && self->backend_->capabilities()["available"].toBool()) {
                reply({"recording_unsupported", "Microphone recording is not supported here", {{"field", "audio.microphone"}, {"reason", "microphone_elements_missing"}}}, {});
                return;
            }
            self->backend_->microphones(self->microphoneDevice(params), [self, reply, params](Error, QJsonObject microphones) {
                if (!self) return;
                QMetaObject::invokeMethod(self, [self, reply, params, microphones] {
                    if (!self || self->stopping_) return;
                    if (!microphones["available"].toBool() && self->backend_->capabilities()["available"].toBool()) {
                        reply({"recording_unsupported", microphones["message"].toString("No microphone is available."), {{"field", "audio.microphone"}, {"reason", microphones["reason"].toString("no_microphone")}}}, {});
                        return;
                    }
                    self->prepareSession(params, reply, false);
                }, Qt::QueuedConnection);
            });
        });
        return;
    }
    commandSession(command, params, std::move(reply));
}
void RecordingService::startCapture(const QString& id, JsonReply reply) {
    if (!session_ || session_->op.id != id) { reply({"recording_missing", "The recording is gone", {}}, {}); return; }
    auto& s = *session_;
    backend_->control(s.op.id, "start", {}, marshal(s.generation, [this, reply](Error error, QJsonObject result) {
        auto& s = *session_;
        // Record on the bar won the race: its `started` event, queued ahead of this
        // reply, already moved the session on, and the helper refused this second
        // start. The recording runs; failing the session would discard it (#1434).
        if (s.state != RecordingState::Starting) { reply({}, publicJson(s)); return; }
        if (error) { fail(error); reply(error, {}); return; }
        for (auto it = result.begin(); it != result.end(); ++it) s.progress[it.key()] = it.value();
        noteCaptured(s, result, true);
        transition(s, RecordingState::Recording);
        reply({}, publicJson(s));
    }));
}
QJsonObject RecordingService::clickCapability() const {
    if (clicks_) {
        auto capability = clicks_->clickCapability();
        capability.remove("beforeCapture");
        // A route that learns only that a button went down (labwc) takes the place from the helper's capture.
        if (capability.take("needsPointer").toBool() && capability["available"].toBool() && !backend_->capabilities()["wlrCapture"].toObject()["pointer"].toBool()) {
            capability["available"] = false;
            capability["targets"] = QJsonArray{};
            capability["reason"] = "pointer_position_unavailable";
            capability["message"] = "The compositor offers no pointer cursor session here, so put.gd cannot tell where a click was.";
        }
        return capability;
    }
    // X11: the helper's own probe (XInput2 raw button events).
    const auto probed = backend_->capabilities()["clickHighlights"].toObject();
    if (!probed.isEmpty()) return probed;
    return {{"available", false}, {"route", "none"}, {"drawnBy", "recorder"}, {"targets", QJsonArray{}}, {"reason", "click_route_missing"},
            {"message", "This desktop gives put.gd no pointer clicks, so recordings cannot highlight them."}};
}
void RecordingService::watchClicks(Session& s, std::function<void()> then) {
    // Once per recording: a watch that failed is not asked again at the next transition or resume.
    if (!s.config.clickHighlights || !clicks_ || clicksWatched_ || s.clickWatchTried) { if (then) then(); return; }
    clicksWatched_ = true;
    s.clickWatchTried = true;
    const auto generation = s.generation;
    QPointer<RecordingService> guard(this);
    auto current = [guard, generation] { return guard && !guard->stopping_ && guard->session_ && guard->session_->generation == generation; };
    // `then` runs once, when the desktop has answered or after a bounded wait: Record never hangs on it.
    auto once = std::make_shared<std::function<void()>>(std::move(then));
    auto proceed = [guard, current, once] {
        if (!*once) return;
        const auto next = std::exchange(*once, {});
        if (guard && current()) next();
    };
    if (*once) QTimer::singleShot(clickWatchWaitMs, this, proceed);
    clicks_->watchClicks(s.target.toJson(), [guard, current](QJsonObject click) {
        if (!guard) return;
        // When the desktop reported it (CLOCK_MONOTONIC, shared with the helper): the ring is drawn at
        // this moment of the clip however long the press takes to reach the helper's pipeline.
        if (!click.contains("atUs")) click["atUs"] = double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        QMetaObject::invokeMethod(guard, [guard, current, click] {
            // Presses while paused are not part of the clip.
            if (!current() || guard->session_->state != RecordingState::Recording) return;
            guard->backend_->control(guard->session_->op.id, "click", click, [guard, current](Error error, QJsonObject) {
                // The helper could not place the press (no pointer position): say so once; the recording goes on.
                if (guard && error && current() && !guard->session_->progress.contains("clickHighlightsError")) guard->session_->progress["clickHighlightsError"] = error.message;
            });
        }, Qt::QueuedConnection);
    }, [guard, current, proceed](Error error, QJsonObject) {
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, current, error, proceed] {
            if (!current()) return;
            if (error) {
                // The recording goes on without highlights; inspect says why.
                logging::warning(logRecording(), "Click highlights are unavailable for this recording", {{"operationId", guard->session_->op.id}, {"error", logging::errorFields(error)}});
                guard->session_->progress["clickHighlightsError"] = error.message;
                guard->unwatchClicks();
            }
            proceed();
        }, Qt::QueuedConnection);
    });
}
void RecordingService::unwatchClicks() {
    if (!std::exchange(clicksWatched_, false) || !clicks_) return;
    clicks_->unwatchClicks();
}
void RecordingService::prepareSession(const QJsonObject& params, JsonReply reply, bool remembered) {
    if (session_) { reply({"recording_active", "A recording is already active; stop or cancel it first", {{"recordingId", session_->op.id}}}, {}); return; }
    {
        Session s;
        if (auto error = recording::parseTarget(params["target"], s.target)) { reply(error, {}); return; }
        auto config = params;
        config.remove("target");
        if (auto error = recording::parseConfig(config, s.config)) { reply(error, {}); return; }
        const auto caps = backend_->capabilities();
        if (!caps["available"].toBool()) {
            // The helper's probe says what to install when it knows (no usable H.264 encoder).
            const auto message = caps["message"].toString();
            const auto hint = caps["hint"].toString();
            QJsonObject details{{"reason", caps["reason"].toString()}};
            if (!hint.isEmpty()) details["hint"] = hint;
            reply({"recording_unavailable", message.isEmpty() ? QStringLiteral("Screen recording is not available on this installation") : hint.isEmpty() ? message : message + ' ' + hint, details}, {});
            return;
        }
        if (!caps["targets"].toArray().contains(s.target.kind)) { reply({"recording_unsupported", caps["targetReasons"].toObject()[s.target.kind].toString(QStringLiteral("This target kind is not supported here")), {{"kind", s.target.kind}}}, {}); return; }
        if (!caps["formats"].toArray().contains(s.config.format)) { reply({"recording_unsupported", "This output format is not supported here", {{"format", s.config.format}}}, {}); return; }
        if (!s.config.codec.isEmpty() && !caps["codecs"].toObject()[s.config.format].toArray().contains(s.config.codec)) { reply({"recording_unsupported", "This codec is not supported here", {{"codec", s.config.codec}}}, {}); return; }
        if (s.config.audio.system && !caps["audio"].toObject()["system"].toBool()) { reply({"recording_unsupported", "System audio capture is not supported here", {{"field", "audio.system"}}}, {}); return; }
        if (s.config.clickHighlights) {
            const auto clicks = clickCapability();
            const bool here = clicks["available"].toBool();
            if (!here || !clicks["targets"].toArray().contains(s.target.kind)) {
                if (!remembered) {
                    reply({"recording_unsupported", here ? clicks["targetReasons"].toObject()[s.target.kind].toString(QStringLiteral("Click highlights are not available for a %1 recording on this desktop").arg(s.target.kind))
                                                         : clicks["message"].toString("Click highlights are not available on this desktop"),
                           {{"field", "clickHighlights"}, {"reason", here ? QStringLiteral("click_target_unsupported") : clicks["reason"].toString("click_route_missing")}}}, {});
                    return;
                }
                s.config.clickHighlights = false;
            }
        }
        // The device chosen in Settings unless the request names one; empty is the system default input.
        if (s.config.audio.microphoneDevice.isEmpty()) s.config.audio.microphoneDevice = host_.settings().recordingMicrophoneDevice;
        // With "auto" or "No limit", free disk space is the real bound: keep the
        // system reserve free and leave room for the export and its upload copy.
        // A disk that can't fund a minimum budget refuses here, before anything is
        // accepted, rather than starting a recording that ends itself (#213).
        std::optional<ArtifactStore::DiskBudget> disk;
        if (QDir().mkpath(stagingRoot_)) {
            disk = ArtifactStore::diskBudget(stagingRoot_);
            if (disk->freeBytes < 0) disk.reset();
        }
        if (disk && disk->available() / 3 < minimumBudgetBytes) { reply(diskSpaceError(*disk), {}); return; }
        if (auto error = host_.accept(s.op)) { reply(error, {}); return; }
        // A request without a rate records at the one chosen in Settings (#984).
        if (!params.contains("fps")) s.config.fps = s.op.settings.recordingFrameRate;
        // Stop before the admitted file limit, leaving room for finalization.
        s.config.budgetBytes = std::max<qint64>(1024 * 1024, s.op.settings.limits.maxFileBytes - std::max<qint64>(s.op.settings.limits.maxFileBytes / 10, 64LL * 1024 * 1024));
        s.budgetLimit = "max_file_bytes";
        if (disk && disk->available() / 3 < s.config.budgetBytes) {
            s.config.budgetBytes = disk->available() / 3;
            s.budgetLimit = "disk_space";
        }
        s.op.advance(OperationState::Recording);
        s.op.phase = "preparing";
        s.staging = stagingRoot_ + "/" + s.op.id;
        if (!QDir().mkpath(s.staging) || !QFile::setPermissions(s.staging, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
            s.op.error = {"recording_io", "Cannot create private recording staging", {}};
            s.op.advance(OperationState::Failed);
            host_.finish(s.op);
            reply(s.op.error, {});
            return;
        }
        s.generation = ++generation_;
        s.clock.start();
        session_ = s;
        const auto generation = s.generation;
        QPointer<RecordingService> guard(this);
        host_.persist(s.op, [guard, generation, reply](Error error) {
            if (!guard || guard->stopping_ || !guard->session_ || guard->session_->generation != generation) { if (!error) reply({"recording_state", "The recording ended before it was accepted", {}}, {}); else reply(error, {}); return; }
            auto& s = *guard->session_;
            if (error) { guard->fail(error); reply(error, {}); return; }
            guard->host_.observe(s.op);
            emit guard->changed(s.op.id);
            reply({}, guard->publicJson(s));
            guard->beginPrepare(generation);
        });
    }
}
void RecordingService::resolveWindow(quint64 generation) {
    windows_->listWindows(marshal(generation, [this](Error error, QJsonObject result) {
        auto& s = *session_;
        if (s.state != RecordingState::Preparing) return;
        if (error) {
            // An integration loaded before an upgrade must be reloaded (#131); one
            // that is off or missing leaves the window to the portal's picker.
            if (error.code == "helper_outdated") { fail(error); return; }
            logging::warning(logRecording(), "Recording a window through the portal picker", {{"operationId", s.op.id}, {"error", error.toJson()}});
            prepareBackend(s.generation);
            return;
        }
        QJsonObject chosen;
        for (const auto& value : result["windows"].toArray()) {
            const auto window = value.toObject();
            if (s.target.identity.isEmpty() ? window["active"].toBool() : window["identity"].toString() == s.target.identity) { chosen = window; break; }
        }
        if (chosen.isEmpty()) {
            if (!s.target.identity.isEmpty()) { fail({"recording_target", "No external window has this identity; recording.windows lists them", {{"identity", s.target.identity}}}); return; }
            fail({"recording_target", result["activeExcluded"].toBool() ? "put.gd's own windows are not recorded; focus another window or pass its identity from recording.windows"
                                                                         : "No external window has focus; focus one or pass its identity from recording.windows",
                  {{"reason", result["activeExcluded"].toBool() ? "own_window" : "no_active_window"}}});
            return;
        }
        s.target.identity = chosen["identity"].toString();
        s.target.window = QJsonObject{{"identity", s.target.identity}, {"title", chosen["title"].toString()}, {"appId", chosen["appId"].toString()}, {"rect", chosen["rect"].toArray()}};
        logging::info(logRecording(), "Recording window resolved", {{"operationId", s.op.id}, {"identity", s.target.identity}, {"appId", chosen["appId"].toString()}, {"elapsedMs", s.clock.elapsed()}});
        host_.observe(s.op);
        emit changed(s.op.id);
        prepareBackend(s.generation);
    }));
}
void RecordingService::beginPrepare(quint64 generation) {
    auto& s = *session_;
    if (s.target.kind == "window" && windows_) { resolveWindow(generation); return; }
    // A region without a rectangle is chosen by the shell where the helper
    // cannot show a selector (GNOME); the helper then streams just that area.
    // A shell that cannot select and knows why (an extension that is off, or
    // updated and still loaded in its old version, #131) says so instead of the
    // helper's generic refusal.
    const bool rectless = s.target.kind == "region" && s.target.rect.isEmpty();
    if (rectless && shell_ && !shell_->selectsRegions()) {
        if (auto error = shell_->regionError()) { fail(error); return; }
    }
    if (rectless && shell_ && shell_->selectsRegions()) {
        s.selector = "shell";
        logging::info(logRecording(), "Recording selector open", {{"operationId", s.op.id}, {"selector", s.selector}, {"elapsedMs", s.clock.elapsed()}});
        const auto token = ++s.selection;
        host_.observe(s.op);
        emit changed(s.op.id);
        shell_->selectRegion(marshal(generation, [this, token](Error error, QJsonObject result) {
            auto& s = *session_;
            if (s.state != RecordingState::Preparing || s.selector != "shell" || s.selection != token) return;
            s.selector.clear();
            QRect rect;
            if (!error && !rectFrom(result["rect"], rect)) error = {"capture_geometry", "The desktop shell returned an invalid selection", {}};
            if (error) { fail(error.code == "cancelled" ? Error{"cancelled", "Region selection cancelled; nothing was recorded", {}} : error); return; }
            s.target.rect = rect;
            prepareBackend(s.generation);
        }));
        return;
    }
    prepareBackend(generation);
}
void RecordingService::prepareBackend(quint64 generation) {
    auto& s = *session_;
    backend_->prepare(s.op.id, s.target, s.config, s.staging, marshal(generation, [this](Error error, QJsonObject result) { prepared(error, result); }), host_.reviewOptions ? host_.reviewOptions(s.op) : QJsonObject{});
}
void RecordingService::prepared(Error error, const QJsonObject& result) {
    auto& s = *session_;
    if (s.state != RecordingState::Preparing) return;
    // fail() logs whether a selector was still open.
    if (error) { fail(error); return; }
    s.selector.clear();
    updateCanvas(s.canvas, result["canvas"]);
    recording::clampAnimationSize(s.config, s.canvas, limits(s.op.settings));
    // A dragged region reports its bounds; the overlay says whether it stayed outside them.
    QRect rect;
    const bool hasRect = rectFrom(result["rect"], rect);
    if (s.target.kind == "region" && hasRect) s.target.rect = rect;
    if (hasRect) s.recorded = rect;
    // What the bar can record: a remembered microphone toggle without a microphone is off (#1364).
    if (result["audio"].isObject()) s.config.audio.microphone = s.config.audio.microphone && result["audio"].toObject()["microphone"].toBool();
    if (result["microphone"].isObject()) s.progress["recordingMicrophone"] = result["microphone"];
    if (result["overlayExcluded"].isBool()) s.overlayExcluded = result["overlayExcluded"].toBool();
    if (result["overlayPlacement"].isString()) s.overlayPlacement = result["overlayPlacement"].toString();
    // The helper could not place its bar (no layer shell): the shell frames the
    // area and moves the bar; overlayExcluded follows where the bar really went.
    if (s.overlayPlacement == "compositor" && shell_ && hasRect && s.target.kind != "window" && backend_->processId(s.op.id) > 0) {
        s.shellFrame = true;
        shell_->showFrame(shellFrame(s, RecordingState::Ready), marshal(s.generation, [this](Error error, QJsonObject result) {
            auto& s = *session_;
            if (s.state != RecordingState::Preparing) return;
            const auto bar = result["bar"].toObject();
            if (!error && bar["placed"].toBool()) {
                // With no room outside, the Shell keeps the bar in the corner and
                // the helper shrinks it to its handle from Record on.
                const bool outside = bar["outside"].toBool();
                s.overlayExcluded = outside;
                s.overlayPlacement = outside ? QStringLiteral("outside") : QStringLiteral("handle");
                if (!outside) backend_->control(s.op.id, "overlay", {{"handle", true}}, [](Error, QJsonObject) {});
            }
            transition(s, RecordingState::Ready);
        }));
        return;
    }
    transition(s, RecordingState::Ready);
}
void RecordingService::commandSession(const QString& command, const QJsonObject& params, JsonReply reply) {
    if (!session_) { reply({"recording_unavailable", "No recording is active", {}}, {}); return; }
    const auto id = params["id"].toString();
    if (id != session_->op.id) { reply({"recording_unavailable", "No active recording has this ID", {{"recordingId", id}}}, {}); return; }
    auto& s = *session_;
    if (command == "inspect") { reply({}, publicJson(s)); return; }
    // A stop after the recording already stopped itself (its budget, #213) is
    // acknowledged like a duplicate stop, with the review state and the reason.
    if (command == "stop" && s.state == RecordingState::Reviewing) { reply({}, publicJson(s)); return; }
    if (auto error = recording::checkTransition(s.state, command)) { reply(error, {}); return; }
    if (command == "cancel") { cancelActive(reply); return; }
    if (command == "select") {
        // Drives the open selector as a drag would: the rectangle is the region.
        QRect rect;
        if (!rectFrom(params["rect"], rect)) { reply(invalid("select takes rect [x, y, width, height] with a positive size"), {}); return; }
        if (s.selector.isEmpty()) { reply({"recording_state", "No region selection is open", {{"state", recording::stateName(s.state)}}}, {}); return; }
        if (s.selector == "shell") {
            ++s.selection;
            s.selector.clear();
            if (shell_) shell_->cancelSelection();
            s.target.rect = rect;
            prepareBackend(s.generation);
            host_.observe(s.op);
            emit changed(s.op.id);
            reply({}, publicJson(s));
            return;
        }
        backend_->control(s.op.id, "select", {{"rect", rectJson(rect)}}, marshal(s.generation, [this, reply](Error error, QJsonObject) { reply(error, error ? QJsonObject{} : publicJson(*session_)); }));
        return;
    }
    if (command == "audio") {
        // The bar's audio toggles, for an agent: only while the bar offers them. The helper answers with
        // what it will record; its configChanged and progress events have brought the session up to date
        // by then.
        if (s.state != RecordingState::Ready) { reply({"recording_state", "The audio sources can be changed only while the recording is ready", {{"state", recording::stateName(s.state)}}}, {}); return; }
        QJsonObject forwarded;
        for (const auto* key : {"system", "microphone"}) if (params[key].isBool()) forwarded[key] = params[key].toBool();
        backend_->control(s.op.id, "audio", forwarded, marshal(s.generation, [this, reply](Error error, QJsonObject) { reply(error, error ? QJsonObject{} : publicJson(*session_)); }));
        return;
    }
    if (command == "raise" || command == "overlay") {
        QJsonObject forwarded;
        if (command == "overlay") forwarded["visible"] = params["visible"].toBool(true);
        backend_->control(s.op.id, command, forwarded, marshal(s.generation, [this, reply](Error error, QJsonObject) { reply(error, error ? QJsonObject{} : publicJson(*session_)); }));
        return;
    }
    if (command == "start" || command == "pause" || command == "resume" || command == "stop") {
        // Duplicate requests are acknowledged with the current state; a different
        // request while one is in flight is refused rather than queued.
        if (s.pending == command) { reply({}, publicJson(s)); return; }
        if (!s.pending.isEmpty()) { reply({"recording_busy", "Another recording command is still being acknowledged", {{"pending", s.pending}}}, {}); return; }
        s.pending = command;
        if (command == "start") transition(s, RecordingState::Starting), s.pending = command;
        if (command == "stop") transition(s, RecordingState::Stopping), s.pending = command;
        // Where starting the click watch shows on screen (labwc reads its configuration again),
        // it is started first and the capture only once the desktop has answered.
        if (command == "start" && clicks_ && s.config.clickHighlights && clicks_->clickCapability()["beforeCapture"].toBool()) {
            const auto id = s.op.id;
            watchClicks(s, [this, id, reply] { startCapture(id, reply); });
            return;
        }
        if (command == "start") { startCapture(s.op.id, reply); return; }
        backend_->control(s.op.id, command, {}, marshal(s.generation, [this, command, reply](Error error, QJsonObject result) {
            auto& s = *session_;
            // A budget stop can reach review before this command's reply. Keep
            // that finalized clip, reason and edits, just like a duplicate stop.
            if (command == "stop" && s.captureStopped) { reply({}, publicJson(s)); return; }
            if (error) {
                if (command == "pause" || command == "resume") { s.pending.clear(); reply(error, {}); return; }
                fail(error);
                reply(error, {});
                return;
            }
            for (auto it = result.begin(); it != result.end(); ++it) s.progress[it.key()] = it.value();
            if (command == "start") {
                noteCaptured(s, result, true);
                transition(s, RecordingState::Recording);
            }
            else if (command == "pause") transition(s, RecordingState::Paused);
            else if (command == "resume") transition(s, RecordingState::Recording);
            else {
                qint64 duration = 0;
                if (!integer(result["durationUs"], 1, recording::maximumClipUs, &duration)) { fail({"recording_failed", "The recording ended without a usable clip", {}}); reply({"recording_failed", "The recording ended without a usable clip", {}}, {}); return; }
                s.duration = duration;
                s.edit = {};
                s.captureStopped = true;
                noteCaptured(s, result, false);
                s.config.audio.system = s.capturedSystemAudio && !s.config.silent();
                s.config.audio.microphone = s.capturedMicrophone && !s.config.silent();
                s.stopReason = "user";
                updateCanvas(s.canvas, result["canvas"]);
                recording::clampAnimationSize(s.config, s.canvas, limits(s.op.settings));
                transition(s, RecordingState::Reviewing);
            }
            reply({}, publicJson(s));
        }));
        return;
    }
    if (command == "edit") {
        qint64 revision = 0;
        if (!integer(params["revision"], 1, 1LL << 52, &revision) || !params["commands"].isArray()) { reply(invalid("edit takes the current revision and 1 to 100 commands"), {}); return; }
        if (quint64(revision) != s.edit.revision) { reply({"recording_stale", "The edit revision moved; inspect and retry", {{"currentRevision", double(s.edit.revision)}}}, {}); return; }
        if (auto error = recording::applyEditCommands(s.edit, params["commands"].toArray(), s.duration, s.canvas, s.config.fps)) { reply(error, {}); return; }
        backend_->setEdit(s.op.id, s.edit, s.config, limitsJson(s));
        host_.observe(s.op);
        emit changed(s.op.id);
        reply({}, publicJson(s));
        return;
    }
    if (command == "preview") {
        qint64 time = 0, maxEdge = 1024;
        if (!integer(params["time"], 0, std::max<qint64>(0, s.duration - 1), &time) || (params.contains("maxEdge") && !integer(params["maxEdge"], 64, 1600, &maxEdge))) { reply(invalid("preview takes a clip time in microseconds and an optional maxEdge of 64 to 1600"), {}); return; }
        const auto edit = s.edit;
        const auto end = s.edit.exportEnd(s.duration);
        const bool retained = time >= s.edit.trimStart && time < end;
        backend_->preview(s.op.id, s.edit, s.config, time, int(maxEdge), marshal(s.generation, [reply, edit, time, retained](Error error, QJsonObject result) {
            if (error) { reply(error, {}); return; }
            result["timeUs"] = double(time);
            result["retained"] = retained;
            result["outputTimeUs"] = retained ? QJsonValue(double(time - edit.trimStart)) : QJsonValue(QJsonValue::Null);
            result["revision"] = double(edit.revision);
            reply({}, result);
        }));
        return;
    }
    if (command == "review") {
        // The window's controls, as a user works them (#283). An edit a control
        // proposes arrives as an event before the helper's answer; events are
        // handled queued, so the answer queues behind it. The resident then has
        // applied the proposal (or refused it) and pushed the result to the
        // window, so the window is read once more: the reply's window state and
        // edit are the ones after the action.
        if (params["action"] == "retryExport") { retryExport(reply); return; }
        auto forwarded = params;
        forwarded.remove("id");
        const bool inspect = forwarded["action"].toString() == "inspect";
        if (s.state == RecordingState::Exporting && !inspect) { reply({"recording_state", "The review controls are disabled while an export runs", {{"state", recording::stateName(s.state)}}}, {}); return; }
        const auto generation = s.generation;
        auto answer = [this, reply](Error error, QJsonObject result) {
            if (error) { reply(error, {}); return; }
            result["state"] = recording::stateName(session_->state);
            result["edit"] = session_->edit.toJson();
            reply({}, result);
        };
        backend_->control(s.op.id, "review", forwarded, marshal(generation, [this, answer, generation, inspect](Error error, QJsonObject result) {
            if (error || inspect) { answer(error, result); return; }
            QMetaObject::invokeMethod(this, [this, answer, generation] {
                if (!session_ || session_->generation != generation) { answer({"recording_unavailable", "The recording ended", {}}, {}); return; }
                backend_->control(session_->op.id, "review", {{"action", "inspect"}}, marshal(generation, answer));
            }, Qt::QueuedConnection);
        }));
        return;
    }
    if (command == "saveCopy" || command == "upload") { exportTo(command, params, reply); return; }
    reply({"unknown_method", "Unknown recording command", {}}, {});
}
void RecordingService::retryExport(JsonReply reply, JsonReply done) {
    auto& s = *session_;
    if (s.state != RecordingState::Reviewing || s.lastExport.value("status") != "failed" || s.lastExportRequest.isEmpty()) {
        reply({"recording_state", "There is no failed export to retry", {}}, {});
        return;
    }
    auto params = s.lastExportRequest;
    params["revision"] = double(s.edit.revision);
    exportTo(s.lastExport["command"].toString(), params, std::move(reply), std::move(done));
}
void RecordingService::exportTo(const QString& command, const QJsonObject& params, JsonReply reply, JsonReply done) {
    auto& s = *session_;
    const auto settings = s.op.settings;
    QString destination;
    bool overwrite = false;
    if (command == "saveCopy") {
        destination = params["path"].toString();
        overwrite = params["overwrite"].toBool();
        if (!QDir::isAbsolutePath(destination) || !destination.endsWith("." + s.config.extension(), Qt::CaseInsensitive)) { reply(invalid("saveCopy needs an absolute path ending in ." + s.config.extension()), {}); return; }
        if (QFileInfo::exists(destination) && !overwrite) { reply({"file_exists", "The file already exists; pass overwrite to replace it", {}}, {}); return; }
    } else {
        qint64 revision = 0;
        if (!integer(params["revision"], 1, 1LL << 52, &revision)) { reply(invalid("upload takes the edit revision it was reviewed at"), {}); return; }
        if (quint64(revision) != s.edit.revision) { reply({"recording_stale", "The edit revision moved; inspect and retry", {{"currentRevision", double(s.edit.revision)}}}, {}); return; }
    }
    s.lastExportRequest = params;
    s.lastExport = {{"command", command}, {"status", "exporting"}, {"revision", double(s.edit.revision)}};
    if (command == "saveCopy") s.lastExport["path"] = destination;
    if (auto error = recording::checkExportBudget(s.config, s.edit, s.duration, s.canvas, limits(settings))) {
        s.lastExport["status"] = "failed";
        s.lastExport["error"] = error.toJson();
        reply(error, {});
        return;
    }
    const auto output = s.staging + QStringLiteral("/export-%1.%2").arg(++s.exports).arg(s.config.extension());
    const auto edit = s.edit;
    const auto config = s.config;
    const auto generation = s.generation;
    transition(s, RecordingState::Exporting, command == "upload" ? QStringLiteral("encoding") : QString());
    s.pending = command;
    // An export may take minutes (the helper allows 600 s), longer than a client
    // waits, so the request is answered once it starts, like prepare. Callers
    // poll inspect: `lastExport` and the operation carry the outcome.
    reply({}, publicJson(s));
    if (!session_ || session_->generation != generation) return;
    backend_->exportClip(s.op.id, edit, config, output, marshal(generation, [this, command, destination, overwrite, output, edit, config, done](Error error, QJsonObject result) {
        auto& s = *session_;
        const auto maximum = s.op.settings.limits.maxFileBytes;
        const QFileInfo file(output);
        auto failed = [&](const Error& cause) {
            QFile::remove(output);
            s.lastExport["status"] = "failed";
            s.lastExport["error"] = cause.toJson();
            transition(s, RecordingState::Reviewing);
            if (done) done(cause, {});
        };
        if (!error && (!file.isFile() || file.size() < 1)) error = {"recording_export_failed", "The export produced no file", {}};
        if (!error && file.size() > maximum) error = {"input_excessive", "The export exceeds the file size limit", {{"bytes", double(file.size())}, {"limit", double(maximum)}}};
        if (error) { failed(error); return; }
        if (command == "saveCopy") {
            if (overwrite && QFileInfo::exists(destination)) QFile::remove(destination);
            if (!QFile::copy(output, destination)) { failed({"local_save_failed", "Cannot write the copy", {{"path", destination}}}); return; }
            QFile::setPermissions(destination, QFile::ReadOwner | QFile::WriteOwner);
            QFile::remove(output);
            s.lastExport["status"] = "saved";
            s.lastExport["bytes"] = double(file.size());
            s.lastExport["durationUs"] = result.value("durationUs");
            // Only the completed local copy is a saved result; output is disposable staging.
            backend_->control(s.op.id, "savedCopy", {{"path", destination}, {"revision", double(edit.revision)}, {"config", config.toJson()}}, [](Error, QJsonObject) {});
            transition(s, RecordingState::Reviewing);
            if (done) done({}, publicJson(s));
            return;
        }
        // Explicit Upload: these exact validated bytes become the operation's only artifact.
        checkStep(s, RecordingState::Submitted);
        auto finished = std::move(*session_);
        session_.reset();
        backend_->discard(finished.op.id);
        ClipboardPayload payload;
        payload.kind = "recording";
        payload.files = {output};
        payload.mime = config.mime();
        auto op = finished.op;
        op.advance(OperationState::Acquiring);
        op.phase = "queued";
        const auto staging = finished.staging;
        host_.admit(op, std::move(payload), [staging] { QDir(staging).removeRecursively(); });
        emit changed(op.id);
        if (done) done({}, {{"recordingId", op.id}, {"operationId", op.id}, {"state", "submitted"}, {"bytes", double(file.size())}, {"durationUs", result["durationUs"]}, {"mime", config.mime()}, {"revision", double(edit.revision)}});
    }));
}
}
