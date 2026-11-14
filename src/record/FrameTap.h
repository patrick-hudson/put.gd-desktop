#pragma once
#include "ScrollSession.h"
#include "core/Error.h"
#include <QRect>
#include <QSize>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <gst/gst.h>
#include <mutex>
#include <vector>

namespace putgd::record {
class ImageCopySource;
// The scroll capture's frame source (#1376): the recording helper's live
// sources (ximagesrc, a PipeWire screen-cast node, ext-image-copy-capture
// through appsrc) into `videoconvert ! BGRx ! appsink`. No videorate and no
// videoscale: the stitcher needs the compositor's pixels untouched. Every
// frame is copied out on the stream's own thread into a latest-frame mailbox
// and its buffer released at once: a compositor's PipeWire stream has only a
// few buffers, and one that finds none free skips the frame, which a loop
// that takes frames only while it waits then never sees (KWin 6.3, real
// desktop run of 2026-11-10). The loop reads the mailbox on its own thread.
class FrameTap final : public RunFrames {
public:
    struct Source {
        // One of: an X11 root rectangle, a PipeWire node (with the portal-free
        // session daemon, fd -1), or an image-copy source.
        QRect x11Rect;
        quint32 pipewireNode = 0;
        int pipewireFd = -1;
        ImageCopySource* imageCopy = nullptr;
        // A whole-output source: the region inside its frames, in stream pixels.
        QSize streamSize;
        QRect crop;
        int fps = 30;  // ximagesrc's grab rate; the other sources send on damage
    };
    FrameTap() = default;
    ~FrameTap() override;
    FrameTap(const FrameTap&) = delete;
    FrameTap& operator=(const FrameTap&) = delete;
    // The pipeline description for a source (also what the tests assert).
    static QString describe(const Source&);
    Error start(const Source&);
    void stop();
    bool running() const { return pipeline_ != nullptr; }
    // RunFrames, on the loop's thread.
    std::optional<RunFrame> nextFrame(int timeoutMs) override;
    // A frame grabbed after the call began. ximagesrc grabs the screen for
    // every frame: one that arrives a frame interval later. A PipeWire stream
    // is linked anew, and the compositor's first frame on a new link is the
    // screen as it is. The image-copy source opens a new capture session,
    // whose first frame is copied whether or not the source changed.
    std::optional<RunFrame> freshFrame(int timeoutMs) override;
    bool lost() const override { return lost_.load(); }
    QString lostReason() const;
    QSize frameSize() const { return QSize(width_.load(), height_.load()); }
    qint64 frames() const { return frames_.load(); }
    // The loop clock: monotonic milliseconds.
    static int64_t nowMs();
private:
    void noteBus();
    // The sink's new-sample callback, on a streaming thread.
    static GstFlowReturn arrived(GstElement* sink, gpointer self);
    void take(GstSample* sample);
    GstElement* pipeline_ = nullptr;
    GstElement* sink_ = nullptr;
    ImageCopySource* imageCopy_ = nullptr;
    Source source_;
    // The mailbox: the newest frame, its size and arrival time, and a count
    // that says whether the loop has had it. frame_ is what the loop was given.
    std::mutex mailbox_;
    std::condition_variable fresh_;
    std::vector<uint8_t> latest_, frame_;
    int latestWidth_ = 0, latestHeight_ = 0;
    int64_t latestAt_ = 0;
    guint64 latestSession_ = 0;  // image-copy: the capture session the newest frame came from
    quint64 arrivedCount_ = 0, givenCount_ = 0;
    std::atomic<bool> lost_{false};
    std::atomic<int> width_{0}, height_{0};
    std::atomic<qint64> frames_{0};
    mutable std::mutex mutex_;
    QString reason_;
};
// The loop's clock in the helper.
struct SteadyClock final : RunClock {
    int64_t nowMs() override { return FrameTap::nowMs(); }
    void sleepMs(int ms) override;
};
}
