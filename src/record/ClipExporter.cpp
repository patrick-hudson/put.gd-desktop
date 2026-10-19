#include "ClipExporter.h"
#include "DebugLog.h"
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QStringList>
#include <algorithm>
#include <vector>
#include <cstring>
#include <memory>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
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
QString enable(qint64 startUs, qint64 endUs) { return QStringLiteral("enable='gte(t\\,%1)*lt(t\\,%2)'").arg(seconds(startUs), seconds(endUs)); }

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
// trim → rebase → RGB → masks → scale → rate; the same chain feeds every format.
qint64 streamOrigin(const AVStream* stream) { return stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time; }
QString baseVideoGraph(const ExportRequest& request, AVRational timeBase, qint64 origin, const QSize& outputSize, int outputFps) {
    const auto start = origin + av_rescale_q(request.edit.trimStart, microseconds, timeBase), end = origin + av_rescale_q(request.edit.exportEnd(request.durationUs), microseconds, timeBase);
    QStringList chain{QStringLiteral("trim=start_pts=%1:end_pts=%2").arg(start).arg(end), "setpts=PTS-STARTPTS", "format=rgb24"};
    QString graph = "[in]" + chain.join(',');
    int label = 0;
    const auto trimEnd = request.edit.exportEnd(request.durationUs);
    for (const auto& mask : request.edit.masks) {
        const auto interval = RecordingEdit::exportInterval(mask, request.edit.trimStart, trimEnd);
        if (!interval) continue;
        const auto r = mask.rect.intersected(QRect(QPoint(0, 0), request.canvas));
        if (r.width() < 2 || r.height() < 2) continue;
        const auto when = enable(interval->first, interval->second);
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
    if (outputFps > 0) graph += QStringLiteral(",fps=%1").arg(outputFps);
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
// A VA-API device and frame pool for `size`: the default render node first,
// then the others, since the display GPU may have no VA driver (an NVIDIA card
// beside an Intel or AMD GPU). Returns the first device whose pool initialises.
bool openVaapiFrames(const QSize& size, AVBufferRef*& device, AVBufferRef*& frames) {
    QStringList nodes{QString()};
    for (int i = 128; i < 136; ++i) { const auto node = QStringLiteral("/dev/dri/renderD%1").arg(i); if (QFile::exists(node)) nodes.append(node); }
    for (const auto& node : nodes) {
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, node.isEmpty() ? nullptr : node.toUtf8().constData(), nullptr, 0) < 0) { device = nullptr; continue; }
        frames = av_hwframe_ctx_alloc(device);
        if (frames) {
            auto* pool = reinterpret_cast<AVHWFramesContext*>(frames->data);
            pool->format = AV_PIX_FMT_VAAPI;
            pool->sw_format = AV_PIX_FMT_NV12;
            pool->width = size.width();
            pool->height = size.height();
            pool->initial_pool_size = 16;
            if (av_hwframe_ctx_init(frames) == 0) return true;
            av_buffer_unref(&frames);
        }
        av_buffer_unref(&device);
    }
    return false;
}
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
        if (vaapi && !openVaapiFrames(size, out.device, out.frames)) { lastError = {"recording_export_failed", "No VA-API device accepts " + name, {}}; continue; }
        out.name = name;
        out.context = avcodec_alloc_context3(codec);
        auto* c = out.context;
        c->width = size.width(); c->height = size.height(); c->pix_fmt = format; c->time_base = timeBase; c->framerate = {fps, 1};
        if (vaapi) { c->pix_fmt = AV_PIX_FMT_VAAPI; c->hw_device_ctx = av_buffer_ref(out.device); c->hw_frames_ctx = av_buffer_ref(out.frames); }
        c->gop_size = std::max(1, fps * 2); c->max_b_frames = 0;
        if (output->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
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

// Decodes the retained interval once through `graph`, handing every filtered
// frame to `consume`. Seeks to the keyframe before the trim start.
Error driveVideo(const QString& path, Graph& graph, qint64 fromUs, qint64 untilUs, const std::atomic_bool& cancelled, const std::function<Error(AVFrame*)>& consume) {
    // A fresh demuxer per pass: seeking a Matroska file back after EOF lands on a later keyframe.
    Input input;
    if (auto e = input.open(path, false)) return e;
    input.seekTo(fromUs, path, false);
    if (!input.format) return fail("recording_export_failed", "Cannot reopen the working clip");
    const auto timeBase = input.format->streams[input.videoIndex]->time_base;
    const auto origin = streamOrigin(input.format->streams[input.videoIndex]);
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc()), filtered(av_frame_alloc());
    bool inputDone = false;
    int pushed = 0, pulled = 0, lastRc = 0;
    const auto untilPts = origin + av_rescale_q(untilUs, microseconds, timeBase);
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
    while (!inputDone) {
        if (cancelled.load()) return {"cancelled", "Export cancelled", {}};
        int rc = input.read(packet.get());
        if (rc == AVERROR_EOF) { avcodec_send_packet(input.video, nullptr); inputDone = true; }
        else if (rc < 0) return fail("recording_export_failed", "Reading the working clip failed", rc);
        else if (packet->stream_index != input.videoIndex) { av_packet_unref(packet.get()); continue; }
        else {
            const bool past = packet->pts != AV_NOPTS_VALUE && packet->pts > untilPts + av_rescale_q(2000000, microseconds, timeBase);
            rc = avcodec_send_packet(input.video, packet.get());
            av_packet_unref(packet.get());
            if (rc < 0 && rc != AVERROR(EAGAIN)) return fail("recording_export_failed", QStringLiteral("Decoding failed (%1, code %2)").arg(QString::fromUtf8(input.video->codec->name)).arg(rc), rc);
            if (past) { avcodec_send_packet(input.video, nullptr); inputDone = true; }
        }
        for (;;) {
            rc = avcodec_receive_frame(input.video, frame.get());
            if (rc == AVERROR(EAGAIN)) break;
            if (rc == AVERROR_EOF) { if ((rc = flushSource(graph.source)) < 0) return fail("recording_export_failed", "Filtering failed", rc); if (auto e = drain()) return e; return {}; }
            if (rc < 0) return fail("recording_export_failed", QStringLiteral("Decoding failed (%1, code %2)").arg(QString::fromUtf8(input.video->codec->name)).arg(rc), rc);
            frame->pts = frame->best_effort_timestamp;
            rc = av_buffersrc_add_frame_flags(graph.source, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
            av_frame_unref(frame.get());
            ++pushed;
            // Once trim has passed its end the graph reports EOF upstream: we are done.
            if (rc == AVERROR_EOF) { auto e = drain(); debugLog(QStringLiteral("drive: source EOF after pushed=%1 pulled=%2 lastRc=%3").arg(pushed).arg(pulled).arg(lastRc)); return e; }
            if (rc < 0) return fail("recording_export_failed", "Filtering failed", rc);
            if (auto e = drain()) return e;
        }
    }
    if (const int flushed = flushSource(graph.source); flushed < 0) return fail("recording_export_failed", "Filtering failed", flushed);
    auto e = drain();
    debugLog(QStringLiteral("drive: input EOF after pushed=%1 pulled=%2 lastRc=%3").arg(pushed).arg(pulled).arg(lastRc));
    return e;
}
}

Error exportClip(const ExportRequest& request, ExportResult& result, const std::atomic_bool& cancelled, const std::function<void(double)>& progress) {
    const bool wantAudio = request.config.audio.system && !request.config.silent();
    Input input;
    if (auto e = input.open(request.input, wantAudio)) return e;
    debugLog(QStringLiteral("export %1 wantAudio=%2 audioIndex=%3 streams=%4").arg(request.config.format).arg(wantAudio).arg(input.audioIndex).arg(input.format->nb_streams));
    const auto videoTimeBase = input.format->streams[input.videoIndex]->time_base;
    const auto outputSize = recording::outputSize(request.canvas, request.config.maxEdge);
    const auto trimEnd = request.edit.exportEnd(request.durationUs);
    const auto retainedUs = trimEnd - request.edit.trimStart;
    if (retainedUs <= 0) return {"invalid_request", "The retained interval is empty", {}};
    const bool animation = request.config.animation();
    const int fps = animation ? std::min(request.config.fps, 15) : request.config.fps;
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
    const auto base = baseVideoGraph(request, videoTimeBase, streamOrigin(input.format->streams[input.videoIndex]), outputSize, fps);
    FramePtr paletteFrame;
    if (request.config.format == "gif") {
        // Pass one builds the palette from the exact frames pass two encodes.
        Graph paletteGraph;
        if (auto e = paletteGraph.build(base + ",palettegen=stats_mode=diff:max_colors=256[out]", input.video, videoTimeBase, false)) return e;
        if (auto e = driveVideo(request.input, paletteGraph, request.edit.trimStart, trimEnd, cancelled, [&](AVFrame* frame) -> Error { paletteFrame.reset(av_frame_clone(frame)); return {}; })) return e;
        if (!paletteFrame) return {"recording_export_failed", "The retained interval produced no frames", {}};
    }
    Graph graph;
    const auto description = request.config.format == "gif" ? base + "[pv];[pv][pal]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle[out]" : base + tail + "[out]";
    if (auto e = graph.build(description, input.video, videoTimeBase, request.config.format == "gif", paletteFrame.get())) return e;
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
        if (progress) progress(std::clamp(double(av_rescale_q(frame->pts, video.context->time_base, microseconds)) / double(retainedUs), 0.0, 1.0));
        return encodeFrame(output, video, frame, packet.get());
    };
    if (auto e = driveVideo(request.input, graph, request.edit.trimStart, trimEnd, cancelled, consume)) return e;
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
        const auto videoStream = input.format->streams[input.videoIndex];
        const auto originUs = av_rescale_q(streamOrigin(videoStream), videoStream->time_base, microseconds);
        const auto audioStartUs = av_rescale_q(streamOrigin(input.format->streams[input.audioIndex]), audioTimeBase, microseconds);
        const auto startUs = originUs + request.edit.trimStart, endUs = originUs + trimEnd;
        const auto end = av_rescale_q(endUs, microseconds, audioTimeBase);
        // atrim's *_pts options count samples; seconds (microsecond precision) match the video trim.
        const auto description = QStringLiteral("[in]atrim=start=%1:end=%2,asetpts=PTS-STARTPTS,aformat=sample_fmts=%3:sample_rates=48000:channel_layouts=stereo[out]").arg(seconds(startUs), seconds(endUs)).arg(av_get_sample_fmt_name(audio.context->sample_fmt));
        AudioGraph audioGraph;
        if (auto e = audioGraph.build(description, input.audio, audioTimeBase, (audio.context->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) ? 0 : audio.context->frame_size)) return e;
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
        auto drain = [&]() -> Error {
            for (;;) {
                const int r = av_buffersink_get_frame(audioGraph.sink, filtered.get());
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return {};
                if (r < 0) return fail("recording_export_failed", "Audio filtering failed", r);
                filtered->pts = samples;
                samples += filtered->nb_samples;
                const auto e = encodeFrame(output, audio, filtered.get(), packet.get());
                av_frame_unref(filtered.get());
                if (e) return e;
            }
        };
        bool done = false;
        const auto untilPts = end + av_rescale_q(2000000, microseconds, audioTimeBase);
        while (!done) {
            if (cancelled.load()) return {"cancelled", "Export cancelled", {}};
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
        if (auto e = encodeFrame(output, audio, nullptr, packet.get())) return e;
        debugLog(QStringLiteral("export audio samples=%1").arg(samples));
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
// The masks active at frameUs, drawn the way the export renders them.
void applyMasks(QImage& image, const RecordingEdit& edit, qint64 frameUs, const QSize& canvas) {
    const QRect bounds(QPoint(0, 0), canvas.isEmpty() ? image.size() : canvas);
    for (const auto& mask : edit.masks) {
        if (!(mask.start <= frameUs && frameUs < mask.end)) continue;
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
    QImage image;
    if (auto e = frameToImage(chosen.get(), image)) return e;
    const auto frameUs = av_rescale_q(chosen->best_effort_timestamp - origin, timeBase, microseconds);
    if (actualTimeUs) *actualTimeUs = frameUs;
    applyMasks(image, edit, frameUs, canvas);
    out = fitPreview(image, maxEdge);
    Q_UNUSED(config);
    return {};
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
