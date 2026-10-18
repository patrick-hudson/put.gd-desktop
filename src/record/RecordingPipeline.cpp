#include "RecordingPipeline.h"
#include "DebugLog.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QElapsedTimer>
#include <algorithm>

namespace putgd::record {
namespace {
struct ProbeContext { RecordingPipeline* owner; std::atomic_bool* paused; void* rebase; };
QString shellQuote(const QString& value) { QString s = value; s.replace('\\', "\\\\").replace('"', "\\\""); return '"' + s + '"'; }
int bitrateKbps(const QSize& size, int fps) {
    // About 0.1 bit per pixel per frame, bounded for tiny and huge canvases.
    const double kbps = double(size.width()) * size.height() * fps * 0.1 / 1000.0;
    return int(std::clamp(kbps, 1500.0, 20000.0));
}
QString encoderDescription(const QString& name, int kbps, int fps) {
    const int keyint = std::max(1, fps * 2);
    if (name == "vah264lpenc" || name == "vah264enc") return QStringLiteral("%1 bitrate=%2 key-int-max=%3").arg(name).arg(kbps).arg(keyint);
    if (name == "vaapih264enc") return QStringLiteral("vaapih264enc rate-control=cbr bitrate=%1 keyframe-period=%2").arg(kbps).arg(keyint);
    if (name == "x264enc") return QStringLiteral("x264enc tune=zerolatency speed-preset=veryfast bitrate=%1 key-int-max=%2 bframes=0 threads=2").arg(kbps).arg(keyint);
    if (name == "openh264enc") return QStringLiteral("openh264enc bitrate=%1 gop-size=%2 complexity=low").arg(qint64(kbps) * 1000).arg(keyint);
    return name;
}
QString encoderFormat(const QString& name) { return name.startsWith("va") ? QStringLiteral("NV12") : QStringLiteral("I420"); }
QString audioDescription(const QString& name) {
    if (name == "avenc_aac" || name == "fdkaacenc" || name == "voaacenc") return name + " bitrate=128000";
    return name;
}
}

RecordingPipeline::RecordingPipeline(QObject* parent) : QObject(parent), bus_(this), progress_(this) {
    bus_.setInterval(50);
    connect(&bus_, &QTimer::timeout, this, &RecordingPipeline::poll);
}
RecordingPipeline::~RecordingPipeline() { tearDown(); }

GstPadProbeReturn RecordingPipeline::probe(GstPad*, GstPadProbeInfo* info, gpointer user) {
    auto* context = static_cast<ProbeContext*>(user);
    auto* rebase = static_cast<Rebase*>(context->rebase);
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer || !GST_BUFFER_PTS_IS_VALID(buffer)) return GST_PAD_PROBE_OK;
    const gint64 pts = gint64(GST_BUFFER_PTS(buffer));
    if (context->paused->load()) { rebase->dropped.store(true); return GST_PAD_PROBE_DROP; }
    const gint64 last = rebase->lastPassed.load();
    if (rebase->dropped.exchange(false) && last >= 0) {
        // Buffers dropped while paused leave a gap; fold it into the offset so the
        // clip timeline continues one step after the last retained buffer.
        const gint64 expected = last + rebase->step.load();
        const gint64 rebased = pts - rebase->offset.load();
        if (rebased > expected) rebase->offset.fetch_add(rebased - expected);
    }
    buffer = gst_buffer_make_writable(buffer);
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    const gint64 shifted = std::max<gint64>(0, pts - rebase->offset.load());
    GST_BUFFER_PTS(buffer) = GstClockTime(shifted);
    if (GST_BUFFER_DTS_IS_VALID(buffer)) GST_BUFFER_DTS(buffer) = GstClockTime(std::max<gint64>(0, gint64(GST_BUFFER_DTS(buffer)) - rebase->offset.load()));
    // Encoders may carry a constant timestamp offset (the VA encoders add an
    // hour); durations are measured from the first retained buffer.
    if (rebase->firstPassed.load() < 0) rebase->firstPassed.store(shifted);
    rebase->lastPassed.store(shifted);
    return GST_PAD_PROBE_OK;
}

bool RecordingPipeline::launch(const Options& options, const QString& encoder, const QString& audioEncoder, QString& failure) {
    const int kbps = bitrateKbps(options.rect.size(), options.fps);
    QString description;
    if (options.window) description = QStringLiteral("ximagesrc name=src xid=%1 use-damage=false show-pointer=true").arg(options.window);
    else description = QStringLiteral("ximagesrc name=src use-damage=false show-pointer=true startx=%1 starty=%2 endx=%3 endy=%4").arg(options.rect.x()).arg(options.rect.y()).arg(options.rect.x() + options.rect.width() - 1).arg(options.rect.y() + options.rect.height() - 1);
    description += QStringLiteral(" ! video/x-raw,framerate=%1/1 ! videoconvert n-threads=2 ! video/x-raw,format=%2 ! %3 ! h264parse ! queue name=vq max-size-buffers=0 max-size-bytes=0 max-size-time=2000000000 ! matroskamux name=mux ! filesink name=sink location=%4")
        .arg(options.fps).arg(encoderFormat(encoder), encoderDescription(encoder, kbps, options.fps), shellQuote(options.output));
    if (options.audio && !audioEncoder.isEmpty())
        description += QStringLiteral(" pulsesrc name=asrc device=%1 ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! %2 ! aacparse ! queue name=aq max-size-buffers=0 max-size-bytes=0 max-size-time=2000000000 ! mux.")
            .arg(shellQuote(options.audioDevice), audioDescription(audioEncoder));
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (!pipeline || error) { failure = error ? QString::fromUtf8(error->message) : QStringLiteral("parse failed"); if (error) g_error_free(error); if (pipeline) gst_object_unref(pipeline); return false; }
    video_.offset = 0; video_.firstPassed = -1; video_.lastPassed = -1; video_.step = frameDurationNs_; video_.dropped = false;
    audio_.offset = 0; audio_.firstPassed = -1; audio_.lastPassed = -1; audio_.step = 20 * GST_MSECOND; audio_.dropped = false;
    debugLog("pipeline: " + description);
    auto attach = [&](const char* element, Rebase& rebase) {
        GstElement* queue = gst_bin_get_by_name(GST_BIN(pipeline), element);
        if (!queue) return false;
        GstPad* pad = gst_element_get_static_pad(queue, "sink");
        auto* context = new ProbeContext{this, &paused_, &rebase};
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &RecordingPipeline::probe, context, [](gpointer data) { delete static_cast<ProbeContext*>(data); });
        gst_object_unref(pad);
        gst_object_unref(queue);
        return true;
    };
    if (!attach("vq", video_) || (options.audio && !audioEncoder.isEmpty() && !attach("aq", audio_))) { failure = "probe attach failed"; gst_object_unref(pipeline); return false; }
    const auto change = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (change == GST_STATE_CHANGE_FAILURE) {
        failure = QStringLiteral("%1 refused to start").arg(encoder);
        GstBus* errors = gst_element_get_bus(pipeline);
        if (GstMessage* message = gst_bus_pop_filtered(errors, GST_MESSAGE_ERROR)) {
            GError* e = nullptr; gchar* debug = nullptr;
            gst_message_parse_error(message, &e, &debug);
            failure += QStringLiteral(": %1 (%2)").arg(e ? QString::fromUtf8(e->message) : QString(), QString::fromUtf8(GST_OBJECT_NAME(GST_MESSAGE_SRC(message))));
            if (e) g_error_free(e);
            g_free(debug);
            gst_message_unref(message);
        }
        gst_object_unref(errors);
        debugLog("start failed: " + failure);
        gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(pipeline); return false;
    }
    // Live sources report ASYNC/NO_PREROLL; watch the bus briefly for a
    // negotiation or device error before accepting this encoder.
    GstBus* bus = gst_element_get_bus(pipeline);
    QElapsedTimer clock; clock.start();
    bool failed = false;
    while (clock.elapsed() < 1500 && !failed) {
        GstMessage* message = gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_STATE_CHANGED));
        if (!message) continue;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* e = nullptr; gchar* debug = nullptr;
            gst_message_parse_error(message, &e, &debug);
            failure = QStringLiteral("%1: %2").arg(encoder, e ? QString::fromUtf8(e->message) : QString());
            if (e) g_error_free(e);
            g_free(debug);
            failed = true;
        } else if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline)) {
            GstState current, pending;
            gst_message_parse_state_changed(message, nullptr, &current, &pending);
            if (current == GST_STATE_PLAYING && pending == GST_STATE_VOID_PENDING) { gst_message_unref(message); break; }
        }
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    if (failed) { debugLog("start failed: " + failure); gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(pipeline); QFile::remove(options.output); return false; }
    debugLog("started with " + encoder + (audioEncoder.isEmpty() ? QString() : " + " + audioEncoder));
    pipeline_ = pipeline;
    return true;
}

Error RecordingPipeline::start(const Options& options) {
    if (pipeline_) return {"recording_state", "The pipeline is already running", {}};
    if (options.fps < 1 || options.output.isEmpty() || (!options.window && (options.rect.width() < 2 || options.rect.height() < 2))) return {"invalid_request", "Invalid pipeline options", {}};
    frameDurationNs_ = GST_SECOND / options.fps;
    output_ = options.output;
    budget_ = options.budgetBytes;
    budgetSignalled_ = false;
    paused_ = false;
    stopping_ = false;
    QStringList failures;
    QString audioEncoder = options.audio && !options.audioEncoders.isEmpty() ? options.audioEncoders.first() : QString();
    if (options.audio && audioEncoder.isEmpty()) return {"recording_unsupported", "No AAC encoder is available for system audio", {}};
    for (const auto& encoder : options.encoders) {
        QString failure;
        if (launch(options, encoder, audioEncoder, failure)) { encoder_ = encoder; audioEncoder_ = audioEncoder; bus_.start(); return {}; }
        failures.append(failure);
    }
    return {"recording_start_failed", "No H.264 encoder could start the capture", {{"attempts", QJsonArray::fromStringList(failures)}}};
}
Error RecordingPipeline::pause() {
    if (!pipeline_ || stopping_) return {"recording_state", "Nothing is recording", {}};
    paused_ = true;
    return {};
}
Error RecordingPipeline::resume() {
    if (!pipeline_ || stopping_) return {"recording_state", "Nothing is recording", {}};
    paused_ = false;
    return {};
}
qint64 RecordingPipeline::retainedUs() const {
    const gint64 first = video_.firstPassed.load(), last = video_.lastPassed.load();
    return last < 0 || first < 0 ? 0 : (last - first + frameDurationNs_) / 1000;
}
qint64 RecordingPipeline::fileBytes() const { return QFileInfo(output_).size(); }
QJsonObject RecordingPipeline::status() const {
    return {{"durationUs", double(retainedUs())}, {"bytes", double(fileBytes())}, {"paused", paused_.load()}, {"encoder", encoder_}, {"audioEncoder", audioEncoder_}};
}
void RecordingPipeline::stop(std::function<void(Error, qint64)> done) {
    if (!pipeline_) { done({"recording_state", "Nothing is recording", {}}, 0); return; }
    if (stopping_) { done({"recording_busy", "Already stopping", {}}, 0); return; }
    stopping_ = true;
    paused_ = false;
    stopCallback_ = std::move(done);
    gst_element_send_event(pipeline_, gst_event_new_eos());
    // The bus poll completes on EOS; a stalled drain is bounded here.
    QTimer::singleShot(10000, this, [this] { if (stopping_ && stopCallback_) { const auto retained = retainedUs(); tearDown(); auto callback = std::move(stopCallback_); stopCallback_ = {}; callback({"recording_stop_timeout", "The capture did not drain in time", {}}, retained); } });
}
void RecordingPipeline::cancel() {
    tearDown();
    if (!output_.isEmpty()) QFile::remove(output_);
}
void RecordingPipeline::poll() {
    if (!pipeline_) return;
    GstBus* bus = gst_element_get_bus(pipeline_);
    while (GstMessage* message = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
        const auto type = GST_MESSAGE_TYPE(message);
        if (type == GST_MESSAGE_EOS) {
            gst_message_unref(message);
            const auto retained = retainedUs();
            tearDown();
            gst_object_unref(bus);
            if (stopCallback_) { auto callback = std::move(stopCallback_); stopCallback_ = {}; callback({}, retained); }
            return;
        }
        GError* e = nullptr; gchar* debug = nullptr;
        gst_message_parse_error(message, &e, &debug);
        const Error error{"recording_failed", QStringLiteral("Capture failed: %1").arg(e ? QString::fromUtf8(e->message) : QString()), {{"debug", QString::fromUtf8(debug ? debug : "").left(512)}}};
        if (e) g_error_free(e);
        g_free(debug);
        gst_message_unref(message);
        const auto retained = retainedUs();
        tearDown();
        gst_object_unref(bus);
        if (stopCallback_) { auto callback = std::move(stopCallback_); stopCallback_ = {}; callback(error, retained); }
        else emit failed(error);
        return;
    }
    gst_object_unref(bus);
    if (budget_ > 0 && !budgetSignalled_ && !stopping_ && fileBytes() >= budget_) { budgetSignalled_ = true; emit budgetReached(); }
}
void RecordingPipeline::tearDown() {
    bus_.stop();
    if (!pipeline_) return;
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    stopping_ = false;
}
}
