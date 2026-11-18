#pragma once
#include "core/RecordingModel.h"
#include <QImage>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <memory>

class QThread;

namespace putgd::record {
// #1613: thumbnails for the review timeline's filmstrip.
//
// One worker thread opens the working clip once and keeps it open. For each
// requested tile time it seeks backward to the keyframe at or before that time
// (AVSEEK_FLAG_BACKWARD), decodes only that keyframe packet and scales the
// frame straight to the tile size: method A of the m74 measurement
// (work/council/test/bench.cpp, put.gd-evidence record
// 2026-11-17-recording-review-redesign/filmstrip-test). It never decodes the
// whole clip. Decoder settings match ClipExporter.cpp (thread_count 0,
// FF_THREAD_FRAME | FF_THREAD_SLICE, software decode).
//
// Tiles are cached by keyframe time (two tile times in one GOP share one
// decode) for the life of the object; the cache holds tile-sized images only.
// A new request replaces the pending one: tiles already decoded stay cached,
// queued ones not yet started are dropped. Requests made before open()
// finishes wait for it. Tiles show the recorded picture without covers.
//
// The window owns one Filmstrip; scrubbing, playback, export and Upload never
// wait for it.
class Filmstrip final : public QObject {
    Q_OBJECT
public:
    explicit Filmstrip(QObject* parent = nullptr);
    // Cancels the request in progress and joins the worker.
    ~Filmstrip() override;
    // Opens the clip on the worker. Failing to open is reported once through
    // failed(); tiles then stay placeholders.
    void open(const QString& clip, qint64 durationUs, const QSize& canvas);
    // Tile times in the order to fill them (the caller puts the two ends of the
    // visible span first) at one tile size in device pixels. Times whose
    // keyframe is cached are answered from the cache at once (tileReady is
    // still emitted, queued, for each); the rest go to the worker. At most 48
    // times per request; extra times are ignored.
    void request(const QList<qint64>& timesUs, const QSize& tileSize);
    // Drops the pending request; a decode in progress finishes and is cached.
    void cancel();
    // The cached tile that covers a time (its keyframe's), or a null image when
    // that keyframe has not been decoded at the current tile size yet.
    QImage tile(qint64 timeUs) const;
    // The keyframe time a tile time resolves to, once the worker has read the
    // clip's index after open(); before that, the time itself.
    qint64 keyframeAtOrBefore(qint64 timeUs) const;
    // Counters for inspect and tests: tiles decoded, cache hits, requests
    // replaced before they finished.
    struct Stats { int decoded = 0, cached = 0, replaced = 0; };
    Stats stats() const;
signals:
    // A tile is ready (decoded or from the cache). requestedUs is the time
    // from the request; keyframeUs the frame's own time.
    void tileReady(qint64 requestedUs, qint64 keyframeUs, const QImage& tile);
    void failed(const QString& message);
private:
    struct Private;
    std::unique_ptr<Private> d_;
};

// #1613: the exact frame under a skimming pointer. It keeps its own input open
// on its own worker thread; at most one decode is in flight, a newer request
// replaces a queued one, and cancel() drops the answer of the one in flight.
// The frame is decoded at the exact time (seek back to the keyframe, decode
// forward, as previewFrame does) with the covers active at that time applied,
// long edge scaled to at most maxEdge. Measured on m74: 217 ms median, 328 ms
// p95 at 4K, so the timeline shows the tile first and asks for this after the
// pointer rests ~120 ms.
class FrameSkimmer final : public QObject {
    Q_OBJECT
public:
    explicit FrameSkimmer(QObject* parent = nullptr);
    ~FrameSkimmer() override;
    void open(const QString& clip, const QSize& canvas);
    void request(qint64 timeUs, const RecordingEdit& edit, int maxEdge);
    void cancel();
signals:
    // requestedUs is the request's time; frameUs the decoded frame's own time.
    void frameReady(qint64 requestedUs, qint64 frameUs, const QImage& frame);
    void failed(const QString& message);
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
