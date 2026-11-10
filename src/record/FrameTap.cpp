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
    description += QStringLiteral(" ! videoconvert ! video/x-raw,format=BGRx ! appsink name=tap max-buffers=2 drop=true sync=false");
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
    lost_.store(false);
    frames_.store(0);
    width_.store(0);
    height_.store(0);
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

void FrameTap::stop() {
    if (!pipeline_) return;
    if (imageCopy_) { imageCopy_->stop(); imageCopy_ = nullptr; }
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

std::optional<RunFrame> FrameTap::nextFrame(int timeoutMs) {
    if (!pipeline_ || !sink_) return std::nullopt;
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), GstClockTime(std::max(0, timeoutMs)) * GST_MSECOND);
    // The newest one waiting: the loop compares each frame with the one before
    // it, and an older frame in between tells it nothing more.
    while (sample) {
        GstSample* newer = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 0);
        if (!newer) break;
        gst_sample_unref(sample);
        sample = newer;
    }
    if (!sample) { noteBus(); return std::nullopt; }
    std::optional<RunFrame> result;
    GstVideoInfo info;
    GstVideoFrame frame;
    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (caps && buffer && gst_video_info_from_caps(&info, caps) && GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_BGRx && gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
        const int width = GST_VIDEO_FRAME_WIDTH(&frame), height = GST_VIDEO_FRAME_HEIGHT(&frame);
        const auto* data = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
        const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        if (width > 0 && height > 0 && data && stride >= width * 4) {
            const size_t row = size_t(width) * 4;
            frame_.resize(row * size_t(height));
            for (int y = 0; y < height; ++y) std::memcpy(frame_.data() + row * size_t(y), data + ptrdiff_t(stride) * y, row);
            width_.store(width);
            height_.store(height);
            frames_.fetch_add(1);
            result = RunFrame{scroll::FrameView{frame_.data(), width, height, std::ptrdiff_t(row)}, nowMs()};
        }
        gst_video_frame_unmap(&frame);
    }
    gst_sample_unref(sample);
    return result;
}
}
