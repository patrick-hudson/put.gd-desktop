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

namespace putgd::record {
// The one edited-media model shared by preview and export: a frame at clip
// time t is retained when trimStart <= t < trimEnd, and a mask applies while
// mask.start <= t < mask.end (half-open, source time). Export rebases output
// time to zero; masks are then enabled on gte(t, a) * lt(t, b) in output time.
struct ExportRequest {
    QString input;      // the working clip (Matroska, H.264 [+ AAC])
    QString output;     // final file; the extension is informational, the format decides
    RecordingEdit edit;
    RecordingConfig config;
    qint64 durationUs = 0;
    QSize canvas;
};
struct ExportResult {
    qint64 bytes = 0, durationUs = 0;
    int frames = 0;
    QString videoEncoder, audioEncoder;
    QSize size;
};
Error exportClip(const ExportRequest&, ExportResult&, const std::atomic_bool& cancelled, const std::function<void(double)>& progress = {});
// One decoded frame at clip time with the masks active at that time applied,
// scaled so the long edge is at most maxEdge. Trimmed-out times still render.
Error previewFrame(const QString& input, const RecordingEdit&, const RecordingConfig&, qint64 timeUs, int maxEdge, const QSize& canvas, QImage& out, qint64* actualTimeUs = nullptr);
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
