#include "RecordingService.h"
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
    if (session_ && backend_) { backend_->control(session_->op.id, "cancel", {}, [](Error, QJsonObject) {}); backend_->discard(session_->op.id); }
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
    result["commands"] = QJsonArray{"prepare", "start", "pause", "resume", "stop", "cancel", "inspect", "raise", "overlay", "edit", "preview", "saveCopy", "upload"};
    return result;
}
QJsonObject RecordingService::publicJson(const Session& s) const {
    const auto retained = s.state == "reviewing" || s.state == "exporting" ? s.edit.exportDuration(s.duration) : 0;
    QJsonObject o{{"recordingId", s.op.id}, {"operationId", s.op.id}, {"state", s.state}, {"pending", s.pending.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s.pending)},
        {"target", s.target.toJson()}, {"config", s.config.toJson()}, {"canvas", sizeJson(s.canvas)}, {"durationUs", double(s.duration)},
        {"retainedUs", double(retained)}, {"edit", s.edit.toJson()}, {"progress", s.progress}, {"exports", s.exports}, {"error", s.op.error.toJson()}};
    if (!s.canvas.isEmpty()) o["outputSize"] = sizeJson(recording::outputSize(s.canvas, s.config.maxEdge));
    if (s.overlayExcluded) o["overlayExcluded"] = *s.overlayExcluded;
    return o;
}
void RecordingService::transition(Session& s, const QString& state, const QString& phase) {
    s.state = state;
    s.pending.clear();
    s.op.phase = phase.isEmpty() ? state : phase;
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
void RecordingService::fail(Error error) {
    if (!session_) return;
    auto s = std::move(*session_);
    session_.reset();
    backend_->discard(s.op.id);
    QDir(s.staging).removeRecursively();
    // A user cancellation reported by the helper (Escape during selection,
    // Cancel on the bar) ends as cancelled, not as a failure.
    const bool cancelled = error.code == "cancelled" || error.code == "source_cancelled";
    s.op.error = error;
    s.op.cancelled = cancelled;
    s.op.state = cancelled ? "cancelled" : "failed";
    s.op.phase = s.op.state;
    host_.finish(s.op);
    emit changed(s.op.id);
}
void RecordingService::cancelActive(JsonReply reply) {
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
        if (event["canvas"].isArray() && event["canvas"].toArray().size() == 2) s.canvas = QSize(event["canvas"].toArray()[0].toInt(), event["canvas"].toArray()[1].toInt());
        transition(s, "reviewing");
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
            s.config = config;
            host_.observe(s.op);
            emit changed(s.op.id);
        }
    } else if (kind == "edit" && s.state == "reviewing") {
        // The review window proposes commands at the revision it showed; the
        // resident's answer (setEdit) is what it displays, accepted or not.
        qint64 revision = 0;
        if (integer(event["revision"], 1, 1LL << 52, &revision) && quint64(revision) == s.edit.revision && event["commands"].isArray())
            if (!recording::applyEditCommands(s.edit, event["commands"].toArray(), s.duration, s.canvas, s.config.fps)) { host_.observe(s.op); emit changed(s.op.id); }
        backend_->setEdit(s.op.id, s.edit, s.config);
    } else if ((kind == "uploadRequested" || kind == "saveCopyRequested") && s.state == "reviewing") {
        QJsonObject params{{"id", s.op.id}, {"revision", event["revision"]}};
        if (kind == "saveCopyRequested") { params["path"] = event["path"]; params["overwrite"] = true; }
        exportTo(kind == "uploadRequested" ? "upload" : "saveCopy", params, [this](Error error, QJsonObject) {
            // The helper learns the outcome from its own export command; a refused
            // request (stale revision, budget) is pushed back as the current edit.
            if (error && session_) backend_->setEdit(session_->op.id, session_->edit, session_->config);
        });
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
        if (!caps["available"].toBool()) { reply({"recording_unavailable", "Screen recording is not available on this installation", {{"reason", caps["reason"].toString()}}}, {}); return; }
        if (!caps["targets"].toArray().contains(s.target.kind)) { reply({"recording_unsupported", "This target kind is not supported here", {{"kind", s.target.kind}}}, {}); return; }
        if (!caps["formats"].toArray().contains(s.config.format)) { reply({"recording_unsupported", "This output format is not supported here", {{"format", s.config.format}}}, {}); return; }
        if (!s.config.codec.isEmpty() && !caps["codecs"].toObject()[s.config.format].toArray().contains(s.config.codec)) { reply({"recording_unsupported", "This codec is not supported here", {{"codec", s.config.codec}}}, {}); return; }
        if (s.config.audio.system && !caps["audio"].toObject()["system"].toBool()) { reply({"recording_unsupported", "System audio capture is not supported here", {{"field", "audio.system"}}}, {}); return; }
        if (auto error = host_.accept(s.op)) { reply(error, {}); return; }
        // Stop before the admitted file limit, leaving room for finalization.
        s.config.budgetBytes = std::max<qint64>(1024 * 1024, s.op.settings.limits.maxFileBytes - std::max<qint64>(s.op.settings.limits.maxFileBytes / 10, 64LL * 1024 * 1024));
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
            guard->backend_->prepare(s.op.id, s.target, s.config, s.staging, guard->marshal(generation, [guard](Error error, QJsonObject result) {
                auto& s = *guard->session_;
                if (s.state != "preparing") return;
                if (error) { guard->fail(error); return; }
                if (result["canvas"].isArray() && result["canvas"].toArray().size() == 2) s.canvas = QSize(result["canvas"].toArray()[0].toInt(), result["canvas"].toArray()[1].toInt());
                // A dragged region reports its bounds; the overlay says whether it stayed outside them.
                if (s.target.kind == "region" && result["rect"].isArray() && result["rect"].toArray().size() == 4) { const auto r = result["rect"].toArray(); s.target.rect = QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt()); }
                if (result["overlayExcluded"].isBool()) s.overlayExcluded = result["overlayExcluded"].toBool();
                guard->transition(s, "ready");
            }));
        });
    }
}
void RecordingService::commandSession(const QString& command, const QJsonObject& params, JsonReply reply) {
    if (!session_) { reply({"recording_unavailable", "No recording is active", {}}, {}); return; }
    const auto id = params["id"].toString();
    if (id != session_->op.id) { reply({"recording_unavailable", "No active recording has this ID", {{"recordingId", id}}}, {}); return; }
    auto& s = *session_;
    if (command == "inspect") { reply({}, publicJson(s)); return; }
    if (auto error = recording::checkTransition(s.state, command)) { reply(error, {}); return; }
    if (command == "cancel") { cancelActive(reply); return; }
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
                if (result["canvas"].isArray() && result["canvas"].toArray().size() == 2) s.canvas = QSize(result["canvas"].toArray()[0].toInt(), result["canvas"].toArray()[1].toInt());
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
        backend_->setEdit(s.op.id, s.edit, s.config);
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
    if (command == "saveCopy" || command == "upload") { exportTo(command, params, reply); return; }
    reply({"unknown_method", "Unknown recording command", {}}, {});
}
void RecordingService::exportTo(const QString& command, const QJsonObject& params, JsonReply reply) {
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
    transition(s, "exporting", command == "upload" ? QStringLiteral("encoding") : QString());
    s.pending = command;
    backend_->exportClip(s.op.id, edit, config, output, marshal(s.generation, [this, command, destination, overwrite, output, edit, config, reply](Error error, QJsonObject result) {
        auto& s = *session_;
        const auto maximum = s.op.settings.limits.maxFileBytes;
        const QFileInfo file(output);
        if (!error && (!file.isFile() || file.size() < 1)) error = {"recording_export_failed", "The export produced no file", {}};
        if (!error && file.size() > maximum) error = {"input_excessive", "The export exceeds the file size limit", {{"bytes", double(file.size())}, {"limit", double(maximum)}}};
        if (error) { QFile::remove(output); transition(s, "reviewing"); reply(error, {}); return; }
        if (command == "saveCopy") {
            if (overwrite && QFileInfo::exists(destination)) QFile::remove(destination);
            if (!QFile::copy(output, destination)) { QFile::remove(output); transition(s, "reviewing"); reply({"local_save_failed", "Cannot write the copy", {{"path", destination}}}, {}); return; }
            QFile::setPermissions(destination, QFile::ReadOwner | QFile::WriteOwner);
            QFile::remove(output);
            transition(s, "reviewing");
            reply({}, {{"recordingId", s.op.id}, {"operationId", s.op.id}, {"path", destination}, {"bytes", double(file.size())}, {"durationUs", result["durationUs"]}, {"revision", double(edit.revision)}, {"state", s.state}});
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
        reply({}, {{"recordingId", op.id}, {"operationId", op.id}, {"state", "submitted"}, {"bytes", double(file.size())}, {"durationUs", result["durationUs"]}, {"mime", config.mime()}, {"revision", double(edit.revision)}});
    }));
}
}
