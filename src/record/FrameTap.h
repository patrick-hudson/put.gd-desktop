#pragma once
#include "FramePace.h"
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
        // ximagesrc's grab rate; the other sources send on damage. 60 a
        // second halves the distance a page moves between two frames, which
        // kept a person's fast scrolling on Cinnamon (#2064). While the tap
        // watches its pace, a source or a loop that cannot keep it up for
        // two seconds in a row is restarted at fallbackFps for the rest of
        // the capture (FramePace).
        int fps = 60;
        int fallbackFps = 30;
    };
    // How the tap's threads fared while the source ran at the rate asked for
    // and the session read (#2099): the loop's, the grab's (ximagesrc) and
    // the copy's (the conversion and the copy into the mailbox, after the
    // queue). Each one's CPU time, and the time it was ready to run but
    // waited for a CPU (the kernel's schedstat). Work that fits a frame
    // interval and long waits for a CPU say a fallback came from a busy
    // host, not from the capture's own work.
    struct Load {
        double ms = 0;  // wall time at the rate asked for
        // Frames the loop took; grabs and copies between the first and the last measured.
        quint64 taken = 0, grabbed = 0, copied = 0;
        bool known = false;  // every thread's times were read
        double loopCpuMs = 0, loopWaitedMs = 0, grabCpuMs = 0, grabWaitedMs = 0, copyCpuMs = 0, copyWaitedMs = 0;  // totals
    };
    // The rate a fixed-rate source was asked for and runs at now, the frames
    // a second it delivered while watched, and why it went down (empty: it did not).
    // `load` covers the time at the rate asked for; `trigger` the two pace
    // windows that made the source go down (empty when it did not).
    struct Rate {
        int asked = 0, now = 0;
        double delivered = 0;
        QString fallback;
        Load load, trigger;
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
    // Watch the pace from now (the session reads), or stop watching. Only a
    // fixed-rate source with a lower rate to go to is watched.
    void watchPace(bool on);
    Rate rate() const;
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
    // The loop's thread: the source again at the lower rate.
    void fallBack(FramePace::Verdict why);
    // The loop's thread: the load at the asked rate begins, and ends (at a fallback or the end of the watch).
    void openLoad();
    void closeLoad();
    // A streaming thread's times at its first and last measured frame.
    struct ThreadSpan {
        double cpuFirst = 0, waitedFirst = 0, cpuLast = 0, waitedLast = 0;
        quint64 frames = 0;
        bool known = true;
    };
    // Every thread's times at one moment, on the loop's thread: the loop's
    // own, and the grab's and the copy's as of their latest frame.
    struct Times {
        int64_t ms = 0;
        qint64 taken = 0;
        bool known = false;
        double loopCpu = 0, loopWaited = 0;
        ThreadSpan grab, copy;
    };
    // On a streaming thread, after each frame: that thread's times, while the load is measured.
    void noteThread(ThreadSpan& span);
    static void spanDelta(const ThreadSpan& from, const ThreadSpan& to, quint64& frames, double& cpu, double& waited, bool& known);
    // The loop's thread, under the mailbox lock: the times now (loadMutex_ held).
    Times timesNow(qint64 taken);
    // The loop's thread, under the mailbox lock, after the pace counters moved:
    // a window that closed is kept, and the next one starts now.
    void markWindows(bool closed, bool begun);
    // ximagesrc's streaming thread, after each grab.
    static GstPadProbeReturn grabbed(GstPad*, GstPadProbeInfo*, gpointer self);
    GstElement* pipeline_ = nullptr;
    GstElement* sink_ = nullptr;
    ImageCopySource* imageCopy_ = nullptr;
    Source source_;
    // The mailbox: the newest frame, its size and arrival time, and a count
    // that says whether the loop has had it. frame_ is what the loop was given.
    mutable std::mutex mailbox_;
    std::condition_variable fresh_;
    std::vector<uint8_t> latest_, frame_;
    int latestWidth_ = 0, latestHeight_ = 0;
    int64_t latestAt_ = 0;
    guint64 latestSession_ = 0;  // image-copy: the capture session the newest frame came from
    quint64 arrivedCount_ = 0, givenCount_ = 0;
    // Pace, under the mailbox lock: the watch, and what the source delivered while watched.
    FramePace pace_;
    bool watching_ = false;
    int askedFps_ = 0;
    int64_t watchedFirst_ = -1, watchedLast_ = -1;
    quint64 watchedFrames_ = 0;
    QString fallback_;
    // The load at the asked rate. The streaming threads check loadOpen_
    // alone while nothing is measured; the rest is under loadMutex_, never
    // the mailbox lock, so a grab never waits on a frame copy. The loop's
    // times when it began; the grab's and the copy's spans.
    std::atomic<bool> loadOpen_{false};
    mutable std::mutex loadMutex_;
    bool loopKnown_ = false;
    int64_t loadFrom_ = 0;
    qint64 takenFrom_ = 0;
    double loopCpuFrom_ = 0, loopWaitedFrom_ = 0;
    ThreadSpan grab_, copy_;
    Load load_;
    // The pace windows at the asked rate: where the current one began, and
    // the last two that closed with a verdict, the ones a fallback comes from.
    Times windowFrom_;
    Load windows_[2];
    int windowCount_ = 0;
    bool fallingBack_ = false;
    Load trigger_;
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
