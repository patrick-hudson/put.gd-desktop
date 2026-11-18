#include "VaapiFrames.h"
#include "AdaptiveRate.h"
#include "ClipExporter.h"
#include "DebugLog.h"
#include "core/KeptSpans.h"
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QStringList>
#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>
#include <cstring>
#include <memory>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

namespace putgd::record {
namespace {
QString avError(int code) { char text[AV_ERROR_MAX_STRING_SIZE] = {}; av_strerror(code, text, sizeof text); return QString::fromUtf8(text); }
Error fail(const QString& code, const QString& message, int av = 0) { return {code, av ? message + ": " + avError(av) : message, {}}; }
// Closes a buffer source. AVERROR_EOF only means the graph already ended, for example at the trim end.
int flushSource(AVFilterContext* source) {
    const int rc = av_buffersrc_add_frame_flags(source, nullptr, 0);
    return rc >= 0 || rc == AVERROR_EOF ? 0 : rc;
}
constexpr AVRational microseconds{1, 1000000};
QString seconds(qint64 us) { return QString::number(us / 1000000.0, 'f', 6); }
// A mask's output intervals, in output frames (ticks of 1/fps), as one
// timeline expression, one gte(t,a)*lt(t,b) term per interval. Frame n is at
// t = n/fps, which can land a hair either side of the exact value: the bounds
// sit half a frame early, so a frame falls on the side it belongs to.
QString enable(const QList<recording::Span>& ticks, int fps) {
    QStringList terms;
    auto bound = [fps](qint64 tick) { return QString::number((double(tick) - 0.5) / fps, 'f', 9); };
    for (const auto& part : ticks) terms << QStringLiteral("gte(t\\,%1)*lt(t\\,%2)").arg(bound(part.start), bound(part.end));
    return QStringLiteral("enable='%1'").arg(terms.join('+'));
}

struct Input {
    AVFormatContext* format = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVPacket* pending = nullptr;  // first packet after a verified seek
    int videoIndex = -1, audioIndex = -1;
    ~Input() { if (pending) av_packet_free(&pending); if (video) avcodec_free_context(&video); if (audio) avcodec_free_context(&audio); if (format) avformat_close_input(&format); }
    // Reads the next packet, serving a pending one first.
    int read(AVPacket* packet) {
        if (pending) { av_packet_move_ref(packet, pending); av_packet_free(&pending); return 0; }
        return av_read_frame(format, packet);
    }
    // Seeks backward to the target video time and verifies the first video
    // packet precedes it; a Matroska clip whose first cluster has no cue point
    // otherwise lands on the next keyframe, so that case reads from the start.
    void seekTo(qint64 targetUs, const QString& path, bool wantAudio) {
        const auto timeBase = format->streams[videoIndex]->time_base;
        const auto origin = format->streams[videoIndex]->start_time == AV_NOPTS_VALUE ? 0 : format->streams[videoIndex]->start_time;
        const auto target = origin + av_rescale_q(targetUs, {1, 1000000}, timeBase);
        // A failed seek can still leave the demuxer parked elsewhere, and a
        // successful one may land on a cluster that starts mid-GOP, so the first
        // packet is checked whatever av_seek_frame reported.
        const int sought = av_seek_frame(format, videoIndex, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(video);
        if (audio) avcodec_flush_buffers(audio);
        AVPacket* packet = av_packet_alloc();
        bool restart = sought < 0;
        while (!restart && av_read_frame(format, packet) >= 0) {
            if (packet->stream_index != videoIndex) { av_packet_unref(packet); continue; }
            // Decoding must start on a keyframe at or before the target; a cluster
            // boundary inside a GOP would make the decoder drop frames until the next one.
            if (packet->pts != AV_NOPTS_VALUE && packet->pts <= target && (packet->flags & AV_PKT_FLAG_KEY)) { pending = packet; debugLog(QStringLiteral("seek ok: first key pts %1 target %2").arg(packet->pts).arg(target)); return; }
            debugLog(QStringLiteral("seek unusable: first pts %1 key %2 target %3").arg(packet->pts).arg(bool(packet->flags & AV_PKT_FLAG_KEY)).arg(target));
            restart = true;
        }
        av_packet_free(&packet);
        // Overshot or unseekable: start over from the beginning of the file.
        avformat_close_input(&format);
        if (video) avcodec_free_context(&video);
        if (audio) avcodec_free_context(&audio);
        videoIndex = audioIndex = -1;
        pending = nullptr;
        open(path, wantAudio);
        debugLog(QStringLiteral("seek restart from start (sought=%1)").arg(sought));
    }
    Error open(const QString& path, bool wantAudio) {
        int rc = avformat_open_input(&format, QFile::encodeName(path).constData(), nullptr, nullptr);
        if (rc < 0) return fail("recording_export_failed", "Cannot open the working clip", rc);
        if ((rc = avformat_find_stream_info(format, nullptr)) < 0) return fail("recording_export_failed", "Cannot read the working clip", rc);
        const AVCodec* decoder = nullptr;
        videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (videoIndex < 0 || !decoder) return fail("recording_export_failed", "The working clip has no video");
        video = avcodec_alloc_context3(decoder);
        if (!video || avcodec_parameters_to_context(video, format->streams[videoIndex]->codecpar) < 0) return fail("recording_export_failed", "Cannot decode the working clip");
        debugLog(QStringLiteral("decoder: %1 (%2) threads auto").arg(decoder->name, decoder->long_name));
        // libavcodec decodes on one thread unless told otherwise; a 3 MP H.264
        // clip then feeds even a GPU encoder at well under real time.
        video->thread_count = 0;
        video->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (avcodec_open2(video, decoder, nullptr) < 0) return fail("recording_export_failed", "Cannot decode the working clip");
        if (wantAudio) {
            const AVCodec* audioDecoder = nullptr;
            audioIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &audioDecoder, 0);
            if (audioIndex >= 0 && audioDecoder) {
                audio = avcodec_alloc_context3(audioDecoder);
                if (!audio || avcodec_parameters_to_context(audio, format->streams[audioIndex]->codecpar) < 0 || avcodec_open2(audio, audioDecoder, nullptr) < 0) return fail("recording_export_failed", "Cannot decode the clip audio");
            } else audioIndex = -1;
        }
        return {};
    }
};
struct Graph {
    AVFilterGraph* graph = nullptr;
    AVFilterContext* source = nullptr;
    AVFilterContext* palette = nullptr;
    AVFilterContext* sink = nullptr;
    ~Graph() { if (graph) avfilter_graph_free(&graph); }
    Error build(const QString& description, const AVCodecContext* decoder, AVRational timeBase, bool withPalette, const AVFrame* paletteFrame = nullptr) {
        graph = avfilter_graph_alloc();
        if (!graph) return fail("recording_export_failed", "Cannot allocate a filter graph");
        const auto args = QStringLiteral("video_size=%1x%2:pix_fmt=%3:time_base=%4/%5:pixel_aspect=1/1").arg(decoder->width).arg(decoder->height).arg(int(decoder->pix_fmt)).arg(timeBase.num).arg(timeBase.den);
        int rc = avfilter_graph_create_filter(&source, avfilter_get_by_name("buffer"), "in", args.toUtf8().constData(), nullptr, graph);
        if (rc < 0) return fail("recording_export_failed", "Cannot create the video source", rc);
        if (withPalette) {
            const auto pal = QStringLiteral("video_size=%1x%2:pix_fmt=%3:time_base=1/1:pixel_aspect=1/1").arg(paletteFrame->width).arg(paletteFrame->height).arg(int(paletteFrame->format));
            if ((rc = avfilter_graph_create_filter(&palette, avfilter_get_by_name("buffer"), "pal", pal.toUtf8().constData(), nullptr, graph)) < 0) return fail("recording_export_failed", "Cannot create the palette source", rc);
        }
        if ((rc = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph)) < 0) return fail("recording_export_failed", "Cannot create the video sink", rc);
        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in"); outputs->filter_ctx = source; outputs->pad_idx = 0; outputs->next = nullptr;
        if (withPalette) { AVFilterInOut* second = avfilter_inout_alloc(); second->name = av_strdup("pal"); second->filter_ctx = palette; second->pad_idx = 0; second->next = nullptr; outputs->next = second; }
        inputs->name = av_strdup("out"); inputs->filter_ctx = sink; inputs->pad_idx = 0; inputs->next = nullptr;
        rc = avfilter_graph_parse_ptr(graph, description.toUtf8().constData(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (rc < 0) return fail("recording_export_failed", "Invalid video filter graph", rc);
        if ((rc = avfilter_graph_config(graph, nullptr)) < 0) return fail("recording_export_failed", "Cannot configure the video filter graph", rc);
        return {};
    }
};
struct AudioGraph {
    AVFilterGraph* graph = nullptr;
    AVFilterContext* source = nullptr;
    AVFilterContext* sink = nullptr;
    ~AudioGraph() { if (graph) avfilter_graph_free(&graph); }
    Error build(const QString& description, const AVCodecContext* decoder, AVRational timeBase, int frameSize) {
        graph = avfilter_graph_alloc();
        char layout[64] = {};
        av_channel_layout_describe(&decoder->ch_layout, layout, sizeof layout);
        const auto args = QStringLiteral("time_base=%1/%2:sample_rate=%3:sample_fmt=%4:channel_layout=%5").arg(timeBase.num).arg(timeBase.den).arg(decoder->sample_rate).arg(av_get_sample_fmt_name(decoder->sample_fmt)).arg(QString::fromUtf8(layout));
        int rc = avfilter_graph_create_filter(&source, avfilter_get_by_name("abuffer"), "in", args.toUtf8().constData(), nullptr, graph);
        if (rc < 0) return fail("recording_export_failed", "Cannot create the audio source", rc);
        if ((rc = avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph)) < 0) return fail("recording_export_failed", "Cannot create the audio sink", rc);
        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in"); outputs->filter_ctx = source; outputs->pad_idx = 0; outputs->next = nullptr;
        inputs->name = av_strdup("out"); inputs->filter_ctx = sink; inputs->pad_idx = 0; inputs->next = nullptr;
        rc = avfilter_graph_parse_ptr(graph, description.toUtf8().constData(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (rc < 0) return fail("recording_export_failed", "Invalid audio filter graph", rc);
        if ((rc = avfilter_graph_config(graph, nullptr)) < 0) return fail("recording_export_failed", "Cannot configure the audio filter graph", rc);
        if (frameSize > 0) av_buffersink_set_frame_size(sink, unsigned(frameSize));
        return {};
    }
};
qint64 streamOrigin(const AVStream* stream) { return stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time; }
// #1613: the kept spans moved onto the clip's own frame grid. A span keeps the
// frames whose timestamps fall inside it, so it really starts on its first kept
// frame and ends where the first frame after it starts (or at the clip end when
// none follows). Laid end to end on those bounds, each kept frame lasts what it
// lasted in the clip and a seam is one frame step like any other: nothing is
// repeated, dropped or shifted there. Video selection, output timestamps, mask
// intervals, the audio layout and progress all read this one map; it differs
// from the edit's own KeptSpans by less than a frame per span bound, and the
// audio heard is the edit's own kept time only (KeptAudio, #1635).
//
// Frames do not always come at the capture rate: after the adaptive rate steps
// down the encoder never sees videorate's repeats (#1045), and a static
// compositor stream resends only once a second. When a span's first frame lies
// more than one floor interval (1/AdaptiveRate::floorFps) past its start S,
// snapping would drop the narration over that still picture, so the span
// starts at S instead and opens on the frame on screen at S: the last frame
// before S, which may lie in a cut or before the trim (`held`). That is the
// right picture for that moment, so audio and video stay in sync. A span whose
// start has no frame before it at all still snaps forward. The mirror holds at
// a span's end E (the trim end, or a cut's start): when the frame after E comes
// more than one floor interval later, the span ends at E exactly, the last kept
// frame shown until E and the audio stopped there, so nothing after E (above
// all, nothing a cut removed) reaches the output.
//
// #1635: the output is whole frames at the output rate (ticks of 1/fps). Span
// i ends on tick round(K_i × fps), K_i the edit's kept time up to its end, so
// the output lasts the kept time the review clock shows, rounded to a frame,
// however many seams there are; every span that has any keeps at least one
// tick while the total allows. A frame lands on the tick nearest its time from
// its span's start; the last picture of a span that rounds past the span's
// last tick still gets that tick when the picture there is only a repeat. An
// animation keeps at least one tick and no more than fit in the edit's kept
// time, the length its limit is checked on.
//
// Container times are rounded (Matroska counts milliseconds) while the review
// snaps edges to round(k / fps) µs, so a frame's time can sit up to half a
// container tick (slackUs) before the edge it truly lies on: frame 154 of a
// 30 fps clip is stored at 5133 ms for 5.133333 s. Edges are compared with
// that slack, so such a frame counts as on the edge, not before it.
struct FrameGrid {
    recording::KeptSpans map;       // snapped source spans
    std::vector<qint64> keyframes;  // keyframe times the scan saw (µs from the video origin), ascending
    // Per map span: the time of the frame it opens on when that frame lies before its start.
    std::vector<std::optional<qint64>> held;
    // Every frame time the scans saw; complete over each mask's kept parts and a
    // little past them (maskTicks needs the frame after each).
    std::set<qint64> frames;
    qint64 slackUs = 0;
    int fps = 0;
    std::vector<qint64> firstTick, ticks;  // per map span: its first output tick and its length in ticks
    qint64 totalTicks = 0;
    // The keyframe decoding has to start from to reach `us`.
    qint64 keyframeAtOrBefore(qint64 us) const {
        const auto it = std::upper_bound(keyframes.begin(), keyframes.end(), us);
        return it == keyframes.begin() ? std::numeric_limits<qint64>::min() : *std::prev(it);
    }
    // The first frame time span i needs decoded: its held frame, or its start.
    qint64 decodeFrom(qsizetype i) const { return held[size_t(i)] ? *held[size_t(i)] : map.spans()[i].start; }
    qint64 outputUs() const { return av_rescale(totalTicks, 1000000, fps); }
    // Lays the spans on the output grid at `rate` by the edit's kept time.
    void layOut(int rate, const recording::KeptSpans& kept, bool animation) {
        fps = rate;
        const auto& spans = map.spans();
        const qsizetype n = spans.size();
        auto round = [this](qint64 us) { return (us * fps + 500000) / 1000000; };
        std::vector<qint64> bound(size_t(n), 0);
        for (qsizetype i = 0; i < n; ++i) bound[size_t(i)] = round(i + 1 == n ? kept.outputDuration() : kept.toOutput(spans[i].end));
        qint64 total = n ? bound.back() : 0;
        if (animation) total = std::min(total, kept.outputDuration() * fps / 1000000);
        totalTicks = n ? std::max<qint64>(1, total) : 0;
        // Monotone, ending on the total, and one tick per span while there are enough.
        const bool everySpan = totalTicks >= n;
        qint64 previous = 0;
        for (qsizetype i = 0; i < n; ++i) {
            const qint64 lo = previous + (everySpan ? 1 : 0), hi = totalTicks - (everySpan ? n - 1 - i : 0);
            bound[size_t(i)] = i + 1 == n ? totalTicks : std::clamp(bound[size_t(i)], lo, hi);
            previous = bound[size_t(i)];
        }
        firstTick.assign(size_t(n), 0);
        ticks.assign(size_t(n), 0);
        for (qsizetype i = 0; i < n; ++i) {
            firstTick[size_t(i)] = i ? bound[size_t(i - 1)] : 0;
            ticks[size_t(i)] = bound[size_t(i)] - firstTick[size_t(i)];
        }
    }
    // The output tick a frame of span i at source time us lands on; past the
    // span's last tick (firstTick + ticks) when it rounds beyond the span.
    qint64 tickOf(qsizetype i, qint64 us) const {
        const qint64 offset = (std::max<qint64>(0, us - map.spans()[i].start) * fps + 500000) / 1000000;
        return firstTick[size_t(i)] + std::min(offset, ticks[size_t(i)]);
    }
    // #1635: the output ticks a mask covers. A cover applies to an output frame
    // when the frame's display interval [tick, next tick), mapped back into its
    // span, overlaps the cover, or when the cover is active at the source time
    // of the picture the frame shows. A frame shown on past its own time (a
    // frame gap, a span held on the frame before its start) keeps the cover it
    // was captured under; a cover inside a gap covers the still picture under
    // it; and a frame a container presents a little late (a GIF counts
    // centiseconds) is covered for its whole interval.
    QList<recording::Span> maskTicks(const RecordingMask& mask) const {
        QList<recording::Span> parts;
        const auto& spans = map.spans();
        auto floorTicks = [this](qint64 us) { return us <= 0 ? 0 : us * fps / 1000000; };
        auto ceilTicks = [this](qint64 us) { return us <= 0 ? 0 : (us * fps + 999999) / 1000000; };
        auto active = [&](qint64 picture) { return mask.start - slackUs <= picture && picture < mask.end - slackUs; };
        for (qsizetype i = 0; i < spans.size(); ++i) {
            const auto& span = spans[i];
            const qint64 first = firstTick[size_t(i)], end = first + ticks[size_t(i)];
            if (ticks[size_t(i)] == 0) continue;
            // Output time: tick n of the span shows source time [start + n/fps, start + (n+1)/fps).
            const qint64 from = first + floorTicks(mask.start - span.start), to = std::min(end, first + ceilTicks(mask.end - span.start));
            if (from < to) parts.append({from, to});
            // Picture time: each frame the span shows, from its tick to the next one's.
            const auto shownFrom = [&](std::set<qint64>::const_iterator it) { return it != frames.end() && *it < span.end ? std::min(end, tickOf(i, *it)) : end; };
            if (const auto& h = held[size_t(i)]; h && active(*h)) parts.append({first, shownFrom(frames.lower_bound(span.start))});
            for (auto it = frames.lower_bound(std::max(mask.start - slackUs, span.start)); it != frames.end() && *it < span.end && active(*it); ++it) {
                const qint64 tick = tickOf(i, *it);
                const auto next = std::next(it);
                // The span's last picture, rounded past its end, may take its last tick (driveVideo).
                if (tick >= end) { if (next == frames.end() || *next >= span.end) parts.append({end - 1, end}); continue; }
                const recording::Span shown{tick, shownFrom(next)};
                if (!shown.empty()) parts.append(shown);
            }
        }
        std::sort(parts.begin(), parts.end(), [](const recording::Span& a, const recording::Span& b) { return a.start < b.start; });
        QList<recording::Span> merged;
        for (const auto& part : std::as_const(parts)) {
            if (!merged.isEmpty() && part.start <= merged.last().end) merged.last().end = std::max(merged.last().end, part.end);
            else merged.append(part);
        }
        return merged;
    }
};
// The longest wait for a span's first frame that still starts the span on it,
// and the longest a span's last frame runs past its end.
constexpr qint64 floorIntervalUs = 1000000 / AdaptiveRate::floorFps;
// Video frame times in microseconds from the clip's first frame (the timeline
// every edit uses), read from the packets: a demux-only pass, nothing decoded.
// Also collects every frame time over each mask's kept parts (FrameGrid::frames).
Error snapToFrames(const QString& path, const recording::KeptSpans& kept, const QList<RecordingMask>& masks, const std::atomic_bool& cancelled, FrameGrid& grid);
// RGB → rate → masks → scale. Frames reach the graph already selected and
// stamped in output ticks (driveVideo), so the same chain feeds every format
// and both GIF passes. The rate filter comes first so that every output frame,
// a repeat in a frame gap included, is masked on its own (FrameGrid::maskTicks).
QString baseVideoGraph(const ExportRequest& request, const FrameGrid& grid, const QSize& outputSize) {
    // fps holds each frame until the next one's tick and the last one until the
    // source closes at the last tick (driveVideo); trim ends on it exactly.
    QString graph = QStringLiteral("[in]format=rgb24,fps=%1:start_time=0,trim=end_frame=%2").arg(grid.fps).arg(grid.totalTicks);
    int label = 0;
    for (const auto& mask : request.edit.masks) {
        // A cover over a cut is one interval; one wholly in a cut or outside the trim has none.
        const auto parts = grid.maskTicks(mask);
        if (parts.isEmpty()) continue;
        const auto r = mask.rect.intersected(QRect(QPoint(0, 0), request.canvas));
        if (r.width() < 2 || r.height() < 2) continue;
        const auto when = enable(parts, grid.fps);
        if (mask.kind == "solid") graph += QStringLiteral(",drawbox=x=%1:y=%2:w=%3:h=%4:color=black@1:t=fill:%5").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(when);
        else {
            const int radius = std::clamp(mask.strength, 2, std::max(2, std::min(r.width(), r.height()) / 2 - 1));
            // Two box passes, as the preview draws them. FFmpeg's boxblur is GPL
            // only; avgblur is LGPL, so the AppImage's own FFmpeg has it too.
            graph +=QStringLiteral("[v%1];[v%1]split[v%1a][v%1b];[v%1b]crop=w=%2:h=%3:x=%4:y=%5,avgblur=sizeX=%6,avgblur=sizeX=%6[m%1];[v%1a][m%1]overlay=x=%4:y=%5:%7").arg(label).arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y()).arg(radius).arg(when);
            ++label;
        }
    }
    if (outputSize != request.canvas) graph += QStringLiteral(",scale=w=%1:h=%2:flags=lanczos").arg(outputSize.width()).arg(outputSize.height());
    return graph;
}
struct Encoder {
    AVCodecContext* context = nullptr;
    AVStream* stream = nullptr;
    QString name;
    // Set for VA-API: frames are uploaded into this pool before encoding.
    AVBufferRef* device = nullptr;
    AVBufferRef* frames = nullptr;
    bool hardware() const { return frames != nullptr; }
    ~Encoder() { if (context) avcodec_free_context(&context); if (frames) av_buffer_unref(&frames); if (device) av_buffer_unref(&device); }
};
// Encoder candidates in preference order. NVENC runs on an NVIDIA GPU and
// takes system-memory frames, so it is tried first where libavcodec knows it;
// opening fails without the driver and the next candidate is used. The lab
// override PUTGD_RECORD_EXPORT_ENCODERS (comma list) replaces the order.
QStringList videoEncoderCandidates(const RecordingConfig& config) {
    const auto override = qEnvironmentVariable("PUTGD_RECORD_EXPORT_ENCODERS");
    if (!override.isEmpty()) return override.split(',', Qt::SkipEmptyParts);
    if (config.format == "mp4" && config.codec == "h264") return {"h264_nvenc", "h264_vaapi", "libx264", "libopenh264"};
    if (config.format == "mp4" && config.codec == "h265") return {"hevc_nvenc", "hevc_vaapi", "libx265"};
    if (config.format == "webm") return {"libvpx-vp9"};
    if (config.format == "gif") return {"gif"};
    return {"libwebp_anim"};
}
Error openVideoEncoder(AVFormatContext* output, const RecordingConfig& config, const QSize& size, int fps, AVPixelFormat format, AVRational timeBase, Encoder& out) {
    Error lastError{"recording_unsupported", "No encoder is available for this format", {{"format", config.format}, {"codec", config.codec}}};
    for (const auto& name : videoEncoderCandidates(config)) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.toUtf8().constData());
        if (!codec) continue;
        if (out.context) avcodec_free_context(&out.context);
        if (out.frames) av_buffer_unref(&out.frames);
        if (out.device) av_buffer_unref(&out.device);
        const bool vaapi = name.endsWith("_vaapi");
        if (vaapi && !openVaapiFrames(size, 16, out.device, out.frames)) { lastError = {"recording_export_failed", "No VA-API device accepts " + name, {}}; continue; }
        out.name = name;
        out.context = avcodec_alloc_context3(codec);
        auto* c = out.context;
        c->width = size.width(); c->height = size.height(); c->pix_fmt = format; c->time_base = timeBase; c->framerate = {fps, 1};
        if (vaapi) { c->pix_fmt = AV_PIX_FMT_VAAPI; c->hw_device_ctx = av_buffer_ref(out.device); c->hw_frames_ctx = av_buffer_ref(out.frames); }
        c->gop_size = std::max(1, fps * 2); c->max_b_frames = 0;
        // VA-API encoders get no global header. Their extradata is libavcodec's
        // own parameter sets, but AMD radeonsi rewrites the ones it emits
        // in-band to what it actually coded (Mesa 26.2 on a Radeon 740M: HEVC
        // max_transform_hierarchy_depth 4 where the extradata says 0, H.264
        // transform_8x8_mode_flag 0 where it says 1). Since FFmpeg 8.0 the MP4
        // muxer drops in-band parameter sets from hvc1 tracks (f0051604c7), so
        // a decoder saw only the wrong ones and HEVC exports decoded as garbage.
        // Without extradata the muxer builds hvcC/avcC from the first packet's
        // in-band sets, which match the slices on every driver.
        if ((output->oformat->flags & AVFMT_GLOBALHEADER) && !vaapi) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        // Apple players need the hvc1 sample entry for HEVC in MP4.
        if (config.format == "mp4" && config.codec == "h265") c->codec_tag = MKTAG('h', 'v', 'c', '1');
        if (name == "libx264") { av_opt_set(c->priv_data, "preset", "veryfast", 0); av_opt_set(c->priv_data, "crf", "23", 0); av_opt_set(c->priv_data, "profile", "high", 0); }
        else if (name == "libopenh264") { c->bit_rate = qint64(std::clamp(double(size.width()) * size.height() * fps * 0.1, 1500000.0, 20000000.0)); av_opt_set(c->priv_data, "profile", "constrained_baseline", 0); }
        else if (name == "libx265") { av_opt_set(c->priv_data, "preset", "faster", 0); av_opt_set(c->priv_data, "crf", "26", 0); av_opt_set(c->priv_data, "x265-params", "log-level=none", 0); }
        // Constant QP is the rate control every VA driver (Intel iHD, AMD radeonsi) offers.
        else if (vaapi) { c->bit_rate = 0; av_opt_set(c->priv_data, "rc_mode", "CQP", 0); c->global_quality = name == "h264_vaapi" ? 24 : 26; }
        else if (name == "h264_nvenc" || name == "hevc_nvenc") { c->bit_rate = 0; av_opt_set(c->priv_data, "preset", "p4", 0); av_opt_set(c->priv_data, "tune", "hq", 0); av_opt_set(c->priv_data, "rc", "vbr", 0); av_opt_set(c->priv_data, "cq", name == "h264_nvenc" ? "26" : "28", 0); av_opt_set(c->priv_data, "b_ref_mode", "disabled", 0); }
        else if (name == "libvpx-vp9") { c->bit_rate = 0; av_opt_set(c->priv_data, "crf", "32", 0); av_opt_set(c->priv_data, "row-mt", "1", 0); av_opt_set(c->priv_data, "cpu-used", "5", 0); av_opt_set(c->priv_data, "deadline", "realtime", 0); c->thread_count = 0; }
        else if (name == "libwebp_anim") { av_opt_set(c->priv_data, "quality", "70", 0); av_opt_set(c->priv_data, "lossless", "0", 0); }
        const int rc = avcodec_open2(c, codec, nullptr);
        if (rc == 0) break;
        lastError = fail("recording_export_failed", "Cannot open the video encoder " + name, rc);
        avcodec_free_context(&out.context);
        if (out.frames) av_buffer_unref(&out.frames);
        if (out.device) av_buffer_unref(&out.device);
        out.name.clear();
    }
    if (!out.context) return lastError;
    auto* c = out.context;
    out.stream = avformat_new_stream(output, nullptr);
    if (!out.stream || avcodec_parameters_from_context(out.stream->codecpar, c) < 0) return fail("recording_export_failed", "Cannot create the video stream");
    out.stream->time_base = c->time_base;
    out.stream->avg_frame_rate = c->framerate;
    return {};
}
Error openAudioEncoder(AVFormatContext* output, const RecordingConfig& config, Encoder& out) {
    const char* name = config.format == "webm" ? "libopus" : "aac";
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (!codec) return {"recording_unsupported", "No audio encoder is available for this format", {{"format", config.format}}};
    out.name = name;
    out.context = avcodec_alloc_context3(codec);
    auto* c = out.context;
    c->sample_rate = 48000;
    av_channel_layout_default(&c->ch_layout, 2);
    c->sample_fmt = config.format == "webm" ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_FLTP;
    c->bit_rate = 128000;
    c->time_base = {1, 48000};
    if (output->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    const int rc = avcodec_open2(c, codec, nullptr);
    if (rc < 0) return fail("recording_export_failed", "Cannot open the audio encoder", rc);
    out.stream = avformat_new_stream(output, nullptr);
    if (!out.stream || avcodec_parameters_from_context(out.stream->codecpar, c) < 0) return fail("recording_export_failed", "Cannot create the audio stream");
    out.stream->time_base = c->time_base;
    return {};
}
Error encodeFrame(AVFormatContext* output, Encoder& encoder, AVFrame* frame, AVPacket* packet) {
    // VA-API encodes from its own surfaces: upload the NV12 frame first.
    AVFrame* uploaded = nullptr;
    if (frame && encoder.hardware()) {
        uploaded = av_frame_alloc();
        if (!uploaded || av_hwframe_get_buffer(encoder.frames, uploaded, 0) < 0 || av_hwframe_transfer_data(uploaded, frame, 0) < 0) { av_frame_free(&uploaded); return fail("recording_export_failed", "Cannot upload a frame to the GPU"); }
        uploaded->pts = frame->pts;
        uploaded->pict_type = frame->pict_type;
        frame = uploaded;
    }
    int rc = avcodec_send_frame(encoder.context, frame);
    av_frame_free(&uploaded);
    if (rc < 0) return fail("recording_export_failed", "Encoding failed", rc);
    for (;;) {
        rc = avcodec_receive_packet(encoder.context, packet);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
        if (rc < 0) return fail("recording_export_failed", "Encoding failed", rc);
        av_packet_rescale_ts(packet, encoder.context->time_base, encoder.stream->time_base);
        packet->stream_index = encoder.stream->index;
        rc = av_interleaved_write_frame(output, packet);
        av_packet_unref(packet);
        if (rc < 0) return fail("recording_export_failed", "Writing the output failed", rc);
    }
}
struct FrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
// The GIF palette from the frames pass two encodes. palettegen's diff mode
// counts the first frame in full and then, wherever a pixel changes, the colour
// it changes from (FFmpeg 8.0 vf_palettegen.c, update_histogram_diff), so a
// colour that appears and stays to the end (a cover reaching the trim end, a
// last slide) was never counted and came out in the nearest earlier colour.
// After the last frame one more goes in with every byte inverted: every pixel
// changes, so the last frame is counted in full too, and the inverted frame,
// never followed by another, counts nothing.
//
// #1635: a frame added to a buffer source only waits in its queue until a sink
// asks for output, and palettegen answers only at EOF, so without a pull after
// every frame the whole export queued up as RGB (2.6 GiB for a 60 s 720p GIF).
struct PaletteStats {
    AVFilterGraph* graph = nullptr;
    AVFilterContext* source = nullptr;
    AVFilterContext* sink = nullptr;
    FramePtr last, early;  // early: a palette the graph gave before EOF (palettegen never does)
    ~PaletteStats() { if (graph) avfilter_graph_free(&graph); }
    // Frames arrive as packed RGB24 (the palette pass ends on format=rgb24).
    Error add(const AVFrame* frame, AVRational timeBase) {
        if (!graph) {
            graph = avfilter_graph_alloc();
            if (!graph) return fail("recording_export_failed", "Cannot allocate a filter graph");
            const auto args = QStringLiteral("video_size=%1x%2:pix_fmt=%3:time_base=%4/%5:pixel_aspect=1/1").arg(frame->width).arg(frame->height).arg(frame->format).arg(timeBase.num).arg(timeBase.den);
            int rc = avfilter_graph_create_filter(&source, avfilter_get_by_name("buffer"), "in", args.toUtf8().constData(), nullptr, graph);
            AVFilterContext* palettegen = nullptr;
            if (rc >= 0) rc = avfilter_graph_create_filter(&palettegen, avfilter_get_by_name("palettegen"), "palettegen", "stats_mode=diff:max_colors=256", nullptr, graph);
            if (rc >= 0) rc = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph);
            if (rc >= 0) rc = avfilter_link(source, 0, palettegen, 0);
            if (rc >= 0) rc = avfilter_link(palettegen, 0, sink, 0);
            if (rc >= 0) rc = avfilter_graph_config(graph, nullptr);
            if (rc < 0) return fail("recording_export_failed", "Cannot build the GIF palette graph", rc);
        }
        last.reset(av_frame_clone(frame));
        if (!last) return fail("recording_export_failed", "Cannot keep a frame for the GIF palette");
        int rc = av_buffersrc_add_frame_flags(source, last.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
        if (rc >= 0) {
            // Let palettegen take the frame now rather than queue it.
            FramePtr out(av_frame_alloc());
            rc = av_buffersink_get_frame(sink, out.get());
            if (rc >= 0) early = std::move(out);
            if (rc == AVERROR(EAGAIN) || rc >= 0) rc = 0;
        }
        return rc < 0 ? fail("recording_export_failed", "Cannot build the GIF palette", rc) : Error{};
    }
    Error finish(FramePtr& palette) {
        if (!last) return {"recording_export_failed", "The retained interval produced no frames", {}};
        if (last->format != AV_PIX_FMT_RGB24) return {"recording_export_failed", "Unexpected GIF palette frame format", {}};
        FramePtr inverted(av_frame_alloc());
        inverted->format = last->format; inverted->width = last->width; inverted->height = last->height;
        if (av_frame_get_buffer(inverted.get(), 0) < 0) return {"recording_export_failed", "Cannot allocate a frame for the GIF palette", {}};
        for (int y = 0; y < last->height; ++y) {
            const uint8_t* from = last->data[0] + qint64(y) * last->linesize[0];
            uint8_t* to = inverted->data[0] + qint64(y) * inverted->linesize[0];
            for (int x = 0; x < last->width * 3; ++x) to[x] = uint8_t(~from[x]);
        }
        inverted->pts = last->pts + 1;
        int rc = av_buffersrc_add_frame_flags(source, inverted.get(), 0);
        if (rc >= 0) rc = flushSource(source);
        if (rc >= 0) { palette.reset(av_frame_alloc()); rc = av_buffersink_get_frame(sink, palette.get()); }
        if (rc == AVERROR_EOF && early) { palette = std::move(early); rc = 0; }
        if (rc < 0) { palette.reset(); return fail("recording_export_failed", "Cannot build the GIF palette", rc); }
        return {};
    }
};

Error snapToFrames(const QString& path, const recording::KeptSpans& kept, const QList<RecordingMask>& masks, const std::atomic_bool& cancelled, FrameGrid& grid) {
    Input input;
    if (auto e = input.open(path, false)) return e;
    const auto* stream = input.format->streams[input.videoIndex];
    const auto timeBase = stream->time_base;
    const auto origin = streamOrigin(stream);
    // Half a container tick, bounded so an odd time base cannot swallow a frame step.
    const qint64 slack = std::clamp<qint64>(av_rescale_q(1, timeBase, microseconds) / 2, 0, 5000);
    grid.slackUs = slack;
    const Error canceled{"cancelled", "Export canceled", {}};
    std::set<qint64> frames, keys;
    PacketPtr packet(av_packet_alloc());
    bool positioned = false, ended = false;
    qint64 readUs = 0;
    int seeks = 0;
    // The first frame at or after `boundary`, nullopt when none follows it.
    auto firstFrameAtOrAfter = [&](qint64 boundary, std::optional<qint64>& found) -> Error {
        // Boundaries come in order; a far one is reached by seeking, a near one by reading on.
        if (!positioned || boundary > readUs + 1000000) {
            input.seekTo(boundary, path, false);
            if (!input.format) return fail("recording_export_failed", "Cannot reopen the working clip");
            positioned = true;
            ended = false;
            ++seeks;
        }
        // Packets come in decode order: a frame that is shown before one read
        // earlier is a few packets behind it at most (H.264 reorders up to 16).
        constexpr qint64 lookahead = 16;
        auto seen = qint64(std::distance(frames.lower_bound(boundary - slack), frames.end()));
        while (!ended && seen < lookahead) {
            if (cancelled.load()) return canceled;
            const int rc = input.read(packet.get());
            if (rc == AVERROR_EOF) { ended = true; break; }
            if (rc < 0) return fail("recording_export_failed", "Reading the working clip failed", rc);
            if (packet->stream_index == input.videoIndex && packet->pts != AV_NOPTS_VALUE) {
                const auto us = av_rescale_q(packet->pts - origin, timeBase, microseconds);
                if (frames.insert(us).second && us >= boundary - slack) ++seen;
                if (packet->flags & AV_PKT_FLAG_KEY) keys.insert(us);
                readUs = std::max<qint64>(readUs, us);
            }
            av_packet_unref(packet.get());
        }
        const auto it = frames.lower_bound(boundary - slack);
        found = it == frames.end() ? std::nullopt : std::optional<qint64>(*it);
        return {};
    };
    QList<recording::Span> spans;
    std::map<qint64, qint64> heldAt;  // span start -> the frame it opens on, for spans held at their start
    for (const auto& span : kept.spans()) {
        std::optional<qint64> start, end;
        if (auto e = firstFrameAtOrAfter(span.start, start)) return e;
        if (!start) break;  // no frame from here on: nothing later is kept either
        // The scan holds every frame from the keyframe it sought to before span.start,
        // so the one before the first frame found is the frame on screen at span.start.
        if (*start - span.start > floorIntervalUs) {
            if (const auto it = frames.lower_bound(span.start - slack); it != frames.begin()) {
                heldAt[span.start] = *std::prev(it);
                start = span.start;
            }
        }
        // The last kept frame lasts until the next frame: kept to there when that
        // is within a floor interval of the span's end, cut at the end otherwise.
        // (A span before a held one then always ends before it: no frame lies
        // between the two, so the next frame is over a floor interval away.)
        if (auto e = firstFrameAtOrAfter(span.end, end)) return e;
        const qint64 stop = end && *end - span.end <= floorIntervalUs ? *end : span.end;
        if (stop > *start) spans.append({*start, stop});
    }
    debugLog(QStringLiteral("export grid: %1 kept spans -> %2 on frames (%3 held at their start), %4 scan seeks").arg(kept.spans().size()).arg(spans.size()).arg(heldAt.size()).arg(seeks));
    grid.held.clear();
    if (spans.isEmpty()) { grid.map = {}; grid.keyframes.assign(keys.begin(), keys.end()); return {}; }
    RecordingEdit snapped;
    snapped.trimStart = spans.first().start;
    snapped.trimEnd = spans.last().end;
    for (int i = 0; i + 1 < spans.size(); ++i) if (spans[i].end < spans[i + 1].start) snapped.cuts.append({QString(), spans[i].end, spans[i + 1].start});
    grid.map = recording::KeptSpans(snapped, spans.last().end);
    for (const auto& span : grid.map.spans()) {
        const auto it = heldAt.find(span.start);
        grid.held.push_back(it == heldAt.end() ? std::nullopt : std::optional<qint64>(it->second));
    }
    // #1635: every frame over each mask's kept parts, read from a seek to each
    // part's start until enough frames past its end show none is still to come.
    // Overlapping parts, and parts within a second of each other, are read in
    // one pass, so no footage is read twice.
    QList<recording::Span> parts, ranges;
    for (const auto& mask : masks) parts << grid.map.keptParts(mask.start - slack, mask.end);
    std::sort(parts.begin(), parts.end(), [](const recording::Span& a, const recording::Span& b) { return a.start < b.start; });
    for (const auto& part : std::as_const(parts)) {
        if (!ranges.isEmpty() && part.start <= ranges.last().end + 1000000) ranges.last().end = std::max(ranges.last().end, part.end);
        else ranges.append(part);
    }
    for (const auto& range : std::as_const(ranges)) {
        input.seekTo(range.start, path, false);
        if (!input.format) return fail("recording_export_failed", "Cannot reopen the working clip");
        ++seeks;
        // seekTo starts on a keyframe at or before the start, so every frame shown
        // from there is read; 16 past the end cover H.264's reordering.
        for (int beyond = 0; beyond < 16;) {
            if (cancelled.load()) return canceled;
            const int rc = input.read(packet.get());
            if (rc == AVERROR_EOF) break;
            if (rc < 0) return fail("recording_export_failed", "Reading the working clip failed", rc);
            if (packet->stream_index == input.videoIndex && packet->pts != AV_NOPTS_VALUE) {
                const auto us = av_rescale_q(packet->pts - origin, timeBase, microseconds);
                frames.insert(us);
                if (packet->flags & AV_PKT_FLAG_KEY) keys.insert(us);
                if (us >= range.end) ++beyond;
            }
            av_packet_unref(packet.get());
        }
    }
    grid.keyframes.assign(keys.begin(), keys.end());
    grid.frames = std::move(frames);
    debugLog(QStringLiteral("export grid: %1 mask ranges scanned, %2 frame times, %3 seeks in all").arg(ranges.size()).arg(grid.frames.size()).arg(seeks));
    return {};
}

// Decodes the kept frames once through `graph`, each stamped with its output
// tick (FrameGrid::tickOf, time base 1/fps), handing every filtered frame to `consume`. Seeks to the keyframe at or
// before the first span and, past a cut that holds the keyframe the next span
// decodes from, to that keyframe: cut-out footage after it is never decoded.
// A span held at its start (FrameGrid::held) first gets the frame on screen
// there, the last one decoded before it, stamped at the span's output start.
// #1635: a span's last picture that rounds past its last tick takes that tick
// when the picture there is only a repeat (it would never be shown otherwise).
Error driveVideo(const QString& path, Graph& graph, const FrameGrid& grid, const std::atomic_bool& cancelled, const std::function<Error(AVFrame*)>& consume) {
    const auto& spans = grid.map.spans();
    if (spans.isEmpty()) return {"recording_export_failed", "The retained interval produced no frames", {}};
    // A fresh demuxer per pass: seeking a Matroska file back after EOF lands on a later keyframe.
    Input input;
    if (auto e = input.open(path, false)) return e;
    input.seekTo(grid.decodeFrom(0), path, false);
    if (!input.format) return fail("recording_export_failed", "Cannot reopen the working clip");
    const auto timeBase = input.format->streams[input.videoIndex]->time_base;
    const auto origin = streamOrigin(input.format->streams[input.videoIndex]);
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc()), filtered(av_frame_alloc()), previous(av_frame_alloc()), late(av_frame_alloc());
    bool inputDone = false, heldPushed = false, haveLate = false;
    qint64 lastTick = -1;  // the last tick a frame of the current span was pushed on
    int pushed = 0, pulled = 0, seeks = 0, lastRc = 0;
    qsizetype span = 0;
    qint64 previousAt = std::numeric_limits<qint64>::min();  // source time of `previous`, the last frame decoded
    auto drain = [&]() -> Error {
        for (;;) {
            const int rc = av_buffersink_get_frame(graph.sink, filtered.get());
            lastRc = rc;
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
            ++pulled;
            if (rc < 0) return fail("recording_export_failed", "Filtering failed", rc);
            const auto error = consume(filtered.get());
            av_frame_unref(filtered.get());
            if (error) return error;
        }
    };
    // The source ends at the last tick, so the rate filter holds the last kept
    // frame for as long as it lasted in the clip.
    auto finish = [&](const char* why) -> Error {
        const int rc = av_buffersrc_close(graph.source, grid.totalTicks, 0);
        if (rc < 0 && rc != AVERROR_EOF) return fail("recording_export_failed", "Filtering failed", rc);
        auto e = drain();
        debugLog(QStringLiteral("drive: %1 after pushed=%2 pulled=%3 seeks=%4 lastRc=%5").arg(QLatin1String(why)).arg(pushed).arg(pulled).arg(seeks).arg(lastRc));
        return e;
    };
    auto decodeFailed = [&](int rc) { return fail("recording_export_failed", QStringLiteral("Decoding failed (%1, code %2)").arg(QString::fromUtf8(input.video->codec->name)).arg(rc), rc); };
    // Hands one frame to the graph. Once trim has passed its end the graph
    // reports EOF upstream and sourceEnded is set: nothing more is wanted.
    bool sourceEnded = false;
    auto push = [&](AVFrame* f) -> Error {
        const int rc = av_buffersrc_add_frame_flags(graph.source, f, AV_BUFFERSRC_FLAG_KEEP_REF);
        ++pushed;
        if (rc == AVERROR_EOF) { sourceEnded = true; auto e = drain(); debugLog(QStringLiteral("drive: source EOF after pushed=%1 pulled=%2 seeks=%3 lastRc=%4").arg(pushed).arg(pulled).arg(seeks).arg(lastRc)); return e; }
        if (rc < 0) return fail("recording_export_failed", "Filtering failed", rc);
        return drain();
    };
    // Ends span `span`: its late picture, if any, takes the last tick when only a repeat is there.
    auto closeSpan = [&]() -> Error {
        const qint64 last = grid.firstTick[size_t(span)] + grid.ticks[size_t(span)] - 1;
        Error e;
        if (haveLate && grid.ticks[size_t(span)] > 0 && lastTick < last) { late->pts = last; e = push(late.get()); }
        av_frame_unref(late.get());
        haveLate = false;
        lastTick = -1;
        return e;
    };
    while (!inputDone) {
        if (cancelled.load()) return {"cancelled", "Export canceled", {}};
        int rc = input.read(packet.get());
        if (rc == AVERROR_EOF) { avcodec_send_packet(input.video, nullptr); inputDone = true; }
        else if (rc < 0) return fail("recording_export_failed", "Reading the working clip failed", rc);
        else if (packet->stream_index != input.videoIndex) { av_packet_unref(packet.get()); continue; }
        else {
            rc = avcodec_send_packet(input.video, packet.get());
            av_packet_unref(packet.get());
            if (rc < 0 && rc != AVERROR(EAGAIN)) return decodeFailed(rc);
        }
        for (;;) {
            rc = avcodec_receive_frame(input.video, frame.get());
            if (rc == AVERROR(EAGAIN)) break;
            if (rc == AVERROR_EOF) { if (auto e = closeSpan(); e || sourceEnded) return e; return finish("input EOF"); }
            if (rc < 0) return decodeFailed(rc);
            const auto at = av_rescale_q(frame->best_effort_timestamp - origin, timeBase, microseconds);
            bool seek = false;
            for (;;) {
                // A held span opens on the last frame decoded before its start, once the
                // first frame at or past that start shows it is the one on screen there.
                if (const auto& held = grid.held[size_t(span)]; held && !heldPushed && at >= spans[span].start && previousAt >= *held && previousAt < spans[span].start) {
                    heldPushed = true;
                    previous->pts = lastTick = grid.firstTick[size_t(span)];
                    previous->duration = 1;
                    if (auto e = push(previous.get()); e || sourceEnded) { av_frame_unref(frame.get()); return e; }
                }
                if (at < spans[span].end) break;
                if (auto e = closeSpan(); e || sourceEnded) { av_frame_unref(frame.get()); return e; }
                heldPushed = false;
                // Decoding on through the cut wastes nothing when the next span's
                // keyframe lies before the cut; when the cut holds it, jump there.
                if (++span == spans.size()) break;
                seek = grid.keyframeAtOrBefore(grid.decodeFrom(span)) > spans[span - 1].end;
            }
            if (span == spans.size()) { av_frame_unref(frame.get()); return finish("last span"); }
            if (seek) {
                av_frame_unref(frame.get());
                av_frame_unref(previous.get());
                previousAt = std::numeric_limits<qint64>::min();
                input.seekTo(grid.decodeFrom(span), path, false);
                if (!input.format) return fail("recording_export_failed", "Cannot reopen the working clip");
                ++seeks;
                inputDone = false;
                break;  // the decoder was flushed; read on from the keyframe
            }
            av_frame_unref(previous.get());
            if (av_frame_ref(previous.get(), frame.get()) < 0) { av_frame_unref(frame.get()); return fail("recording_export_failed", "Cannot keep a decoded frame"); }
            previousAt = at;
            if (at < spans[span].start) { av_frame_unref(frame.get()); continue; }
            frame->pts = grid.tickOf(span, at);
            frame->duration = 1;
            if (frame->pts >= grid.firstTick[size_t(span)] + grid.ticks[size_t(span)]) {
                // Past the span's last tick: kept aside in case it is the span's last picture.
                av_frame_unref(late.get());
                av_frame_move_ref(late.get(), frame.get());
                haveLate = true;
                continue;
            }
            lastTick = frame->pts;
            const auto e = push(frame.get());
            av_frame_unref(frame.get());
            // Once trim has passed its end the graph reports EOF upstream: we are done.
            if (e || sourceEnded) return e;
        }
    }
    if (span < spans.size()) { if (auto e = closeSpan(); e || sourceEnded) return e; }
    return finish("input done");
}
// #1613: cuts the trimmed audio (48 kHz float, counted in samples from the first
// kept frame) to the kept spans, laid end to end, and queues it for the encoder
// in whole frames. Each side of a seam fades over 4 ms, so a join never clicks.
// #1635: each grid span fills exactly its ticks' share of the output, so audio
// and video stay together across any number of seams, but only the edit's own
// kept time is heard: where the grid runs a span past the edit's end (up to the
// next frame) or the ticks outlast the span, the output gets silence instead.
struct KeptAudio {
    static constexpr qint64 fade = 192;
    struct Range { qint64 from = 0, to = 0; };
    // A span's source samples [from, to) of the trimmed stream, the audible
    // parts of them, and the output samples it fills (to - from plus silence).
    struct Piece { qint64 from = 0, to = 0, length = 0; QList<Range> audible; };
    QList<Piece> pieces;
    qsizetype current = 0;
    qint64 done = 0;      // output samples of the current piece queued so far
    AVAudioFifo* fifo = nullptr;
    qint64 position = 0;  // samples of the trimmed stream seen so far
    std::vector<float> zeros;
    ~KeptAudio() { if (fifo) av_audio_fifo_free(fifo); }
    KeptAudio(const FrameGrid& grid, const recording::KeptSpans& kept, AVSampleFormat format, int channels) {
        const auto& spans = grid.map.spans();
        const qint64 first = spans.first().start;
        // The first sample at or after a time: a half-open edge [a, b) holds the
        // samples from sample(a) up to, not including, sample(b), none from outside it.
        auto sample = [first](qint64 us) -> qint64 { return av_rescale_rnd(us - first, 48000, 1000000, AV_ROUND_UP); };
        for (qsizetype i = 0; i < spans.size(); ++i) {
            Piece piece;
            piece.from = sample(spans[i].start);
            piece.length = av_rescale(grid.firstTick[size_t(i)] + grid.ticks[size_t(i)], 48000, grid.fps) - av_rescale(grid.firstTick[size_t(i)], 48000, grid.fps);
            piece.to = piece.from + piece.length;
            if (i + 1 < spans.size()) piece.to = std::min(piece.to, sample(spans[i + 1].start));
            for (const auto& part : kept.keptParts(spans[i].start, first + av_rescale(piece.to, 1000000, 48000) + 1)) {
                const Range range{std::max(piece.from, sample(part.start)), std::min(piece.to, sample(part.end))};
                if (range.from < range.to) piece.audible.append(range);
            }
            if (piece.length > 0) pieces.append(piece);
        }
        fifo = av_audio_fifo_alloc(format, channels, 4096);
        zeros.assign(size_t(4096) * size_t(std::max(1, channels)), 0.0f);
    }
    // Queues `count` samples of silence.
    Error silence(qint64 count) {
        void* parts[AV_NUM_DATA_POINTERS];
        for (auto& part : parts) part = zeros.data();
        while (count > 0) {
            const int n = int(std::min<qint64>(count, 4096));
            if (av_audio_fifo_write(fifo, parts, n) < n) return {"recording_export_failed", "Cannot queue the clip audio", {}};
            count -= n;
        }
        return {};
    }
    // The gain of sample n of piece k: 0 outside its audible parts, a fade at
    // their edges (not at the very start or end of the output).
    double gain(qsizetype k, qint64 n) const {
        const auto& piece = pieces[k];
        for (const auto& range : piece.audible) {
            if (n < range.from || n >= range.to) continue;
            const qint64 length = std::min(fade, (range.to - range.from) / 2);
            if (length <= 0) return 1.0;
            double g = 1.0;
            if (!(k == 0 && range.from == piece.from) && n < range.from + length) g = std::min(g, (double(n - range.from) + 0.5) / double(length));
            if (!(k + 1 == pieces.size() && range.to == piece.to) && n >= range.to - length) g = std::min(g, (double(range.to - n) - 0.5) / double(length));
            return g;
        }
        return 0.0;
    }
    // Takes one filtered frame (packed or planar float), fading it in place at the seams.
    Error take(AVFrame* in) {
        const qint64 from = position, to = position + in->nb_samples;
        position = to;
        const auto format = AVSampleFormat(in->format);
        if (format != AV_SAMPLE_FMT_FLT && format != AV_SAMPLE_FMT_FLTP) return {"recording_export_failed", "Unexpected audio sample format", {}};
        if (av_frame_make_writable(in) < 0) return {"recording_export_failed", "Cannot cut the clip audio", {}};
        const bool planar = av_sample_fmt_is_planar(format);
        const int channels = in->ch_layout.nb_channels;
        while (current < pieces.size()) {
            const auto& piece = pieces[current];
            if (piece.from >= to) break;
            const qint64 a = std::max(from, piece.from + done), b = std::min(to, piece.to);
            if (a < b) {
                for (qint64 n = a; n < b; ++n) {
                    const double g = gain(current, n);
                    if (g == 1.0) continue;
                    const qint64 i = n - from;
                    for (int c = 0; c < channels; ++c) {
                        if (planar) reinterpret_cast<float*>(in->extended_data[c])[i] *= float(g);
                        else reinterpret_cast<float*>(in->data[0])[i * channels + c] *= float(g);
                    }
                }
                void* parts[AV_NUM_DATA_POINTERS] = {};
                const int bytes = av_get_bytes_per_sample(format);
                if (planar) for (int c = 0; c < channels && c < AV_NUM_DATA_POINTERS; ++c) parts[c] = in->extended_data[c] + (a - from) * bytes;
                else parts[0] = in->data[0] + (a - from) * bytes * channels;
                if (av_audio_fifo_write(fifo, parts, int(b - a)) < int(b - a)) return {"recording_export_failed", "Cannot queue the clip audio", {}};
                done += b - a;
            }
            if (to < piece.to) break;
            // The piece's source is used up: silence fills the rest of its output time.
            if (auto e = silence(piece.length - done)) return e;
            ++current;
            done = 0;
        }
        return {};
    }
    // After the last audio: up to `limit` samples of the silence for the output
    // time the audio did not reach (finished() once it is all queued). Called
    // in encoder-sized batches, so an audio track that ends early never queues
    // its whole silent tail at once.
    bool finished() const { return current >= pieces.size(); }
    Error finish(qint64 limit) {
        while (limit > 0 && current < pieces.size()) {
            const qint64 n = std::min(limit, pieces[current].length - done);
            if (auto e = silence(n)) return e;
            done += n;
            limit -= n;
            if (done >= pieces[current].length) { ++current; done = 0; }
        }
        return {};
    }
};
}

Error exportClip(const ExportRequest& request, ExportResult& result, const std::atomic_bool& cancelled, const std::function<void(double)>& progress) {
    const bool wantAudio = request.config.audio.any() && !request.config.silent();
    Input input;
    if (auto e = input.open(request.input, wantAudio)) return e;
    debugLog(QStringLiteral("export %1 wantAudio=%2 audioIndex=%3 streams=%4").arg(request.config.format).arg(wantAudio).arg(input.audioIndex).arg(input.format->nb_streams));
    const auto outputSize = recording::outputSize(request.canvas, request.config.maxEdge);
    // #1613: the output is the kept spans laid end to end, on the clip's frame grid.
    const recording::KeptSpans kept(request.edit, request.durationUs);
    if (kept.outputDuration() <= 0) return {"invalid_request", "The retained interval is empty", {}};
    FrameGrid grid;
    if (auto e = snapToFrames(request.input, kept, request.edit.masks, cancelled, grid)) return e;
    if (grid.map.empty()) return {"recording_export_failed", "The retained interval produced no frames", {}};
    const bool animation = request.config.animation();
    const int fps = std::max(1, animation ? std::min(request.config.fps, 15) : request.config.fps);
    // GIF/WebP: the length limit was checked on the edit's kept time, so the
    // output keeps no more whole frames than fit in it (and at least one).
    grid.layOut(fps, kept, animation);
    const auto outputUs = grid.outputUs();
    const AVPixelFormat encodeFormat = request.config.format == "gif" ? AV_PIX_FMT_PAL8 : AV_PIX_FMT_YUV420P;

    AVFormatContext* output = nullptr;
    const char* muxer = request.config.format == "webp" ? "webp" : request.config.format == "gif" ? "gif" : request.config.format == "webm" ? "webm" : "mp4";
    int rc = avformat_alloc_output_context2(&output, nullptr, muxer, QFile::encodeName(request.output).constData());
    if (rc < 0 || !output) return fail("recording_export_failed", "Cannot create the output container", rc);
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> outputGuard(output, [](AVFormatContext* f) { if (f && !(f->oformat->flags & AVFMT_NOFILE) && f->pb) avio_closep(&f->pb); avformat_free_context(f); });
    Encoder video, audio;
    if (auto e = openVideoEncoder(output, request.config, outputSize, fps, encodeFormat, {1, fps}, video)) return e;
    // VA-API uploads NV12; the software encoders take planar 4:2:0.
    const QString tail = request.config.format == "gif" ? QString() : video.hardware() ? QStringLiteral(",format=nv12") : QStringLiteral(",format=yuv420p");
    if (wantAudio && input.audioIndex >= 0) { if (auto e = openAudioEncoder(output, request.config, audio)) return e; }
    AVDictionary* muxOptions = nullptr;
    if (request.config.format == "mp4") av_dict_set(&muxOptions, "movflags", "+faststart", 0);
    if (animation) av_dict_set(&muxOptions, "loop", "0", 0);
    if (!(output->oformat->flags & AVFMT_NOFILE) && (rc = avio_open(&output->pb, QFile::encodeName(request.output).constData(), AVIO_FLAG_WRITE)) < 0) { av_dict_free(&muxOptions); return fail("recording_export_failed", "Cannot write the output file", rc); }
    rc = avformat_write_header(output, &muxOptions);
    av_dict_free(&muxOptions);
    if (rc < 0) return fail("recording_export_failed", "Cannot write the output header", rc);

    PacketPtr packet(av_packet_alloc());
    const auto base = baseVideoGraph(request, grid, outputSize);
    FramePtr paletteFrame;
    if (request.config.format == "gif") {
        // Pass one builds the palette from the exact frames pass two encodes.
        Graph paletteGraph;
        if (auto e = paletteGraph.build(base + ",format=rgb24[out]", input.video, {1, fps}, false)) return e;
        PaletteStats stats;
        const auto statsTimeBase = av_buffersink_get_time_base(paletteGraph.sink);
        if (auto e = driveVideo(request.input, paletteGraph, grid, cancelled, [&](AVFrame* frame) { return stats.add(frame, statsTimeBase); })) return e;
        if (auto e = stats.finish(paletteFrame)) return e;
    }
    Graph graph;
    const auto description = request.config.format == "gif" ? base + "[pv];[pv][pal]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle[out]" : base + tail + "[out]";
    if (auto e = graph.build(description, input.video, {1, fps}, request.config.format == "gif", paletteFrame.get())) return e;
    if (graph.palette) {
        rc = av_buffersrc_add_frame_flags(graph.palette, paletteFrame.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
        if (rc >= 0) rc = flushSource(graph.palette);
        if (rc < 0) return fail("recording_export_failed", "Cannot load the GIF palette", rc);
    }
    int frames = 0;
    qint64 lastPts = 0;
    const auto sinkTimeBase = av_buffersink_get_time_base(graph.sink);
    auto consume = [&](AVFrame* frame) -> Error {
        frame->pts = av_rescale_q(frame->pts, sinkTimeBase, video.context->time_base);
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        lastPts = frame->pts;
        ++frames;
        if (progress) progress(std::clamp(double(av_rescale_q(frame->pts, video.context->time_base, microseconds)) / double(outputUs), 0.0, 1.0));
        return encodeFrame(output, video, frame, packet.get());
    };
    if (auto e = driveVideo(request.input, graph, grid, cancelled, consume)) return e;
    if (auto e = encodeFrame(output, video, nullptr, packet.get())) return e;
    if (frames < 1) return {"recording_export_failed", "The retained interval produced no frames", {}};

    if (audio.context) {
        // A fresh demuxer for the audio pass: the video pass consumed the file.
        Input audioInput;
        if (auto e = audioInput.open(request.input, true)) return e;
        if (audioInput.audioIndex < 0) return {"recording_export_failed", "The clip audio disappeared", {}};
        Input& input = audioInput;
        const auto audioTimeBase = input.format->streams[input.audioIndex]->time_base;
        // The clip timeline starts at the first video frame, as the video trim
        // does. Audio usually starts later (the capture opens the sink monitor
        // after the stream), and that gap is part of the sync: the retained
        // audio is cut from the same instants and silence fills the time before
        // its first sample (#86; rebasing it to its own start played it early).
        // #1613: the cut points are the video's, on its frame grid: the trimmed
        // stream runs from the first kept frame to the last span's end and
        // KeptAudio lays the same spans of it on the output grid, hearing only
        // the edit's own kept time (#1635).
        const auto videoStream = input.format->streams[input.videoIndex];
        const auto originUs = av_rescale_q(streamOrigin(videoStream), videoStream->time_base, microseconds);
        const auto audioStartUs = av_rescale_q(streamOrigin(input.format->streams[input.audioIndex]), audioTimeBase, microseconds);
        // A last span's ticks can outlast it by half a frame; KeptAudio drops what it does not use.
        const auto startUs = originUs + grid.map.spans().first().start, endUs = originUs + grid.map.spans().last().end + 1000000 / fps;
        const auto end = av_rescale_q(endUs, microseconds, audioTimeBase);
        // atrim's *_pts options count samples; seconds (microsecond precision) match the video trim.
        // The encoder numbers its frames by samples, so aresample turns the clip's
        // timestamps into samples first: a hole (audio dropped around a pause) becomes
        // silence and an overlap is cut, so later audio keeps its place (#130).
        // #1635: rebased on the requested start, not the first sample after it, so
        // a start inside a hole keeps the hole as silence too.
        const auto description = QStringLiteral("[in]atrim=start=%1:end=%2,asetpts=PTS-%3,aresample=async=1:min_hard_comp=0.001:first_pts=0,aformat=sample_fmts=%4:sample_rates=48000:channel_layouts=stereo[out]").arg(seconds(startUs), seconds(endUs)).arg(av_rescale_q(startUs, microseconds, audioTimeBase)).arg(av_get_sample_fmt_name(audio.context->sample_fmt));
        AudioGraph audioGraph;
        if (auto e = audioGraph.build(description, input.audio, audioTimeBase, 0)) return e;
        avcodec_flush_buffers(input.audio);
        FramePtr frame(av_frame_alloc()), filtered(av_frame_alloc());
        if (const qint64 leadUs = std::min<qint64>(audioStartUs, endUs) - startUs; leadUs > 0 && input.audio->sample_rate > 0) {
            // One silent frame at the trim start; asetpts then rebases on it, so the first real sample lands at leadUs.
            FramePtr silence(av_frame_alloc());
            silence->format = input.audio->sample_fmt;
            silence->sample_rate = input.audio->sample_rate;
            silence->nb_samples = int(av_rescale(leadUs, input.audio->sample_rate, 1000000));
            silence->pts = av_rescale_q(startUs, microseconds, audioTimeBase);
            int rc = av_channel_layout_copy(&silence->ch_layout, &input.audio->ch_layout);
            if (rc >= 0 && silence->nb_samples > 0 && (rc = av_frame_get_buffer(silence.get(), 0)) >= 0) {
                av_samples_set_silence(silence->extended_data, 0, silence->nb_samples, silence->ch_layout.nb_channels, AVSampleFormat(silence->format));
                rc = av_buffersrc_add_frame_flags(audioGraph.source, silence.get(), 0);
            }
            if (rc < 0) return fail("recording_export_failed", "Cannot pad the start of the clip audio", rc);
            debugLog(QStringLiteral("export audio lead %1 us of silence").arg(leadUs));
        }
        qint64 samples = 0;
        KeptAudio keptAudio(grid, kept, audio.context->sample_fmt, audio.context->ch_layout.nb_channels);
        if (!keptAudio.fifo) return {"recording_export_failed", "Cannot allocate the audio queue", {}};
        const int frameSize = (audio.context->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) || audio.context->frame_size <= 0 ? 1024 : audio.context->frame_size;
        // Whole encoder frames from the queue; the rest too once the audio ended.
        auto encodeQueued = [&](bool all) -> Error {
            while (av_audio_fifo_size(keptAudio.fifo) >= frameSize || (all && av_audio_fifo_size(keptAudio.fifo) > 0)) {
                FramePtr out(av_frame_alloc());
                out->format = audio.context->sample_fmt;
                out->sample_rate = audio.context->sample_rate;
                out->nb_samples = std::min(frameSize, av_audio_fifo_size(keptAudio.fifo));
                if (av_channel_layout_copy(&out->ch_layout, &audio.context->ch_layout) < 0 || av_frame_get_buffer(out.get(), 0) < 0) return {"recording_export_failed", "Cannot allocate audio", {}};
                if (av_audio_fifo_read(keptAudio.fifo, reinterpret_cast<void**>(out->extended_data), out->nb_samples) < out->nb_samples) return {"recording_export_failed", "Cannot read the audio queue", {}};
                out->pts = samples;
                samples += out->nb_samples;
                if (auto e = encodeFrame(output, audio, out.get(), packet.get())) return e;
            }
            return {};
        };
        auto drain = [&]() -> Error {
            for (;;) {
                const int r = av_buffersink_get_frame(audioGraph.sink, filtered.get());
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return {};
                if (r < 0) return fail("recording_export_failed", "Audio filtering failed", r);
                auto e = keptAudio.take(filtered.get());
                av_frame_unref(filtered.get());
                if (!e) e = encodeQueued(false);
                if (e) return e;
            }
        };
        bool done = false;
        const auto untilPts = end + av_rescale_q(2000000, microseconds, audioTimeBase);
        while (!done) {
            if (cancelled.load()) return {"cancelled", "Export canceled", {}};
            int r = input.read(packet.get());
            if (r == AVERROR_EOF) { avcodec_send_packet(input.audio, nullptr); done = true; }
            else if (r < 0) return fail("recording_export_failed", "Reading the clip audio failed", r);
            else if (packet->stream_index != input.audioIndex) { av_packet_unref(packet.get()); continue; }
            else { const bool past = packet->pts != AV_NOPTS_VALUE && packet->pts > untilPts; r = avcodec_send_packet(input.audio, packet.get()); av_packet_unref(packet.get()); if (r < 0 && r != AVERROR(EAGAIN)) return fail("recording_export_failed", "Audio decoding failed", r); if (past) { avcodec_send_packet(input.audio, nullptr); done = true; } }
            for (;;) {
                r = avcodec_receive_frame(input.audio, frame.get());
                if (r == AVERROR(EAGAIN)) break;
                if (r == AVERROR_EOF) { if ((r = flushSource(audioGraph.source)) < 0) return fail("recording_export_failed", "Audio filtering failed", r); if (auto e = drain()) return e; break; }
                if (r < 0) return fail("recording_export_failed", "Audio decoding failed", r);
                frame->pts = frame->best_effort_timestamp;
                r = av_buffersrc_add_frame_flags(audioGraph.source, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
                av_frame_unref(frame.get());
                if (r == AVERROR_EOF) { done = true; if (auto e = drain()) return e; break; }
                if (r < 0) return fail("recording_export_failed", "Audio filtering failed", r);
                if (auto e = drain()) return e;
            }
        }
        while (!keptAudio.finished()) {
            if (cancelled.load()) return {"cancelled", "Export canceled", {}};
            if (auto e = keptAudio.finish(frameSize)) return e;
            if (auto e = encodeQueued(false)) return e;
        }
        if (auto e = encodeQueued(true)) return e;
        if (auto e = encodeFrame(output, audio, nullptr, packet.get())) return e;
        debugLog(QStringLiteral("export audio samples=%1 of %2 trimmed in %3 spans").arg(samples).arg(keptAudio.position).arg(keptAudio.pieces.size()));
        result.audioEncoder = audio.name;
    }
    if ((rc = av_write_trailer(output)) < 0) return fail("recording_export_failed", "Cannot finish the output file", rc);
    outputGuard.reset();
    result.bytes = QFileInfo(request.output).size();
    result.frames = frames;
    result.durationUs = av_rescale_q(lastPts + 1, video.context->time_base, microseconds);
    result.videoEncoder = video.name;
    result.size = outputSize;
    if (result.bytes < 1) return {"recording_export_failed", "The export produced no file", {}};
    return {};
}

namespace {
void boxBlur(QImage& image, const QRect& rect, int radius) {
    // Two passes of a separable box filter inside rect, as the export applies avgblur twice.
    // Prefix sums make each output pixel one subtraction per channel, so the
    // cost no longer grows with the radius; only the rect is copied.
    const int w = rect.width(), h = rect.height();
    if (w < 1 || h < 1) return;
    std::vector<int> sumR(size_t(std::max(w, h)) + 1), sumG(sumR.size()), sumB(sumR.size());
    auto average = [&](int count, int lo, int hi, QRgb* out) {
        // [lo, hi] clamped to the line, like the clamped window of the direct filter.
        for (int i = 0; i < count; ++i) {
            const int a = std::max(0, i - radius), b = std::min(count - 1, i + radius), n = b - a + 1;
            out[i] = qRgb((sumR[b + 1] - sumR[a]) / n, (sumG[b + 1] - sumG[a]) / n, (sumB[b + 1] - sumB[a]) / n);
        }
        Q_UNUSED(lo); Q_UNUSED(hi);
    };
    std::vector<QRgb> line(size_t(std::max(w, h)));
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y)) + rect.left();
            sumR[0] = sumG[0] = sumB[0] = 0;
            for (int x = 0; x < w; ++x) { sumR[x + 1] = sumR[x] + qRed(row[x]); sumG[x + 1] = sumG[x] + qGreen(row[x]); sumB[x + 1] = sumB[x] + qBlue(row[x]); }
            average(w, 0, w - 1, row);
        }
        for (int x = rect.left(); x <= rect.right(); ++x) {
            sumR[0] = sumG[0] = sumB[0] = 0;
            for (int y = 0; y < h; ++y) { const QRgb p = reinterpret_cast<const QRgb*>(image.constScanLine(rect.top() + y))[x]; sumR[y + 1] = sumR[y] + qRed(p); sumG[y + 1] = sumG[y] + qGreen(p); sumB[y + 1] = sumB[y] + qBlue(p); }
            average(h, 0, h - 1, line.data());
            for (int y = 0; y < h; ++y) reinterpret_cast<QRgb*>(image.scanLine(rect.top() + y))[x] = line[size_t(y)];
        }
    }
}
}
namespace {
// Convert into a frame FFmpeg allocates with its own alignment and padding:
// libswscale's SIMD paths write whole vectors, so a row that is not a
// multiple of 16 pixels overruns a tightly sized buffer on its last row.
Error frameToImage(const AVFrame* chosen, QImage& image) {
    const int width = chosen->width, height = chosen->height;
    FramePtr rgb(av_frame_alloc());
    rgb->format = AV_PIX_FMT_BGRA;
    rgb->width = width;
    rgb->height = height;
    if (av_frame_get_buffer(rgb.get(), 64) < 0) return {"recording_export_failed", "Cannot allocate the frame", {}};
    SwsContext* scaler = sws_getContext(width, height, AVPixelFormat(chosen->format), width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scaler) return {"recording_export_failed", "Cannot convert the frame", {}};
    sws_scale(scaler, chosen->data, chosen->linesize, 0, height, rgb->data, rgb->linesize);
    sws_freeContext(scaler);
    image = QImage(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) std::memcpy(image.scanLine(y), rgb->data[0] + qint64(y) * rgb->linesize[0], size_t(width) * 4);
    return {};
}
// The masks active at frameUs, or at shownAtUs (the time the frame is shown
// for, when that differs), drawn the way the export renders them.
void applyMasks(QImage& image, const RecordingEdit& edit, qint64 frameUs, const QSize& canvas, std::optional<qint64> shownAtUs = std::nullopt) {
    const QRect bounds(QPoint(0, 0), canvas.isEmpty() ? image.size() : canvas);
    for (const auto& mask : edit.masks) {
        const auto active = [&](qint64 t) { return mask.start <= t && t < mask.end; };
        if (!active(frameUs) && !(shownAtUs && active(*shownAtUs))) continue;
        const auto r = mask.rect.intersected(bounds).intersected(image.rect());
        if (r.width() < 2 || r.height() < 2) continue;
        if (mask.kind == "solid") { QPainter painter(&image); painter.fillRect(r, Qt::black); }
        else boxBlur(image, r, std::clamp(mask.strength, 2, std::max(2, std::min(r.width(), r.height()) / 2 - 1)));
    }
}
QImage fitPreview(const QImage& image, int maxEdge) {
    const auto size = recording::outputSize(image.size(), maxEdge);
    return size == image.size() ? image : image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
}
Error probeClip(const QString& path, qint64& durationUs, QSize& canvas) {
    Input input;
    if (auto e = input.open(path, false)) return e;
    const auto* stream = input.format->streams[input.videoIndex];
    canvas = QSize(stream->codecpar->width, stream->codecpar->height);
    durationUs = input.format->duration > 0 ? input.format->duration : (stream->duration > 0 ? av_rescale_q(stream->duration, stream->time_base, microseconds) : 0);
    return {};
}
Error renderPreviewFrame(const AVFrame* frame, const RecordingEdit& edit, qint64 frameUs, const QSize& canvas, int maxEdge, QImage& out, std::optional<qint64> requestedUs) {
    QImage image;
    if (auto e = frameToImage(frame, image)) return e;
    applyMasks(image, edit, frameUs, canvas, requestedUs);
    out = fitPreview(image, maxEdge);
    return {};
}
Error previewFrame(const QString& path, const RecordingEdit& edit, const RecordingConfig& config, qint64 timeUs, int maxEdge, const QSize& canvas, QImage& out, qint64* actualTimeUs) {
    Input input;
    if (auto e = input.open(path, false)) return e;
    input.seekTo(timeUs, path, false);
    if (!input.format) return fail("recording_export_failed", "Cannot reopen the working clip");
    const auto timeBase = input.format->streams[input.videoIndex]->time_base;
    const auto origin = streamOrigin(input.format->streams[input.videoIndex]);
    const auto target = origin + av_rescale_q(timeUs, microseconds, timeBase);
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc()), chosen;
    bool done = false;
    while (!done) {
        int rc = input.read(packet.get());
        if (rc == AVERROR_EOF) { avcodec_send_packet(input.video, nullptr); done = true; }
        else if (rc < 0) return fail("recording_export_failed", "Reading the working clip failed", rc);
        else if (packet->stream_index != input.videoIndex) { av_packet_unref(packet.get()); continue; }
        else { rc = avcodec_send_packet(input.video, packet.get()); av_packet_unref(packet.get()); if (rc < 0 && rc != AVERROR(EAGAIN)) return fail("recording_export_failed", QStringLiteral("Decoding failed (%1, code %2)").arg(QString::fromUtf8(input.video->codec->name)).arg(rc), rc); }
        for (;;) {
            rc = avcodec_receive_frame(input.video, frame.get());
            if (rc == AVERROR(EAGAIN)) break;
            if (rc == AVERROR_EOF) { done = true; break; }
            if (rc < 0) return fail("recording_export_failed", QStringLiteral("Decoding failed (%1, code %2)").arg(QString::fromUtf8(input.video->codec->name)).arg(rc), rc);
            const auto pts = frame->best_effort_timestamp;
            chosen.reset(av_frame_clone(frame.get()));
            av_frame_unref(frame.get());
            if (pts >= target) { done = true; break; }
        }
    }
    if (!chosen) return {"recording_export_failed", "No frame could be decoded at this time", {}};
    const auto frameUs = av_rescale_q(chosen->best_effort_timestamp - origin, timeBase, microseconds);
    if (actualTimeUs) *actualTimeUs = frameUs;
    Q_UNUSED(config);
    return renderPreviewFrame(chosen.get(), edit, frameUs, canvas, maxEdge, out, timeUs);
}

struct FrameSequence::Private {
    Input input;
    PacketPtr packet{av_packet_alloc()};
    FramePtr frame{av_frame_alloc()};
    AVRational timeBase{1, 1};
    qint64 origin = 0, startTarget = 0;
    bool ended = false, flushed = false;
};
FrameSequence::FrameSequence() : d_(std::make_unique<Private>()) {}
FrameSequence::~FrameSequence() = default;
Error FrameSequence::open(const QString& path, qint64 startUs) {
    d_ = std::make_unique<Private>();
    if (auto e = d_->input.open(path, false)) return e;
    d_->input.seekTo(startUs, path, false);
    if (!d_->input.format) return fail("recording_export_failed", "Cannot reopen the working clip");
    const auto* stream = d_->input.format->streams[d_->input.videoIndex];
    d_->timeBase = stream->time_base;
    d_->origin = streamOrigin(stream);
    d_->startTarget = d_->origin + av_rescale_q(startUs, microseconds, d_->timeBase);
    return {};
}
bool FrameSequence::next(const RecordingEdit& edit, int maxEdge, const QSize& canvas, QImage& out, qint64& frameUs, Error* error, const std::function<bool(qint64)>& wanted) {
    auto& d = *d_;
    auto report = [&](const Error& e) { if (error) *error = e; d.ended = true; return false; };
    while (!d.ended) {
        int rc = avcodec_receive_frame(d.input.video, d.frame.get());
        if (rc == AVERROR(EAGAIN)) {
            if (d.flushed) { d.ended = true; break; }
            rc = d.input.read(d.packet.get());
            if (rc == AVERROR_EOF) { avcodec_send_packet(d.input.video, nullptr); d.flushed = true; continue; }
            if (rc < 0) return report(fail("recording_export_failed", "Reading the working clip failed", rc));
            if (d.packet->stream_index != d.input.videoIndex) { av_packet_unref(d.packet.get()); continue; }
            rc = avcodec_send_packet(d.input.video, d.packet.get());
            av_packet_unref(d.packet.get());
            if (rc < 0 && rc != AVERROR(EAGAIN)) return report(fail("recording_export_failed", "Decoding failed", rc));
            continue;
        }
        if (rc == AVERROR_EOF) { d.ended = true; break; }
        if (rc < 0) return report(fail("recording_export_failed", "Decoding failed", rc));
        const auto pts = d.frame->best_effort_timestamp;
        // A backward seek decodes from the keyframe before the start; skip up to it.
        if (pts < d.startTarget) { av_frame_unref(d.frame.get()); continue; }
        const auto at = av_rescale_q(pts - d.origin, d.timeBase, microseconds);
        if (wanted && !wanted(at)) { av_frame_unref(d.frame.get()); continue; }
        QImage image;
        const auto converted = frameToImage(d.frame.get(), image);
        av_frame_unref(d.frame.get());
        if (converted) return report(converted);
        frameUs = av_rescale_q(pts - d.origin, d.timeBase, microseconds);
        applyMasks(image, edit, frameUs, canvas);
        out = fitPreview(image, maxEdge);
        return true;
    }
    return false;
}
}
