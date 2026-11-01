#include "RecordingPipeline.h"
#include "DebugLog.h"
#include "WaylandAcquisition.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QStorageInfo>
#include <QThread>
#include <algorithm>
#include <gst/base/gstbasesrc.h>
#include <ctime>
#include <cstring>

namespace putgd::record {
namespace {
struct ProbeContext { const PauseTimeline* timeline; void* rebase; bool audio; };
QString shellQuote(const QString& value) { QString s = value; s.replace('\\', "\\\\").replace('"', "\\\""); return '"' + s + '"'; }
int bitrateKbps(const QSize& size, int fps) {
    // About 0.1 bit per pixel per frame, bounded for tiny and huge canvases.
    const double kbps = double(size.width()) * size.height() * fps * 0.1 / 1000.0;
    return int(std::clamp(kbps, 1500.0, 20000.0));
}
// Threads for the CPU stages: enough to keep up with the frame rate, never
// the whole machine, so the desktop being recorded keeps its own cores.
int cpuThreads(int minimum, int maximum) {
    const int cores = std::max(1, QThread::idealThreadCount());
    return std::clamp(cores * 2 / 3, minimum, maximum);
}
// x264's presets at the pixel rate: a 1080p30 region takes veryfast; above
// about 1440p30 or 1080p60 superfast, above 1440p60 ultrafast, so a software
// encode stays real time on the lab's four-core machines at 60 fps too.
QString x264Preset(const QSize& size, int fps) {
    const double pixelRate = double(size.width()) * size.height() * fps;
    if (pixelRate > 2560.0 * 1440 * 45) return QStringLiteral("ultrafast");
    if (pixelRate > 1920.0 * 1080 * 45) return QStringLiteral("superfast");
    return QStringLiteral("veryfast");
}
QString encoderDescription(const QString& name, int kbps, int fps, const QSize& size) {
    const int keyint = std::max(1, fps * 2);
    if (name == "vah264lpenc" || name == "vah264enc") return QStringLiteral("%1 bitrate=%2 key-int-max=%3").arg(name).arg(kbps).arg(keyint);
    // Intel's iHD driver refuses CBR and VBR for this element; constant QP is the one mode every VA driver has.
    if (name == "vaapih264enc") return QStringLiteral("vaapih264enc rate-control=cqp init-qp=24 keyframe-period=%1").arg(keyint);
    // A recording has no latency budget: frame threads (one frame in flight
    // per thread) and a short lookahead encode far more frames per second
    // than zerolatency's two sliced threads did, which stalled the capture at
    // 1080p and above and made clips choppy. EOS flushes the frames in flight.
    if (name == "x264enc") return QStringLiteral("x264enc speed-preset=%1 bitrate=%2 key-int-max=%3 bframes=0 threads=%4 sliced-threads=false rc-lookahead=10 sync-lookahead=0").arg(x264Preset(size, fps)).arg(kbps).arg(keyint).arg(cpuThreads(2, 8));
    if (name == "openh264enc") return QStringLiteral("openh264enc bitrate=%1 gop-size=%2 complexity=low multi-thread=%3").arg(qint64(kbps) * 1000).arg(keyint).arg(cpuThreads(2, 4));
    return name;
}
// Capture and encoding run in their own threads. The capture queue leaks its
// oldest frame when the stages behind it fall behind, so the source (and, for a
// PipeWire stream, the compositor waiting for its buffers back) never blocks on
// the encoder; videorate after the colour conversion restores the constant
// frame rate by repeating converted frames, so a source slower than the clip
// (a compositor that sends only changes, or the adaptive rate) converts only
// the frames it delivered (#984). Encoded
// output is queued before the muxer, which also waits on the audio branch.
QString captureQueue() { return QStringLiteral(" ! queue name=cq leaky=downstream max-size-buffers=3 max-size-bytes=0 max-size-time=0"); }
QString encodeQueue() { return QStringLiteral(" ! queue name=eq max-size-buffers=3 max-size-bytes=0 max-size-time=0"); }
QString convert() { return QStringLiteral(" ! videoconvert n-threads=%1").arg(cpuThreads(2, 4)); }
QString encoderFormat(const QString& name) { return name.startsWith("va") ? QStringLiteral("NV12") : QStringLiteral("I420"); }
QString audioDescription(const QString& name) {
    if (name == "avenc_aac" || name == "fdkaacenc" || name == "voaacenc") return name + " bitrate=128000";
    return name;
}
// The element's running time now, or -1 before it has a clock.
gint64 runningNow(GstElement* element) {
    GstClock* clock = element ? gst_element_get_clock(element) : nullptr;
    if (!clock) return -1;
    const gint64 now = gint64(gst_clock_get_time(clock)) - gint64(gst_element_get_base_time(element));
    gst_object_unref(clock);
    return now;
}
gint64 runningNow(GstPad* pad) { return runningNow(GST_ELEMENT(GST_PAD_PARENT(pad))); }
gint64 monotonicNow() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return gint64(now.tv_sec) * GST_SECOND + now.tv_nsec;
}
// pipewiresrc as a live GstBaseSrc: with a header PTS on every frame, base
// class sync adds one constant to all of them so that the first frame equals
// its arrival time, which bakes that first frame's delay (0-90 ms measured on
// KWin) into the whole video track, and then waits on the clock for frames that
// arrive sooner than the first did. This private subclass only drops the sync
// times; stampFrame stamps each frame itself. Empty when pipewiresrc is missing.
void noSyncTimes(GstBaseSrc*, GstBuffer*, GstClockTime* start, GstClockTime* end) { *start = *end = GST_CLOCK_TIME_NONE; }
void frameClockSourceInit(gpointer klass, gpointer) { GST_BASE_SRC_CLASS(klass)->get_times = noSyncTimes; }
QString frameClockSource() {
    static const QString name = [] {
        GstElementFactory* factory = gst_element_factory_find("pipewiresrc");
        if (!factory) return QString();
        GstPluginFeature* loaded = gst_plugin_feature_load(GST_PLUGIN_FEATURE(factory));
        gst_object_unref(factory);
        if (!loaded) return QString();
        const GType parent = gst_element_factory_get_element_type(GST_ELEMENT_FACTORY(loaded));
        gst_object_unref(loaded);
        GTypeQuery query{};
        g_type_query(parent, &query);
        if (!query.type || !g_type_is_a(parent, GST_TYPE_BASE_SRC)) return QString();
        const GTypeInfo info{guint16(query.class_size), nullptr, nullptr, frameClockSourceInit, nullptr, nullptr, guint16(query.instance_size), 0, nullptr, nullptr};
        const GType type = g_type_register_static(parent, "PutgdPipeWireSrc", &info, GTypeFlags(0));
        if (!type || !gst_element_register(nullptr, "putgdpipewiresrc", GST_RANK_NONE, type)) return QString();
        return QStringLiteral("putgdpipewiresrc");
    }();
    return name;
}
}

void RecordingPipeline::Spread::add(gint64 value) {
    min = count ? std::min(min, value) : value;
    max = count ? std::max(max, value) : value;
    sum += value;
    ++count;
}
QString RecordingPipeline::Spread::text() const {
    if (!count) return QStringLiteral("none");
    return QStringLiteral("%1/%2/%3 ms (n=%4)").arg(double(min) / 1e6, 0, 'f', 1).arg(double(sum) / double(count) / 1e6, 0, 'f', 1).arg(double(max) / 1e6, 0, 'f', 1).arg(count);
}

RecordingPipeline::RecordingPipeline(QObject* parent) : QObject(parent), bus_(this), progress_(this) {
    bus_.setInterval(50);
    connect(&bus_, &QTimer::timeout, this, &RecordingPipeline::poll);
}
RecordingPipeline::~RecordingPipeline() { tearDown(); }

GstPadProbeReturn RecordingPipeline::probe(GstPad* pad, GstPadProbeInfo* info, gpointer user) {
    auto* context = static_cast<ProbeContext*>(user);
    auto* rebase = static_cast<Rebase*>(context->rebase);
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer || !GST_BUFFER_PTS_IS_VALID(buffer)) return GST_PAD_PROBE_OK;
    const gint64 now = runningNow(pad);
    gint64 pts = gint64(GST_BUFFER_PTS(buffer));
    // Only the audio branch stamps with a device clock that can run away (#129).
    if (context->audio) pts = rebase->clock.apply(pts, GST_BUFFER_DURATION_IS_VALID(buffer) ? gint64(GST_BUFFER_DURATION(buffer)) : 0, now);
    // What was captured while paused is dropped and what follows moves back by
    // the paused time, the same for both tracks. Video frames are instants; an
    // audio buffer reaching into a pause is dropped whole, and the export fills
    // the hole that leaves with silence.
    const gint64 duration = context->audio && GST_BUFFER_DURATION_IS_VALID(buffer) ? gint64(GST_BUFFER_DURATION(buffer)) : 0;
    const auto mapped = context->timeline->map(pts, duration);
    if (!mapped) return GST_PAD_PROBE_DROP;
    if (now >= 0) rebase->lateness.add(now - pts);
    const gint64 offset = gint64(GST_BUFFER_PTS(buffer)) - *mapped;
    buffer = gst_buffer_make_writable(buffer);
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    const gint64 shifted = std::max<gint64>(0, *mapped);
    GST_BUFFER_PTS(buffer) = GstClockTime(shifted);
    if (GST_BUFFER_DTS_IS_VALID(buffer)) GST_BUFFER_DTS(buffer) = GstClockTime(std::max<gint64>(0, gint64(GST_BUFFER_DTS(buffer)) - offset));
    // Encoders may carry a constant timestamp offset (the VA encoders add an
    // hour); durations are measured from the first retained buffer.
    if (rebase->firstPassed.load() < 0) rebase->firstPassed.store(shifted);
    rebase->lastPassed.store(shifted);
    rebase->passed.fetch_add(1);
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn RecordingPipeline::stampFrame(GstPad* pad, GstPadProbeInfo* info, gpointer user) {
    auto* clock = static_cast<FrameClock*>(user);
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    const gint64 running = runningNow(pad);
    if (!buffer || running < 0) return GST_PAD_PROBE_OK;
    // The adaptive rate lowers the lock's period; its tolerance stays the
    // configured frame's, so frames at the old pace fall early and are dropped.
    if (clock->gate) {
        clock->gate->delivered.fetch_add(1);
        if (const gint64 period = clock->gate->periodNs.load(); period > 0) clock->lock.periodNs = period;
    }
    const gint64 headerPts = GST_BUFFER_PTS_IS_VALID(buffer) ? gint64(GST_BUFFER_PTS(buffer)) : -1;
    if (headerPts >= 0 && headerPts == clock->headerPts) ++clock->repeatedPts;
    if (headerPts >= 0) clock->headerPts = headerPts;
    if (clock->lastArrival >= 0) clock->interval.add(running - clock->lastArrival);
    clock->lastArrival = running;
    // The same content again adds nothing a repeat at the encoder would not,
    // and dropping it keeps its slot for the next new frame. The comparison is
    // exact (a small change anywhere counts) and usually stops at the first
    // differing bytes; an identical frame costs one pass over the copy.
    bool same = false;
    if (clock->lastKept) {
        GstMapInfo map, kept;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            if (gst_buffer_map(clock->lastKept, &kept, GST_MAP_READ)) {
                same = map.size == kept.size && std::memcmp(map.data, kept.data, map.size) == 0;
                gst_buffer_unmap(clock->lastKept, &kept);
            }
            gst_buffer_unmap(buffer, &map);
        }
        // A static screen's keepalive resends pass once per period so the timeline advances.
        if (same && (clock->lastPassed < 0 || running - clock->lastPassed < clock->lock.periodNs)) { ++clock->sameContents; return GST_PAD_PROBE_DROP; }
    }
    if (!same) ++clock->distinctContents;
    auto frame = pipewireFrameTime(headerPts, monotonicNow(), running, clock->previous);
    if (frame.ageNs < 0) ++clock->arrival;
    else clock->age.add(frame.ageNs);
    // Arrival is the anchor: KWin's header time is the output's previous
    // presentation, ~40 ms before the frame arrives, and recordings stamped with
    // it played the sound late (+35..+48 ms) where arrival times matched X11.
    // The cadence lock keeps that anchor and puts the stamps on the grid.
    if (!clock->compositorTimes) {
        frame = pipewireFrameTime(-1, 0, running, clock->previous);
        if (clock->locked) {
            qint64 locked = lockedFrameTime(running, clock->previous, clock->lock);
            // A change right after a passed repeat takes the next slot rather
            // than waiting for the next keepalive (or missing a clip stopped first).
            if (locked < 0 && clock->lastWasRepeat && !same) { locked = clock->previous + clock->lock.periodNs; ++clock->afterRepeat; }
            if (locked < 0) { ++clock->early; return GST_PAD_PROBE_DROP; }
            if (clock->previous >= 0 && locked != clock->previous + clock->lock.periodNs) ++clock->resyncs;
            frame.running = locked;
        }
    }
    clock->previous = frame.running;
    clock->lastPassed = running;
    if (clock->gate) clock->gate->kept.fetch_add(1);
    clock->lastWasRepeat = same;
    // The compositor owns a handful of stream buffers (KWin offers 2-4) and
    // skips a frame whenever none is free. Copy the frame out here, on the
    // source thread, so the stream buffer goes back at once instead of waiting
    // in the capture queue and the converter; the copy is a few hundred
    // microseconds of memcpy against a frame the compositor would drop.
    GstBuffer* copy = gst_buffer_copy_deep(buffer);
    if (copy) { gst_buffer_unref(buffer); buffer = copy; }
    else {
        // make_writable consumes the reference and returns null when it cannot copy.
        buffer = gst_buffer_make_writable(buffer);
        if (!buffer) { GST_PAD_PROBE_INFO_DATA(info) = nullptr; return GST_PAD_PROBE_DROP; }
    }
    gst_buffer_replace(&clock->lastKept, buffer);
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    GST_BUFFER_PTS(buffer) = GST_BUFFER_DTS(buffer) = GstClockTime(frame.running);
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn RecordingPipeline::gateFrame(GstPad*, GstPadProbeInfo* info, gpointer user) {
    auto* gate = static_cast<RateGate*>(user);
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer || !GST_BUFFER_PTS_IS_VALID(buffer)) return GST_PAD_PROBE_OK;
    gate->delivered.fetch_add(1);
    // At a lowered rate a frame is kept once a period has passed since the
    // last kept one, less half a configured frame for the grab's jitter.
    const gint64 pts = gint64(GST_BUFFER_PTS(buffer));
    if (gate->lastKept >= 0 && pts >= gate->lastKept && pts - gate->lastKept < gate->periodNs.load() - gate->toleranceNs) return GST_PAD_PROBE_DROP;
    gate->lastKept = pts;
    gate->kept.fetch_add(1);
    return GST_PAD_PROBE_OK;
}

bool RecordingPipeline::launch(const Options& options, const QString& encoder, const QString& audioEncoder, bool unthrottled, QString& failure) {
    const QSize encoded = options.crop.isValid() ? options.crop.size() : options.rect.size();
    const int kbps = bitrateKbps(encoded, options.fps);
    QString description;
    // Lab A/B runs: PUTGD_RECORD_PIPEWIRE_TIMESTAMPS=legacy restores the
    // first-frame-anchored timestamps of releases before #86, =compositor maps
    // each frame's header time instead of stamping its arrival.
    const QString frameSource = options.pipewireNode && qEnvironmentVariable("PUTGD_RECORD_PIPEWIRE_TIMESTAMPS") != "legacy" ? frameClockSource() : QString();
    if (options.pipewireNode) {
        // The stream arrives at the compositor's pace; videorate after the
        // conversion holds the configured frame rate, and a region is cropped
        // out of the shared monitor (crop values are stream-relative) before
        // the colour conversion, so only the recorded pixels are converted.
        // Lab A/B: PUTGD_RECORD_PIPEWIRE_UNTHROTTLED_CAPS replaces the request
        // (for example a rate above the output's to exercise the fallback).
        const QString labCaps = qEnvironmentVariable("PUTGD_RECORD_PIPEWIRE_UNTHROTTLED_CAPS");
        description = pipewireSourceDescription(frameSource.isEmpty() ? QStringLiteral("pipewiresrc") : frameSource, options.pipewireNode, options.pipewireFd, frameSource.isEmpty())
            + (unthrottled ? (labCaps.isEmpty() ? pipewireUnthrottledCaps() : QStringLiteral(" ! ") + labCaps) : QString()) + captureQueue();
        if (options.crop.isValid()) description += QStringLiteral(" ! videocrop left=%1 top=%2 right=%3 bottom=%4").arg(options.crop.x()).arg(options.crop.y()).arg(std::max(0, options.rect.width() - options.crop.x() - options.crop.width())).arg(std::max(0, options.rect.height() - options.crop.y() - options.crop.height()));
        description += convert();
    } else {
        // ximagesrc skips frame numbers when a grab runs late, leaving gaps in
        // the timestamps. The va encoders (vah264lpenc, vah264enc) stamp their
        // output contiguously regardless, which shortened a recording by every
        // skipped frame (a 10 s capture played in 7 s on a three-monitor X11
        // desktop). videorate fills the gaps with repeats, as on Wayland.
        if (options.window) description = QStringLiteral("ximagesrc name=src xid=%1 use-damage=false show-pointer=true ! video/x-raw,framerate=%2/1").arg(options.window).arg(options.fps) + captureQueue() + convert();
        else description = QStringLiteral("ximagesrc name=src use-damage=false show-pointer=true startx=%1 starty=%2 endx=%3 endy=%4 ! video/x-raw,framerate=%5/1").arg(options.rect.x()).arg(options.rect.y()).arg(options.rect.x() + options.rect.width() - 1).arg(options.rect.y() + options.rect.height() - 1).arg(options.fps) + captureQueue() + convert();
    }
    // The pause probes sit on the encoders' sink pads (raw frames and samples):
    // dropping encoded buffers would leave P-frames whose references are gone,
    // which strict decoders (OpenH264 on Fedora) refuse while FFmpeg's conceals.
    // videorate after the conversion holds the configured rate for both sources.
    description += QStringLiteral(" ! videorate ! video/x-raw,format=%1,framerate=%2/1").arg(encoderFormat(encoder)).arg(options.fps) + encodeQueue() + QStringLiteral(" ! %1 name=venc ! h264parse ! queue name=vq max-size-buffers=0 max-size-bytes=0 max-size-time=2000000000 ! matroskamux name=mux ! filesink name=sink location=%2")
        .arg(encoderDescription(encoder, kbps, options.fps, encoded), shellQuote(options.output));
    if (options.audio && !audioEncoder.isEmpty())
        description += QStringLiteral(" pulsesrc name=asrc device=%1 ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! %2 name=aenc ! aacparse ! queue name=aq max-size-buffers=0 max-size-bytes=0 max-size-time=2000000000 ! mux.")
            .arg(shellQuote(options.audioDevice), audioDescription(audioEncoder));
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (!pipeline || error) { failure = error ? QString::fromUtf8(error->message) : QStringLiteral("parse failed"); if (error) g_error_free(error); if (pipeline) gst_object_unref(pipeline); return false; }
    video_.firstPassed = -1; video_.lastPassed = -1; video_.passed = 0; video_.lateness = {}; video_.clock.reset();
    audio_.firstPassed = -1; audio_.lastPassed = -1; audio_.passed = 0; audio_.lateness = {}; audio_.clock.reset();
    timeline_.reset();
    timeline_.setFrameRate(options.fps);
    gst_buffer_replace(&frameClock_.lastKept, nullptr);
    frameClock_ = {};
    frameClock_.compositorTimes = qEnvironmentVariable("PUTGD_RECORD_PIPEWIRE_TIMESTAMPS") == "compositor";
    // =arrival: raw arrival times (the default before #6), for lab A/B runs.
    frameClock_.locked = qEnvironmentVariable("PUTGD_RECORD_PIPEWIRE_TIMESTAMPS") != "arrival";
    frameClock_.lock = frameLock(options.fps);
    frameClock_.gate = &gate_;
    gate_.periodNs = GST_SECOND / options.fps; gate_.delivered = 0; gate_.kept = 0; gate_.lastKept = -1; gate_.toleranceNs = GST_SECOND / options.fps / 2;
    // Lab A/B: PUTGD_RECORD_ADAPTIVE_RATE=off keeps the configured rate whatever the load.
    rate_ = AdaptiveRate(options.fps, options.adaptive && qEnvironmentVariable("PUTGD_RECORD_ADAPTIVE_RATE") != "off", !options.pipewireNode);
    rateClock_.invalidate();
    rateChange_ = {};
    clockName_.clear();
    if (!frameSource.isEmpty()) {
        // The monotonic system clock is the compositors' frame clock, so frame
        // ages read directly; pulsesrc slaves to it rather than providing the
        // clock (pipewiresrc would otherwise offer its own stream clock).
        GstClock* system = gst_system_clock_obtain();
        gst_pipeline_use_clock(GST_PIPELINE(pipeline), system);
        gst_object_unref(system);
        GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), "src");
        GstPad* pad = source ? gst_element_get_static_pad(source, "src") : nullptr;
        if (pad) gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &RecordingPipeline::stampFrame, &frameClock_, nullptr);
        if (pad) gst_object_unref(pad);
        if (source) gst_object_unref(source);
    } else if (GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), "src")) {
        // ximagesrc (and the legacy PipeWire lab mode): the rate gate drops frames before the capture queue.
        if (GstPad* pad = gst_element_get_static_pad(source, "src")) { gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &RecordingPipeline::gateFrame, &gate_, nullptr); gst_object_unref(pad); }
        gst_object_unref(source);
    }
    debugLog("pipeline: " + description);
    auto attach = [&](const char* element, Rebase& rebase) {
        GstElement* queue = gst_bin_get_by_name(GST_BIN(pipeline), element);
        if (!queue) return false;
        GstPad* pad = gst_element_get_static_pad(queue, "sink");
        auto* context = new ProbeContext{&timeline_, &rebase, &rebase == &audio_};
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &RecordingPipeline::probe, context, [](gpointer data) { delete static_cast<ProbeContext*>(data); });
        gst_object_unref(pad);
        gst_object_unref(queue);
        return true;
    };
    if (!attach("venc", video_) || (options.audio && !audioEncoder.isEmpty() && !attach("aenc", audio_))) { failure = "probe attach failed"; gst_object_unref(pipeline); return false; }
    // Frames the capture queue had to leak: the stages behind it were slower
    // than the frame rate, and the clip repeats frames there (lab statistic).
    dropped_.store(0);
    if (GstElement* capture = gst_bin_get_by_name(GST_BIN(pipeline), "cq")) {
        g_signal_connect(capture, "overrun", G_CALLBACK(+[](GstElement*, gpointer user) { static_cast<std::atomic<gint64>*>(user)->fetch_add(1); }), &dropped_);
        gst_object_unref(capture);
    }
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
    // negotiation or device error before accepting this encoder. A PipeWire
    // stream negotiates on its streaming thread after the pipeline is PLAYING,
    // so an unthrottled request keeps watching for its refusal a while longer
    // (relaunchThrottled covers one that arrives later still).
    GstBus* bus = gst_element_get_bus(pipeline);
    QElapsedTimer clock; clock.start();
    bool failed = false;
    qint64 playingAt = -1;
    const qint64 settleMs = options.pipewireNode && unthrottled ? 500 : 0;
    while (clock.elapsed() < 1500 && !failed) {
        if (playingAt >= 0 && clock.elapsed() - playingAt >= settleMs) break;
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
            if (current == GST_STATE_PLAYING && pending == GST_STATE_VOID_PENDING && playingAt < 0) { playingAt = clock.elapsed(); if (settleMs == 0) { gst_message_unref(message); break; } }
        }
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    if (failed) { debugLog("start failed: " + failure); gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(pipeline); QFile::remove(options.output); return false; }
    if (GstClock* clock = gst_pipeline_get_clock(GST_PIPELINE(pipeline))) { clockName_ = QString::fromUtf8(G_OBJECT_TYPE_NAME(clock)); gst_object_unref(clock); }
    debugLog("started with " + encoder + (audioEncoder.isEmpty() ? QString() : " + " + audioEncoder) + " on " + clockName_ + (options.pipewireNode ? (unthrottled ? QStringLiteral(", max_framerate unthrottled") : QStringLiteral(", max_framerate throttled")) : QString()));
    pipeline_ = pipeline;
    options_ = options;
    unthrottled_ = options.pipewireNode && unthrottled;
    return true;
}

Error RecordingPipeline::start(const Options& options) {
    if (pipeline_) return {"recording_state", "The pipeline is already running", {}};
    if (options.fps < 1 || options.output.isEmpty() || (!options.window && (options.rect.width() < 2 || options.rect.height() < 2))) return {"invalid_request", "Invalid pipeline options", {}};
    frameDurationNs_ = GST_SECOND / options.fps;
    output_ = options.output;
    budget_ = options.budgetBytes;
    budgetSignalled_ = false;
    diskStop_ = {};
    storageClock_.invalidate();
    stopping_ = false;
    QStringList failures;
    QString audioEncoder = options.audio && !options.audioEncoders.isEmpty() ? options.audioEncoders.first() : QString();
    if (options.audio && audioEncoder.isEmpty()) return {"recording_unsupported", "No AAC encoder is available for system audio", {}};
    // A PipeWire stream first asks for max_framerate 0/1; a compositor that
    // refuses it (Mutter's range starts at 1/1) gets the plain request. Owned
    // here: a conditional between two braced lists leaves the loop reading arrays
    // that ended with the condition, and GCC never stores their values (#1115).
    const QList<bool> requests = options.pipewireNode ? QList<bool>{true, false} : QList<bool>{false};
    for (const auto& encoder : options.encoders) {
        for (const bool unthrottled : requests) {
            QString failure;
            if (launch(options, encoder, audioEncoder, unthrottled, failure)) { encoder_ = encoder; audioEncoder_ = audioEncoder; bus_.start(); return {}; }
            failures.append(failure);
        }
    }
    return {"recording_start_failed", "No H.264 encoder could start the capture", {{"attempts", QJsonArray::fromStringList(failures)}}};
}
gint64 RecordingPipeline::runningTime() const { return pipeline_ ? runningNow(pipeline_) : -1; }
Error RecordingPipeline::pause() {
    if (!pipeline_ || stopping_) return {"recording_state", "Nothing is recording", {}};
    timeline_.pause(std::max<gint64>(0, runningTime()));
    return {};
}
Error RecordingPipeline::resume() {
    if (!pipeline_ || stopping_) return {"recording_state", "Nothing is recording", {}};
    timeline_.resume(std::max<gint64>(0, runningTime()));
    return {};
}
qint64 RecordingPipeline::retainedUs() const {
    const gint64 first = video_.firstPassed.load(), last = video_.lastPassed.load();
    return last < 0 || first < 0 ? 0 : (last - first + frameDurationNs_) / 1000;
}
qint64 RecordingPipeline::fileBytes() const { return QFileInfo(output_).size(); }
RecordingStorage RecordingPipeline::storage() const {
    if (storageProbe_) return storageProbe_();
    QStorageInfo volume(QFileInfo(output_).absolutePath());
    volume.refresh();
    return {volume.isValid() && volume.isReady() ? volume.bytesAvailable() : -1,
            volume.isValid() ? volume.bytesTotal() : -1, fileBytes()};
}
qint64 RecordingPipeline::framesEncoded() const { return video_.passed.load(); }
qint64 RecordingPipeline::framesDropped() const { return dropped_.load(); }
QJsonObject RecordingPipeline::status() const {
    QJsonObject status{{"durationUs", double(retainedUs())}, {"bytes", double(fileBytes())}, {"paused", timeline_.paused()}, {"encoder", encoder_}, {"audioEncoder", audioEncoder_},
            {"framesEncoded", double(framesEncoded())}, {"framesDropped", double(framesDropped())}};
    // What the source delivered (a compositor sends what changed, at most its
    // own pace, #983), the frames kept for the clip before videorate's repeats,
    // the rate kept now and, after the adaptive rate lowered it, when and why.
    status["framesDelivered"] = double(gate_.delivered.load());
    status["framesCaptured"] = double(gate_.kept.load());
    status["frameRate"] = rate_.current();
    if (!rateChange_.isEmpty()) status["frameRateReduced"] = rateChange_;
    return status;
}
void RecordingPipeline::stop(std::function<void(Error, qint64)> done) {
    if (!pipeline_) { done({"recording_state", "Nothing is recording", {}}, 0); return; }
    if (stopping_) { done({"recording_busy", "Already stopping", {}}, 0); return; }
    stopping_ = true;
    stopCallback_ = std::move(done);
    // Stop ends the clip now: nothing captured after it is kept, however long the
    // drain takes (a recording stopped while paused already keeps nothing after the pause).
    timeline_.pause(std::max<gint64>(0, runningTime()));
    // EOS enters each branch at the element after its source, never through the
    // sources (#129): a source takes its streaming lock for EOS, and pulsesrc holds
    // that lock while its thread waits, for audio or for room in a full audio queue,
    // so a sink monitor that stopped delivering or ran its timestamps away (a
    // suspended null sink in a VM) blocked Stop on the main thread for good. Each
    // branch gets its own thread: a branch whose thread is stuck downstream gets its
    // EOS once the other branch's EOS lets the muxer drain it, and no wait reaches
    // the main thread.
    QList<GstPad*> entries;
    for (const char* name : {"asrc", "src"}) {
        GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline_), name);
        GstPad* pad = source ? gst_element_get_static_pad(source, "src") : nullptr;
        if (GstPad* peer = pad ? gst_pad_get_peer(pad) : nullptr) entries.append(peer);
        if (pad) gst_object_unref(pad);
        if (source) gst_object_unref(source);
    }
    for (GstPad* pad : entries) drains_.emplace_back([pad] { gst_pad_send_event(pad, gst_event_new_eos()); gst_object_unref(pad); });
    // The bus poll completes on EOS. A drain that stalls anyway is bounded: the
    // capture stops and keeps what reached the file, whose Matroska index is
    // then missing but whose clusters decode.
    const quint64 generation = ++stops_;
    QTimer::singleShot(stopDrainMs, this, [this, generation] {
        if (generation != stops_ || !stopping_ || !stopCallback_) return;
        const auto retained = retainedUs();
        tearDown();
        const bool kept = retained > 0 && fileBytes() > 0;
        debugLog(kept ? QStringLiteral("stop: drain timed out; keeping the clip as written") : QStringLiteral("stop: drain timed out with nothing written"));
        auto callback = std::move(stopCallback_);
        stopCallback_ = {};
        callback(kept ? Error{} : Error{"recording_stop_timeout", "The capture did not drain in time", {}}, retained);
    });
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
        // A compositor that refused the unthrottled request after PLAYING: start
        // again with the plain request while nothing has been encoded yet.
        if (unthrottled_ && !stopping_ && framesEncoded() == 0) {
            debugLog(QStringLiteral("unthrottled stream failed before any frame (%1); relaunching throttled").arg(e ? QString::fromUtf8(e->message) : QString()));
            if (e) g_error_free(e);
            g_free(debug);
            gst_message_unref(message);
            gst_object_unref(bus);
            if (relaunchThrottled()) return;
            emit failed({"recording_failed", "Capture failed: the compositor refused the stream twice", {}});
            return;
        }
        Error error{"recording_failed", QStringLiteral("Capture failed: %1").arg(e ? QString::fromUtf8(e->message) : QString()), {{"debug", QString::fromUtf8(debug ? debug : "").left(512)}}};
        const auto disk = storage();
        // A previous low-space stop is context, not evidence that a later
        // encoder/source/drain error was caused by storage. Even a generic WRITE
        // failure plus low free space does not establish ENOSPC. Keep its diagnosis.
        if (e && e->domain == GST_RESOURCE_ERROR && e->code == GST_RESOURCE_ERROR_NO_SPACE_LEFT) {
            const auto cause = error.toJson();
            error = {"recording_disk_full", "The disk filled before the recording could be finalized. No reviewable clip was retained and nothing was uploaded. Free up space and try again.", disk.toJson()};
            error.details["reason"] = "disk_space";
            error.details["finalized"] = false;
            error.details["cause"] = cause;
        }
        if (!diskStop_.isEmpty()) {
            error.details["stopDetails"] = diskStop_;
            error.details["finalized"] = false;
        }
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
    if (budgetSignalled_ || stopping_) return;
    observeRate();
    const bool byteLimit = budget_ > 0 && fileBytes() >= budget_;
    // Refresh at a byte stop too: simultaneous pressure must name the disk,
    // rather than choosing a stale sample just inside the periodic interval.
    if (byteLimit || !storageClock_.isValid() || storageClock_.elapsed() >= RecordingStorage::pollMs) {
        storageClock_.start();
        const auto disk = storage();
        if (disk.low()) {
            diskStop_ = disk.toJson();
            diskStop_["reason"] = "disk_space";
            budgetSignalled_ = true;
            emit budgetReached(diskStop_);
            return;
        }
    }
    if (byteLimit) { budgetSignalled_ = true; emit budgetReached({}); }
}
void RecordingPipeline::observeRate() {
    if (!rate_.enabled()) return;
    if (timeline_.paused()) { rateClock_.invalidate(); return; }
    auto mark = [this] { rateMarks_ = {gate_.delivered.load(), dropped_.load(), cpuProbe_ ? cpuProbe_() : readCpu()}; rateClock_.start(); };
    if (!rateClock_.isValid()) { mark(); return; }
    if (rateClock_.elapsed() < 1000) return;
    const gint64 elapsed = rateClock_.nsecsElapsed();
    const auto before = rateMarks_;
    mark();
    AdaptiveRate::Window window;
    window.durationNs = elapsed;
    window.sourceFrames = rateMarks_.delivered - before.delivered;
    window.leaked = rateMarks_.leaked - before.leaked;
    cpuShares(before.cpu, rateMarks_.cpu, elapsed, window.systemBusy, window.recorderCores);
    const int from = rate_.current();
    const QString against = rate_.judge(window);
    const bool lowered = rate_.observe(window);
    debugLog(QStringLiteral("rate window: %1 fps, %2 delivered and %3 leaked in %4 ms, machine %5% busy, recorder %6 cores%7")
                 .arg(from).arg(window.sourceFrames).arg(window.leaked).arg(elapsed / 1000000).arg(window.systemBusy * 100, 0, 'f', 0).arg(window.recorderCores, 0, 'f', 2)
                 .arg(against.isEmpty() ? QString() : ", against: " + against));
    if (!lowered) return;
    gate_.periodNs.store(GST_SECOND / rate_.current());
    rateChange_ = {{"from", from}, {"to", rate_.current()}, {"reason", rate_.reason()}, {"atUs", double(retainedUs())}};
    debugLog(QStringLiteral("rate lowered from %1 to %2 fps (%3)").arg(from).arg(rate_.current()).arg(rate_.reason()));
}
bool RecordingPipeline::relaunchThrottled() {
    tearDown();
    const auto options = options_;
    const QString audioEncoder = options.audio && !options.audioEncoders.isEmpty() ? options.audioEncoders.first() : QString();
    for (const auto& encoder : options.encoders) {
        QString failure;
        if (launch(options, encoder, audioEncoder, false, failure)) { encoder_ = encoder; audioEncoder_ = audioEncoder; bus_.start(); return true; }
        debugLog("relaunch failed: " + failure);
    }
    return false;
}
void RecordingPipeline::tearDown() {
    bus_.stop();
    if (!pipeline_) return;
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    // Flushing in NULL ends any EOS send still waiting on a branch.
    for (auto& drain : drains_) if (drain.joinable()) drain.join();
    drains_.clear();
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    gst_buffer_replace(&frameClock_.lastKept, nullptr);
    // Lateness: running time at the encoder minus the buffer's time; age: how
    // old a compositor frame time was when the frame reached the source pad.
    debugLog(QStringLiteral("timestamps: clock %1, video lateness %2, audio lateness %3, frame age %4, arrival-stamped frames %5, audio re-anchored %6 times")
                 .arg(clockName_, video_.lateness.text(), audio_.lateness.text(), frameClock_.age.text()).arg(frameClock_.arrival).arg(audio_.clock.corrections()));
    debugLog(QStringLiteral("frames: %1 reached the video encoder, %2 leaked at the capture queue; source arrivals %3 (interval %4), %5 with a repeated compositor time, contents %6 new / %7 same dropped, cadence lock %8 early dropped / %9 resyncs / %10 changes slotted after a repeat")
                 .arg(video_.passed.load()).arg(dropped_.load()).arg(frameClock_.interval.count + (frameClock_.lastArrival >= 0 ? 1 : 0)).arg(frameClock_.interval.text()).arg(frameClock_.repeatedPts).arg(frameClock_.distinctContents).arg(frameClock_.sameContents).arg(frameClock_.early).arg(frameClock_.resyncs).arg(frameClock_.afterRepeat));
    stopping_ = false;
}
}
