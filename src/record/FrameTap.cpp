#include "FrameTap.h"
#include "DebugLog.h"
#include "ImageCopySource.h"
#include "SourceDescription.h"
#include <chrono>
#include <cstring>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <thread>

namespace putgd::record {
int64_t FrameTap::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void SteadyClock::sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

FrameTap::~FrameTap() { stop(); }

QString FrameTap::describe(const Source& source) {
    QString description;
    if (source.imageCopy) description = imageCopySourceDescription(source.imageCopy->caps());
    else if (source.pipewireNode) description = pipewireSourceDescription(QStringLiteral("pipewiresrc"), source.pipewireNode, source.pipewireFd, true);
    else description = x11SourceDescription(source.x11Rect, 0, source.fps, false);
    // The newest frames only: a slow loop never makes the source wait.
    description += QStringLiteral(" ! queue name=cq leaky=downstream max-size-buffers=2 max-size-bytes=0 max-size-time=0");
    if (source.crop.isValid() && !source.streamSize.isEmpty()) description += cropDescription(source.streamSize, source.crop);
    description += QStringLiteral(" ! videoconvert ! video/x-raw,format=BGRx ! appsink name=tap max-buffers=2 drop=true sync=false emit-signals=true enable-last-sample=false");
    return description;
}

Error FrameTap::start(const Source& source) {
    if (pipeline_) return {"recording_state", "The frame tap is already running", {}};
    const QString description = describe(source);
    debugLog("scroll tap: " + description);
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (!pipeline || error) {
        const QString message = error ? QString::fromUtf8(error->message) : QStringLiteral("parse failed");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        return {"scroll_unavailable", "The frame source could not be built: " + message, {{"reason", "frame_source_unavailable"}}};
    }
    sink_ = gst_bin_get_by_name(GST_BIN(pipeline), "tap");
    pipeline_ = pipeline;
    source_ = source;
    lost_.store(false);
    frames_.store(0);
    width_.store(0);
    height_.store(0);
    {
        std::lock_guard lock(mailbox_);
        arrivedCount_ = givenCount_ = 0;
        watching_ = false;
        askedFps_ = source.x11Rect.isValid() ? source.fps : 0;
        watchedFirst_ = watchedLast_ = -1;
        watchedFrames_ = 0;
        fallback_.clear();
    }
    if (sink_) g_signal_connect(sink_, "new-sample", G_CALLBACK(&FrameTap::arrived), this);
    { std::lock_guard lock(mutex_); reason_.clear(); }
    if (source.imageCopy) {
        // The capture thread pushes into appsrc; it is stopped before the pipeline.
        imageCopy_ = source.imageCopy;
        if (GstElement* appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src")) {
            imageCopy_->attach(appsrc, [this](Error e) { { std::lock_guard lock(mutex_); if (reason_.isEmpty()) reason_ = e.message; } lost_.store(true); });
            gst_object_unref(appsrc);
        }
    }
    if (!sink_ || gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        noteBus();
        const QString reason = lostReason();
        stop();
        return {"scroll_unavailable", "The frame source could not be started" + (reason.isEmpty() ? QString() : ": " + reason), {{"reason", "frame_source_unavailable"}}};
    }
    return {};
}

std::optional<RunFrame> FrameTap::freshFrame(int timeoutMs) {
    if (!pipeline_) return std::nullopt;
    const int64_t began = nowMs();
    if (source_.imageCopy) {
        // A new capture session: its first frame is copied whether or not the
        // source changed. Frames of the old session are passed over.
        const quint64 session = source_.imageCopy->refresh();
        std::unique_lock lock(mailbox_);
        if (!fresh_.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)), [&] { return arrivedCount_ != givenCount_ && latestSession_ >= session && latestSession_ != GST_BUFFER_OFFSET_NONE; })) return std::nullopt;
        lock.unlock();
        return nextFrame(0);
    }
    if (source_.pipewireNode) {
        const Source source = source_;
        const qint64 given = frames_.load();
        stop();
        if (start(source)) {
            // The stream cannot be linked again: no frame will come.
            { std::lock_guard lock(mutex_); if (reason_.isEmpty()) reason_ = QStringLiteral("the stream could not be linked again"); }
            lost_.store(true);
            return std::nullopt;
        }
        frames_.store(given);
        return nextFrame(timeoutMs);
    }
    // ximagesrc grabs the screen for every frame. What is in the mailbox, and
    // a frame already on its way, were grabbed before now: the one wanted
    // arrives a whole frame interval after the call began.
    { std::lock_guard lock(mailbox_); givenCount_ = arrivedCount_; pace_.begin(nowMs()); }
    const int interval = 1000 / std::max(1, source_.fps) + 1;
    for (;;) {
        const int left = timeoutMs - int(nowMs() - began);
        if (left <= 0) return std::nullopt;
        auto frame = nextFrame(left);
        if (!frame) return std::nullopt;
        if (frame->timestampMs - began >= interval) return frame;
    }
}

void FrameTap::stop() {
    if (!pipeline_) return;
    if (imageCopy_) { imageCopy_->stop(); imageCopy_ = nullptr; }
    // The sink first. It keeps the last buffer until it reaches NULL, which the
    // pipeline does sink first but after the source has stopped. Every ximagesrc
    // buffer holds a reference to its source, and one handed back to a stopped
    // ximagesrc goes into its emptied pool, so source, pads and buffer then keep
    // each other alive for good: 2800 bytes and an XShm image per start, found by
    // LeakSanitizer. Stopped first, the sink returns it to the running source,
    // whose stop frees it. No new-sample call reaches this object after this.
    if (sink_) gst_element_set_state(sink_, GST_STATE_NULL);
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (sink_) { gst_object_unref(sink_); sink_ = nullptr; }
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    debugLog(QStringLiteral("scroll tap: stopped after %1 frames").arg(frames_.load()));
}

QString FrameTap::lostReason() const {
    std::lock_guard lock(mutex_);
    return reason_;
}

// An error or the end of the stream on the bus: no more frames will come.
void FrameTap::noteBus() {
    if (!pipeline_) return;
    GstBus* bus = gst_element_get_bus(pipeline_);
    while (GstMessage* message = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
        QString text = QStringLiteral("the stream ended");
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            text = QStringLiteral("%1: %2").arg(QString::fromUtf8(GST_OBJECT_NAME(message->src)), error ? QString::fromUtf8(error->message) : QString());
            if (error) g_error_free(error);
            g_free(debug);
        }
        gst_message_unref(message);
        debugLog("scroll tap: " + text);
        { std::lock_guard lock(mutex_); if (reason_.isEmpty()) reason_ = text; }
        lost_.store(true);
    }
    gst_object_unref(bus);
}

GstFlowReturn FrameTap::arrived(GstElement* sink, gpointer self) {
    if (GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink))) {
        static_cast<FrameTap*>(self)->take(sample);
        gst_sample_unref(sample);
    }
    return GST_FLOW_OK;
}

// Copies the frame into the mailbox; the compositor's buffer is free again when this returns.
void FrameTap::take(GstSample* sample) {
    GstVideoInfo info;
    GstVideoFrame frame;
    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRx || !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) return;
    const int width = GST_VIDEO_FRAME_WIDTH(&frame), height = GST_VIDEO_FRAME_HEIGHT(&frame);
    const auto* data = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    if (width > 0 && height > 0 && data && stride >= width * 4) {
        const size_t row = size_t(width) * 4;
        std::lock_guard lock(mailbox_);
        latest_.resize(row * size_t(height));
        for (int y = 0; y < height; ++y) std::memcpy(latest_.data() + row * size_t(y), data + ptrdiff_t(stride) * y, row);
        latestWidth_ = width;
        latestHeight_ = height;
        latestAt_ = nowMs();
        latestSession_ = GST_BUFFER_OFFSET(buffer);
        ++arrivedCount_;
        if (watching_) {
            pace_.arrived();
            if (watchedFirst_ < 0) watchedFirst_ = latestAt_;
            watchedLast_ = latestAt_;
            ++watchedFrames_;
        }
        if (debugLogEnabled()) {
            // Lab timeline: every buffer the stream delivered, with a sum over
            // every sixteenth row (never the pixels), so a buffer that shows
            // the old picture again can be told from one that shows a new one.
            quint64 sum = 0;
            for (int y = 0; y < height; y += 16) { const uint8_t* line = latest_.data() + row * size_t(y); for (size_t x = 0; x < row; x += 4) sum = sum * 31 + line[x] + (line[x + 1] << 8) + (line[x + 2] << 16); }
            debugLog(QStringLiteral("scroll tap buffer: t=%1 sum=%2").arg(latestAt_ % 1000000).arg(sum % 100000));
        }
        fresh_.notify_all();
    }
    gst_video_frame_unmap(&frame);
}

// The newest frame the loop has not had yet, or nothing after the timeout.
std::optional<RunFrame> FrameTap::nextFrame(int timeoutMs) {
    if (!pipeline_ || !sink_) return std::nullopt;
    std::unique_lock lock(mailbox_);
    if (!fresh_.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)), [this] { return arrivedCount_ != givenCount_; })) {
        lock.unlock();
        noteBus();
        return std::nullopt;
    }
    const quint64 missed = arrivedCount_ - givenCount_ - 1;
    givenCount_ = arrivedCount_;
    const FramePace::Verdict pace = watching_ ? pace_.taken(nowMs(), missed) : FramePace::Verdict::Keep;
    // The loop's copy: the mailbox takes the next frame while the loop works on this one.
    std::swap(frame_, latest_);
    const int width = latestWidth_, height = latestHeight_;
    const int64_t at = latestAt_;
    lock.unlock();
    width_.store(width);
    height_.store(height);
    frames_.fetch_add(1);
    // frame_ is the loop's until its next call: a restart writes only the mailbox.
    if (pace != FramePace::Verdict::Keep && source_.fallbackFps > 0 && source_.fps > source_.fallbackFps) fallBack(pace);
    return RunFrame{scroll::FrameView{frame_.data(), width, height, std::ptrdiff_t(width) * 4}, at};
}

void FrameTap::watchPace(bool on) {
    std::lock_guard lock(mailbox_);
    watching_ = on && source_.x11Rect.isValid() && !source_.imageCopy && !source_.pipewireNode;
    if (!watching_) return;
    pace_.restart(source_.fps, nowMs());
    // Frames replaced before the watch began are not the loop's to answer for.
    if (arrivedCount_ - givenCount_ > 1) givenCount_ = arrivedCount_ - 1;
}

FrameTap::Rate FrameTap::rate() const {
    std::lock_guard lock(mailbox_);
    Rate rate;
    rate.asked = askedFps_;
    rate.now = askedFps_ ? source_.fps : 0;
    if (watchedFrames_ > 1 && watchedLast_ > watchedFirst_) rate.delivered = double(watchedFrames_ - 1) * 1000.0 / double(watchedLast_ - watchedFirst_);
    rate.fallback = fallback_;
    return rate;
}

// ximagesrc grabs and copies every frame whether or not the loop takes it:
// at the lower rate for the rest of the capture, never back up (#2064).
void FrameTap::fallBack(FramePace::Verdict why) {
    QString reason;
    int64_t first = -1, last = -1;
    quint64 watched = 0;
    int asked = 0;
    {
        std::lock_guard lock(mailbox_);
        reason = why == FramePace::Verdict::SourceSlow
            ? QStringLiteral("the source sent %1 frames a second").arg(pace_.lastSent(), 0, 'f', 1)
            : QStringLiteral("the loop missed %1% of the frames").arg(100 * pace_.lastMissedShare(), 0, 'f', 0);
        first = watchedFirst_;
        last = watchedLast_;
        watched = watchedFrames_;
        asked = askedFps_;
    }
    debugLog(QStringLiteral("scroll tap: %1 frames a second not kept (%2), now %3").arg(source_.fps).arg(reason, QString::number(source_.fallbackFps)));
    Source lower = source_;
    lower.fps = source_.fallbackFps;
    const qint64 given = frames_.load();
    stop();
    if (start(lower)) {
        { std::lock_guard lock(mutex_); if (reason_.isEmpty()) reason_ = QStringLiteral("the source could not be started again at a lower rate"); }
        lost_.store(true);
        return;
    }
    frames_.store(given);
    std::lock_guard lock(mailbox_);
    askedFps_ = asked;
    fallback_ = reason;
    watchedFirst_ = first;
    watchedLast_ = last;
    watchedFrames_ = watched;
    watching_ = true;
    pace_.restart(lower.fps, nowMs());
}
}
