#pragma once
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

namespace putgd::record {
// One live GStreamer capture: ximagesrc (root rectangle or window) or
// pipewiresrc (a PipeWire screen-cast stream, optionally cropped) → H.264
// (first encoder that negotiates) [+ pulsesrc monitor → AAC] → Matroska
// working clip. Pause drops buffers and rebases timestamps on the encoder
// queues, so the clip timeline excludes paused time; the retained duration
// is the last rebased video timestamp plus one frame. A PipeWire stream runs
// on the monotonic system clock and each frame keeps the compositor's own
// frame time (see pipewireFrameTime), so video and audio share one time base.
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
    // Drains with EOS and finalizes the file; the callback reports the retained duration.
    void stop(std::function<void(Error, qint64 durationUs)>);
    void cancel();
    bool running() const { return pipeline_ != nullptr; }
    bool paused() const { return paused_.load(); }
    qint64 retainedUs() const;
    qint64 fileBytes() const;
    QString encoder() const { return encoder_; }
    QString audioEncoder() const { return audioEncoder_; }
    QJsonObject status() const;
signals:
    void failed(putgd::Error);
    void budgetReached();
private:
    // Lab statistics for PUTGD_RECORD_DEBUG_LOG, written on streaming threads and read after teardown.
    struct Spread { gint64 count = 0, min = 0, max = 0, sum = 0; void add(gint64); QString text() const; };
    struct Rebase { std::atomic<gint64> offset{0}; std::atomic<gint64> firstPassed{-1}; std::atomic<gint64> lastPassed{-1}; std::atomic<gint64> step{0}; std::atomic_bool dropped{false}; Spread lateness; };
    struct FrameClock { gint64 previous = -1, arrival = 0; Spread age; };
    static GstPadProbeReturn probe(GstPad*, GstPadProbeInfo*, gpointer);
    static GstPadProbeReturn stampFrame(GstPad*, GstPadProbeInfo*, gpointer);
    bool launch(const Options&, const QString& encoder, const QString& audioEncoder, QString& failure);
    void poll();
    void tearDown();
    GstElement* pipeline_ = nullptr;
    QTimer bus_, progress_;
    std::atomic_bool paused_{false};
    Rebase video_, audio_;
    FrameClock frameClock_;
    QString clockName_;
    gint64 frameDurationNs_ = 0;
    QString encoder_, audioEncoder_, output_;
    qint64 budget_ = 0;
    bool stopping_ = false, budgetSignalled_ = false;
    std::function<void(Error, qint64)> stopCallback_;
};
}
