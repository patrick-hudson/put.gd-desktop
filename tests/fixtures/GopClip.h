#pragma once
// A synthetic working clip for review tests (#1613): a flat grey picture whose
// level steps up one unit per frame inside a 30-frame GOP and jumps by 15
// between GOPs (30 fps, one keyframe a second), so a keyframe and the exact
// frame in the same GOP differ visibly. MPEG-4 in Matroska: an encoder every
// FFmpeg has.
#include <QFile>
#include <QString>
#include <algorithm>
#include <cmath>
#include <functional>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace putgd::test::gopclip {
constexpr int fps = 30, gop = 30;

inline int levelOf(int frame) { return 30 + 15 * (frame / gop % 8) + frame % gop; }
// The grey the decoder produces for a limited-range luma level.
inline int grayOf(int level) { return int(std::lround((level - 16) * 255.0 / 219.0)); }

// rate: frames a second; ptsMs, when given, the stored time of frame i in
// milliseconds (a rounded or gapped timeline); else frame i is at i / rate.
// level, when given, the luma of frame i (else levelOf).
inline bool write(const QString& path, int seconds, int width, int height, int rate = fps, const std::function<qint64(int)>& ptsMs = {}, const std::function<int(int)>& level = {}) {
    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "matroska", QFile::encodeName(path).constData()) < 0) return false;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    AVStream* stream = avformat_new_stream(out, nullptr);
    AVCodecContext* c = codec ? avcodec_alloc_context3(codec) : nullptr;
    bool ok = codec && stream && c;
    if (ok) {
        c->width = width; c->height = height; c->time_base = ptsMs ? AVRational{1, 1000} : AVRational{1, rate}; c->framerate = {rate, 1};
        c->pix_fmt = AV_PIX_FMT_YUV420P; c->gop_size = gop; c->max_b_frames = 0;
        c->flags |= AV_CODEC_FLAG_QSCALE; c->global_quality = FF_QP2LAMBDA * 2;
        if (out->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ok = avcodec_open2(c, codec, nullptr) >= 0 && avcodec_parameters_from_context(stream->codecpar, c) >= 0
            && avio_open(&out->pb, QFile::encodeName(path).constData(), AVIO_FLAG_WRITE) >= 0 && avformat_write_header(out, nullptr) >= 0;
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto drain = [&] {
        while (avcodec_receive_packet(c, packet) == 0) {
            av_packet_rescale_ts(packet, c->time_base, stream->time_base);
            packet->stream_index = stream->index;
            if (av_interleaved_write_frame(out, packet) < 0) ok = false;
        }
    };
    for (int i = 0; ok && i < seconds * rate; ++i) {
        av_frame_unref(frame);
        frame->format = AV_PIX_FMT_YUV420P; frame->width = width; frame->height = height;
        ok = av_frame_get_buffer(frame, 0) >= 0;
        if (!ok) break;
        for (int y = 0; y < height; ++y) std::fill_n(frame->data[0] + qint64(y) * frame->linesize[0], width, uint8_t(level ? level(i) : levelOf(i)));
        for (int p = 1; p < 3; ++p) for (int y = 0; y < height / 2; ++y) std::fill_n(frame->data[p] + qint64(y) * frame->linesize[p], width / 2, uint8_t(128));
        frame->pts = ptsMs ? ptsMs(i) : i;
        if (i % gop == 0) { frame->pict_type = AV_PICTURE_TYPE_I; frame->flags |= AV_FRAME_FLAG_KEY; }
        ok = avcodec_send_frame(c, frame) >= 0;
        drain();
    }
    if (ok) { avcodec_send_frame(c, nullptr); drain(); ok = av_write_trailer(out) >= 0; }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&c);
    if (out) { if (out->pb) avio_closep(&out->pb); avformat_free_context(out); }
    return ok;
}
}
