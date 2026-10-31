#pragma once
#include "PauseTimeline.h"
#include "RecordingStorage.h"
#include "WaylandAcquisition.h"
#include <QElapsedTimer>
#include "core/Error.h"
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <functional>
#include <gst/gst.h>
#include <thread>
#include <vector>

namespace putgd::record {
// One live GStreamer capture: ximagesrc (root rectangle or window) or
// pipewiresrc (a PipeWire screen-cast stream, optionally cropped) → leaky
// capture queue → [videorate, videocrop] → videoconvert → queue → H.264
// (first encoder that negotiates) [+ pulsesrc monitor → AAC] → Matroska
// working clip. Capture, conversion and encoding run in separate threads;
// the capture queue leaks its oldest frame rather than block the source. Pause drops buffers and rebases timestamps at the encoder
// inputs by one paused interval shared by both tracks (#130), so the clip
// timeline excludes paused time; the retained duration is the last rebased
// video timestamp plus one frame. A PipeWire stream runs
// on the monotonic system clock and each frame is stamped when it reaches the
// pipeline, as ximagesrc stamps a frame when it reads it (#86).
class RecordingPipeline final : public QObject {
    Q_OBJECT
public:
    struct Options {
        QRect rect;                  // root coordinates; ignored when window != 0; for PipeWire the stream size
        unsigned long window = 0;
        int pipewireFd = -1;         // portal ScreenCast: PipeWire remote fd and node; replaces ximagesrc
        quint32 pipewireNode = 0;
        QRect crop;                  // optional stream-relative crop for region recording
        int fps = 30;
        bool audio = false;
        QString audioDevice;         // PulseAudio/PipeWire source name (sink monitor)
        QString output;              // working clip path (.mkv)
        qint64 budgetBytes = 0;      // stop automatically near this file size; 0 = none
        QStringList encoders;        // preference order of GStreamer H.264 encoders
        QStringList audioEncoders;   // preference order of AAC encoders
    };
    explicit RecordingPipeline(QObject* parent = nullptr);
    ~RecordingPipeline() override;
    Error start(const Options&);
    Error pause();
    Error resume();
    // Drains with EOS and finalizes the file; the callback reports the retained
    // duration. A drain that does not finish in stopDrainMs ends the capture
    // anyway and keeps what was written (#129).
    void stop(std::function<void(Error, qint64 durationUs)>);
    static constexpr int stopDrainMs = 10000;
    void cancel();
    bool running() const { return pipeline_ != nullptr; }
    bool paused() const { return timeline_.paused(); }
    qint64 retainedUs() const;
    qint64 fileBytes() const;
    QString encoder() const { return encoder_; }
    QString audioEncoder() const { return audioEncoder_; }
    // Frames that reached the video encoder, and frames the capture queue had
    // to leak because conversion or encoding fell behind the frame rate.
    qint64 framesEncoded() const;
    qint64 framesDropped() const;
    QJsonObject status() const;
signals:
    void failed(putgd::Error);
    void budgetReached(QJsonObject details);
private:
    friend class RecordingStorageContract;
    // In-process storage fault seam; never exposed through helper commands or environment.
    std::function<RecordingStorage()> storageProbe_;
    // Lab statistics for PUTGD_RECORD_DEBUG_LOG, written on streaming threads and read after teardown.
    struct Spread { gint64 count = 0, min = 0, max = 0, sum = 0; void add(gint64); QString text() const; };
    struct Rebase { std::atomic<gint64> firstPassed{-1}; std::atomic<gint64> lastPassed{-1}; std::atomic<gint64> passed{0}; Spread lateness; AudioClockGuard clock; };
    // Lab statistics: repeated compositor times (KWin stamps the output's last
    // presentation, so frames rendered between two presentations share one),
    // distinct frame contents (one sampled row hashed) and arrival intervals.
    struct FrameClock { gint64 previous = -1, arrival = 0, headerPts = -1, repeatedPts = 0, lastArrival = -1; quint64 contentHash = 0; gint64 distinctContents = 0, sameContents = 0; bool compositorTimes = false, smoothed = true, sampleContents = false; Spread age, interval; FrameSmoothing smoothing; };
    static GstPadProbeReturn probe(GstPad*, GstPadProbeInfo*, gpointer);
    static GstPadProbeReturn stampFrame(GstPad*, GstPadProbeInfo*, gpointer);
    // unthrottled: a PipeWire source asks the compositor for max_framerate 0/1 (see pipewireUnthrottledCaps).
    bool launch(const Options&, const QString& encoder, const QString& audioEncoder, bool unthrottled, QString& failure);
    void poll();
    void tearDown();
    RecordingStorage storage() const;
    // The pipeline's running time now, or -1 before it has a clock.
    gint64 runningTime() const;
    GstElement* pipeline_ = nullptr;
    QTimer bus_, progress_;
    PauseTimeline timeline_;
    Rebase video_, audio_;
    // Send EOS into the branches on stop, one per branch; joined at teardown.
    std::vector<std::thread> drains_;
    quint64 stops_ = 0;
    FrameClock frameClock_;
    std::atomic<gint64> dropped_{0};
    QString clockName_;
    gint64 frameDurationNs_ = 0;
    QString encoder_, audioEncoder_, output_;
    qint64 budget_ = 0;
    QElapsedTimer storageClock_;
    QJsonObject diskStop_;
    bool stopping_ = false, budgetSignalled_ = false;
    std::function<void(Error, qint64)> stopCallback_;
};
}
