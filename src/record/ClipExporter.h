#pragma once
#include "core/Error.h"
#include "core/RecordingModel.h"
#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

struct AVFrame;
namespace putgd::record {
// The one edited-media model shared by preview and export: a frame at clip
// time t is retained when t lies in a kept span (the trim minus every cut,
// recording::KeptSpans), and a mask applies while mask.start <= t < mask.end
// (half-open, source time). Export lays the kept spans end to end from zero on
// the clip's own frame grid (#1613), video and audio cut at the same instants.
// A mask covers an output frame while it is active at that frame's time or at
// the source time of the picture the frame shows (#1635), so a still picture
// held across a frame gap or a seam keeps the cover it was captured under.
// A span whose first frame comes more than 1/AdaptiveRate::floorFps after its
// start (a frame gap in a stepped-down or static capture) starts at its start
// on the frame on screen there, so its narration is kept; likewise a span whose
// last frame would run more than that past its end stops at its end.
struct ExportRequest {
    QString input;      // the working clip (Matroska, H.264 [+ AAC])
    QString output;     // final file; the extension is informational, the format decides
    RecordingEdit edit;
    RecordingConfig config;
    qint64 durationUs = 0;
    QSize canvas;
};
struct ExportResult {
    qint64 bytes = 0, durationUs = 0;  // durationUs: the output's (kept) length
    int frames = 0;
    QString videoEncoder, audioEncoder;
    QSize size;
};
// progress runs 0..1 over the output (kept) duration.
Error exportClip(const ExportRequest&, ExportResult&, const std::atomic_bool& cancelled, const std::function<void(double)>& progress = {});
// One decoded frame at clip time with the masks active at that time or at the
// frame's own time applied (as the export covers it), scaled so the long edge
// is at most maxEdge. Trimmed-out times still render.
Error previewFrame(const QString& input, const RecordingEdit&, const RecordingConfig&, qint64 timeUs, int maxEdge, const QSize& canvas, QImage& out, qint64* actualTimeUs = nullptr);
// One decoded frame (any pixel format) as an image with the masks active at
// frameUs, or at requestedUs (the time it is shown for), applied the way
// previewFrame renders them, long edge at most maxEdge.
// Shared by previewFrame and the review window's FrameSkimmer.
Error renderPreviewFrame(const AVFrame* frame, const RecordingEdit&, qint64 frameUs, const QSize& canvas, int maxEdge, QImage& out, std::optional<qint64> requestedUs = std::nullopt);
// One cover's effect (a redaction's fill or a blur) on a preview-sized picture:
// the review viewer paints a cover being moved with it (#1613 D4). The rect and
// strength are in canvas pixels, scaled to the picture. Never used for export.
void paintCoverPreview(QImage& image, const RecordingMask& mask, const QSize& canvas);
// The working clip's duration and frame size (lab diagnostics and tests).
Error probeClip(const QString& input, qint64& durationUs, QSize& canvas);
// Sequential frames for review playback: opened at a clip time, each call
// returns the next decoded frame at or after it with the masks active at its
// time applied and the long edge scaled to at most maxEdge. Decoding runs on
// the calling thread; one instance serves one playback run. No audio.
class FrameSequence {
public:
    FrameSequence();
    ~FrameSequence();
    Error open(const QString& input, qint64 startUs);
    // false at the end of the clip, or on a decode error reported through error.
    // Frames `wanted` declines are decoded (the stream must stay in order) but
    // not converted, masked or scaled, so a late player catches up cheaply.
    bool next(const RecordingEdit&, int maxEdge, const QSize& canvas, QImage& out, qint64& frameUs, Error* error = nullptr, const std::function<bool(qint64)>& wanted = {});
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
