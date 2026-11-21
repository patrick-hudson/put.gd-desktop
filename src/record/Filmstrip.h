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

// #1613: the exact frame at a time, decoded on its own worker thread with its
// own input kept open. The window owns two: one for the skimming pointer and
// one for the playhead (scrub, edge drags, held keys). At most one decode is in
// flight; a newer request replaces a queued one (latest wins) without stopping
// the one in flight, so a scrub shows frames continuously; cancel() drops the
// answer of the one in flight. The frame is decoded at the exact time (seek
// back to the keyframe, decode forward, as previewFrame does) with the covers
// active at that time applied, long edge scaled to at most maxEdge. A later
// time in the same GOP is decoded forward from the last frame without a seek,
// and a time the last frame already answers (a new edit at the same playhead)
// is rendered again without decoding. A frame is scaled straight to the
// preview size and its covers painted at that size. Measured on m74 (seek and
// decode, before direct scaling): 217 ms median, 328 ms p95 at 4K.
class FrameSkimmer final : public QObject {
    Q_OBJECT
public:
    explicit FrameSkimmer(QObject* parent = nullptr);
    ~FrameSkimmer() override;
    // Opens (or opens again, after a failure) the clip on the worker.
    void open(const QString& clip, const QSize& canvas);
    void request(qint64 timeUs, const RecordingEdit& edit, int maxEdge);
    void cancel();
    // The last open() failed; requests are then answered by frameFailed at once.
    bool failedToOpen() const;
signals:
    // requestedUs is the request's time; frameUs the decoded frame's own time;
    // revision the request's edit revision. exact: the frame shown for that
    // time; false for the frame decoded so far when a newer request came in
    // (a fast scrub sees real frames on the way; only an exact one stands for
    // the request's time).
    void frameReady(qint64 requestedUs, qint64 frameUs, const QImage& frame, quint64 revision, bool exact);
    // A request was not answered: the clip could not be opened or read, or no
    // frame decoded at that time. message says which (previewFrame's wording).
    void frameFailed(qint64 requestedUs, const QString& message);
    void failed(const QString& message);
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
