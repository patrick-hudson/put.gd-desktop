#pragma once
#include "core/Error.h"
#include "core/RecordingModel.h"
#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <atomic>
#include <functional>

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
}
