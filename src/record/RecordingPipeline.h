#pragma once
#include "AdaptiveRate.h"
#include "ClickOverlay.h"
#include "Microphones.h"
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
#include <deque>
#include <functional>
#include <gst/gst.h>
#include <mutex>
#include <thread>
#include <vector>

namespace putgd::record {
class ImageCopySource;
// One live GStreamer capture: ximagesrc (root rectangle or window) or
// pipewiresrc (a PipeWire screen-cast stream, optionally cropped) → leaky
// capture queue → [videocrop] → videoconvert → videorate → queue → H.264
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
        // ext-image-copy-capture (wlr-capture): frames come from this source
        // through appsrc; rect is the stream size. Replaces ximagesrc.
        ImageCopySource* imageCopy = nullptr;
        QRect crop;                  // optional stream-relative crop for region recording
        // A window stream: its frames change size with the window, so they are
        // scaled, letterboxed, into this fixed canvas before the encoder.
        QSize canvas;
        // Mutter's window stream draws the window into a larger buffer and says
        // where in each frame's PipeWire crop region; follow it per frame.
        bool followCrop = false;
        // A source error is reported as sourceLost rather than failed and the
        // pipeline keeps running, so the session can reopen the stream
        // (reopenSource) or end it as a stop (a recorded window closed).
        bool reopenableSource = false;
        int fps = 30;
        bool adaptive = true;        // lower the rate when the machine cannot keep up (AdaptiveRate)
        bool audio = false;          // system audio: the default sink's monitor
        QString audioDevice;         // PulseAudio/PipeWire source name (sink monitor)
        // #1364: an input device, alone or mixed with the system audio into the
        // clip's one audio track. The name is a source the session resolved; never a monitor.
        bool microphone = false;
        QString microphoneDevice;
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
    // Replaces the PipeWire source with a new one on the same node, keeping the
    // clip, its clock and its timeline. pipewiresrc before 1.5 drops its
    // possible caps once negotiated, so KWin's renegotiation after a window
    // resize fails it with "unhandled format"; a new source negotiates afresh.
    bool reopenSource();
    bool running() const { return pipeline_ != nullptr; }
    bool stopping() const { return stopping_; }
    bool paused() const { return timeline_.paused(); }
    qint64 retainedUs() const;
    qint64 fileBytes() const;
    QString encoder() const { return encoder_; }
    QString audioEncoder() const { return audioEncoder_; }
    // What the clip's audio track holds: the sources that were opened.
    bool recordsSystemAudio() const { return !audioEncoder_.isEmpty() && options_.audio; }
    bool recordsMicrophone() const { return !audioEncoder_.isEmpty() && options_.microphone; }
    // The microphone went away (unplugged, or the sound server moved the stream
    // to another source): the clip goes on and that input is silent from there.
    bool microphoneLost() const { return microphone_.lost.load(); }
    QString microphoneLostReason() const { return microphoneLostReason_; }
    // #1364: a click at this canvas position now; drawn as a ring into the
    // frames from here on. Ignored unless a capture is running.
    void addClick(QPointF canvasPoint, int button, qint64 ageNs = 0);
    ClickOverlay& clicks() { return clicks_; }
    // The microphone's peak level since the last call, 0..1, for the bar's meter.
    float takeMicrophoneLevel() { return microphone_.level.take(); }
    // Frames that reached the video encoder, and frames the capture queue had
    // to leak because conversion or encoding fell behind the frame rate.
    qint64 framesEncoded() const;
    qint64 framesDropped() const;
    // The rate frames are kept at now: the configured one until the adaptive
    // rate lowers it. The clip keeps the configured rate (videorate repeats).
    int frameRate() const { return rate_.current(); }
    QJsonObject status() const;
signals:
    void failed(putgd::Error);
    // The PipeWire source failed while reopenableSource is set; once per failure.
    void sourceLost(putgd::Error);
    void budgetReached(QJsonObject details);
    // The image-copy source ended: its window closed or its output went away.
    void imageCopyLost(putgd::Error);
    // The microphone stopped delivering; the recording continues without it.
    void microphoneGone(QString reason);
private:
    friend class RecordingStorageContract;
    friend class RecordingPipelineThroughput;
    // In-process storage fault seam; never exposed through helper commands or environment.
    std::function<RecordingStorage()> storageProbe_;
    // Lab statistics for PUTGD_RECORD_DEBUG_LOG, written on streaming threads and read after teardown.
    struct Spread { gint64 count = 0, min = 0, max = 0, sum = 0; void add(gint64); QString text() const; };
    struct Rebase { std::atomic<gint64> firstPassed{-1}; std::atomic<gint64> lastPassed{-1}; std::atomic<gint64> passed{0}; Spread lateness; AudioClockGuard clock; };
    // After a step of the adaptive rate (#1045): videorate's repeats, flagged GAP,
    // stop before the encoder, which encodes only the kept frames with their
    // real timestamps. The caps keep the configured rate, because a mid-stream
    // rate change is refused by matroskamux and would re-initialise the encoder.
    // Encoders that keep their input PTS need nothing more; the va encoders
    // restamp their output contiguously, so each input time waits in `inEncoder`
    // and restoreEncodedTime puts it back on the output, in order. `stepped` is
    // sticky for restamping; `dropping` ends at Stop, so the clip ends where its
    // last slot was instead of at the last kept frame.
    struct RepeatSkip { std::atomic<bool> dropping{false}, stepped{false}; std::atomic<gint64> skipped{0}; bool restamps = false; std::mutex mutex; std::deque<gint64> inEncoder; gint64 encoderOffset = 0; bool offsetKnown = false; };
    // The rate gate before the capture queue: frames the source delivered,
    // frames kept, and the period kept frames are spaced by (the configured
    // frame's until the adaptive rate lowers it). ximagesrc's frames pass
    // through gateFrame; a PipeWire stream's cadence lock takes the period.
    struct RateGate { std::atomic<gint64> periodNs{0}, delivered{0}, kept{0}; gint64 lastKept = -1, toleranceNs = 0; };
    // Lab statistics: repeated compositor times (KWin stamps the output's last
    // presentation, so frames rendered between two presentations share one),
    // distinct frame contents (one sampled row hashed) and arrival intervals.
    // A frame identical to the previous kept one (KWin records the same
    // presentation again after a cursor move or a second render) is dropped
    // before it is copied; the rest take cadence-locked stamps. lastKept holds
    // the previous kept copy for the comparison and is released at teardown.
    // An identical frame still passes once no frame has passed for a period:
    // pipewiresrc resends the last frame of a static screen (keepalive-time),
    // and those resends keep the clip's timeline and the retained duration
    // advancing through videorate.
    // A passed repeat must not cost a changed frame its slot: a change arriving
    // early after a repeat takes the next slot instead of being dropped.
    struct FrameClock { RateGate* gate = nullptr; gint64 previous = -1, arrival = 0, headerPts = -1, repeatedPts = 0, lastArrival = -1, lastPassed = -1; GstBuffer* lastKept = nullptr; gint64 distinctContents = 0, sameContents = 0, early = 0, resyncs = 0, afterRepeat = 0, ownHeld = 0; bool compositorTimes = false, locked = true, lastWasRepeat = false, ownFrames = false; Spread age, interval; FrameLock lock; };
    static GstPadProbeReturn probe(GstPad*, GstPadProbeInfo*, gpointer);
    // One capture branch before the mix (or the microphone alone): keeps a
    // runaway device clock out of the mixer (#129), feeds the level meter and
    // drops everything once the microphone is gone or moved to another source.
    struct AudioTap { AudioClockGuard clock; LevelMeter level; std::atomic<bool> lost{false}; bool meter = false; GstElement* source = nullptr; QByteArray device; std::atomic<gint64> buffers{0};
        // Lab statistic: each buffer's time against a contiguous run from the first one.
        gint64 first = -1, expected = 0, count = 0; QStringList drift; };
    static GstPadProbeReturn tapAudio(GstPad*, GstPadProbeInfo*, gpointer);
    AudioTap microphone_, system_;
    ClickOverlay clicks_;
    // The encoder's input format, read from its caps for the ring's planes.
    struct ClickFrame { bool known = false, drawable = false; int width = 0, height = 0; bool nv12 = false; };
    ClickFrame clickFrame_;
    // The buffer to encode in the frame's place: its own picture with the rings drawn, or the frame itself when that cannot be made. Takes the reference.
    static GstBuffer* drawClicks(RecordingPipeline* self, GstPad* pad, GstBuffer* buffer, gint64 runningNs);
    QString microphoneLostReason_;
    // The poll's watch for a microphone that stopped delivering buffers.
    gint64 microphoneSeen_ = 0;
    QElapsedTimer microphoneQuiet_;
    void loseMicrophone(const QString& reason);
    static GstPadProbeReturn restoreEncodedTime(GstPad*, GstPadProbeInfo*, gpointer);
    static GstPadProbeReturn stampFrame(GstPad*, GstPadProbeInfo*, gpointer);
    static GstPadProbeReturn gateFrame(GstPad*, GstPadProbeInfo*, gpointer);
    // reopenableSource: the source's own EOS (sent after its error) must not
    // finalize the clip; stop() sends EOS past the source instead.
    static GstPadProbeReturn dropSourceEos(GstPad*, GstPadProbeInfo*, gpointer);
    // followCrop: sets the window crop from the frame's crop meta, on videocrop's sink pad.
    static GstPadProbeReturn applyCropMeta(GstPad*, GstPadProbeInfo*, gpointer);
    struct CropFollow { GstElement* crop = nullptr; QRect applied; gint64 changes = 0; };
    CropFollow cropFollow_;
    // Probes on a (new) source pad: frame stamping or the rate gate, and the EOS guard.
    void attachSourceProbes(GstElement* source, bool stamped, bool guardEos);
    bool sourceLost_ = false;
    int sourceReopens_ = 0;
    // One adaptive-rate window a second while recording (not paused or stopping).
    void observeRate();
    // unthrottled: a PipeWire source asks the compositor for max_framerate 0/1 (see pipewireUnthrottledCaps).
    // failedElement names the element whose error ended a failed launch, when one did.
    bool launch(const Options&, const QString& encoder, const QString& audioEncoder, bool unthrottled, QString& failure, QString* failedElement = nullptr);
    // Tries every encoder (and PipeWire request) for these options.
    bool launchAny(const Options&, const QString& audioEncoder, QStringList& failures, bool* microphoneFailed);
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
    RateGate gate_;
    RepeatSkip repeatSkip_;
    bool encodeRepeats_ = false;
    AdaptiveRate rate_;
    QElapsedTimer rateClock_;
    struct RateMarks { gint64 delivered = 0, leaked = 0; CpuSample cpu; } rateMarks_;
    QJsonObject rateChange_;
    std::atomic<gint64> dropped_{0};
    QString clockName_;
    gint64 frameDurationNs_ = 0;
    QString encoder_, audioEncoder_, output_;
    // The options of the running capture and whether its PipeWire stream was
    // negotiated unthrottled: a compositor that refuses that request after the
    // pipeline reached PLAYING fails the stream later, and the capture is then
    // relaunched with the plain request as long as no frame has been encoded.
    Options options_;
    bool unthrottled_ = false;
    bool relaunchThrottled();
    qint64 budget_ = 0;
    QElapsedTimer storageClock_;
    QJsonObject diskStop_;
    bool stopping_ = false, budgetSignalled_ = false;
    std::function<void(Error, qint64)> stopCallback_;
};
}
