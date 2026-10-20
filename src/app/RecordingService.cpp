#include "RecordingService.h"
#include "core/Log.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMetaObject>
#include <QThread>
#include <algorithm>
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
    result["commands"] = QJsonArray{"prepare", "start", "pause", "resume", "stop", "cancel", "inspect", "select", "raise", "overlay", "edit", "preview", "review", "saveCopy", "upload"};
    return result;
}
QJsonObject RecordingService::publicJson(const Session& s) const {
    const auto retained = s.state == "reviewing" || s.state == "exporting" ? s.edit.exportDuration(s.duration) : 0;
    QJsonObject o{{"recordingId", s.op.id}, {"operationId", s.op.id}, {"state", s.state}, {"pending", s.pending.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s.pending)},
        {"target", s.target.toJson()}, {"config", s.config.toJson()}, {"canvas", sizeJson(s.canvas)}, {"durationUs", double(s.duration)},
        {"retainedUs", double(retained)}, {"edit", s.edit.toJson()}, {"progress", s.progress}, {"exports", s.exports},
        {"lastExport", s.lastExport.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s.lastExport)}, {"error", s.op.error.toJson()}};
    if (!s.canvas.isEmpty()) o["outputSize"] = sizeJson(recording::outputSize(s.canvas, s.config.maxEdge));
    if (s.overlayExcluded) o["overlayExcluded"] = *s.overlayExcluded;
    if (!s.overlayPlacement.isEmpty()) o["overlayPlacement"] = s.overlayPlacement;
    o["selecting"] = !s.selector.isEmpty();
    if (!s.stopReason.isEmpty()) o["stopReason"] = s.stopReason;
    if (!s.stopDetails.isEmpty()) o["stopDetails"] = s.stopDetails;
    return o;
}
// The helper ended the capture on its byte budget (#225): say which bound ran
// out, with the figures and what to do, in the review window, the tray and
// recording.inspect. The clip recorded so far stays for review.
void RecordingService::stoppedOnBudget(Session& s) {
    const auto maxFile = s.op.settings.limits.maxFileBytes;
    auto disk = ArtifactStore::diskBudget(stagingRoot_);
    // A disk that filled up during the capture ends it on the disk, whatever the prepare-time bound was.
    const bool diskFull = s.budgetLimit == "disk_space" || (disk.freeBytes >= 0 && disk.available() < 2 * minimumBudgetBytes);
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
void RecordingService::invoke(const QString& mode, JsonReply reply) {
    QString step = "raise";
    if (!session_) step = "prepare";
    else if (mode == "toggle" && session_->state == "ready") step = "start";
    else if (mode == "toggle" && (session_->state == "recording" || session_->state == "paused")) step = "stop";
    const auto params = step == "prepare" ? QJsonObject{{"target", QJsonObject{{"kind", "region"}}}, {"format", "mp4"}} : QJsonObject{{"id", session_->op.id}};
    command(step, params, [step, reply](Error error, QJsonObject result) {
        result["step"] = step;
        reply(error, result);
    });
}
void RecordingService::transition(Session& s, const QString& state, const QString& phase) {
    logging::info(logRecording(), "Recording step", {{"operationId", s.op.id}, {"from", s.state}, {"state", state}, {"elapsedMs", s.clock.isValid() ? s.clock.elapsed() : -1}});
    s.state = state;
    s.pending.clear();
    s.op.phase = phase.isEmpty() ? state : phase;
    // The shell's ring stays through start; dimming covers recording, pause and the stop.
    if (s.shellFrame && shell_ && (state == "recording" || state == "paused")) shell_->showFrame(shellFrame(s, state), [](Error, QJsonObject) {});
    else if (s.shellFrame && state != "ready" && state != "starting" && state != "stopping") closeShell(s);
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
        if (QThread::currentThread() == guard->thread()) deliver();
        else QMetaObject::invokeMethod(guard, std::move(deliver), Qt::QueuedConnection);
    };
}
QJsonObject RecordingService::shellFrame(const Session& s, const QString& state) const {
    return {{"rect", rectJson(s.recorded)}, {"kind", s.target.kind}, {"state", state}, {"pid", double(backend_->processId(s.op.id))}};
}
void RecordingService::closeShell(Session& s) {
    if (!shell_) return;
    if (s.selector == "shell") { ++s.selection; s.selector.clear(); shell_->cancelSelection(); }
    if (s.shellFrame) { s.shellFrame = false; shell_->hideFrame(); }
}
void RecordingService::fail(Error error) {
    if (!session_) return;
    closeShell(*session_);
    auto s = std::move(*session_);
    session_.reset();
    backend_->discard(s.op.id);
    QDir(s.staging).removeRecursively();
    // A user cancellation reported by the helper (Escape during selection,
    // Cancel on the bar) ends as cancelled, not as a failure.
    const bool cancelled = error.code == "cancelled" || error.code == "source_cancelled";
    // Which step ended it and how long in (#250): a timeout names its step,
    // otherwise the state the session was in.
    logging::write(logRecording(), cancelled ? QtInfoMsg : QtWarningMsg, cancelled ? "Recording cancelled" : "Recording failed",
        {{"operationId", s.op.id}, {"error", logging::errorFields(error)}, {"step", error.details["step"].toString(s.state)}, {"state", s.state}, {"phase", s.op.phase},
         {"selector", s.selector}, {"elapsedMs", s.clock.isValid() ? s.clock.elapsed() : -1}});
    s.op.error = error;
    s.op.cancelled = cancelled;
    s.op.state = cancelled ? "cancelled" : "failed";
    s.op.phase = s.op.state;
    host_.finish(s.op);
    emit changed(s.op.id);
}
void RecordingService::cancelActive(JsonReply reply) {
    closeShell(*session_);
    auto s = std::move(*session_);
    session_.reset();
    backend_->control(s.op.id, "cancel", {}, [](Error, QJsonObject) {});
    backend_->discard(s.op.id);
    QDir(s.staging).removeRecursively();
    s.op.cancelled = true;
    s.op.state = "cancelled";
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
        s.progress = progress;
        qint64 duration = 0;
        if (integer(event["durationUs"], 0, recording::maximumClipUs, &duration)) s.duration = duration;
        host_.observe(s.op);
    } else if (kind == "stopped" && (s.state == "recording" || s.state == "paused" || s.state == "stopping")) {
        // The helper stopped on its own (budget, source loss) with a reviewable clip.
        qint64 duration = 0;
        if (!integer(event["durationUs"], 1, recording::maximumClipUs, &duration)) { fail({"recording_failed", "The recording ended without a usable clip", {}}); return; }
        s.duration = duration;
        s.edit = {};
        s.stopReason = event["reason"].toString();
        updateCanvas(s.canvas, event["canvas"]);
        recording::clampAnimationSize(s.config, s.canvas, limits(s.op.settings));
        if (s.stopReason == "budget") stoppedOnBudget(s);
        transition(s, "reviewing");
    } else if (kind == "selecting" && s.state == "preparing") {
        // The helper's own selector is open (X11, KDE): recording.select can drive it.
        s.selector = "helper";
        logging::info(logRecording(), "Recording selector open", {{"operationId", s.op.id}, {"selector", s.selector}, {"elapsedMs", s.clock.elapsed()}});
        host_.observe(s.op);
        emit changed(s.op.id);
    } else if (kind == "failed") {
        auto error = event["error"].toObject();
        fail({error["code"].toString("recording_failed"), error["message"].toString("The recording helper failed"), error["details"].toObject()});
    } else if (kind == "started" && (s.state == "ready" || s.state == "starting")) {
        // The user pressed Record on the helper's control bar.
        for (auto it = event.begin(); it != event.end(); ++it) if (it.key() != "id" && it.key() != "event") s.progress[it.key()] = it.value();
        transition(s, "recording");
    } else if (kind == "paused" && s.state == "recording") transition(s, "paused");
    else if (kind == "resumed" && s.state == "paused") transition(s, "recording");
    else if (kind == "cancelled" && !recording::terminal(s.state)) cancelActive({});
    else if (kind == "configChanged" && (s.state == "ready" || s.state == "reviewing")) {
        auto json = event["config"].toObject();
        for (const auto* key : {"mime", "budgetBytes"}) json.remove(key);
        if (s.state == "reviewing") {
            // After the clip exists only the output can change: the capture rate is
            // fixed and audio cannot appear (and is dropped for silent formats).
            const auto format = json["format"].toString();
            json.remove("fps");
            json["audio"] = QJsonObject{{"system", s.config.audio.system && format != "gif" && format != "webp"}, {"microphone", false}};
        }
        RecordingConfig config;
        if (!recording::parseConfig(json, config)) {
            config.budgetBytes = s.config.budgetBytes;
            if (s.state == "reviewing") config.fps = s.config.fps;
            // Choosing GIF or WebP is a format choice: fit the output to the animation limit.
            const bool clamped = recording::clampAnimationSize(config, s.canvas, limits(s.op.settings));
            s.config = config;
            if (clamped && s.state == "reviewing") backend_->setEdit(s.op.id, s.edit, s.config, limitsJson(s));
            host_.observe(s.op);
            emit changed(s.op.id);
        }
    } else if (kind == "edit" && s.state == "reviewing") {
        // The review window proposes commands at the revision it showed; the
        // resident's answer (setEdit) is what it displays, accepted or not.
        qint64 revision = 0;
        if (integer(event["revision"], 1, 1LL << 52, &revision) && quint64(revision) == s.edit.revision && event["commands"].isArray())
            if (!recording::applyEditCommands(s.edit, event["commands"].toArray(), s.duration, s.canvas, s.config.fps)) { host_.observe(s.op); emit changed(s.op.id); }
        backend_->setEdit(s.op.id, s.edit, s.config, limitsJson(s));
    } else if ((kind == "uploadRequested" || kind == "saveCopyRequested") && s.state == "reviewing") {
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
        exportTo(kind == "uploadRequested" ? "upload" : "saveCopy", params, refused, refused);
    }
}
void RecordingService::command(const QString& command, const QJsonObject& params, JsonReply reply) {
    if (stopping_) { reply({"session_unavailable", "The resident is shutting down", {}}, {}); return; }
    QPointer<RecordingService> self(this);
    if (command == "capabilities") { backend_->refreshCapabilities([self, reply] { if (self && !self->stopping_) reply({}, self->capabilities()); }); return; }
    if (command == "prepare") {
        // The helper probe is lazy; make it current before judging support.
        auto probed = std::make_shared<bool>(false);
        backend_->refreshCapabilities([self, reply, params, probed] { if (!self || self->stopping_ || std::exchange(*probed, true)) return; self->prepareSession(params, reply); });
        return;
    }
    commandSession(command, params, std::move(reply));
}
void RecordingService::prepareSession(const QJsonObject& params, JsonReply reply) {
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
        if (!caps["targets"].toArray().contains(s.target.kind)) { reply({"recording_unsupported", "This target kind is not supported here", {{"kind", s.target.kind}}}, {}); return; }
        if (!caps["formats"].toArray().contains(s.config.format)) { reply({"recording_unsupported", "This output format is not supported here", {{"format", s.config.format}}}, {}); return; }
        if (!s.config.codec.isEmpty() && !caps["codecs"].toObject()[s.config.format].toArray().contains(s.config.codec)) { reply({"recording_unsupported", "This codec is not supported here", {{"codec", s.config.codec}}}, {}); return; }
        if (s.config.audio.system && !caps["audio"].toObject()["system"].toBool()) { reply({"recording_unsupported", "System audio capture is not supported here", {{"field", "audio.system"}}}, {}); return; }
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
        // Stop before the admitted file limit, leaving room for finalization.
        s.config.budgetBytes = std::max<qint64>(1024 * 1024, s.op.settings.limits.maxFileBytes - std::max<qint64>(s.op.settings.limits.maxFileBytes / 10, 64LL * 1024 * 1024));
        s.budgetLimit = "max_file_bytes";
        if (disk && disk->available() / 3 < s.config.budgetBytes) {
            s.config.budgetBytes = disk->available() / 3;
            s.budgetLimit = "disk_space";
        }
        s.op.state = "recording";
        s.op.phase = "preparing";
        s.staging = stagingRoot_ + "/" + s.op.id;
        if (!QDir().mkpath(s.staging) || !QFile::setPermissions(s.staging, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
            s.op.error = {"recording_io", "Cannot create private recording staging", {}};
            s.op.state = "failed";
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
void RecordingService::beginPrepare(quint64 generation) {
    auto& s = *session_;
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
            if (s.state != "preparing" || s.selector != "shell" || s.selection != token) return;
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
    backend_->prepare(s.op.id, s.target, s.config, s.staging, marshal(generation, [this](Error error, QJsonObject result) { prepared(error, result); }));
}
void RecordingService::prepared(Error error, const QJsonObject& result) {
    auto& s = *session_;
    if (s.state != "preparing") return;
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
    if (result["overlayExcluded"].isBool()) s.overlayExcluded = result["overlayExcluded"].toBool();
    if (result["overlayPlacement"].isString()) s.overlayPlacement = result["overlayPlacement"].toString();
    // The helper could not place its bar (no layer shell): the shell frames the
    // area and moves the bar; overlayExcluded follows where the bar really went.
    if (s.overlayPlacement == "compositor" && shell_ && hasRect && s.target.kind != "window" && backend_->processId(s.op.id) > 0) {
        s.shellFrame = true;
        shell_->showFrame(shellFrame(s, "ready"), marshal(s.generation, [this](Error error, QJsonObject result) {
            auto& s = *session_;
            if (s.state != "preparing") return;
            const auto bar = result["bar"].toObject();
            if (!error && bar["placed"].toBool()) {
                // With no room outside, the Shell keeps the bar in the corner and
                // the helper shrinks it to its handle from Record on.
                const bool outside = bar["outside"].toBool();
                s.overlayExcluded = outside;
                s.overlayPlacement = outside ? QStringLiteral("outside") : QStringLiteral("handle");
                if (!outside) backend_->control(s.op.id, "overlay", {{"handle", true}}, [](Error, QJsonObject) {});
            }
            transition(s, "ready");
        }));
        return;
    }
    transition(s, "ready");
}
void RecordingService::commandSession(const QString& command, const QJsonObject& params, JsonReply reply) {
    if (!session_) { reply({"recording_unavailable", "No recording is active", {}}, {}); return; }
    const auto id = params["id"].toString();
    if (id != session_->op.id) { reply({"recording_unavailable", "No active recording has this ID", {{"recordingId", id}}}, {}); return; }
    auto& s = *session_;
    if (command == "inspect") { reply({}, publicJson(s)); return; }
    // A stop after the recording already stopped itself (its budget, #213) is
    // acknowledged like a duplicate stop, with the review state and the reason.
    if (command == "stop" && s.state == "reviewing") { reply({}, publicJson(s)); return; }
    if (auto error = recording::checkTransition(s.state, command)) { reply(error, {}); return; }
    if (command == "cancel") { cancelActive(reply); return; }
    if (command == "select") {
        // Drives the open selector as a drag would: the rectangle is the region.
        QRect rect;
        if (!rectFrom(params["rect"], rect)) { reply(invalid("select takes rect [x, y, width, height] with a positive size"), {}); return; }
        if (s.selector.isEmpty()) { reply({"recording_state", "No region selection is open", {{"state", s.state}}}, {}); return; }
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
        if (command == "start") transition(s, "starting"), s.pending = command;
        if (command == "stop") transition(s, "stopping"), s.pending = command;
        backend_->control(s.op.id, command, {}, marshal(s.generation, [this, command, reply](Error error, QJsonObject result) {
            auto& s = *session_;
            if (error) {
                if (command == "pause" || command == "resume") { s.pending.clear(); reply(error, {}); return; }
                fail(error);
                reply(error, {});
                return;
            }
            for (auto it = result.begin(); it != result.end(); ++it) s.progress[it.key()] = it.value();
            if (command == "start") transition(s, "recording");
            else if (command == "pause") transition(s, "paused");
            else if (command == "resume") transition(s, "recording");
            else {
                qint64 duration = 0;
                if (!integer(result["durationUs"], 1, recording::maximumClipUs, &duration)) { fail({"recording_failed", "The recording ended without a usable clip", {}}); reply({"recording_failed", "The recording ended without a usable clip", {}}, {}); return; }
                s.duration = duration;
                s.edit = {};
                updateCanvas(s.canvas, result["canvas"]);
                recording::clampAnimationSize(s.config, s.canvas, limits(s.op.settings));
                transition(s, "reviewing");
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
        backend_->preview(s.op.id, s.edit, s.config, time, int(maxEdge), marshal(s.generation, [this, reply, edit, time, retained](Error error, QJsonObject result) {
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
        // handled queued, so the answer queues behind it and its edit is the one
        // the proposal produced (or the unchanged one it was refused against).
        auto forwarded = params;
        forwarded.remove("id");
        if (s.state == "exporting" && forwarded["action"].toString() != "inspect") { reply({"recording_state", "The review controls are disabled while an export runs", {{"state", s.state}}}, {}); return; }
        const auto generation = s.generation;
        backend_->control(s.op.id, "review", forwarded, marshal(generation, [this, reply, generation](Error error, QJsonObject result) {
            if (error) { reply(error, {}); return; }
            QMetaObject::invokeMethod(this, [this, reply, generation, result]() mutable {
                if (!session_ || session_->generation != generation) { reply({"recording_unavailable", "The recording ended", {}}, {}); return; }
                result["state"] = session_->state;
                result["edit"] = session_->edit.toJson();
                reply({}, result);
            }, Qt::QueuedConnection);
        }));
        return;
    }
    if (command == "saveCopy" || command == "upload") { exportTo(command, params, reply); return; }
    reply({"unknown_method", "Unknown recording command", {}}, {});
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
    if (auto error = recording::checkExportBudget(s.config, s.edit, s.duration, s.canvas, limits(settings))) { reply(error, {}); return; }
    const auto output = s.staging + QStringLiteral("/export-%1.%2").arg(++s.exports).arg(s.config.extension());
    const auto edit = s.edit;
    const auto config = s.config;
    const auto generation = s.generation;
    s.lastExport = {{"command", command}, {"status", "exporting"}, {"revision", double(edit.revision)}};
    if (command == "saveCopy") s.lastExport["path"] = destination;
    transition(s, "exporting", command == "upload" ? QStringLiteral("encoding") : QString());
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
            transition(s, "reviewing");
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
            transition(s, "reviewing");
            if (done) done({}, publicJson(s));
            return;
        }
        // Explicit Upload: these exact validated bytes become the operation's only artifact.
        auto finished = std::move(*session_);
        session_.reset();
        backend_->discard(finished.op.id);
        ClipboardPayload payload;
        payload.kind = "recording";
        payload.files = {output};
        payload.mime = config.mime();
        auto op = finished.op;
        op.state = "acquiring";
        op.phase = "queued";
        const auto staging = finished.staging;
        host_.admit(op, std::move(payload), [staging] { QDir(staging).removeRecursively(); });
        emit changed(op.id);
        if (done) done({}, {{"recordingId", op.id}, {"operationId", op.id}, {"state", "submitted"}, {"bytes", double(file.size())}, {"durationUs", result["durationUs"]}, {"mime", config.mime()}, {"revision", double(edit.revision)}});
    }));
}
}
