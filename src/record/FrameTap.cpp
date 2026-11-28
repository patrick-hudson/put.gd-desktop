#include "FrameTap.h"
#include "DebugLog.h"
#include "ImageCopySource.h"
#include "SourceDescription.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <thread>
#include <unistd.h>

namespace putgd::record {
int64_t FrameTap::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void SteadyClock::sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

FrameTap::~FrameTap() { stop(); }

namespace {
// This thread's CPU time and the time it was ready to run but waited for a
// CPU, in milliseconds (/proc/thread-self/schedstat); false when the kernel
// does not say.
bool threadTimes(double& cpuMs, double& waitedMs) {
    const int fd = ::open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char text[128];
    const ssize_t n = ::read(fd, text, sizeof text - 1);
    ::close(fd);
    if (n <= 0) return false;
    text[n] = 0;
    unsigned long long cpu = 0, waited = 0;
    if (std::sscanf(text, "%llu %llu", &cpu, &waited) != 2) return false;
    cpuMs = double(cpu) / 1e6;
    waitedMs = double(waited) / 1e6;
    return true;
}
}

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
    if (source.x11Rect.isValid() && !source.imageCopy && !source.pipewireNode)
        if (GstElement* grab = gst_bin_get_by_name(GST_BIN(pipeline), "src")) {
            if (GstPad* pad = gst_element_get_static_pad(grab, "src")) {
                gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &FrameTap::grabbed, this, nullptr);
                gst_object_unref(pad);
            }
            gst_object_unref(grab);
        }
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
    {
        std::lock_guard lock(mailbox_);
        givenCount_ = arrivedCount_;
        pace_.begin(nowMs());
        if (loadOpen_.load(std::memory_order_relaxed)) markWindows(false, true, false);
    }
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
    auto* tap = static_cast<FrameTap*>(self);
    if (GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink))) {
        tap->take(sample);
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
    bool published = false;
    if (width > 0 && height > 0 && data && stride >= width * 4) {
        const size_t row = size_t(width) * 4;
        std::lock_guard lock(mailbox_);
        published = true;
        latest_.resize(row * size_t(height));
        for (int y = 0; y < height; ++y) std::memcpy(latest_.data() + row * size_t(y), data + ptrdiff_t(stride) * y, row);
        latestWidth_ = width;
        latestHeight_ = height;
        latestAt_ = nowMs();
        latestSession_ = GST_BUFFER_OFFSET(buffer);
        // While the load is measured, this frame's copy is in the measure
        // before the frame is published: a loop that takes it, notified or
        // not, closes a pace window on timing that includes it (#2099).
        // Measurement before the mailbox's in the lock order, as in markWindows.
        noteThread(copy_);
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
    }
    gst_video_frame_unmap(&frame);
    if (published) fresh_.notify_all();
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
    const uint64_t closed = pace_.closed(), begun = pace_.begun();
    const FramePace::Verdict pace = watching_ ? pace_.taken(nowMs(), missed) : FramePace::Verdict::Keep;
    if (loadOpen_.load(std::memory_order_relaxed) && (pace_.closed() != closed || pace_.begun() != begun)) markWindows(pace_.closed() != closed, pace_.begun() != begun, true);
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
    if (!on) closeLoad();
    {
        std::lock_guard lock(mailbox_);
        watching_ = on && source_.x11Rect.isValid() && !source_.imageCopy && !source_.pipewireNode;
        if (!watching_) return;
        pace_.restart(source_.fps, nowMs());
        // Frames replaced before the watch began are not the loop's to answer for.
        if (arrivedCount_ - givenCount_ > 1) givenCount_ = arrivedCount_ - 1;
        if (source_.fps != askedFps_) return;
    }
    openLoad();
}

void FrameTap::openLoad() {
    double cpu = 0, waited = 0;
    const bool known = threadTimes(cpu, waited);
    const qint64 taken = frames_.load();
    std::lock_guard lock(loadMutex_);
    loopKnown_ = known;
    loopCpuFrom_ = cpu;
    loopWaitedFrom_ = waited;
    loadFrom_ = nowMs();
    takenFrom_ = taken;
    grab_ = {};
    copy_ = {};
    load_ = {};
    trigger_ = {};
    windowCount_ = 0;
    fallingBack_ = false;
    windowFrom_ = Times{loadFrom_, taken, known, cpu, waited, {}, {}};
    loadOpen_.store(true);
}

FrameTap::Times FrameTap::timesNow(qint64 taken) {
    Times now;
    now.ms = nowMs();
    now.taken = taken;
    now.known = threadTimes(now.loopCpu, now.loopWaited);
    now.grab = grab_;
    now.copy = copy_;
    return now;
}

// A thread's frames and times between two moments: from its latest frame
// at the first (its first frame, when it had none yet) to its latest at the second.
void FrameTap::spanDelta(const ThreadSpan& from, const ThreadSpan& to, quint64& frames, double& cpu, double& waited, bool& known) {
    const bool fromFirst = from.frames == 0;
    const quint64 base = fromFirst ? 1 : from.frames;
    frames = to.frames > base ? to.frames - base : 0;
    cpu = to.cpuLast - (fromFirst ? to.cpuFirst : from.cpuLast);
    waited = to.waitedLast - (fromFirst ? to.waitedFirst : from.waitedLast);
    known = from.known && to.known && frames > 0;
}

void FrameTap::markWindows(bool closed, bool begun, bool taking) {
    // The frame the loop is taking now closes the window: it counts in it.
    const qint64 taken = frames_.load() + (taking ? 1 : 0);
    std::lock_guard lock(loadMutex_);
    if (!loadOpen_.load(std::memory_order_relaxed)) return;
    const Times now = timesNow(taken);
    if (closed) {
        const Times& from = windowFrom_;
        Load window;
        window.ms = double(now.ms - from.ms);
        window.taken = quint64(std::max<qint64>(0, now.taken - from.taken));
        window.loopCpuMs = now.loopCpu - from.loopCpu;
        window.loopWaitedMs = now.loopWaited - from.loopWaited;
        bool grabKnown = false, copyKnown = false;
        spanDelta(from.grab, now.grab, window.grabbed, window.grabCpuMs, window.grabWaitedMs, grabKnown);
        spanDelta(from.copy, now.copy, window.copied, window.copyCpuMs, window.copyWaitedMs, copyKnown);
        window.known = from.known && now.known && grabKnown && copyKnown;
        windows_[0] = windows_[1];
        windows_[1] = window;
        windowCount_ = std::min(windowCount_ + 1, 2);
    }
    if (begun) windowFrom_ = now;
}


void FrameTap::closeLoad() {
    double cpu = 0, waited = 0;
    const bool known = threadTimes(cpu, waited);
    const qint64 taken = frames_.load();
    std::lock_guard lock(loadMutex_);
    if (!loadOpen_.exchange(false)) return;
    if (fallingBack_ && windowCount_ == 2) {
        const Load& a = windows_[0];
        const Load& b = windows_[1];
        trigger_.ms = a.ms + b.ms;
        trigger_.taken = a.taken + b.taken;
        trigger_.grabbed = a.grabbed + b.grabbed;
        trigger_.copied = a.copied + b.copied;
        trigger_.known = a.known && b.known;
        trigger_.loopCpuMs = a.loopCpuMs + b.loopCpuMs;
        trigger_.loopWaitedMs = a.loopWaitedMs + b.loopWaitedMs;
        trigger_.grabCpuMs = a.grabCpuMs + b.grabCpuMs;
        trigger_.grabWaitedMs = a.grabWaitedMs + b.grabWaitedMs;
        trigger_.copyCpuMs = a.copyCpuMs + b.copyCpuMs;
        trigger_.copyWaitedMs = a.copyWaitedMs + b.copyWaitedMs;
    }
    load_.ms = double(nowMs() - loadFrom_);
    load_.taken = quint64(std::max<qint64>(0, taken - takenFrom_));
    load_.grabbed = grab_.frames > 1 ? grab_.frames - 1 : 0;
    load_.copied = copy_.frames > 1 ? copy_.frames - 1 : 0;
    load_.known = known && loopKnown_ && grab_.known && copy_.known && load_.grabbed > 0 && load_.copied > 0;
    if (!load_.known) return;
    load_.loopCpuMs = cpu - loopCpuFrom_;
    load_.loopWaitedMs = waited - loopWaitedFrom_;
    load_.grabCpuMs = grab_.cpuLast - grab_.cpuFirst;
    load_.grabWaitedMs = grab_.waitedLast - grab_.waitedFirst;
    load_.copyCpuMs = copy_.cpuLast - copy_.cpuFirst;
    load_.copyWaitedMs = copy_.waitedLast - copy_.waitedFirst;
}

void FrameTap::noteThread(ThreadSpan& span) {
    if (!loadOpen_.load(std::memory_order_relaxed)) return;
    double cpu = 0, waited = 0;
    const bool known = threadTimes(cpu, waited);
    std::lock_guard lock(loadMutex_);
    if (!loadOpen_.load(std::memory_order_relaxed)) return;
    if (!known) span.known = false;
    if (span.frames++ == 0) {
        span.cpuFirst = cpu;
        span.waitedFirst = waited;
    }
    span.cpuLast = cpu;
    span.waitedLast = waited;
}

GstPadProbeReturn FrameTap::grabbed(GstPad*, GstPadProbeInfo*, gpointer self) {
    auto* tap = static_cast<FrameTap*>(self);
    tap->noteThread(tap->grab_);
    return GST_PAD_PROBE_OK;
}

FrameTap::Rate FrameTap::rate() const {
    std::unique_lock lock(mailbox_);
    Rate rate;
    rate.asked = askedFps_;
    rate.now = askedFps_ ? source_.fps : 0;
    if (watchedFrames_ > 1 && watchedLast_ > watchedFirst_) rate.delivered = double(watchedFrames_ - 1) * 1000.0 / double(watchedLast_ - watchedFirst_);
    rate.fallback = fallback_;
    lock.unlock();
    std::lock_guard loadLock(loadMutex_);
    rate.load = load_;
    rate.trigger = trigger_;
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
    { std::lock_guard lock(loadMutex_); fallingBack_ = true; }
    closeLoad();
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
