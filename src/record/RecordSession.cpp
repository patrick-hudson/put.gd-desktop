#include "RecordSession.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <cmath>

namespace putgd::record {
namespace {
QJsonArray sizeJson(const QSize& s) { return {s.width(), s.height()}; }
bool integer(const QJsonValue& value, qint64 min, qint64 max, qint64* out) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < double(min) || number > double(max)) return false;
    *out = qint64(number);
    return true;
}
// Encoder preference: hardware (VA) first, then x264, then OpenH264.
const QStringList videoEncoders{"vah264lpenc", "vah264enc", "vaapih264enc", "x264enc", "openh264enc"};
const QStringList audioEncoders{"avenc_aac", "fdkaacenc", "voaacenc"};
QStringList available(const QStringList& names) {
    QStringList result;
    for (const auto& name : names) if (GstElementFactory* f = gst_element_factory_find(name.toUtf8().constData())) { gst_object_unref(f); result.append(name); }
    return result;
}
}

RecordSession::RecordSession(QObject* parent) : QObject(parent), pipeline_(this), progress_(this) {
    progress_.setInterval(1000);
    connect(&progress_, &QTimer::timeout, this, &RecordSession::emitProgress);
    connect(&pipeline_, &RecordingPipeline::failed, this, [this](Error error) {
        progress_.stop();
        emit event({{"event", "failed"}, {"error", error.toJson()}});
    });
    connect(&pipeline_, &RecordingPipeline::budgetReached, this, [this] {
        // The admitted byte budget is nearly spent: finish the clip and open review.
        pipeline_.stop([this](Error error, qint64 durationUs) {
            progress_.stop();
            if (error || durationUs < 1) { emit event({{"event", "failed"}, {"error", (error ? error : Error{"recording_failed", "The clip could not be finalized", {}}).toJson()}}); return; }
            durationUs_ = durationUs;
            recorded_ = true;
            emit event({{"event", "stopped"}, {"durationUs", double(durationUs)}, {"canvas", sizeJson(resolved_.canvas)}, {"reason", "budget"}});
        });
    });
}
RecordSession::~RecordSession() {
    if (exportCancel_) exportCancel_->store(true);
    if (exportThread_) { exportThread_->wait(); delete exportThread_; }
    pipeline_.cancel();
}
void RecordSession::command(const QString& command, const QJsonObject& params, Reply reply) {
    if (command == "prepare") { prepare(params, std::move(reply)); return; }
    if (!prepared_) { reply({"recording_state", "The helper has no prepared session", {}}, {}); return; }
    if (command == "start") { start(std::move(reply)); return; }
    if (command == "pause") { if (auto e = pipeline_.pause()) reply(e, {}); else { emitProgress(); reply({}, pipeline_.status()); } return; }
    if (command == "resume") { if (auto e = pipeline_.resume()) reply(e, {}); else { emitProgress(); reply({}, pipeline_.status()); } return; }
    if (command == "stop") { stop(std::move(reply)); return; }
    if (command == "cancel") {
        if (exportCancel_) exportCancel_->store(true);
        progress_.stop();
        pipeline_.cancel();
        QFile::remove(workingClip_);
        reply({}, {});
        return;
    }
    if (command == "raise" || command == "overlay") { reply({}, {}); return; }
    if (command == "preview") { preview(params, std::move(reply)); return; }
    if (command == "export") { exportClip(params, std::move(reply)); return; }
    reply({"unknown_method", "Unknown recording helper command", {}}, {});
}
void RecordSession::prepare(const QJsonObject& params, Reply reply) {
    if (prepared_) { reply({"recording_state", "The helper already has a session", {}}, {}); return; }
    RecordingTarget target;
    RecordingConfig config;
    if (auto e = recording::parseTarget(params["target"], target)) { reply(e, {}); return; }
    auto configJson = params["config"].toObject();
    qint64 budget = 0;
    integer(configJson["budgetBytes"], 0, 1LL << 50, &budget);
    for (const auto* key : {"mime", "budgetBytes"}) configJson.remove(key);
    if (auto e = recording::parseConfig(configJson, config)) { reply(e, {}); return; }
    config.budgetBytes = budget;
    const auto staging = params["staging"].toString();
    if (staging.isEmpty() || !QFileInfo(staging).isDir()) { reply({"recording_io", "The staging directory is missing", {}}, {}); return; }
    ResolvedTarget resolved;
    if (auto e = resolveX11Target(target, resolved)) { reply(e, {}); return; }
    if (target.kind == "region" && resolved.rect.size() != target.rect.size()) {
        // Even dimensions only: the recorded area shrinks by at most one pixel per side.
    }
    id_ = params["id"].toString();
    target_ = target;
    config_ = config;
    resolved_ = resolved;
    staging_ = staging;
    workingClip_ = staging + "/working.mkv";
    prepared_ = true;
    reply({}, {{"canvas", sizeJson(resolved_.canvas)}, {"rect", QJsonArray{resolved_.rect.x(), resolved_.rect.y(), resolved_.rect.width(), resolved_.rect.height()}}, {"window", double(resolved_.window)}});
}
void RecordSession::start(Reply reply) {
    if (pipeline_.running() || recorded_) { reply({"recording_state", "The session already recorded", {}}, {}); return; }
    RecordingPipeline::Options options;
    options.rect = resolved_.rect;
    options.window = resolved_.window;
    options.fps = config_.fps;
    options.audio = config_.audio.system && !config_.silent();
    options.audioDevice = "@DEFAULT_MONITOR@";
    options.output = workingClip_;
    options.budgetBytes = config_.budgetBytes;
    options.encoders = available(videoEncoders);
    options.audioEncoders = available(audioEncoders);
    if (options.encoders.isEmpty()) { reply({"recording_unsupported", "No H.264 encoder is installed", {}}, {}); return; }
    if (auto e = pipeline_.start(options)) { reply(e, {}); return; }
    progress_.start();
    reply({}, pipeline_.status());
}
void RecordSession::stop(Reply reply) {
    if (!pipeline_.running()) { reply({"recording_state", "Nothing is recording", {}}, {}); return; }
    QPointer<RecordSession> guard(this);
    pipeline_.stop([guard, reply](Error error, qint64 durationUs) {
        if (!guard) return;
        guard->progress_.stop();
        if (error) { reply(error, {}); return; }
        if (durationUs < 1 || !QFileInfo(guard->workingClip_).isFile()) { reply({"recording_failed", "The recording produced no retained frames", {}}, {}); return; }
        guard->durationUs_ = durationUs;
        guard->recorded_ = true;
        // Lab diagnostics only: keep a private copy of the working clip for inspection.
        if (const auto keep = qEnvironmentVariable("PUTGD_RECORD_KEEP_WORKING"); !keep.isEmpty()) QFile::copy(guard->workingClip_, keep + "/" + guard->id_ + ".mkv");
        reply({}, {{"durationUs", double(durationUs)}, {"canvas", sizeJson(guard->resolved_.canvas)}, {"bytes", double(QFileInfo(guard->workingClip_).size())}, {"encoder", guard->pipeline_.encoder()}, {"audioEncoder", guard->pipeline_.audioEncoder()}});
    });
}
void RecordSession::emitProgress() {
    if (!pipeline_.running()) return;
    auto status = pipeline_.status();
    status["event"] = "progress";
    emit event(status);
}
bool RecordSession::parseEdit(const QJsonObject& json, RecordingEdit& edit) const {
    qint64 start = 0, end = 0, revision = 0;
    if (!integer(json["trimStart"], 0, durationUs_, &start) || !integer(json["revision"], 1, 1LL << 52, &revision) || !json["masks"].isArray()) return false;
    edit.trimStart = start;
    edit.revision = quint64(revision);
    if (json["trimEnd"].isNull() || json["trimEnd"].isUndefined()) edit.trimEnd.reset();
    else if (integer(json["trimEnd"], 0, durationUs_, &end)) edit.trimEnd = end;
    else return false;
    for (const auto& value : json["masks"].toArray()) {
        const auto m = value.toObject();
        RecordingMask mask;
        mask.id = m["id"].toString();
        mask.kind = m["kind"].toString("blur");
        const auto r = m["rect"].toArray();
        if (r.size() != 4) return false;
        mask.rect = QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt());
        qint64 strength = 16;
        if (!integer(m["start"], 0, 1LL << 50, &mask.start) || !integer(m["end"], 0, 1LL << 50, &mask.end)) return false;
        if (m.contains("strength")) integer(m["strength"], 2, 64, &strength);
        mask.strength = int(strength);
        edit.masks.append(mask);
    }
    return true;
}
void RecordSession::preview(const QJsonObject& params, Reply reply) {
    if (!recorded_) { reply({"recording_state", "Nothing to preview before stop", {}}, {}); return; }
    RecordingEdit edit;
    qint64 time = 0, maxEdge = 1024;
    if (!parseEdit(params["edit"].toObject(), edit) || !integer(params["time"], 0, std::max<qint64>(0, durationUs_), &time) || !integer(params["maxEdge"], 16, 8192, &maxEdge)) { reply({"invalid_request", "Invalid preview request", {}}, {}); return; }
    QImage image;
    qint64 actual = 0;
    if (auto e = previewFrame(workingClip_, edit, config_, time, int(maxEdge), resolved_.canvas, image, &actual)) { reply(e, {}); return; }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) { reply({"recording_export_failed", "Cannot encode the preview", {}}, {}); return; }
    reply({}, {{"png", QString::fromLatin1(png.toBase64())}, {"frameTimeUs", double(actual)}, {"width", image.width()}, {"height", image.height()}});
}
void RecordSession::exportClip(const QJsonObject& params, Reply reply) {
    if (!recorded_) { reply({"recording_state", "Nothing to export before stop", {}}, {}); return; }
    if (exportThread_) { reply({"recording_busy", "An export is already running", {}}, {}); return; }
    ExportRequest request;
    request.input = workingClip_;
    request.output = params["output"].toString();
    request.durationUs = durationUs_;
    request.canvas = resolved_.canvas;
    auto configJson = params["config"].toObject();
    for (const auto* key : {"mime", "budgetBytes"}) configJson.remove(key);
    if (!parseEdit(params["edit"].toObject(), request.edit) || request.output.isEmpty() || recording::parseConfig(configJson, request.config)) { reply({"invalid_request", "Invalid export request", {}}, {}); return; }
    request.config.audio.system = request.config.audio.system && config_.audio.system;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    exportCancel_ = cancel;
    auto result = std::make_shared<ExportResult>();
    auto error = std::make_shared<Error>();
    QPointer<RecordSession> guard(this);
    auto* thread = QThread::create([request, result, error, cancel, guard] {
        *error = record::exportClip(request, *result, *cancel, [guard, cancel](double fraction) {
            if (!guard || cancel->load()) return;
            QMetaObject::invokeMethod(guard, [guard, fraction] { if (guard) emit guard->event({{"event", "progress"}, {"exportFraction", fraction}}); }, Qt::QueuedConnection);
        });
    });
    exportThread_ = thread;
    connect(thread, &QThread::finished, this, [this, thread, result, error, request, reply] {
        thread->deleteLater();
        if (exportThread_ == thread) exportThread_ = nullptr;
        exportCancel_.reset();
        if (*error) { QFile::remove(request.output); reply(*error, {}); return; }
        reply({}, {{"bytes", double(result->bytes)}, {"durationUs", double(result->durationUs)}, {"frames", result->frames}, {"videoEncoder", result->videoEncoder}, {"audioEncoder", result->audioEncoder}, {"size", sizeJson(result->size)}});
    });
    thread->start();
}
}
