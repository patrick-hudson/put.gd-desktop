#include "Filmstrip.h"
#include "ClipExporter.h"
#include <QFile>
#include <QHash>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace putgd::record {
namespace {
constexpr AVRational microseconds{1, 1000000};
constexpr int maximumTiles = 48;
using FramePtr = std::unique_ptr<AVFrame, decltype([](AVFrame* f) { av_frame_free(&f); })>;

QString avText(int code) { char text[AV_ERROR_MAX_STRING_SIZE] = {}; av_strerror(code, text, sizeof text); return QString::fromUtf8(text); }

// One open clip with a video decoder, used by one worker thread. Decoder
// settings are the exporter's (ClipExporter.cpp Input::open).
struct Source {
    explicit Source(const std::atomic_bool* stop) : stop_(stop) {}
    ~Source() { close(); av_packet_free(&packet); }
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    int stream = -1;
    qint64 origin = 0;
    AVRational timeBase{1, 1};
    bool pending = false;  // packet holds a verified key packet not yet sent
    bool drained = false;  // the decoder was sent end of stream; it needs a seek before more
    QString path;

    static int interrupted(void* opaque) { return static_cast<const std::atomic_bool*>(opaque)->load() ? 1 : 0; }
    void close() {
        av_packet_unref(packet);
        pending = false;
        drained = false;
        if (codec) avcodec_free_context(&codec);
        if (format) avformat_close_input(&format);
        stream = -1;
    }
    // An empty string on success, otherwise what failed.
    QString open(const QString& clip) {
        close();
        path = clip;
        format = avformat_alloc_context();
        if (!format) return QStringLiteral("Cannot open the working clip");
        // A closing review window must not wait for a slow open or read.
        format->interrupt_callback = {&Source::interrupted, const_cast<std::atomic_bool*>(stop_)};
        int rc = avformat_open_input(&format, QFile::encodeName(clip).constData(), nullptr, nullptr);
        if (rc < 0) return QStringLiteral("Cannot open the working clip: ") + avText(rc);
        if ((rc = avformat_find_stream_info(format, nullptr)) < 0) return QStringLiteral("Cannot read the working clip: ") + avText(rc);
        const AVCodec* decoder = nullptr;
        stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (stream < 0 || !decoder) return QStringLiteral("The working clip has no video");
        codec = avcodec_alloc_context3(decoder);
        const AVStream* track = format->streams[stream];
        if (!codec || avcodec_parameters_to_context(codec, track->codecpar) < 0) return QStringLiteral("Cannot decode the working clip");
        codec->thread_count = 0;
        codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (avcodec_open2(codec, decoder, nullptr) < 0) return QStringLiteral("Cannot decode the working clip");
        timeBase = track->time_base;
        origin = track->start_time == AV_NOPTS_VALUE ? 0 : track->start_time;
        return {};
    }
    qint64 streamTime(qint64 us) const { return origin + av_rescale_q(us, microseconds, timeBase); }
    // The indexed keyframe at or before a stream time, or nullopt when the index
    // has none (the caller then seeks).
    std::optional<qint64> indexedKeyframe(qint64 target) const {
        AVStream* track = format->streams[stream];
        const int at = av_index_search_timestamp(track, target, AVSEEK_FLAG_BACKWARD);
        if (at < 0) return std::nullopt;
        const AVIndexEntry* entry = avformat_index_get_entry(track, at);
        if (!entry) return std::nullopt;
        return entry->timestamp;
    }
    qint64 clipTime(qint64 ts) const { return av_rescale_q(ts - origin, timeBase, microseconds); }
    int readVideo() {
        int status;
        while ((status = av_read_frame(format, packet)) >= 0 && packet->stream_index != stream) av_packet_unref(packet);
        return status;
    }
    // Seeks backward to the keyframe at or before target (stream units) and
    // leaves its packet pending. When the demuxer lands anywhere else (a first
    // Matroska cluster with no cue point overshoots) the clip is reopened and
    // starts from its first packet, as ClipExporter's Input::seekTo does.
    bool seekKey(qint64 target) {
        av_packet_unref(packet);
        pending = false;
        drained = false;
        const int sought = av_seek_frame(format, stream, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec);
        if (sought >= 0 && readVideo() >= 0 && packet->pts != AV_NOPTS_VALUE && packet->pts <= target && (packet->flags & AV_PKT_FLAG_KEY)) return pending = true;
        const QString clip = path;
        if (!open(clip).isEmpty()) return false;
        if (readVideo() < 0 || !(packet->flags & AV_PKT_FLAG_KEY)) return false;
        return pending = true;
    }
    // The pending key packet alone, drained: the keyframe, nothing after it.
    bool decodeKey(AVFrame* out, qint64& keyPts) {
        keyPts = packet->pts;
        const int sent = avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
        pending = false;
        if (sent < 0) return false;
        avcodec_send_packet(codec, nullptr);
        return avcodec_receive_frame(codec, out) == 0;
    }
    // From the pending keyframe forward to the first frame at or after target
    // (the last frame when the clip ends first), as previewFrame decodes it.
    // abort() is polled once per packet. When yield() says a newer request is
    // waiting, it stops at the latest frame decoded so far (partial).
    template <typename Abort, typename Yield>
    bool decodeTo(qint64 target, AVFrame* out, qint64& framePts, const Abort& abort, const Yield& yield, bool& partial) {
        partial = false;
        FramePtr frame(av_frame_alloc());
        bool have = false, done = false;
        while (!done) {
            if (abort()) return false;
            // The pending key packet (from seekKey) is sent first, then the ones read after it.
            int rc = 0;
            if (pending) pending = false;
            else rc = readVideo();
            if (rc == AVERROR_EOF) { avcodec_send_packet(codec, nullptr); drained = true; }
            else if (rc < 0) return have;
            else {
                rc = avcodec_send_packet(codec, packet);
                // A full decoder takes the packet again once drained below; dropping
                // it would decode later frames from the wrong reference.
                if (rc == AVERROR(EAGAIN)) pending = true;
                else {
                    av_packet_unref(packet);
                    if (rc < 0) return have;
                }
            }
            for (;;) {
                rc = avcodec_receive_frame(codec, frame.get());
                if (rc == AVERROR(EAGAIN)) break;
                if (rc < 0) { done = true; break; }
                const qint64 pts = frame->best_effort_timestamp;
                av_frame_unref(out);
                av_frame_move_ref(out, frame.get());
                framePts = pts;
                have = true;
                if (pts >= target) { done = true; break; }
                if (yield()) { partial = true; done = true; break; }
            }
        }
        return have;
    }
private:
    const std::atomic_bool* stop_;
};

// Stops and joins a worker thread; one decode in progress is all it waits for.
template <typename Worker>
void shutdownWorker(Worker& w) {
    {
        QMutexLocker lock(&w.mutex);
        w.quit = true;
        w.wake.wakeAll();
    }
    w.wait();
}
}

// ---- Filmstrip ----

struct Filmstrip::Private final : QThread {
    explicit Private(Filmstrip* o) : owner(o) {}
    ~Private() override { shutdownWorker(*this); }

    Filmstrip* owner;
    mutable QMutex mutex;
    QWaitCondition wake;
    std::atomic_bool quit{false};
    // Guarded by mutex.
    bool openRequested = false, opened = false;
    QString clip;
    qint64 durationUs = 0;
    std::vector<qint64> keyframes;  // sorted, unique, clip time
    QSize tileSize;
    QHash<qint64, QImage> cache;    // keyframe time -> tile at tileSize
    struct Item { qint64 keyframe; QList<qint64> times; };
    std::deque<Item> queue;
    Stats stats;

    qint64 keyframeLocked(qint64 t) const {
        const auto next = std::upper_bound(keyframes.begin(), keyframes.end(), t);
        return next == keyframes.begin() ? t : *(next - 1);
    }
    // Emits on the owner's thread, queued.
    void deliver(qint64 requested, qint64 keyframe, const QImage& tile) {
        QMetaObject::invokeMethod(owner, [o = owner, requested, keyframe, tile] { emit o->tileReady(requested, keyframe, tile); }, Qt::QueuedConnection);
    }
    void report(const QString& message) {
        QMetaObject::invokeMethod(owner, [o = owner, message] { emit o->failed(message); }, Qt::QueuedConnection);
    }
    void learnLocked(qint64 keyframe) {
        const auto at = std::lower_bound(keyframes.begin(), keyframes.end(), keyframe);
        if (at == keyframes.end() || *at != keyframe) keyframes.insert(at, keyframe);
    }

    void run() override {
        Source source(&quit);
        SwsContext* scaler = nullptr;
        FramePtr tileFrame;
        FramePtr decoded(av_frame_alloc());
        while (true) {
            Item item;
            QSize size;
            QString clipPath;
            qint64 openDuration = 0;
            bool open = false;
            {
                QMutexLocker lock(&mutex);
                while (!quit && !openRequested && !(opened && !queue.empty())) wake.wait(&mutex);
                if (quit) break;
                if (openRequested) { openRequested = false; clipPath = clip; openDuration = durationUs; open = true; }
                else { item = std::move(queue.front()); queue.pop_front(); size = tileSize; }
            }
            if (open) {
                const QString error = source.open(clipPath);
                if (!error.isEmpty()) {
                    if (!quit) report(error);
                    QMutexLocker lock(&mutex);
                    opened = false;
                    queue.clear();
                    continue;
                }
                // Matroska reads its cue index on the first seek, not at open:
                // before that the index holds one entry and every time would
                // look like part of the first GOP.
                av_seek_frame(source.format, source.stream, source.streamTime(std::max<qint64>(0, openDuration / 2)), AVSEEK_FLAG_BACKWARD);
                std::vector<qint64> found;
                AVStream* track = source.format->streams[source.stream];
                for (int i = 0, n = avformat_index_get_entries_count(track); i < n; ++i) {
                    const AVIndexEntry* entry = avformat_index_get_entry(track, i);
                    if (entry && (entry->flags & AVINDEX_KEYFRAME)) found.push_back(source.clipTime(entry->timestamp));
                }
                std::sort(found.begin(), found.end());
                found.erase(std::unique(found.begin(), found.end()), found.end());
                QMutexLocker lock(&mutex);
                keyframes = std::move(found);
                opened = true;
                continue;
            }
            // A request made while the previous decode ran may have cached it.
            {
                QMutexLocker lock(&mutex);
                const qint64 known = keyframeLocked(item.times.first());
                const auto hit = cache.constFind(known);
                if (hit != cache.constEnd() && size == tileSize) {
                    const QImage tile = *hit;
                    stats.cached += int(item.times.size());
                    lock.unlock();
                    for (const qint64 t : std::as_const(item.times)) deliver(t, known, tile);
                    continue;
                }
            }
            qint64 keyPts = 0;
            if (!source.seekKey(source.streamTime(std::max<qint64>(0, item.times.first()))) || !source.decodeKey(decoded.get(), keyPts)) continue;
            const qint64 keyUs = source.clipTime(keyPts);
            // Straight to the tile size; the scaler and its buffer outlive one tile.
            if (!tileFrame || tileFrame->width != size.width() || tileFrame->height != size.height()) {
                tileFrame.reset(av_frame_alloc());
                tileFrame->format = AV_PIX_FMT_BGRA;
                tileFrame->width = size.width();
                tileFrame->height = size.height();
                if (av_frame_get_buffer(tileFrame.get(), 64) < 0) { tileFrame.reset(); continue; }
            }
            scaler = sws_getCachedContext(scaler, decoded->width, decoded->height, AVPixelFormat(decoded->format), size.width(), size.height(), AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!scaler) continue;
            sws_scale(scaler, decoded->data, decoded->linesize, 0, decoded->height, tileFrame->data, tileFrame->linesize);
            av_frame_unref(decoded.get());
            QImage tile(size, QImage::Format_RGB32);
            for (int y = 0; y < size.height(); ++y) std::memcpy(tile.scanLine(y), tileFrame->data[0] + qint64(y) * tileFrame->linesize[0], size_t(size.width()) * 4);
            {
                QMutexLocker lock(&mutex);
                if (quit) break;
                learnLocked(keyUs);
                if (size != tileSize) continue;  // asked for another size meanwhile
                if (cache.contains(keyUs)) stats.cached += int(item.times.size());
                else { cache.insert(keyUs, tile); ++stats.decoded; }
            }
            for (const qint64 t : std::as_const(item.times)) deliver(t, keyUs, tile);
        }
        if (scaler) sws_freeContext(scaler);
    }
};

Filmstrip::Filmstrip(QObject* parent) : QObject(parent), d_(std::make_unique<Private>(this)) {
    d_->start();
}
Filmstrip::~Filmstrip() = default;  // Private's destructor stops and joins the worker.

void Filmstrip::open(const QString& clip, qint64 durationUs, const QSize&) {
    QMutexLocker lock(&d_->mutex);
    d_->clip = clip;
    d_->durationUs = durationUs;
    d_->openRequested = true;
    d_->opened = false;
    d_->keyframes.clear();
    d_->cache.clear();
    d_->queue.clear();
    d_->wake.wakeAll();
}
void Filmstrip::request(const QList<qint64>& timesUs, const QSize& tileSize) {
    if (tileSize.width() < 1 || tileSize.height() < 1) return;
    struct Hit { qint64 requested, keyframe; QImage tile; };
    QList<Hit> hits;
    {
        QMutexLocker lock(&d_->mutex);
        if (tileSize != d_->tileSize) { d_->cache.clear(); d_->tileSize = tileSize; }
        if (!d_->queue.empty()) ++d_->stats.replaced;
        d_->queue.clear();
        int count = 0;
        for (qint64 t : timesUs) {
            if (++count > maximumTiles) break;
            t = std::clamp<qint64>(t, 0, std::max<qint64>(0, d_->durationUs));
            const qint64 keyframe = d_->keyframeLocked(t);
            if (const auto hit = d_->cache.constFind(keyframe); hit != d_->cache.constEnd()) {
                hits.append({t, keyframe, *hit});
                ++d_->stats.cached;
                continue;
            }
            const auto same = std::find_if(d_->queue.begin(), d_->queue.end(), [&](const Private::Item& i) { return i.keyframe == keyframe; });
            if (same != d_->queue.end()) same->times.append(t);
            else d_->queue.push_back({keyframe, {t}});
        }
        d_->wake.wakeAll();
    }
    for (const auto& hit : std::as_const(hits)) d_->deliver(hit.requested, hit.keyframe, hit.tile);
}
void Filmstrip::cancel() {
    QMutexLocker lock(&d_->mutex);
    d_->queue.clear();
}
QImage Filmstrip::tile(qint64 timeUs) const {
    QMutexLocker lock(&d_->mutex);
    return d_->cache.value(d_->keyframeLocked(timeUs));
}
qint64 Filmstrip::keyframeAtOrBefore(qint64 timeUs) const {
    QMutexLocker lock(&d_->mutex);
    return d_->keyframeLocked(timeUs);
}
Filmstrip::Stats Filmstrip::stats() const {
    QMutexLocker lock(&d_->mutex);
    return d_->stats;
}

// ---- FrameSkimmer ----

struct FrameSkimmer::Private final : QThread {
    explicit Private(FrameSkimmer* o) : owner(o) {}
    ~Private() override { shutdownWorker(*this); }

    FrameSkimmer* owner;
    QMutex mutex;
    QWaitCondition wake;
    std::atomic_bool quit{false};
    std::atomic_bool newer{false};      // a request is waiting behind the one in flight
    std::atomic<quint64> cancelled{0};  // advances on cancel(): answers older than it are dropped
    // Guarded by mutex.
    bool openRequested = false, opened = false, havePending = false, openFailed = false;
    QString clip, openError;
    QSize canvas;
    qint64 pendingTime = 0;
    RecordingEdit pendingEdit;
    int pendingEdge = 0;

    void fail(qint64 requested, quint64 generation, const QString& message) {
        QMetaObject::invokeMethod(owner, [o = owner, d = this, generation, requested, message] {
            if (d->cancelled.load() == generation) emit o->frameFailed(requested, message);
        }, Qt::QueuedConnection);
    }
    void run() override {
        Source source(&quit);
        FramePtr frame(av_frame_alloc());
        // The last decoded frame and the request it answered (stream units): a
        // request between the two is answered by it again (a new edit at the same
        // time is rendered without decoding), and a later one in the same GOP is
        // decoded forward from it without a seek (a scrub or a held key).
        FramePtr kept(av_frame_alloc());
        bool haveKept = false, decoderFollowsKept = false;
        qint64 keptRequest = 0, keptPts = 0;
        SwsContext* scaler = nullptr;
        while (true) {
            qint64 time = 0;
            RecordingEdit edit;
            int edge = 0;
            QSize canvasSize;
            QString clipPath;
            quint64 generation = 0;
            bool open = false;
            {
                QMutexLocker lock(&mutex);
                while (!quit && !openRequested && !(opened && havePending)) wake.wait(&mutex);
                if (quit) break;
                if (openRequested) { openRequested = false; clipPath = clip; open = true; }
                else {
                    havePending = false;
                    newer = false;
                    time = pendingTime;
                    edit = pendingEdit;
                    edge = pendingEdge;
                    canvasSize = canvas;
                    generation = cancelled.load();
                }
            }
            if (open) {
                const QString error = source.open(clipPath);
                av_frame_unref(kept.get());
                haveKept = decoderFollowsKept = false;
                QMutexLocker lock(&mutex);
                opened = error.isEmpty();
                openFailed = !opened;
                openError = error;
                if (!opened) {
                    // A request waiting for the open is answered with the reason.
                    if (havePending && !quit) fail(pendingTime, cancelled.load(), error);
                    havePending = false;
                    if (!quit) QMetaObject::invokeMethod(owner, [o = owner, error] { emit o->failed(error); }, Qt::QueuedConnection);
                }
                continue;
            }
            const auto abort = [&] { return quit.load() || cancelled.load() != generation; };
            const auto yield = [&] { return newer.load(); };
            const qint64 target = source.streamTime(std::max<qint64>(0, time));
            bool partial = false;
            if (!(haveKept && keptRequest <= target && target <= keptPts)) {
                const auto key = decoderFollowsKept && haveKept && target > keptPts ? source.indexedKeyframe(target) : std::nullopt;
                const bool forward = key && *key <= keptPts && !source.drained;
                qint64 pts = 0;
                if (!forward && !source.seekKey(target)) {
                    haveKept = decoderFollowsKept = false;
                    if (!abort()) fail(time, generation, QStringLiteral("Reading the working clip failed"));
                    continue;
                }
                const bool decoded = source.decodeTo(target, frame.get(), pts, abort, yield, partial);
                if (abort()) { decoderFollowsKept = false; continue; }
                if (!decoded) {
                    haveKept = decoderFollowsKept = false;
                    fail(time, generation, QStringLiteral("No frame could be decoded at this time"));
                    continue;
                }
                av_frame_unref(kept.get());
                av_frame_move_ref(kept.get(), frame.get());
                haveKept = true;
                // A partial frame answers only its own time.
                keptRequest = partial ? pts : target;
                keptPts = pts;
                decoderFollowsKept = !source.drained;
            }
            const qint64 frameUs = source.clipTime(keptPts);
            QImage image;
            if (!renderScaled(scaler, kept.get(), edit, frameUs, time, canvasSize, edge, image)) {
                if (!abort()) fail(time, generation, QStringLiteral("Cannot convert the frame"));
                continue;
            }
            if (abort()) continue;
            // A cancel that lands after the post still drops the answer: checked again on delivery.
            const quint64 revision = edit.revision;
            const bool exact = !partial;
            QMetaObject::invokeMethod(owner, [o = owner, d = this, generation, time, frameUs, image, revision, exact] {
                if (d->cancelled.load() == generation) emit o->frameReady(time, frameUs, image, revision, exact);
            }, Qt::QueuedConnection);
        }
        if (scaler) sws_freeContext(scaler);
    }
    // The frame scaled straight to the preview size (long edge at most maxEdge,
    // as fitPreview sizes it) with the covers active at its time, or at the
    // time it is shown for, painted at that size: a 4K frame is never
    // converted at full size for a 1280 px viewer.
    static bool renderScaled(SwsContext*& scaler, const AVFrame* frame, const RecordingEdit& edit, qint64 frameUs, qint64 requestedUs, const QSize& canvas, int maxEdge, QImage& out) {
        const QSize full(frame->width, frame->height);
        const QSize size = recording::outputSize(full, maxEdge);
        if (size.isEmpty()) return false;
        scaler = sws_getCachedContext(scaler, frame->width, frame->height, AVPixelFormat(frame->format), size.width(), size.height(), AV_PIX_FMT_BGRA, size == full ? SWS_POINT : SWS_AREA, nullptr, nullptr, nullptr);
        if (!scaler) return false;
        QImage image(size, QImage::Format_RGB32);
        uint8_t* planes[4] = {image.bits(), nullptr, nullptr, nullptr};
        const int strides[4] = {int(image.bytesPerLine()), 0, 0, 0};
        sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
        for (const auto& mask : edit.masks) {
            const auto active = [&](qint64 t) { return mask.start <= t && t < mask.end; };
            if (active(frameUs) || active(requestedUs)) paintCoverPreview(image, mask, canvas.isEmpty() ? full : canvas);
        }
        out = image;
        return true;
    }
};

FrameSkimmer::FrameSkimmer(QObject* parent) : QObject(parent), d_(std::make_unique<Private>(this)) {
    d_->start();
}
FrameSkimmer::~FrameSkimmer() = default;

void FrameSkimmer::open(const QString& clip, const QSize& canvas) {
    QMutexLocker lock(&d_->mutex);
    d_->clip = clip;
    d_->canvas = canvas;
    d_->openRequested = true;
    d_->opened = false;
    d_->openFailed = false;
    d_->havePending = false;
    d_->wake.wakeAll();
}
void FrameSkimmer::request(qint64 timeUs, const RecordingEdit& edit, int maxEdge) {
    QMutexLocker lock(&d_->mutex);
    if (d_->openFailed) {
        // The clip could not be opened: every request is answered with why.
        d_->fail(timeUs, d_->cancelled.load(), d_->openError);
        return;
    }
    d_->pendingTime = timeUs;
    d_->pendingEdit = edit;
    d_->pendingEdge = std::max(1, maxEdge);
    d_->havePending = true;
    d_->newer = true;
    d_->wake.wakeAll();
}
void FrameSkimmer::cancel() {
    QMutexLocker lock(&d_->mutex);
    d_->havePending = false;
    d_->newer = false;
    d_->cancelled.fetch_add(1);
}
bool FrameSkimmer::failedToOpen() const {
    QMutexLocker lock(&d_->mutex);
    return d_->openFailed;
}
}
