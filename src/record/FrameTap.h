#pragma once
#include "ScrollRun.h"
#include "core/Error.h"
#include <QRect>
#include <QSize>
#include <QString>
#include <atomic>
#include <gst/gst.h>
#include <mutex>
#include <vector>

namespace putgd::record {
class ImageCopySource;
// The scroll capture's frame source (#1376): the recording helper's live
// sources (ximagesrc, a PipeWire screen-cast node, ext-image-copy-capture
// through appsrc) into `videoconvert ! BGRx ! appsink`. No videorate and no
// videoscale: the stitcher needs the compositor's pixels untouched. The loop
// pulls on its own thread; each returned frame is this object's copy, so no
// compositor buffer is held while the loop works on it.
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
    bool lost() const override { return lost_.load(); }
    QString lostReason() const;
    QSize frameSize() const { return QSize(width_.load(), height_.load()); }
    qint64 frames() const { return frames_.load(); }
    // The loop clock: monotonic milliseconds.
    static int64_t nowMs();
private:
    void noteBus();
    GstElement* pipeline_ = nullptr;
    GstElement* sink_ = nullptr;
    ImageCopySource* imageCopy_ = nullptr;
    std::vector<uint8_t> frame_;
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
