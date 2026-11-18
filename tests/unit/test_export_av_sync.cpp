#include "core/KeptSpans.h"
#include "record/AdaptiveRate.h"
#include "record/AvSync.h"
#include "record/ClipExporter.h"
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
using namespace putgd::record;
using putgd::recording::KeptSpans;

// Collects the errors one decoder context logs.
namespace {
const AVCodecContext* checkedDecoder = nullptr;
QStringList decoderErrors;
void recordDecoderErrors(void* avcl, int level, const char* format, va_list args) {
    if (avcl && avcl == checkedDecoder && level <= AV_LOG_ERROR) {
        char line[512];
        va_list copy;
        va_copy(copy, args);
        std::vsnprintf(line, sizeof line, format, copy);
        va_end(copy);
        decoderErrors.append(QString::fromUtf8(line).trimmed());
    }
    av_log_default_callback(avcl, level, format, args);
}
}

// #1613: the cut clip. 6 s of 640x160 at 30 fps in GOPs of 15 frames (MPEG-4
// Part 2, which every FFmpeg encodes), with 48 kHz PCM starting 40 ms late. A
// bar moves 3 px per frame, so a frame's position in the clip can be read back
// from any export. Kept footage is grey; everything the edit leaves out (the
// trim and both cuts) is magenta, a colour nothing else has. White flashes with
// 1 kHz bursts sit on both sides of each cut, and one burst sits inside cut A.
namespace cutclip {
using putgd::Error;
constexpr int fps = 30, gop = 15, width = 640, height = 160, frames = 180, rate = 48000, audioDelayMs = 40;
constexpr qint64 durationUs = 6'000'000;
const QList<int> flashFrames{15, 33, 84, 111, 138, 162};  // 0.5, 1.1, 2.8, 3.7, 4.6, 5.4 s
constexpr int cutBurstFrame = 60;                          // 2.0 s, inside cut A
constexpr int flashLength = 3;                              // frames (100 ms)
const QRect spanningMask(0, 0, 64, 40), cutOnlyMask(576, 0, 64, 40);
constexpr qint64 spanningFrom = 1'000'000, spanningTo = 3'000'000;
// A frame's container time: Matroska counts milliseconds.
qint64 frameUs(int index) { return av_rescale_q(index, {1, fps}, {1, 1000}) * 1000; }
putgd::RecordingEdit edit() {
    putgd::RecordingEdit e;
    e.trimStart = 200'000;
    e.trimEnd = 5'800'000;
    // A starts between frames 42 and 43, so 43 (not a keyframe) is its first
    // frame; it ends on frame 78, mid-GOP after the keyframe at 75. B lies
    // inside one GOP and ends at 4.33 s, between frames 129 and 130.
    e.cuts = {{"a", 1'410'000, 2'600'000}, {"b", 4'120'000, 4'330'000}};
    putgd::RecordingMask spanning;
    spanning.kind = "solid"; spanning.rect = spanningMask; spanning.start = spanningFrom; spanning.end = spanningTo;
    putgd::RecordingMask inCut = spanning;
    inCut.rect = cutOnlyMask; inCut.start = 1'600'000; inCut.end = 2'400'000;
    e.masks = {spanning, inCut};
    return e;
}
bool flash(int index) { for (int f : flashFrames) if (index >= f && index < f + flashLength) return true; return false; }
Error write(const QString& path) {
    const KeptSpans kept(edit(), durationUs);
    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "matroska", QFile::encodeName(path).constData()) < 0 || !out) return {"fixture", "no muxer", {}};
    struct Closer { AVFormatContext*& f; ~Closer() { if (f && !(f->oformat->flags & AVFMT_NOFILE)) avio_closep(&f->pb); avformat_free_context(f); } } closer{out};
    AVCodecContext *video = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_MPEG4)), *audio = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE));
    struct Contexts { AVCodecContext*& v; AVCodecContext*& a; ~Contexts() { avcodec_free_context(&v); avcodec_free_context(&a); } } contexts{video, audio};
    if (!video || !audio) return {"fixture", "no encoder", {}};
    video->width = width; video->height = height; video->time_base = {1, fps}; video->framerate = {fps, 1};
    video->pix_fmt = AV_PIX_FMT_YUV420P; video->gop_size = gop; video->max_b_frames = 0;
    video->flags |= AV_CODEC_FLAG_QSCALE; video->global_quality = FF_QP2LAMBDA * 3;
    av_opt_set_int(video->priv_data, "sc_threshold", 1000000000, 0);  // keyframes on the GOP grid only
    audio->sample_rate = rate; audio->sample_fmt = AV_SAMPLE_FMT_S16; audio->time_base = {1, rate};
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&audio->ch_layout, &stereo);
    AVStream* streams[2] = {avformat_new_stream(out, nullptr), avformat_new_stream(out, nullptr)};
    AVCodecContext* contextOf[2] = {video, audio};
    for (int i = 0; i < 2; ++i) {
        if (out->oformat->flags & AVFMT_GLOBALHEADER) contextOf[i]->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (!streams[i] || avcodec_open2(contextOf[i], contextOf[i]->codec, nullptr) < 0 || avcodec_parameters_from_context(streams[i]->codecpar, contextOf[i]) < 0) return {"fixture", "cannot open an encoder", {}};
        streams[i]->time_base = contextOf[i]->time_base;
    }
    if (avio_open(&out->pb, QFile::encodeName(path).constData(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(out, nullptr) < 0) return {"fixture", "cannot write", {}};
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    struct Frames { AVPacket*& p; AVFrame*& f; ~Frames() { av_packet_free(&p); av_frame_free(&f); } } owned{packet, frame};
    auto encode = [&](int stream, AVFrame* input) -> Error {
        if (avcodec_send_frame(contextOf[stream], input) < 0) return {"fixture", "encoder refused a frame", {}};
        while (avcodec_receive_packet(contextOf[stream], packet) == 0) {
            av_packet_rescale_ts(packet, contextOf[stream]->time_base, streams[stream]->time_base);
            packet->stream_index = stream;
            if (av_interleaved_write_frame(out, packet) < 0) return {"fixture", "cannot write a packet", {}};
        }
        return {};
    };
    QList<qint64> bursts;
    for (int f : flashFrames) bursts << frameUs(f) * rate / 1'000'000;
    bursts << frameUs(cutBurstFrame) * rate / 1'000'000;
    qint64 samples = qint64(audioDelayMs) * rate / 1000;
    for (int index = 0; index < frames; ++index) {
        const bool white = flash(index), left = !kept.kept(frameUs(index));
        av_frame_unref(frame);
        frame->format = AV_PIX_FMT_YUV420P; frame->width = width; frame->height = height;
        if (av_frame_get_buffer(frame, 0) < 0) return {"fixture", "cannot allocate a frame", {}};
        const int barX = 8 + 3 * index;
        for (int y = 0; y < height; ++y) {
            auto* row = frame->data[0] + qint64(y) * frame->linesize[0];
            std::fill_n(row, width, uint8_t(white ? 235 : 100));
            std::fill_n(row + barX, 8, uint8_t(white ? 60 : 200));
        }
        for (int plane = 1; plane < 3; ++plane) for (int y = 0; y < height / 2; ++y) std::fill_n(frame->data[plane] + qint64(y) * frame->linesize[plane], width / 2, uint8_t(left ? 255 : 128));
        frame->pts = index;
        if (auto e = encode(0, frame)) return e;
        const qint64 until = qint64(index + 1) * rate / fps;
        while (samples < until) {
            const int count = int(std::min<qint64>(1024, until - samples));
            av_frame_unref(frame);
            frame->format = AV_SAMPLE_FMT_S16; frame->sample_rate = rate; frame->nb_samples = count;
            av_channel_layout_copy(&frame->ch_layout, &stereo);
            if (av_frame_get_buffer(frame, 0) < 0) return {"fixture", "cannot allocate audio", {}};
            auto* pcm = reinterpret_cast<int16_t*>(frame->data[0]);
            for (int i = 0; i < count; ++i) {
                const qint64 n = samples + i;
                bool burst = false;
                for (qint64 b : bursts) burst = burst || (n >= b && n < b + rate / 10);
                pcm[2 * i] = pcm[2 * i + 1] = int16_t(burst ? std::lround(8000.0 * std::sin(2.0 * M_PI * 1000.0 * double(n) / rate)) : 0);
            }
            frame->pts = samples;
            samples += count;
            if (auto e = encode(1, frame)) return e;
        }
    }
    if (auto e = encode(0, nullptr)) return e;
    if (auto e = encode(1, nullptr)) return e;
    if (av_write_trailer(out) < 0) return {"fixture", "cannot finish", {}};
    return {};
}
// What an export holds: per video frame its time, the clip frame the bar says
// it shows, whether each cover is drawn, flash and magenta; burst onsets.
struct Decoded {
    struct Frame { qint64 us = 0, durationUs = 0; int source = -1; bool white = false, spanningCovered = false, cutOnlyCovered = false; double chroma = 0; };
    QList<Frame> frames;
    QList<qint64> bursts;  // onset times in the output, µs
    int audioFrameSize = 1024;
    qint64 audioEndUs = 0;  // where the decoded audio ends, in output time (0 without audio)
    // The first channel, sample n at output time n / rate: each decoded audio
    // frame is placed at its own timestamp, so a hole or a shift shows as one.
    std::vector<float> audio;
};
// Reads one decoded picture (any format, the export's size) into result.frames.
QString measure(const AVFrame* f, qint64 us, qint64 durationUs, Decoded& result, SwsContext*& scaler, AVFrame* yuv) {
    scaler = sws_getCachedContext(scaler, f->width, f->height, AVPixelFormat(f->format), width, height, AV_PIX_FMT_YUV444P, SWS_POINT, nullptr, nullptr, nullptr);
    av_frame_unref(yuv);
    yuv->format = AV_PIX_FMT_YUV444P; yuv->width = width; yuv->height = height;
    if (!scaler || av_frame_get_buffer(yuv, 0) < 0) return "cannot convert a frame";
    sws_scale(scaler, f->data, f->linesize, 0, f->height, yuv->data, yuv->linesize);
    auto mean = [&](int plane, const QRect& r) {
        double sum = 0;
        for (int y = r.top(); y <= r.bottom(); ++y) for (int x = r.left(); x <= r.right(); ++x) sum += yuv->data[plane][qint64(y) * yuv->linesize[plane] + x];
        return sum / double(r.width() * r.height());
    };
    Decoded::Frame out;
    out.us = us;
    out.durationUs = durationUs;
    const double band = mean(0, QRect(0, 100, width, 50));
    out.white = band > 180;
    // The bar is the 8 columns that differ most from the frame's background.
    std::vector<double> columns(width);
    for (int x = 0; x < width; ++x) { double sum = 0; for (int y = 100; y < 150; ++y) sum += yuv->data[0][qint64(y) * yuv->linesize[0] + x]; columns[size_t(x)] = sum / 50; }
    std::vector<double> sorted = columns;
    std::nth_element(sorted.begin(), sorted.begin() + width / 2, sorted.end());
    const double background = sorted[size_t(width / 2)];
    double best = -1;
    int bestX = 0;
    for (int x = 0; x + 8 <= width; ++x) { double d = 0; for (int i = 0; i < 8; ++i) d += std::abs(columns[size_t(x + i)] - background); if (d > best) { best = d; bestX = x; } }
    out.source = int(std::lround((bestX - 8) / 3.0));
    out.spanningCovered = mean(0, spanningMask.adjusted(4, 4, -4, -4)) < 50;
    out.cutOnlyCovered = mean(0, cutOnlyMask.adjusted(4, 4, -4, -4)) < 50;
    out.chroma = std::abs(mean(1, QRect(0, 0, width, height)) - 128) + std::abs(mean(2, QRect(0, 0, width, height)) - 128);
    result.frames.append(out);
    return {};
}
// #1635: an animated WebP, frame by frame. FFmpeg's webp decoder (6.1 to 7.1,
// as Debian and Ubuntu ship it) reads still images only: it skips the ANIM and
// ANMF chunks and fails every packet. So each ANMF frame is rewrapped as a
// still WebP, decoded on its own, and composited onto the canvas as libwebp's
// demuxer does (offset, alpha blending unless the frame says not to, disposal
// to transparent). Each frame lasts its ANMF duration, so pictures libwebp's
// encoder merged come back as one long frame, and onGrid samples them by time.
QString analyseAnimatedWebp(const QByteArray& file, Decoded& result) {
    auto u24 = [&](qsizetype at) { return int(uchar(file[at])) | int(uchar(file[at + 1])) << 8 | int(uchar(file[at + 2])) << 16; };
    auto u32 = [&](qsizetype at) { return quint32(u24(at)) | quint32(uchar(file[at + 3])) << 24; };
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WEBP);
    if (!codec) return "cannot set up a decoder";
    auto chunk = [](const char* fourcc, const QByteArray& payload) {
        QByteArray out(fourcc, 4);
        const quint32 size = quint32(payload.size());
        for (int i = 0; i < 4; ++i) out += char((size >> (8 * i)) & 0xff);
        out += payload;
        if (payload.size() % 2) out += '\0';
        return out;
    };
    int canvasW = 0, canvasH = 0;
    std::vector<uint8_t> canvas;  // RGBA
    AVFrame* rgba = av_frame_alloc();
    AVFrame* yuv = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    AVFrame* decoded = av_frame_alloc();
    SwsContext* scaler = nullptr;
    SwsContext* toRgba = nullptr;
    struct Owned { AVFrame*& a; AVFrame*& b; AVFrame*& c; AVPacket*& p; SwsContext*& s; SwsContext*& t; ~Owned() { av_frame_free(&a); av_frame_free(&b); av_frame_free(&c); av_packet_free(&p); sws_freeContext(s); sws_freeContext(t); } } owned{rgba, yuv, decoded, packet, scaler, toRgba};
    qint64 us = 0;
    for (qsizetype at = 12; at + 8 <= file.size();) {
        const QByteArray fourcc = file.mid(at, 4);
        const qsizetype size = qsizetype(u32(at + 4)), body = at + 8;
        if (body + size > file.size()) return "truncated WebP chunk";
        at = body + size + (size % 2);
        if (fourcc == "VP8X") { canvasW = u24(body + 4) + 1; canvasH = u24(body + 7) + 1; canvas.assign(size_t(canvasW) * size_t(canvasH) * 4, 0); continue; }
        if (fourcc != "ANMF") continue;
        if (canvas.empty() || size < 16) return "ANMF frame before the canvas";
        const int x0 = 2 * u24(body), y0 = 2 * u24(body + 3), w = u24(body + 6) + 1, h = u24(body + 9) + 1, durationMs = u24(body + 12);
        const uchar flags = uchar(file[body + 15]);
        const bool blend = !(flags & 2), dispose = flags & 1;
        // The frame's own chunks (ALPH, VP8 or VP8L) as a still image.
        QByteArray frameChunks = file.mid(body + 16, size - 16), still;
        if (frameChunks.startsWith("ALPH")) {
            QByteArray vp8x(10, '\0');
            vp8x[0] = 0x10;  // alpha
            for (int i = 0; i < 3; ++i) { vp8x[4 + i] = char(((w - 1) >> (8 * i)) & 0xff); vp8x[7 + i] = char(((h - 1) >> (8 * i)) & 0xff); }
            still = chunk("VP8X", vp8x) + frameChunks;
        } else still = frameChunks;
        QByteArray riff = "RIFF";
        const quint32 riffSize = quint32(still.size() + 4);
        for (int i = 0; i < 4; ++i) riff += char((riffSize >> (8 * i)) & 0xff);
        riff += "WEBP" + still;
        AVCodecContext* decoder = avcodec_alloc_context3(codec);
        struct Ctx { AVCodecContext*& d; ~Ctx() { avcodec_free_context(&d); } } ctx{decoder};
        if (!decoder || avcodec_open2(decoder, codec, nullptr) < 0) return "cannot open a decoder";
        av_packet_unref(packet);
        if (av_new_packet(packet, int(riff.size())) < 0) return "cannot allocate a packet";
        std::memcpy(packet->data, riff.constData(), size_t(riff.size()));
        if (avcodec_send_packet(decoder, packet) < 0 || avcodec_receive_frame(decoder, decoded) < 0) return QStringLiteral("cannot decode WebP frame %1").arg(result.frames.size());
        if (decoded->width != w || decoded->height != h) return "WebP frame size mismatch";
        av_frame_unref(rgba);
        rgba->format = AV_PIX_FMT_RGBA; rgba->width = w; rgba->height = h;
        toRgba = sws_getCachedContext(toRgba, w, h, AVPixelFormat(decoded->format), w, h, AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
        if (!toRgba || av_frame_get_buffer(rgba, 0) < 0) return "cannot convert a WebP frame";
        sws_scale(toRgba, decoded->data, decoded->linesize, 0, h, rgba->data, rgba->linesize);
        av_frame_unref(decoded);
        for (int y = 0; y < h && y0 + y < canvasH; ++y) for (int x = 0; x < w && x0 + x < canvasW; ++x) {
            const uint8_t* src = rgba->data[0] + qint64(y) * rgba->linesize[0] + 4 * x;
            uint8_t* dst = canvas.data() + (size_t(y0 + y) * size_t(canvasW) + size_t(x0 + x)) * 4;
            if (!blend || src[3] == 255) { std::copy_n(src, 4, dst); continue; }
            const double a = src[3] / 255.0, b = dst[3] / 255.0 * (1 - a), out = a + b;
            for (int c = 0; c < 3; ++c) dst[c] = uint8_t(out > 0 ? std::lround((src[c] * a + dst[c] * b) / out) : 0);
            dst[3] = uint8_t(std::lround(out * 255));
        }
        // Hand the canvas over as an RGBA picture.
        av_frame_unref(rgba);
        rgba->format = AV_PIX_FMT_RGBA; rgba->width = canvasW; rgba->height = canvasH;
        if (av_frame_get_buffer(rgba, 0) < 0) return "cannot allocate the canvas";
        for (int y = 0; y < canvasH; ++y) std::memcpy(rgba->data[0] + qint64(y) * rgba->linesize[0], canvas.data() + size_t(y) * size_t(canvasW) * 4, size_t(canvasW) * 4);
        if (auto failure = measure(rgba, us, qint64(durationMs) * 1000, result, scaler, yuv); !failure.isEmpty()) return failure;
        us += qint64(durationMs) * 1000;
        if (dispose) for (int y = y0; y < std::min(canvasH, y0 + h); ++y) std::fill_n(canvas.data() + (size_t(y) * size_t(canvasW) + size_t(x0)) * 4, size_t(std::min(w, canvasW - x0)) * 4, uint8_t(0));
    }
    return result.frames.isEmpty() ? QStringLiteral("no WebP frame decoded") : QString();
}
QString analyse(const QString& path, Decoded& result) {
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const auto bytes = file.readAll();
            if (bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP" && bytes.mid(12, 4) == "VP8X" && bytes.contains("ANMF")) return analyseAnimatedWebp(bytes, result);
        }
    }
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, QFile::encodeName(path).constData(), nullptr, nullptr) < 0 || avformat_find_stream_info(format, nullptr) < 0) return "cannot open the export";
    struct Closer { AVFormatContext*& f; ~Closer() { avformat_close_input(&f); } } closer{format};
    const int videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    AVCodecContext* decoders[2] = {};
    int indexes[2] = {videoIndex, audioIndex};
    struct Contexts { AVCodecContext** d; ~Contexts() { avcodec_free_context(&d[0]); avcodec_free_context(&d[1]); } } contexts{decoders};
    for (int i = 0; i < 2; ++i) {
        if (indexes[i] < 0) continue;
        const AVCodec* codec = avcodec_find_decoder(format->streams[indexes[i]]->codecpar->codec_id);
        decoders[i] = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!decoders[i] || avcodec_parameters_to_context(decoders[i], format->streams[indexes[i]]->codecpar) < 0) return "cannot set up a decoder";
        decoders[i]->pkt_timebase = format->streams[indexes[i]]->time_base;
        if (avcodec_open2(decoders[i], codec, nullptr) < 0) return "cannot open a decoder";
    }
    if (audioIndex >= 0 && format->streams[audioIndex]->codecpar->frame_size > 0) result.audioFrameSize = format->streams[audioIndex]->codecpar->frame_size;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* yuv = av_frame_alloc();
    struct Frames { AVPacket*& p; AVFrame*& f; AVFrame*& y; ~Frames() { av_packet_free(&p); av_frame_free(&f); av_frame_free(&y); } } owned{packet, frame, yuv};
    SwsContext* scaler = nullptr;
    std::vector<float>& mono = result.audio;
    auto takeVideo = [&](AVFrame* f) -> QString {
        const auto timeBase = format->streams[videoIndex]->time_base;
        return measure(f, av_rescale_q(f->best_effort_timestamp, timeBase, {1, 1000000}), f->duration > 0 ? av_rescale_q(f->duration, timeBase, {1, 1000000}) : 0, result, scaler, yuv);
    };
    auto takeAudio = [&](AVFrame* f) {
        const qint64 at = av_rescale_q(f->best_effort_timestamp, format->streams[audioIndex]->time_base, {1, rate});
        const bool planar = av_sample_fmt_is_planar(AVSampleFormat(f->format));
        if (at + f->nb_samples > qint64(mono.size())) mono.resize(size_t(at + f->nb_samples), 0.0f);
        for (int i = 0; i < f->nb_samples; ++i) {
            float v = 0;
            if (f->format == AV_SAMPLE_FMT_FLTP || f->format == AV_SAMPLE_FMT_FLT) v = planar ? reinterpret_cast<const float*>(f->extended_data[0])[i] : reinterpret_cast<const float*>(f->data[0])[i * f->ch_layout.nb_channels];
            else if (f->format == AV_SAMPLE_FMT_S16P || f->format == AV_SAMPLE_FMT_S16) v = (planar ? reinterpret_cast<const int16_t*>(f->extended_data[0])[i] : reinterpret_cast<const int16_t*>(f->data[0])[i * f->ch_layout.nb_channels]) / 32768.0f;
            if (at + i >= 0) mono[size_t(at + i)] = v;
        }
    };
    QString failure;
    auto receive = [&](int i) {
        while (failure.isEmpty() && avcodec_receive_frame(decoders[i], frame) >= 0) {
            if (i == 0) failure = takeVideo(frame); else takeAudio(frame);
            av_frame_unref(frame);
        }
    };
    while (failure.isEmpty() && av_read_frame(format, packet) >= 0) {
        for (int i = 0; i < 2; ++i) if (decoders[i] && packet->stream_index == indexes[i] && avcodec_send_packet(decoders[i], packet) >= 0) receive(i);
        av_packet_unref(packet);
    }
    for (int i = 0; i < 2; ++i) if (decoders[i] && avcodec_send_packet(decoders[i], nullptr) >= 0) receive(i);
    sws_freeContext(scaler);
    // A decoder that rejects every packet must not pass as an empty export.
    if (failure.isEmpty() && videoIndex >= 0 && result.frames.isEmpty()) failure = "no video frame decoded";
    result.audioEndUs = qint64(mono.size()) * 1'000'000 / rate;
    // Burst onsets: a 1 ms window over 0.05 RMS after at least 20 ms under 0.02.
    const int window = rate / 1000;
    int quiet = 0;
    for (size_t at = 0; at + size_t(window) <= mono.size(); at += size_t(window)) {
        double energy = 0;
        for (int i = 0; i < window; ++i) energy += double(mono[at + size_t(i)]) * mono[at + size_t(i)];
        const double rms = std::sqrt(energy / window);
        if (rms > 0.05 && quiet >= 20) result.bursts << qint64(at) * 1'000'000 / rate;
        quiet = rms < 0.02 ? quiet + 1 : (rms > 0.05 ? 0 : quiet);
    }
    return failure;
}
// Every colour a GIF's global and local colour tables hold, as 0xRRGGBB.
QList<quint32> gifPalette(const QByteArray& gif) {
    QList<quint32> colours;
    auto table = [&](qsizetype at, int bits) { const qsizetype n = qsizetype(1) << (bits + 1); for (qsizetype i = 0; i < n && at + 3 * i + 2 < gif.size(); ++i) colours << (quint32(uchar(gif[at + 3 * i])) << 16 | quint32(uchar(gif[at + 3 * i + 1])) << 8 | uchar(gif[at + 3 * i + 2])); return at + 3 * n; };
    auto skipBlocks = [&](qsizetype at) { while (at < gif.size() && uchar(gif[at])) at += 1 + uchar(gif[at]); return at + 1; };
    if (gif.size() < 13 || !gif.startsWith("GIF")) return colours;
    qsizetype at = 13;
    if (uchar(gif[10]) & 0x80) at = table(13, uchar(gif[10]) & 7);
    while (at < gif.size()) {
        const uchar block = uchar(gif[at++]);
        if (block == 0x21) at = skipBlocks(at + 1);
        else if (block == 0x2C) {
            if (at + 9 > gif.size()) break;
            const uchar packed = uchar(gif[at + 8]);
            at += 9;
            if (packed & 0x80) at = table(at, packed & 7);
            at = skipBlocks(at + 1);
        } else break;
    }
    return colours;
}
// The picture on screen at each output step from the first frame to the end of
// the last one, read from each frame's own time and duration: an encoder that
// merges identical frames (libwebp's animation encoder does) shows the same
// sequence as one that repeats them. Each entry's us is its step's exact time.
QList<Decoded::Frame> onGrid(const QList<Decoded::Frame>& frames, int fps) {
    QList<Decoded::Frame> grid;
    if (frames.isEmpty()) return grid;
    const qint64 first = frames.first().us, step = 1'000'000 / fps;
    const qint64 end = frames.last().us + (frames.last().durationUs > 0 ? frames.last().durationUs : step);
    qsizetype shown = 0;
    for (qint64 k = 0;; ++k) {
        const qint64 t = first + av_rescale(k, 1'000'000, fps);
        if (t >= end - step / 2) break;
        // Container ticks (GIF counts centiseconds) put a frame up to half a step off its time.
        while (shown + 1 < frames.size() && frames[shown + 1].us <= t + step / 2) ++shown;
        auto f = frames[shown];
        f.us = t;
        f.durationUs = step;
        grid.append(f);
    }
    return grid;
}
// The amplitude of a tone of `hz` in the 10 ms of audio centred on output time
// us (Hann-windowed, so a neighbouring tone or a fade barely leaks into it).
double toneAt(const std::vector<float>& audio, qint64 us, double hz) {
    constexpr int length = rate / 100;
    const qint64 from = us * rate / 1'000'000 - length / 2;
    double re = 0, im = 0, weight = 0;
    for (int i = 0; i < length; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (length - 1));
        const qint64 n = from + i;
        const double v = n >= 0 && n < qint64(audio.size()) ? audio[size_t(n)] : 0.0;
        re += w * v * std::cos(2.0 * M_PI * hz * double(n) / rate);
        im += w * v * std::sin(2.0 * M_PI * hz * double(n) / rate);
        weight += w;
    }
    return 2.0 * std::hypot(re, im) / weight;
}
// The RMS of the audio over [fromUs, toUs).
double rmsOver(const std::vector<float>& audio, qint64 fromUs, qint64 toUs) {
    double energy = 0;
    qint64 count = 0;
    for (qint64 n = fromUs * rate / 1'000'000; n < toUs * rate / 1'000'000; ++n, ++count) if (n >= 0 && n < qint64(audio.size())) energy += double(audio[size_t(n)]) * audio[size_t(n)];
    return count ? std::sqrt(energy / double(count)) : 0.0;
}
}

// #1613: a clip with a real frame gap, as a stepped-down or static capture
// leaves one. 640x160 MPEG-4 Part 2 in GOPs of 15 frames on a millisecond time
// base: frames 0..47 at 30 fps, frame 47 (1.567 s) then held a second with no
// frame, and frames from 48 on at 30 fps again from 2.6 s; 48 kHz PCM runs
// throughout. The bar names the frame, as in the cut clip. Flashes with bursts
// on frames 6, 51 and 60, and one more burst inside the gap (2.2 s), over the
// still frame 47.
namespace gapclip {
using putgd::Error;
using cutclip::width, cutclip::height, cutclip::rate;
constexpr int fps = 30, gop = 15, frames = 108, held = 47;
constexpr qint64 resumeMs = 2600, gapBurstMs = 2200, durationUs = 4'600'000;
const QList<int> flashFrames{6, 51, 60};  // 0.2, 2.7, 3.0 s
constexpr int flashLength = 3;
qint64 frameMs(int index) { return index <= held ? av_rescale(index, 1000, fps) : resumeMs + av_rescale(index - held - 1, 1000, fps); }
qint64 frameUs(int index) { return frameMs(index) * 1000; }
// The clip frame on screen at a source time.
int onScreen(qint64 sourceUs) { int index = 0; while (index + 1 < frames && frameUs(index + 1) <= sourceUs) ++index; return index; }
QList<qint64> burstsUs() { QList<qint64> out{gapBurstMs * 1000}; for (int f : flashFrames) out << frameUs(f); std::sort(out.begin(), out.end()); return out; }
bool flash(int index) { for (int f : flashFrames) if (index >= f && index < f + flashLength) return true; return false; }
Error write(const QString& path) {
    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "matroska", QFile::encodeName(path).constData()) < 0 || !out) return {"fixture", "no muxer", {}};
    struct Closer { AVFormatContext*& f; ~Closer() { if (f && !(f->oformat->flags & AVFMT_NOFILE)) avio_closep(&f->pb); avformat_free_context(f); } } closer{out};
    AVCodecContext *video = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_MPEG4)), *audio = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE));
    struct Contexts { AVCodecContext*& v; AVCodecContext*& a; ~Contexts() { avcodec_free_context(&v); avcodec_free_context(&a); } } contexts{video, audio};
    if (!video || !audio) return {"fixture", "no encoder", {}};
    video->width = width; video->height = height; video->time_base = {1, 1000}; video->framerate = {fps, 1};
    video->pix_fmt = AV_PIX_FMT_YUV420P; video->gop_size = gop; video->max_b_frames = 0;
    video->flags |= AV_CODEC_FLAG_QSCALE; video->global_quality = FF_QP2LAMBDA * 3;
    av_opt_set_int(video->priv_data, "sc_threshold", 1000000000, 0);
    audio->sample_rate = rate; audio->sample_fmt = AV_SAMPLE_FMT_S16; audio->time_base = {1, rate};
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&audio->ch_layout, &stereo);
    AVStream* streams[2] = {avformat_new_stream(out, nullptr), avformat_new_stream(out, nullptr)};
    AVCodecContext* contextOf[2] = {video, audio};
    for (int i = 0; i < 2; ++i) {
        if (out->oformat->flags & AVFMT_GLOBALHEADER) contextOf[i]->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (!streams[i] || avcodec_open2(contextOf[i], contextOf[i]->codec, nullptr) < 0 || avcodec_parameters_from_context(streams[i]->codecpar, contextOf[i]) < 0) return {"fixture", "cannot open an encoder", {}};
        streams[i]->time_base = contextOf[i]->time_base;
    }
    if (avio_open(&out->pb, QFile::encodeName(path).constData(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(out, nullptr) < 0) return {"fixture", "cannot write", {}};
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    struct Frames { AVPacket*& p; AVFrame*& f; ~Frames() { av_packet_free(&p); av_frame_free(&f); } } owned{packet, frame};
    auto encode = [&](int stream, AVFrame* input) -> Error {
        if (avcodec_send_frame(contextOf[stream], input) < 0) return {"fixture", "encoder refused a frame", {}};
        while (avcodec_receive_packet(contextOf[stream], packet) == 0) {
            av_packet_rescale_ts(packet, contextOf[stream]->time_base, streams[stream]->time_base);
            packet->stream_index = stream;
            if (av_interleaved_write_frame(out, packet) < 0) return {"fixture", "cannot write a packet", {}};
        }
        return {};
    };
    QList<qint64> bursts;
    for (qint64 us : burstsUs()) bursts << us * rate / 1'000'000;
    qint64 samples = 0;
    for (int index = 0; index < frames; ++index) {
        const bool white = flash(index);
        av_frame_unref(frame);
        frame->format = AV_PIX_FMT_YUV420P; frame->width = width; frame->height = height;
        if (av_frame_get_buffer(frame, 0) < 0) return {"fixture", "cannot allocate a frame", {}};
        const int barX = 8 + 3 * index;
        for (int y = 0; y < height; ++y) {
            auto* row = frame->data[0] + qint64(y) * frame->linesize[0];
            std::fill_n(row, width, uint8_t(white ? 235 : 100));
            std::fill_n(row + barX, 8, uint8_t(white ? 60 : 200));
        }
        for (int plane = 1; plane < 3; ++plane) for (int y = 0; y < height / 2; ++y) std::fill_n(frame->data[plane] + qint64(y) * frame->linesize[plane], width / 2, uint8_t(128));
        frame->pts = frameMs(index);
        if (auto e = encode(0, frame)) return e;
        // Audio up to the next frame's time, so the file interleaves across the gap too.
        const qint64 until = (index + 1 < frames ? frameUs(index + 1) : durationUs) * rate / 1'000'000;
        while (samples < until) {
            const int count = int(std::min<qint64>(1024, until - samples));
            av_frame_unref(frame);
            frame->format = AV_SAMPLE_FMT_S16; frame->sample_rate = rate; frame->nb_samples = count;
            av_channel_layout_copy(&frame->ch_layout, &stereo);
            if (av_frame_get_buffer(frame, 0) < 0) return {"fixture", "cannot allocate audio", {}};
            auto* pcm = reinterpret_cast<int16_t*>(frame->data[0]);
            for (int i = 0; i < count; ++i) {
                const qint64 n = samples + i;
                bool burst = false;
                for (qint64 b : bursts) burst = burst || (n >= b && n < b + rate / 10);
                pcm[2 * i] = pcm[2 * i + 1] = int16_t(burst ? std::lround(8000.0 * std::sin(2.0 * M_PI * 1000.0 * double(n) / rate)) : 0);
            }
            frame->pts = samples;
            samples += count;
            if (auto e = encode(1, frame)) return e;
        }
    }
    if (auto e = encode(0, nullptr)) return e;
    if (auto e = encode(1, nullptr)) return e;
    if (av_write_trailer(out) < 0) return {"fixture", "cannot finish", {}};
    return {};
}
}

// #1635: a plain clip on Matroska's millisecond clock: `frames` frames at
// `fps`, frame k at round(k * 1000 / fps) ms, the bar naming the frame as in
// the cut clip. With a signal, 48 kHz PCM of signal(source µs) in [-1, 1]
// throughout, except that no samples are written over [holeFromUs, holeToUs)
// (a hole in the audio timestamps, as a pause can leave). With timesMs, frame
// k is at timesMs[k] instead (a variable-rate capture). The bar names the
// frame modulo 200 (the frame width allows 210); unwrap() restores it.
namespace plainclip {
using putgd::Error;
using cutclip::width, cutclip::height, cutclip::rate;
qint64 frameUs(int fps, int index) { return av_rescale(index, 1000, fps) * 1000; }
// The review's time of frame k: edges snap to round(k / fps) µs.
qint64 gridUs(int fps, int index) { return qRound64(index * 1e6 / fps); }
// Clip frame numbers from bar positions (modulo 200), given they only increase.
QList<int> unwrap(const QList<cutclip::Decoded::Frame>& frames) {
    QList<int> out;
    int base = 0;
    for (const auto& f : frames) {
        if (!out.isEmpty() && base + f.source < out.last() - 100) base += 200;
        out << base + f.source;
    }
    return out;
}
// muxer and videoRate pick another container and video clock (AVI at 1/30 s:
// a time base coarser than Matroska's millisecond).
Error write(const QString& path, int fps, int frames, const std::function<double(qint64)>& signal = {}, qint64 holeFromUs = 0, qint64 holeToUs = 0, const QList<qint64>& timesMs = {}, const char* muxer = "matroska", int videoRate = 1000) {
    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, muxer, QFile::encodeName(path).constData()) < 0 || !out) return {"fixture", "no muxer", {}};
    struct Closer { AVFormatContext*& f; ~Closer() { if (f && !(f->oformat->flags & AVFMT_NOFILE)) avio_closep(&f->pb); avformat_free_context(f); } } closer{out};
    AVCodecContext *video = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_MPEG4)), *audio = signal ? avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE)) : nullptr;
    struct Contexts { AVCodecContext*& v; AVCodecContext*& a; ~Contexts() { avcodec_free_context(&v); avcodec_free_context(&a); } } contexts{video, audio};
    if (!video || (signal && !audio)) return {"fixture", "no encoder", {}};
    video->width = width; video->height = height; video->time_base = {1, videoRate}; video->framerate = {fps, 1};
    video->pix_fmt = AV_PIX_FMT_YUV420P; video->gop_size = 15; video->max_b_frames = 0;
    video->flags |= AV_CODEC_FLAG_QSCALE; video->global_quality = FF_QP2LAMBDA * 3;
    av_opt_set_int(video->priv_data, "sc_threshold", 1000000000, 0);
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (audio) { audio->sample_rate = rate; audio->sample_fmt = AV_SAMPLE_FMT_S16; audio->time_base = {1, rate}; av_channel_layout_copy(&audio->ch_layout, &stereo); }
    const int streamCount = audio ? 2 : 1;
    AVStream* streams[2] = {};
    AVCodecContext* contextOf[2] = {video, audio};
    for (int i = 0; i < streamCount; ++i) {
        streams[i] = avformat_new_stream(out, nullptr);
        if (out->oformat->flags & AVFMT_GLOBALHEADER) contextOf[i]->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (!streams[i] || avcodec_open2(contextOf[i], contextOf[i]->codec, nullptr) < 0 || avcodec_parameters_from_context(streams[i]->codecpar, contextOf[i]) < 0) return {"fixture", "cannot open an encoder", {}};
        streams[i]->time_base = contextOf[i]->time_base;
    }
    if (avio_open(&out->pb, QFile::encodeName(path).constData(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(out, nullptr) < 0) return {"fixture", "cannot write", {}};
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    struct Frames { AVPacket*& p; AVFrame*& f; ~Frames() { av_packet_free(&p); av_frame_free(&f); } } owned{packet, frame};
    auto encode = [&](int stream, AVFrame* input) -> Error {
        if (avcodec_send_frame(contextOf[stream], input) < 0) return {"fixture", "encoder refused a frame", {}};
        while (avcodec_receive_packet(contextOf[stream], packet) == 0) {
            av_packet_rescale_ts(packet, contextOf[stream]->time_base, streams[stream]->time_base);
            packet->stream_index = stream;
            if (av_interleaved_write_frame(out, packet) < 0) return {"fixture", "cannot write a packet", {}};
        }
        return {};
    };
    const qint64 holeFrom = holeFromUs * rate / 1'000'000, holeTo = holeToUs * rate / 1'000'000;
    qint64 samples = 0;
    for (int index = 0; index < frames; ++index) {
        av_frame_unref(frame);
        frame->format = AV_PIX_FMT_YUV420P; frame->width = width; frame->height = height;
        if (av_frame_get_buffer(frame, 0) < 0) return {"fixture", "cannot allocate a frame", {}};
        for (int y = 0; y < height; ++y) {
            auto* row = frame->data[0] + qint64(y) * frame->linesize[0];
            std::fill_n(row, width, uint8_t(100));
            std::fill_n(row + 8 + 3 * (index % 200), 8, uint8_t(200));
        }
        for (int plane = 1; plane < 3; ++plane) for (int y = 0; y < height / 2; ++y) std::fill_n(frame->data[plane] + qint64(y) * frame->linesize[plane], width / 2, uint8_t(128));
        auto timeUs = [&](int k) { return timesMs.isEmpty() ? frameUs(fps, k) : k < timesMs.size() ? timesMs[k] * 1000 : timesMs.last() * 1000 + 1'000'000 / fps; };
        frame->pts = av_rescale(timeUs(index), videoRate, 1'000'000);
        if (auto e = encode(0, frame)) return e;
        if (!audio) continue;
        const qint64 until = timeUs(index + 1) * rate / 1'000'000;
        while (samples < until) {
            if (samples >= holeFrom && samples < holeTo) { samples = std::min(holeTo, until); continue; }
            const int count = int(std::min<qint64>({1024, until - samples, samples < holeFrom ? holeFrom - samples : 1024}));
            av_frame_unref(frame);
            frame->format = AV_SAMPLE_FMT_S16; frame->sample_rate = rate; frame->nb_samples = count;
            av_channel_layout_copy(&frame->ch_layout, &stereo);
            if (av_frame_get_buffer(frame, 0) < 0) return {"fixture", "cannot allocate audio", {}};
            auto* pcm = reinterpret_cast<int16_t*>(frame->data[0]);
            for (int i = 0; i < count; ++i) pcm[2 * i] = pcm[2 * i + 1] = int16_t(std::lround(32767.0 * std::clamp(signal((samples + i) * 1'000'000 / rate), -1.0, 1.0)));
            frame->pts = samples;
            samples += count;
            if (auto e = encode(1, frame)) return e;
        }
    }
    for (int i = 0; i < streamCount; ++i) if (auto e = encode(i, nullptr)) return e;
    if (av_write_trailer(out) < 0) return {"fixture", "cannot finish", {}};
    return {};
}
}

// #86: a working clip's audio starts after its first video frame (the sink
// monitor opens after the screen stream; 20-110 ms on the lab desktops). Every
// export with sound must keep that gap rather than rebase the audio to its own
// first sample; silent exports (GIF, WebP, audio off) never read the audio.
// #130: a pause can leave a hole in the working clip's audio timestamps; the
// export fills it with silence instead of pulling the later audio earlier.
// #119: every Opus packet of a WebM export, the end-trimmed last one included,
// decodes without error and the decoded audio lasts as long as the export.
class ExportAvSync : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString clip_, gapped_, cut_, frameGap_, sixty_;
    static double median(const QString& path) {
        QJsonObject sync;
        const auto error = measureAvSync(path, sync);
        if (error) qWarning() << error.code << error.message;
        return error ? NAN : sync["medianMs"].toDouble();
    }
    // Decodes every audio packet of path, as written, with the named decoder and
    // returns the samples it produced, or -1 when it rejects a packet or logs an
    // error. The demuxer's Opus parser stays out: it would drop a broken packet
    // before the decoder saw it, and FFmpeg 8.0's logs a spurious "Error parsing
    // Opus packet header" at EOF for every Matroska Opus file, whoever wrote it
    // (fixed upstream in 8.1 by 618fc15e65).
    static qint64 decodeAudio(const QString& path, const char* decoderName, int& packets, QString& failure) {
        AVFormatContext* format = avformat_alloc_context();
        if (format) format->flags |= AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
        if (!format || avformat_open_input(&format, QFile::encodeName(path).constData(), nullptr, nullptr) < 0 || avformat_find_stream_info(format, nullptr) < 0) { failure = "cannot open"; avformat_close_input(&format); return -1; }
        const int index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        const AVCodec* codec = avcodec_find_decoder_by_name(decoderName);
        AVCodecContext* decoder = codec && index >= 0 ? avcodec_alloc_context3(codec) : nullptr;
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        qint64 samples = 0;
        packets = 0;
        decoderErrors.clear();
        checkedDecoder = decoder;
        av_log_set_callback(recordDecoderErrors);
        if (!decoder || avcodec_parameters_to_context(decoder, format->streams[index]->codecpar) < 0 || avcodec_open2(decoder, codec, nullptr) < 0) failure = "cannot open the decoder";
        auto receive = [&]() {
            int rc;
            while ((rc = avcodec_receive_frame(decoder, frame)) >= 0) { samples += frame->nb_samples; av_frame_unref(frame); }
            if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) failure = QStringLiteral("receive failed after packet %1").arg(packets);
        };
        while (failure.isEmpty() && av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == index) {
                ++packets;
                if (avcodec_send_packet(decoder, packet) < 0) failure = QStringLiteral("packet %1 at pts %2 rejected").arg(packets).arg(packet->pts);
                else receive();
            }
            av_packet_unref(packet);
        }
        if (failure.isEmpty() && avcodec_send_packet(decoder, nullptr) >= 0) receive();
        av_log_set_callback(av_log_default_callback);
        checkedDecoder = nullptr;
        if (failure.isEmpty() && !decoderErrors.isEmpty()) failure = decoderErrors.join("; ");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return failure.isEmpty() ? samples : -1;
    }
    // Exports the clip; false with a skip reason when this FFmpeg lacks the format's encoders.
    bool exportClip(const QString& name, const QString& format, const QString& codec, bool audio, qint64 trimStart, QString* skip, const QString& input = {}) {
        ExportRequest request;
        request.input = input.isEmpty() ? clip_ : input;
        request.output = dir_.filePath(name);
        request.durationUs = 4'000'000;
        request.canvas = {1280, 720};
        request.edit.trimStart = trimStart;
        request.config.format = format;
        request.config.codec = codec;
        request.config.audio.system = audio;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") {
            *skip = QStringLiteral("%1 export unavailable in this FFmpeg: %2").arg(format, error.message);
            return false;
        }
        if (error) { qWarning() << error.code << error.message; return false; }
        if (result.frames < 1) return false;
        return audio != result.audioEncoder.isEmpty();
    }
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        clip_ = dir_.filePath("working.mkv");
        QVERIFY(!writeAvMarker(clip_, 4, 110));
        // The marker itself is in sync in container time, with audio starting 110 ms in.
        QVERIFY(std::abs(median(clip_)) <= 1.0);
        // The same with 60 ms of audio missing at 0.5 s, before three of the bursts.
        gapped_ = dir_.filePath("gapped.mkv");
        QVERIFY(!writeAvMarker(gapped_, 4, 110, 500, 60));
        QVERIFY(std::abs(median(gapped_)) <= 1.0);
        cut_ = dir_.filePath("cut.mkv");
        const auto written = cutclip::write(cut_);
        QVERIFY2(!written, qPrintable(written.message));
        frameGap_ = dir_.filePath("frame-gap.mkv");
        const auto writtenGap = gapclip::write(frameGap_);
        QVERIFY2(!writtenGap, qPrintable(writtenGap.message));
        sixty_ = dir_.filePath("sixty.mkv");
        const auto writtenSixty = plainclip::write(sixty_, 60, 150);
        QVERIFY2(!writtenSixty, qPrintable(writtenSixty.message));
    }
    void exportsWithSoundKeepTheAudioStart_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("codec");
        QTest::addColumn<qint64>("trimStart");
        // #1045: a trim can start between two frames, as it does here 10 ms past
        // the frame at 1 s of this 30 fps clip. Rebasing video on its first frame
        // played it that gap early. A stepped clip has such gaps throughout.
        QTest::newRow("mp4 trimmed between frames") << "mp4" << "h264" << qint64(1'010'000);
        QTest::newRow("webm trimmed") << "webm" << "vp9" << qint64(1'000'000);
    }
    void exportsWithSoundKeepTheAudioStart() {
        QFETCH(QString, format);
        QFETCH(QString, codec);
        QFETCH(qint64, trimStart);
        const auto name = QStringLiteral("%1-%2.%3").arg(format).arg(trimStart).arg(format);
        QString skip;
        const bool exported = exportClip(name, format, codec, true, trimStart, &skip);
        if (!skip.isEmpty()) QSKIP(qPrintable(skip));
        QVERIFY(exported);
        // Within half a frame at 30 fps; before the fix this read -110 ms.
        const double offset = median(dir_.filePath(name));
        QVERIFY2(std::abs(offset) <= 17.0, qPrintable(QString::number(offset)));
    }
    void exportsKeepAnAudioGap_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("codec");
        QTest::newRow("mp4") << "mp4" << "h264";
    }
    void exportsKeepAnAudioGap() {
        QFETCH(QString, format);
        QFETCH(QString, codec);
        const auto name = QStringLiteral("gapped.%1").arg(format);
        QString skip;
        const bool exported = exportClip(name, format, codec, true, 0, &skip, gapped_);
        if (!skip.isEmpty()) QSKIP(qPrintable(skip));
        QVERIFY(exported);
        // Before the fix the bursts after the hole read -60 ms.
        const double offset = median(dir_.filePath(name));
        QVERIFY2(std::abs(offset) <= 17.0, qPrintable(QString::number(offset)));
    }
    // #1613: an edit with cuts exports exactly the kept footage, laid end to
    // end on the clip's frame grid. The cut clip's bar names the clip frame each
    // output frame shows, so the whole sequence is checked: no frame from a cut
    // or the trim (nor its colour, nor a GIF palette built from it), none
    // repeated or dropped at a seam, one constant frame step, flashes and their
    // bursts at their output times, and a cover over cut A drawn on both sides
    // of it and nowhere else while a cover inside the cut draws nothing.
    void cutsJoinTheKeptFootage_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("codec");
        QTest::newRow("mp4") << "mp4" << "h264";
        QTest::newRow("webm") << "webm" << "vp9";
        QTest::newRow("gif") << "gif" << "";
    }
    void cutsJoinTheKeptFootage() {
        QFETCH(QString, format);
        QFETCH(QString, codec);
        using namespace cutclip;
        const auto editWithCuts = cutclip::edit();
        const KeptSpans kept(editWithCuts, durationUs);
        ExportRequest request;
        request.input = cut_;
        request.output = dir_.filePath("cut-out." + format);
        request.durationUs = durationUs;
        request.canvas = {width, height};
        request.edit = editWithCuts;
        request.config.format = format;
        request.config.codec = codec;
        request.config.audio.system = format != "gif";
        ExportResult result;
        std::atomic_bool cancelled{false};
        QList<double> progress;
        const auto error = putgd::record::exportClip(request, result, cancelled, [&](double p) { progress << p; });
        if (error.code == "recording_unsupported") QSKIP(qPrintable(format + " export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        const int outputFps = format == "gif" ? 15 : fps;
        const qint64 step = 1'000'000 / outputFps;
        QVERIFY2(std::abs(result.durationUs - kept.outputDuration()) <= step, qPrintable(QString::number(result.durationUs)));
        // #1635: the GIF/WebP length limit is checked on the time the edit keeps, so an animation never runs longer.
        if (format == "gif") QVERIFY2(result.durationUs <= kept.outputDuration(), qPrintable(QStringLiteral("%1 us of GIF for %2 kept").arg(result.durationUs).arg(kept.outputDuration())));
        QVERIFY(!progress.isEmpty() && progress.last() > 0.95);
        for (qsizetype i = 1; i < progress.size(); ++i) QVERIFY2(progress[i] >= progress[i - 1], qPrintable(QStringLiteral("progress %1 after %2").arg(progress[i]).arg(progress[i - 1])));

        Decoded decoded;
        const auto failure = analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        const auto& frames = decoded.frames;
        QVERIFY(frames.size() > 10);
        const qint64 first = frames.first().us;
        // One constant step across every seam; a rounding tick of the container's time base at most.
        const qint64 tick = format == "gif" ? 10'000 : format == "webm" ? 1'000 : 100;
        for (qsizetype i = 1; i < frames.size(); ++i) {
            const qint64 diff = frames[i].us - frames[i - 1].us;
            QVERIFY2(diff > 0 && std::abs(diff - step) <= tick, qPrintable(QStringLiteral("frame %1 steps %2 us").arg(i).arg(diff)));
        }
        const qint64 length = frames.last().us + step - first;
        QVERIFY2(std::abs(length - kept.outputDuration()) <= step, qPrintable(QStringLiteral("%1 us of video for %2 kept").arg(length).arg(kept.outputDuration())));
        for (const auto& f : frames) {
            QVERIFY2(f.chroma < 30, qPrintable(QStringLiteral("left-out colour at %1 us").arg(f.us - first)));
            QVERIFY2(!f.cutOnlyCovered, qPrintable(QStringLiteral("a cover inside the cut drawn at %1 us").arg(f.us - first)));
        }
        if (format != "gif") {
            // The frame sequence: kept clip frames once each, in order, as many as
            // the kept time rounds to (the review clock's length, #1635). Cut A
            // starts 10 ms after frame 42, so that frame's 10 ms round away; a
            // kept frame may go only so, as the last of its span.
            QCOMPARE(qint64(frames.size()), (kept.outputDuration() * fps + 500000) / 1000000);
            for (qsizetype i = 0; i < frames.size(); ++i) {
                const int source = frames[i].source;
                QVERIFY2(kept.kept(frameUs(source)), qPrintable(QStringLiteral("output frame %1 shows clip frame %2, which the edit leaves out").arg(i).arg(source)));
                if (i > 0) QVERIFY2(source > frames[i - 1].source, qPrintable(QStringLiteral("clip frame %1 after %2").arg(source).arg(frames[i - 1].source)));
                if (i > 0) for (int skipped = frames[i - 1].source + 1; skipped < source; ++skipped) QVERIFY2(!kept.kept(frameUs(skipped)) || !kept.kept(frameUs(skipped + 1)), qPrintable(QStringLiteral("kept clip frame %1 missing inside its span").arg(skipped)));
                // A stored time within half a millisecond of a bound may be either
                // side of it, so it is covered (#1635 E18): frame 90, at 3.000 s.
                const qint64 sourceUs = frameUs(source);
                const bool cover = sourceUs >= spanningFrom - 500 && sourceUs < spanningTo + 500;
                QVERIFY2(frames[i].spanningCovered == cover, qPrintable(QStringLiteral("clip frame %1: cover %2, expected %3").arg(source).arg(frames[i].spanningCovered).arg(cover)));
            }
        }
        // Each kept flash at its output time; the cover on the two around cut A only.
        QList<qint64> flashes;
        QList<bool> covered;
        for (qsizetype i = 0; i < frames.size(); ++i) if (frames[i].white && (i == 0 || !frames[i - 1].white)) { flashes << frames[i].us - first; covered << frames[i].spanningCovered; }
        QCOMPARE(flashes.size(), flashFrames.size());
        for (qsizetype i = 0; i < flashes.size(); ++i) {
            const qint64 expectedUs = kept.toOutput(frameUs(flashFrames[i]));
            QVERIFY2(std::abs(flashes[i] - expectedUs) <= step, qPrintable(QStringLiteral("flash %1 at %2 us, expected %3").arg(i).arg(flashes[i]).arg(expectedUs)));
            const qint64 sourceUs = frameUs(flashFrames[i]);
            QCOMPARE(covered[i], sourceUs >= spanningFrom && sourceUs < spanningTo);
        }
        if (format == "gif") {
            QFile file(request.output);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto palette = gifPalette(file.readAll());
            QVERIFY(!palette.isEmpty());
            for (quint32 colour : palette) {
                const int r = int(colour >> 16), g = int((colour >> 8) & 0xff), b = int(colour & 0xff);
                QVERIFY2(!(r > 150 && b > 150 && g < 110), qPrintable(QStringLiteral("left-out colour #%1 in the palette").arg(colour, 6, 16, QLatin1Char('0'))));
            }
            return;
        }
        // Each burst within one audio frame of its flash, and none from inside cut A.
        QCOMPARE(decoded.bursts.size(), flashes.size());
        const qint64 audioFrameUs = qint64(decoded.audioFrameSize) * 1'000'000 / rate;
        for (qsizetype i = 0; i < flashes.size(); ++i) {
            const qint64 offset = decoded.bursts[i] - first - flashes[i];
            QVERIFY2(std::abs(offset) <= audioFrameUs, qPrintable(QStringLiteral("burst %1 is %2 us from its flash").arg(i).arg(offset)));
        }
    }
    // #1613: a kept span that starts inside a long frame gap (more than one
    // floor interval, 1/15 s, before its first frame) starts at its start, on
    // the frame on screen there, with the audio from that instant: the
    // narration over a still slide is kept and stays in sync, at the trim start
    // and at a seam after a cut alike. A span starting less than a floor
    // interval before a frame still starts on that frame. The mirror at a span's
    // end: a trim end or cut start inside the gap ends the span there exactly,
    // so nothing after it (no audio, no frame) leaks into the output. A cover
    // across the gap is drawn where the same map puts it, held frames included.
    void frameGapsKeepTheSpanStart_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("codec");
        QTest::addColumn<qint64>("trimStart");
        QTest::addColumn<qint64>("cutStart");
        QTest::addColumn<qint64>("cutEnd");
        QTest::addColumn<qint64>("keptFrom");  // where the output really starts
        QTest::addColumn<qint64>("trimEnd");
        constexpr qint64 none = -1, floorIntervalUs = 1'000'000 / AdaptiveRate::floorFps;
        static_assert(gapclip::resumeMs * 1000 - 2'000'000 > floorIntervalUs && gapclip::resumeMs * 1000 - 2'560'000 < floorIntervalUs);
        // 0.8 s before frame 48: opens on frame 47, shown from before the trim.
        QTest::newRow("trim in a gap, mp4") << "mp4" << "h264" << qint64(1'800'000) << none << none << qint64(1'800'000) << qint64(4'000'000);
        QTest::newRow("trim in a gap, webm") << "webm" << "vp9" << qint64(1'800'000) << none << none << qint64(1'800'000) << qint64(4'000'000);
        QTest::newRow("trim in a gap, gif") << "gif" << "" << qint64(1'800'000) << none << none << qint64(1'800'000) << qint64(4'000'000);
        // The cut holds frame 47 and ends 0.6 s before frame 48: the seam opens on 47.
        QTest::newRow("cut ending in a gap, mp4") << "mp4" << "h264" << qint64(0) << qint64(800'000) << qint64(2'000'000) << qint64(0) << qint64(4'000'000);
        QTest::newRow("cut ending in a gap, gif") << "gif" << "" << qint64(0) << qint64(800'000) << qint64(2'000'000) << qint64(0) << qint64(4'000'000);
        // 40 ms before frame 48, under the 66.7 ms floor interval: starts on frame 48.
        QTest::newRow("trim just before a frame, mp4") << "mp4" << "h264" << qint64(2'560'000) << none << none << qint64(2'600'000) << qint64(4'000'000);
        // A trim end 0.6 s before frame 48 ends the export at 2.0 s on frame 47;
        // running on to frame 48 would keep the burst at 2.2 s after the trim end.
        // A cut from 1.8 s (0.8 s before frame 48) to 3.2 s (on frame 66) likewise
        // ends the first span at 1.8 s: none of the cut's burst at 2.2 s, nor its frames.
        static_assert(gapclip::resumeMs * 1000 - 2'000'000 > floorIntervalUs && gapclip::resumeMs * 1000 - 1'800'000 > floorIntervalUs);
        for (const auto& [format, codec] : {std::pair<QString, QString>{"mp4", "h264"}, {"webm", "vp9"}, {"gif", ""}, {"webp", ""}}) {
            QTest::newRow(qPrintable("trim end in a gap, " + format)) << format << codec << qint64(0) << none << none << qint64(0) << qint64(2'000'000);
            QTest::newRow(qPrintable("cut starting in a gap, " + format)) << format << codec << qint64(0) << qint64(1'800'000) << qint64(3'200'000) << qint64(0) << qint64(4'000'000);
        }
        QTest::newRow("trim in a gap, webp") << "webp" << "" << qint64(1'800'000) << none << none << qint64(1'800'000) << qint64(4'000'000);
    }
    void frameGapsKeepTheSpanStart() {
        QFETCH(QString, format);
        QFETCH(QString, codec);
        QFETCH(qint64, trimStart);
        QFETCH(qint64, cutStart);
        QFETCH(qint64, cutEnd);
        QFETCH(qint64, keptFrom);
        QFETCH(qint64, trimEnd);
        using namespace gapclip;
        putgd::RecordingEdit edit;
        edit.trimStart = trimStart;
        edit.trimEnd = trimEnd;  // 4.0 s is on frame 90
        if (cutStart >= 0) edit.cuts = {{"gap", cutStart, cutEnd}};
        // A cover over the gap, from 1.0 s to 3.5 s.
        constexpr qint64 coverFrom = 1'000'000, coverTo = 3'500'000;
        putgd::RecordingMask cover;
        cover.kind = "solid"; cover.rect = cutclip::spanningMask; cover.start = coverFrom; cover.end = coverTo;
        edit.masks = {cover};
        putgd::RecordingEdit expectedEdit = edit;
        expectedEdit.trimStart = keptFrom;
        const KeptSpans expected(expectedEdit, durationUs);
        ExportRequest request;
        request.input = frameGap_;
        request.output = dir_.filePath(QStringLiteral("gap-%1-%2-%3.%4").arg(trimStart).arg(cutStart).arg(trimEnd).arg(format));
        request.durationUs = durationUs;
        request.canvas = {width, height};
        request.edit = edit;
        request.config.format = format;
        request.config.codec = codec;
        request.config.audio.system = format == "mp4" || format == "webm";
        ExportResult result;
        std::atomic_bool cancelled{false};
        QList<double> progress;
        const auto error = putgd::record::exportClip(request, result, cancelled, [&](double p) { progress << p; });
        if (error.code == "recording_unsupported") QSKIP(qPrintable(format + " export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        const bool animation = format == "gif" || format == "webp";
        const int outputFps = animation ? 15 : fps;
        const qint64 step = 1'000'000 / outputFps;
        // The output lasts exactly the time kept, whole output frames here, and progress runs over it.
        // #1635: the review clock's length (the edit's kept time) in whole output frames.
        const qint64 clockUs = av_rescale(std::max<qint64>(1, (KeptSpans(edit, durationUs).outputDuration() * outputFps + 500000) / 1000000), 1'000'000, outputFps);
        QVERIFY2(std::abs(result.durationUs - clockUs) <= 1, qPrintable(QStringLiteral("%1 us, expected %2").arg(result.durationUs).arg(clockUs)));
        QVERIFY(!progress.isEmpty() && progress.last() > 0.95);
        for (qsizetype i = 1; i < progress.size(); ++i) QVERIFY(progress[i] >= progress[i - 1]);

        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        if (format == "webp" && failure == "cannot set up a decoder") QSKIP("this FFmpeg has no WebP decoder: only the duration and progress were checked");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // #1635 E22: a GIF ends on its last tick's rounded centisecond, not one
        // frame delay after the frame before it.
        if (format == "gif") {
            const qint64 ticks = std::max<qint64>(1, KeptSpans(edit, durationUs).outputDuration() * outputFps / 1'000'000);
            const auto& last = decoded.frames.last();
            const qint64 endUs = last.us - decoded.frames.first().us + last.durationUs;
            QVERIFY2(endUs == av_rescale(ticks, 100, outputFps) * 10'000, qPrintable(QStringLiteral("the GIF ends at %1 us, expected %2").arg(endUs).arg(av_rescale(ticks, 100, outputFps) * 10'000)));
        }
        // Checked on the output grid by each frame's time and duration: libwebp
        // merges identical frames, so a held picture decodes as one long frame.
        const auto shown = cutclip::onGrid(decoded.frames, outputFps);
        QVERIFY(shown.size() > 10);
        const qint64 first = shown.first().us;
        const qint64 length = shown.last().us + step - first;
        QVERIFY2(std::abs(length - expected.outputDuration()) <= step, qPrintable(QStringLiteral("%1 us of video for %2 kept").arg(length).arg(expected.outputDuration())));
        // A 15 fps step keeps the later of two clip frames; the window check below covers it.
        if (!animation) QCOMPARE(shown.first().source, onScreen(keptFrom));
        // Every output frame shows the clip frame on screen at its source time,
        // give or take the output rate's half-step rounding, and never a cut-out one.
        for (const auto& f : shown) {
            const qint64 t = f.us - first;
            const int lo = onScreen(expected.toSource(std::max<qint64>(0, t - step / 2)));
            const int hi = onScreen(expected.toSource(std::min(t + step / 2, expected.outputDuration() - 1)));
            const bool between = f.source > lo && f.source < hi && expected.kept(frameUs(f.source));
            QVERIFY2(f.source == lo || f.source == hi || between, qPrintable(QStringLiteral("output %1 us shows clip frame %2, expected %3..%4").arg(t).arg(f.source).arg(lo).arg(hi)));
        }
        for (const qint64 seam : expected.seams()) {
            const auto after = std::find_if(shown.begin(), shown.end(), [&](const cutclip::Decoded::Frame& f) { return f.us - first >= seam; });
            QVERIFY(after != shown.end());
            // A 15 fps step may keep the frame after the first one there.
            const int opens = onScreen(expected.toSource(seam)), next = onScreen(expected.toSource(seam + step / 2));
            QVERIFY2(after->source == opens || (animation && after->source == next), qPrintable(QStringLiteral("the seam at %1 us opens on clip frame %2, expected %3").arg(seam).arg(after->source).arg(opens)));
        }
        // The first frame after the gap, when kept, lands where it belongs in the output.
        if (expected.kept(resumeMs * 1000)) {
            const auto resumed = std::find_if(shown.begin(), shown.end(), [](const cutclip::Decoded::Frame& f) { return f.source > held; });
            QVERIFY(resumed != shown.end());
            const qint64 resumedUs = expected.toOutput(resumeMs * 1000);
            QVERIFY2(std::abs(resumed->us - first - resumedUs) <= step, qPrintable(QStringLiteral("frame %1 at %2 us, expected %3").arg(resumed->source).arg(resumed->us - first).arg(resumedUs)));
        }
        // The cover on exactly the output the map gives it. A frame stamped within
        // one output step of a bound may land either side after the rate filter.
        const auto coverParts = expected.outputParts(coverFrom, coverTo);
        for (const auto& f : shown) {
            const qint64 t = f.us - first;
            bool inside = false, nearBound = false;
            for (const auto& part : coverParts) { inside = inside || part.contains(t); nearBound = nearBound || std::abs(t - part.start) < step || std::abs(t - part.end) < step; }
            if (!nearBound) QVERIFY2(f.spanningCovered == inside, qPrintable(QStringLiteral("output %1 us (clip frame %2): cover %3, expected %4").arg(t).arg(f.source).arg(f.spanningCovered).arg(inside)));
        }
        QList<qint64> flashes, expectedFlashes;
        for (qsizetype i = 0; i < shown.size(); ++i) if (shown[i].white && (i == 0 || !shown[i - 1].white)) flashes << shown[i].us - first;
        for (int f : flashFrames) if (expected.kept(frameUs(f))) expectedFlashes << expected.toOutput(frameUs(f));
        QCOMPARE(flashes.size(), expectedFlashes.size());
        for (qsizetype i = 0; i < flashes.size(); ++i) QVERIFY2(std::abs(flashes[i] - expectedFlashes[i]) <= step, qPrintable(QStringLiteral("flash %1 at %2 us, expected %3").arg(i).arg(flashes[i]).arg(expectedFlashes[i])));
        if (!request.config.audio.system) return;
        // Every kept burst, the one over the still frame included, at its output
        // time: within one audio frame, as its flash is within one video frame,
        // and no audio past the output's end (an end inside the gap stops it there).
        QList<qint64> expectedBursts;
        for (qint64 us : burstsUs()) if (expected.kept(us)) expectedBursts << expected.toOutput(us);
        QCOMPARE(decoded.bursts.size(), expectedBursts.size());
        const qint64 audioFrameUs = qint64(decoded.audioFrameSize) * 1'000'000 / rate;
        for (qsizetype i = 0; i < expectedBursts.size(); ++i) {
            const qint64 offset = decoded.bursts[i] - first - expectedBursts[i];
            QVERIFY2(std::abs(offset) <= audioFrameUs, qPrintable(QStringLiteral("burst %1 is %2 us from %3").arg(i).arg(offset).arg(expectedBursts[i])));
        }
        QVERIFY2(decoded.audioEndUs - first <= clockUs + audioFrameUs, qPrintable(QStringLiteral("audio runs to %1 us of %2").arg(decoded.audioEndUs - first).arg(clockUs)));
    }
    // #1635: a cover applies to an output frame while it is active at that
    // frame's output time (mapped back through the kept spans) or at the source
    // time of the picture the frame shows. Three ways a frame gap broke that: a
    // cover wholly inside the gap met no frame, so the still picture played
    // uncovered under it; a cover inside a cut over the frame a later span opens
    // on (held across the seam) was dropped with the cut; and a cover ending in
    // the gap uncovered the still picture it had covered. A frame is covered
    // for its whole display interval: one whose interval overlaps the cover is
    // covered even when the cover starts after the frame's tick, since the
    // file presents it a little late (a 15 fps GIF shows its second frame at
    // 70 ms, not 66.7 ms).
    void coversFollowTheShownPicture_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<qint64>("trimStart");
        QTest::addColumn<qint64>("cutStart");
        QTest::addColumn<qint64>("cutEnd");
        QTest::addColumn<qint64>("coverFrom");
        QTest::addColumn<qint64>("coverTo");
        for (const QString format : {"mp4", "gif"}) {
            // Frame 47 (1.567 s) is on screen until 2.6 s.
            QTest::newRow(qPrintable("cover inside a gap, " + format)) << format << qint64(0) << qint64(-1) << qint64(-1) << qint64(1'800'000) << qint64(2'200'000);
            // The cut holds frame 47; the span from 2.0 s opens on it.
            QTest::newRow(qPrintable("cover in a cut on the held frame, " + format)) << format << qint64(0) << qint64(1'000'000) << qint64(2'000'000) << qint64(1'500'000) << qint64(1'600'000);
            QTest::newRow(qPrintable("cover ending in a gap, " + format)) << format << qint64(0) << qint64(-1) << qint64(-1) << qint64(1'000'000) << qint64(2'000'000);
            // The output opens at 1.6 s on frame 47, held; the cover runs 68-80 ms
            // into it, inside the first GIF frame as the file shows it (0-70 ms).
            QTest::newRow(qPrintable("cover between two ticks, " + format)) << format << qint64(1'600'000) << qint64(-1) << qint64(-1) << qint64(1'668'000) << qint64(1'680'000);
            // E18: frame 47 is stored at 1567 ms for 1566.667 ms, 0.1 ms past this
            // cover's end: within the container's half-millisecond, so covered.
            QTest::newRow(qPrintable("cover ending just before a stored frame time, " + format)) << format << qint64(0) << qint64(1'000'000) << qint64(2'000'000) << qint64(1'500'000) << qint64(1'566'900);
        }
    }
    void coversFollowTheShownPicture() {
        QFETCH(QString, format);
        QFETCH(qint64, trimStart);
        QFETCH(qint64, cutStart);
        QFETCH(qint64, cutEnd);
        QFETCH(qint64, coverFrom);
        QFETCH(qint64, coverTo);
        using namespace gapclip;
        putgd::RecordingEdit edit;
        edit.trimStart = trimStart;
        edit.trimEnd = 4'000'000;
        if (cutStart >= 0) edit.cuts = {{"cut", cutStart, cutEnd}};
        putgd::RecordingMask mask;
        mask.kind = "solid"; mask.rect = cutclip::spanningMask; mask.start = coverFrom; mask.end = coverTo;
        edit.masks = {mask};
        const KeptSpans kept(edit, durationUs);
        ExportRequest request;
        request.input = frameGap_;
        request.output = dir_.filePath(QStringLiteral("cover-%1-%2-%3-%4.%5").arg(trimStart).arg(cutStart).arg(coverFrom).arg(coverTo).arg(format));
        request.durationUs = durationUs;
        request.canvas = {width, height};
        request.edit = edit;
        request.config.format = format;
        request.config.codec = format == "mp4" ? "h264" : "";
        request.config.audio.system = false;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable(format + " export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        const int outputFps = format == "gif" ? 15 : fps;
        const qint64 step = 1'000'000 / outputFps;
        // #1635 E19: the frames as the file presents them (a GIF in
        // centiseconds), not snapped back to the ideal grid.
        const auto& shown = decoded.frames;
        QVERIFY(shown.size() > 10);
        const putgd::recording::Span cover{coverFrom, coverTo};
        auto distance = [&](qint64 t) { return t < cover.start ? cover.start - t : t >= cover.end ? t - cover.end + 1 : 0; };
        const auto coverParts = kept.outputParts(coverFrom, coverTo);
        int mustCover = 0;
        for (const auto& f : shown) {
            const qint64 t = f.us - shown.first().us, until = t + (f.durationUs > 0 ? f.durationUs : step), picture = frameUs(f.source), at = kept.toSource(t);
            const bool overlaps = std::any_of(coverParts.begin(), coverParts.end(), [&](const putgd::recording::Span& part) { return part.start < until && t < part.end; });
            // A stored picture time within half a millisecond of a bound may lie on either side: covered.
            if ((picture >= cover.start - 500 && picture < cover.end + 500) || overlaps) {
                ++mustCover;
                QVERIFY2(f.spanningCovered, qPrintable(QStringLiteral("output %1 us shows clip frame %2 (%3 us) at source %4 us uncovered").arg(t).arg(f.source).arg(picture).arg(at)));
            } else if (distance(picture) > step && distance(at) > step) {
                QVERIFY2(!f.spanningCovered, qPrintable(QStringLiteral("output %1 us (clip frame %2) covered far from the cover").arg(t).arg(f.source)));
            }
        }
        QVERIFY(mustCover > 0);
    }
    // #1635: export audio comes from the edit's own kept spans only. A span end
    // that the frame grid moves forward to the next frame (up to 1/15 s) fills
    // that time with silence, not with the cut or trimmed audio after it; a
    // leading cut that ends inside a hole in the audio keeps the hole as
    // silence, so later audio keeps its place and nothing from a later cut
    // slips in. The clip plays 1 kHz over kept time and 3 kHz over everything
    // the edit removes, so any cut audio in the output shows as 3 kHz. Each
    // decoded audio frame is placed at its own timestamp, so a shift shows; a
    // tone running into a seam from both sides checks the seam's time and fade.
    void audioKeepsOnlyTheKeptSpans_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("codec");
        QTest::addColumn<bool>("leadingCut");
        for (const auto& [format, codec] : {std::pair<QString, QString>{"mp4", "h264"}, {"webm", "vp9"}}) {
            QTest::newRow(qPrintable("span ends between frames, " + format)) << format << codec << false;
            QTest::newRow(qPrintable("leading cut ending in an audio hole, " + format)) << format << codec << true;
        }
    }
    void audioKeepsOnlyTheKeptSpans() {
        QFETCH(QString, format);
        QFETCH(QString, codec);
        QFETCH(bool, leadingCut);
        using putgd::recording::Span;
        constexpr int fps = 30;
        constexpr qint64 durationUs = 4'000'000;
        putgd::RecordingEdit edit;
        qint64 holeFrom = 0, holeTo = 0, seam = -1;
        QList<Span> tone, silent;  // output time
        if (!leadingCut) {
            // Both span ends lie between frames (frames 36/37 and 105/106), so the
            // grid runs the spans on to frames 37 and 106 in the clip; in the
            // output the first lasts the kept 1.21 s rounded to 36 frames (1.2 s)
            // and the second ends at 2.7333 s, the kept 2.72 s in frames. The
            // second span's last 23 ms, past the trim end in the clip, are silence.
            edit.trimEnd = 3'510'000;
            edit.cuts = {{"a", 1'210'000, 2'000'000}};
            tone = {{0, 1'200'000}, {1'200'000, 1'200'000 + 1'510'000}};
            silent = {{1'200'000 + 1'510'000, 2'733'333}};
            seam = 1'200'000;
        } else {
            // The clip's audio has a hole from 0.4 s to 0.7 s; the first span starts at 0.5 s.
            holeFrom = 400'000; holeTo = 700'000;
            edit.cuts = {{"lead", 0, 500'000}, {"b", 2'000'000, 2'500'000}};
            tone = {{200'000, 1'500'000}, {1'500'000, 3'000'000}};
            silent = {{0, 200'000}};
            seam = 1'500'000;
        }
        const KeptSpans kept(edit, durationUs);
        const auto path = dir_.filePath(QStringLiteral("tones-%1.mkv").arg(leadingCut));
        const auto written = plainclip::write(path, fps, 120, [&](qint64 us) { return 0.25 * std::sin(2.0 * M_PI * (kept.kept(us) ? 1000.0 : 3000.0) * double(us) / 1e6); }, holeFrom, holeTo);
        QVERIFY2(!written, qPrintable(written.message));
        ExportRequest request;
        request.input = path;
        request.output = dir_.filePath(QStringLiteral("tones-%1.%2").arg(leadingCut).arg(format));
        request.durationUs = durationUs;
        request.canvas = {cutclip::width, cutclip::height};
        request.edit = edit;
        request.config.format = format;
        request.config.codec = codec;
        request.config.fps = fps;
        request.config.audio.system = true;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable(format + " export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(!decoded.audio.empty());
        const qint64 videoEnd = decoded.frames.last().us + 1'000'000 / fps;
        // No cut audio anywhere.
        for (qint64 t = 0; t < decoded.audioEndUs; t += 2500) {
            const double cut = cutclip::toneAt(decoded.audio, t, 3000.0);
            QVERIFY2(cut < 0.03, qPrintable(QStringLiteral("cut audio (3 kHz at %1) at %2 us of output").arg(cut).arg(t)));
        }
        // The kept tone where it belongs, silence where the grid or a hole adds
        // time. The encoders ramp up over their first frame, so from 40 ms on.
        for (const auto& part : tone) for (qint64 t = std::max<qint64>(part.start + 10'000, 40'000); t + 10'000 <= part.end; t += 5000) {
            const double level = cutclip::toneAt(decoded.audio, t, 1000.0);
            QVERIFY2(level > 0.15, qPrintable(QStringLiteral("kept tone at %1 at %2 us of output").arg(level).arg(t)));
        }
        for (const auto& part : silent) for (qint64 t = part.start + 6000; t + 6000 <= part.end; t += 2500) {
            const double level = cutclip::toneAt(decoded.audio, t, 1000.0);
            QVERIFY2(level < 0.03, qPrintable(QStringLiteral("sound (%1) at %2 us of output, expected silence").arg(level).arg(t)));
        }
        if (seam >= 0) {
            // Full tone up to 5 ms from the seam on both sides, faded to near nothing on it.
            const double before = cutclip::rmsOver(decoded.audio, seam - 15'000, seam - 5'000), after = cutclip::rmsOver(decoded.audio, seam + 5'000, seam + 15'000);
            const double on = cutclip::rmsOver(decoded.audio, seam - 1'000, seam + 1'000);
            QVERIFY2(before > 0.12 && after > 0.12, qPrintable(QStringLiteral("tone around the seam: %1 before, %2 after").arg(before).arg(after)));
            QVERIFY2(on < 0.4 * std::min(before, after), qPrintable(QStringLiteral("no fade at the seam: %1 against %2").arg(on).arg(std::min(before, after))));
        }
        const qint64 audioFrameUs = qint64(decoded.audioFrameSize) * 1'000'000 / cutclip::rate;
        QVERIFY2(decoded.audioEndUs <= videoEnd + audioFrameUs, qPrintable(QStringLiteral("audio runs to %1 us of %2").arg(decoded.audioEndUs).arg(videoEnd)));
    }
    // #1635: at 60 fps Matroska's millisecond timestamps make frame steps of 16
    // and 17 ms, and store a frame up to 0.5 ms before the frame time the
    // review snaps an edge to (frame 2 at 33 ms for 33.333 ms). Spans of two
    // frames with one frame cut between them each, all through the clip, on the
    // review's frame grid, still export as every kept frame exactly once, in
    // order: never two on one output tick and the next tick empty, and never
    // the cut frame stored just before its cut.
    void sixtyFpsCutsKeepEveryFrame_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("codec");
        QTest::newRow("mp4") << "mp4" << "h264";
        QTest::newRow("webm") << "webm" << "vp9";
    }
    void sixtyFpsCutsKeepEveryFrame() {
        QFETCH(QString, format);
        QFETCH(QString, codec);
        constexpr int fps = 60, frames = 150;
        putgd::RecordingEdit edit;
        QList<int> expected;
        for (int k = 0; k < frames; ++k) {
            if (k % 3 == 2) edit.cuts.append({QStringLiteral("c%1").arg(k), qRound64(k * 1e6 / fps), qRound64((k + 1) * 1e6 / fps)});
            else expected << k;
        }
        ExportRequest request;
        request.input = sixty_;
        request.output = dir_.filePath("sixty." + format);
        request.durationUs = plainclip::frameUs(fps, frames);
        request.canvas = {cutclip::width, cutclip::height};
        request.edit = edit;
        request.config.format = format;
        request.config.codec = codec;
        request.config.fps = fps;
        request.config.audio.system = false;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable(format + " export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QList<int> shown;
        for (const auto& f : decoded.frames) shown << f.source;
        QCOMPARE(shown, expected);
    }
    // #1635: an animation whose edit keeps one frame (16.7 ms at 60 fps, under
    // the 15 fps output step) still exports that frame.
    void oneKeptFrameMakesAnAnimation_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<bool>("trailing");
        QTest::newRow("gif, first frame") << "gif" << false;
        QTest::newRow("gif, last frame") << "gif" << true;
        QTest::newRow("webp, first frame") << "webp" << false;
    }
    void oneKeptFrameMakesAnAnimation() {
        QFETCH(QString, format);
        QFETCH(bool, trailing);
        constexpr int fps = 60, frames = 150;
        const int keep = trailing ? frames - 1 : 0;
        putgd::RecordingEdit edit;
        if (trailing) edit.cuts = {{"lead", 0, plainclip::frameUs(fps, keep)}};
        else edit.cuts = {{"tail", plainclip::frameUs(fps, 1), plainclip::frameUs(fps, frames)}};
        ExportRequest request;
        request.input = sixty_;
        request.output = dir_.filePath(QStringLiteral("one-%1.%2").arg(keep).arg(format));
        request.durationUs = plainclip::frameUs(fps, frames);
        request.canvas = {cutclip::width, cutclip::height};
        request.edit = edit;
        request.config.format = format;
        request.config.fps = fps;
        request.config.audio.system = false;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable(format + " export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QCOMPARE(result.frames, 1);
        QVERIFY2(std::abs(result.durationUs - 1'000'000 / 15) <= 1, qPrintable(QString::number(result.durationUs)));
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        if (format == "webp" && failure == "cannot set up a decoder") QSKIP("this FFmpeg has no WebP decoder: only the frame count and duration were checked");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(decoded.frames.size(), 1);
        QCOMPARE(decoded.frames.first().source, keep);
    }
    // #1635: the GIF/WebP length limit is checked on the time the edit keeps.
    // Span ends between frames move to the next frame on the grid, which could
    // make the animation outrun that by up to a frame per span; it never does.
    void animationsKeepWithinTheKeptTime() {
        constexpr int fps = 60, frames = 150;
        putgd::RecordingEdit edit;
        // Five spans of 20 frames and 8 ms of the next, 10 frames cut after each.
        for (int k = 0; k < frames; k += 30) edit.cuts.append({QStringLiteral("c%1").arg(k), plainclip::frameUs(fps, k + 20) + 8000, plainclip::frameUs(fps, k + 30)});
        const KeptSpans kept(edit, plainclip::frameUs(fps, frames));
        ExportRequest request;
        request.input = sixty_;
        request.output = dir_.filePath("limit.gif");
        request.durationUs = plainclip::frameUs(fps, frames);
        request.canvas = {cutclip::width, cutclip::height};
        request.edit = edit;
        request.config.format = "gif";
        request.config.fps = fps;
        request.config.audio.system = false;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable("gif export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QVERIFY2(result.durationUs <= kept.outputDuration(), qPrintable(QStringLiteral("%1 us of GIF for %2 kept").arg(result.durationUs).arg(kept.outputDuration())));
        // Each span rounds to whole 15 fps frames: half a frame each at most.
        QVERIFY2(result.durationUs >= kept.outputDuration() - 5 * 1'000'000 / 30, qPrintable(QString::number(result.durationUs)));
    }
    // #1635 AC1: an MP4 cut on whole frames, as the review snaps edges, lasts
    // what the review clock shows. Trim 1.266667-11.333333 s and a cut from
    // 5.133333 to 6.266667 s keep 8.933 s; the export came out 9.000 s, two
    // frames long: Matroska stores frame 154 at 5133 ms and frame 340 at
    // 11333 ms, a third of a millisecond before the edges they lie on, so the
    // first cut frame and the first frame after the trim were exported too.
    void frameExactEdgesMatchTheClock() {
        constexpr int fps = 30, frames = 360;
        const auto path = dir_.filePath("long30.mkv");
        const auto written = plainclip::write(path, fps, frames);
        QVERIFY2(!written, qPrintable(written.message));
        putgd::RecordingEdit edit;
        edit.trimStart = plainclip::gridUs(fps, 38);
        edit.trimEnd = plainclip::gridUs(fps, 340);
        edit.cuts = {{"cut", plainclip::gridUs(fps, 154), plainclip::gridUs(fps, 188)}};
        QCOMPARE(edit.trimStart, qint64(1'266'667));
        QCOMPARE(edit.cuts[0].start, qint64(5'133'333));
        const KeptSpans kept(edit, plainclip::frameUs(fps, frames));
        QVERIFY(std::abs(kept.outputDuration() - 8'933'333) <= 1);  // the clock: 268 frames
        ExportRequest request;
        request.input = path;
        request.output = dir_.filePath("clock.mp4");
        request.durationUs = plainclip::frameUs(fps, frames);
        request.canvas = {cutclip::width, cutclip::height};
        request.edit = edit;
        request.config.format = "mp4";
        request.config.codec = "h264";
        request.config.fps = fps;
        request.config.audio.system = false;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable("mp4 export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QVERIFY2(std::abs(result.durationUs - 8'933'333) <= 1, qPrintable(QString::number(result.durationUs)));
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QList<int> expected;
        for (int k = 38; k < 340; ++k) if (k < 154 || k >= 188) expected << k;
        QCOMPARE(expected.size(), 268);
        QCOMPARE(plainclip::unwrap(decoded.frames), expected);
    }
    // #1635 E10: the GIF/WebP cap never erases a span. Kept spans of 100, 100,
    // 100 and 67 ms at 15 fps fit 5 frames (the 367 ms kept); rounding each span
    // and trimming the excess from the end left the last span none.
    void animationCapKeepsEverySpan() {
        constexpr int fps = 60;
        putgd::RecordingEdit edit;
        edit.trimEnd = plainclip::gridUs(fps, 40);
        edit.cuts = {{"a", plainclip::gridUs(fps, 6), plainclip::gridUs(fps, 12)}, {"b", plainclip::gridUs(fps, 18), plainclip::gridUs(fps, 24)}, {"c", plainclip::gridUs(fps, 30), plainclip::gridUs(fps, 36)}};
        const KeptSpans kept(edit, plainclip::frameUs(fps, 150));
        QCOMPARE(kept.spans().size(), 4);
        ExportRequest request;
        request.input = sixty_;
        request.output = dir_.filePath("cap.gif");
        request.durationUs = plainclip::frameUs(fps, 150);
        request.canvas = {cutclip::width, cutclip::height};
        request.edit = edit;
        request.config.format = "gif";
        request.config.fps = fps;
        request.config.audio.system = false;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable("gif export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QVERIFY2(result.durationUs <= kept.outputDuration(), qPrintable(QString::number(result.durationUs)));
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // Each span on screen: frames 0-5, 12-17, 24-29 and 36-39.
        for (int from : {0, 12, 24, 36}) {
            const bool shown = std::any_of(decoded.frames.begin(), decoded.frames.end(), [&](const cutclip::Decoded::Frame& f) { return f.source >= from && f.source < from + 6; });
            QVERIFY2(shown, qPrintable(QStringLiteral("no frame of the span from clip frame %1").arg(from)));
        }
    }
    // #1635 E11: in a variable-rate clip (pictures at 0, 1 and 2 s) trimmed to
    // 1.010 s, the picture at 1 s rounds to the tick where the output ends; it
    // takes the last tick instead of being dropped, since the picture there is
    // only the one from 0 s repeated.
    void lastPictureOfASpanIsShown() {
        const auto path = dir_.filePath("vfr.mkv");
        const auto written = plainclip::write(path, 30, 3, {}, 0, 0, {0, 1000, 2000});
        QVERIFY2(!written, qPrintable(written.message));
        ExportRequest request;
        request.input = path;
        request.output = dir_.filePath("vfr.mp4");
        request.durationUs = 2'033'333;
        request.canvas = {cutclip::width, cutclip::height};
        request.edit.trimEnd = 1'010'000;
        request.config.format = "mp4";
        request.config.codec = "h264";
        request.config.fps = 30;
        request.config.audio.system = false;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable("mp4 export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QCOMPARE(result.frames, 30);
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(decoded.frames.size(), 30);
        QCOMPARE(decoded.frames.first().source, 0);
        QCOMPARE(decoded.frames.last().source, 1);
    }
    // #1635 E14: audio keeps the samples at or after each kept start, never one
    // before it. A kept part of 40 µs between two cuts (2 samples, so its fade
    // is a single sample at half gain) starts 0.48 of a sample after the
    // impulse that ends the first cut; rounding to the nearest sample let that
    // impulse through at half its level.
    void audioKeepsNoSampleBeforeAKeptStart() {
        constexpr int fps = 30;
        const auto path = dir_.filePath("impulse.mkv");
        // Sample 47760 (0.995 s) is the impulse; a 1 kHz tone plays over 0.1-0.85 s
        // and 2.15-2.9 s, kept time on both sides of it.
        const auto written = plainclip::write(path, fps, 90, [](qint64 us) {
            if (us == 995'000) return 0.9;
            const bool tone = (us >= 100'000 && us < 850'000) || (us >= 2'150'000 && us < 2'900'000);
            return tone ? 0.25 * std::sin(2.0 * M_PI * 1000.0 * double(us) / 1e6) : 0.0;
        });
        QVERIFY2(!written, qPrintable(written.message));
        ExportRequest request;
        request.input = path;
        request.output = dir_.filePath("impulse.mp4");
        request.durationUs = 3'000'000;
        request.canvas = {cutclip::width, cutclip::height};
        // Kept 0-0.99 s, 0.995010-0.995050 s (inside the grid's first span, which
        // runs to the frame at 1.0 s) and 2-3 s.
        request.edit.cuts = {{"a", 990'000, 995'010}, {"b", 995'050, 2'000'000}};
        request.config.format = "mp4";
        request.config.codec = "h264";
        request.config.fps = fps;
        request.config.audio.system = true;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable("mp4 export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // #1635 E23: the kept tone survives on both sides (output 0.5 s, and 1.5 s
        // for 2.5 s: the second span starts on output frame 30)...
        for (const qint64 t : {500'000LL, 1'500'000LL}) QVERIFY2(cutclip::toneAt(decoded.audio, t, 1000.0) > 0.15, qPrintable(QStringLiteral("kept tone missing at %1 us of output").arg(t)));
        // ...while nothing of the impulse is heard where it would land (0.995 s).
        float peak = 0;
        for (qint64 n = 920'000 * 48 / 1000; n < 1'080'000 * 48 / 1000 && n < qint64(decoded.audio.size()); ++n) peak = std::max(peak, std::abs(decoded.audio[size_t(n)]));
        QVERIFY2(peak < 0.02, qPrintable(QStringLiteral("a sample from inside the cut reached the output (peak %1)").arg(peak)));
    }
    // #1635 E20: a span shorter than one output frame gets no frame reserved
    // for it; its time rounds into the running total. At 15 fps, a 16.7 ms span
    // between two of 500 ms leaves the third span starting at its clock time
    // (0.517 s, output frame 8), where reserving a frame for the short span
    // pushed it a frame later.
    void subFrameSpansReserveNoFrame() {
        constexpr int fps = 60;
        putgd::RecordingEdit edit;
        edit.trimEnd = plainclip::gridUs(fps, 72);
        edit.cuts = {{"a", plainclip::gridUs(fps, 30), plainclip::gridUs(fps, 36)}, {"b", plainclip::gridUs(fps, 37), plainclip::gridUs(fps, 42)}};
        const KeptSpans kept(edit, plainclip::frameUs(fps, 150));
        QCOMPARE(kept.spans().size(), 3);
        ExportRequest request;
        request.input = sixty_;
        request.output = dir_.filePath("short-span.gif");
        request.durationUs = plainclip::frameUs(fps, 150);
        request.canvas = {cutclip::width, cutclip::height};
        request.edit = edit;
        request.config.format = "gif";
        request.config.fps = fps;
        request.config.audio.system = false;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable("gif export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        const auto third = std::find_if(decoded.frames.begin(), decoded.frames.end(), [](const cutclip::Decoded::Frame& f) { return f.source >= 42; });
        QVERIFY(third != decoded.frames.end());
        const qint64 at = third->us - decoded.frames.first().us, clock = kept.toOutput(plainclip::gridUs(fps, 42));
        QVERIFY2(std::abs(at - clock) <= 1'000'000 / 30, qPrintable(QStringLiteral("the third span starts at %1 us of output, its clock time is %2").arg(at).arg(clock)));
    }
    // #1635 E21: audio takes the trim start and the cuts from one sample
    // origin, and keeps clear of the container's rounding at every kept edge. A
    // trim start at 1.833333 s, inside a frame gap (the output opens on the
    // picture from 1.567 s), sits a third of a millisecond after an impulse in
    // the trimmed-off audio. The audio frame holding it follows a hole and
    // starts at 1832.5 ms, which Matroska stores as 1833 ms: placed by that
    // time, the impulse lands just after the trim start, and the trim taken in
    // seconds on the rounded times let it in at the very first sample.
    void heldTrimStartTakesNoTrimmedAudio() {
        const auto path = dir_.filePath("held-impulse.mkv");
        QList<qint64> times;
        for (int k = 0; k < gapclip::frames; ++k) times << gapclip::frameMs(k);
        // Sample 87984 (1.833 s) is the impulse; a tone plays from 1.9 s.
        const auto written = plainclip::write(path, 30, gapclip::frames, [](qint64 us) {
            if (us == 1'833'000) return 0.9;
            return us >= 1'900'000 ? 0.25 * std::sin(2.0 * M_PI * 1000.0 * double(us) / 1e6) : 0.0;
        }, 1'800'000, 1'832'500, times);
        QVERIFY2(!written, qPrintable(written.message));
        ExportRequest request;
        request.input = path;
        request.output = dir_.filePath("held-impulse.mp4");
        request.durationUs = gapclip::durationUs;
        request.canvas = {cutclip::width, cutclip::height};
        request.edit.trimStart = plainclip::gridUs(30, 55);
        request.edit.trimEnd = 3'000'000;
        request.config.format = "mp4";
        request.config.codec = "h264";
        request.config.fps = 30;
        request.config.audio.system = true;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable("mp4 export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(decoded.frames.first().source, gapclip::held);
        float peak = 0;
        for (qint64 n = 0; n < 30 * 48 && n < qint64(decoded.audio.size()); ++n) peak = std::max(peak, std::abs(decoded.audio[size_t(n)]));
        QVERIFY2(peak < 0.02, qPrintable(QStringLiteral("trimmed-off audio at the start (peak %1)").arg(peak)));
        QVERIFY2(cutclip::toneAt(decoded.audio, 400'000, 1000.0) > 0.15, "the kept tone is missing");
    }
    // #1635 E17: the half-millisecond slack belongs to put.gd's own Matroska,
    // where a stored frame time is its grid time rounded to the millisecond.
    // A coarser clock promises nothing of the kind, so its bounds are strict: in
    // an AVI clip at 1/30 s a trim starting 3 ms after frame 10 starts on frame 11,
    // never on the trimmed-off frame 10.
    void coarseTimeBaseUsesStrictBounds() {
        const auto path = dir_.filePath("coarse.avi");
        const auto written = plainclip::write(path, 30, 90, {}, 0, 0, {}, "avi", 30);
        QVERIFY2(!written, qPrintable(written.message));
        ExportRequest request;
        request.input = path;
        request.output = dir_.filePath("coarse.mp4");
        request.durationUs = 3'000'000;
        request.canvas = {cutclip::width, cutclip::height};
        request.edit.trimStart = plainclip::gridUs(30, 10) + 3000;
        request.config.format = "mp4";
        request.config.codec = "h264";
        request.config.fps = 30;
        request.config.audio.system = false;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable("mp4 export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(decoded.frames.first().source, 11);
    }
    // #1635 E22: a GIF ends on its last tick's rounded centisecond. 32 frames
    // at 15 fps end at 213 cs (2.1333 s); the last frame's 6 cs delay is not the
    // 7 cs of one frame step, which would end it at 214 cs.
    void gifEndsOnItsLastTick() {
        constexpr int fps = 60;
        ExportRequest request;
        request.input = sixty_;
        request.output = dir_.filePath("end.gif");
        request.durationUs = plainclip::frameUs(fps, 150);
        request.canvas = {cutclip::width, cutclip::height};
        request.edit.trimEnd = plainclip::gridUs(fps, 129);  // 2.15 s: 32 frames at 15 fps
        request.config.format = "gif";
        request.config.fps = fps;
        request.config.audio.system = false;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = putgd::record::exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") QSKIP(qPrintable("gif export unavailable in this FFmpeg: " + error.message));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QCOMPARE(result.frames, 32);
        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        const auto& last = decoded.frames.last();
        QCOMPARE(last.us - decoded.frames.first().us + last.durationUs, qint64(2'130'000));
    }
    void webmAudioDecodesEveryPacket_data() {
        QTest::addColumn<QString>("decoder");
        QTest::addColumn<qint64>("trimStart");
        QTest::newRow("opus trimmed") << "opus" << qint64(1'000'000);
    }
    void webmAudioDecodesEveryPacket() {
        QFETCH(QString, decoder);
        QFETCH(qint64, trimStart);
        if (!avcodec_find_decoder_by_name(decoder.toUtf8().constData())) QSKIP(qPrintable(decoder + " decoder unavailable in this FFmpeg"));
        const auto name = QStringLiteral("decode-%1.webm").arg(trimStart);
        QString skip;
        const bool exported = exportClip(name, "webm", "vp9", true, trimStart, &skip);
        if (!skip.isEmpty()) QSKIP(qPrintable(skip));
        QVERIFY(exported);
        int packets = 0;
        QString failure;
        const qint64 samples = decodeAudio(dir_.filePath(name), decoder.toUtf8().constData(), packets, failure);
        QVERIFY2(samples >= 0, qPrintable(failure));
        // 20 ms packets over the retained interval, the end trim applied by the decoder.
        const qint64 expected = (4'000'000 - trimStart) * 48 / 1000;
        QVERIFY2(packets >= expected / 960, qPrintable(QString::number(packets)));
        QVERIFY2(std::abs(samples - expected) <= 960, qPrintable(QStringLiteral("%1 samples, expected %2").arg(samples).arg(expected)));
    }
};
QTEST_GUILESS_MAIN(ExportAvSync)
#include "test_export_av_sync.moc"
