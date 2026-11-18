#include "core/KeptSpans.h"
#include "record/AdaptiveRate.h"
#include "record/AvSync.h"
#include "record/ClipExporter.h"
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
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
    struct Frame { qint64 us = 0; int source = -1; bool white = false, spanningCovered = false, cutOnlyCovered = false; double chroma = 0; };
    QList<Frame> frames;
    QList<qint64> bursts;  // onset times in the output, µs
    int audioFrameSize = 1024;
    qint64 audioEndUs = 0;  // where the decoded audio ends, in output time (0 without audio)
};
QString analyse(const QString& path, Decoded& result) {
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
    std::vector<float> mono;
    qint64 audioStartUs = 0;
    bool audioStarted = false;
    auto mean = [&](int plane, const QRect& r) {
        double sum = 0;
        for (int y = r.top(); y <= r.bottom(); ++y) for (int x = r.left(); x <= r.right(); ++x) sum += yuv->data[plane][qint64(y) * yuv->linesize[plane] + x];
        return sum / double(r.width() * r.height());
    };
    auto takeVideo = [&](AVFrame* f) -> QString {
        scaler = sws_getCachedContext(scaler, f->width, f->height, AVPixelFormat(f->format), width, height, AV_PIX_FMT_YUV444P, SWS_POINT, nullptr, nullptr, nullptr);
        av_frame_unref(yuv);
        yuv->format = AV_PIX_FMT_YUV444P; yuv->width = width; yuv->height = height;
        if (!scaler || av_frame_get_buffer(yuv, 0) < 0) return "cannot convert a frame";
        sws_scale(scaler, f->data, f->linesize, 0, f->height, yuv->data, yuv->linesize);
        Decoded::Frame out;
        out.us = av_rescale_q(f->best_effort_timestamp, format->streams[videoIndex]->time_base, {1, 1000000});
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
    };
    auto takeAudio = [&](AVFrame* f) {
        if (!audioStarted) { audioStartUs = av_rescale_q(f->best_effort_timestamp, format->streams[audioIndex]->time_base, {1, 1000000}); audioStarted = true; }
        const bool planar = av_sample_fmt_is_planar(AVSampleFormat(f->format));
        for (int i = 0; i < f->nb_samples; ++i) {
            float v = 0;
            if (f->format == AV_SAMPLE_FMT_FLTP || f->format == AV_SAMPLE_FMT_FLT) v = planar ? reinterpret_cast<const float*>(f->extended_data[0])[i] : reinterpret_cast<const float*>(f->data[0])[i * f->ch_layout.nb_channels];
            else if (f->format == AV_SAMPLE_FMT_S16P || f->format == AV_SAMPLE_FMT_S16) v = (planar ? reinterpret_cast<const int16_t*>(f->extended_data[0])[i] : reinterpret_cast<const int16_t*>(f->data[0])[i * f->ch_layout.nb_channels]) / 32768.0f;
            mono.push_back(v);
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
    if (audioStarted) result.audioEndUs = audioStartUs + qint64(mono.size()) * 1'000'000 / rate;
    // Burst onsets: a 1 ms window over 0.05 RMS after at least 20 ms under 0.02.
    const int window = rate / 1000;
    int quiet = 0;
    for (size_t at = 0; at + size_t(window) <= mono.size(); at += size_t(window)) {
        double energy = 0;
        for (int i = 0; i < window; ++i) energy += double(mono[at + size_t(i)]) * mono[at + size_t(i)];
        const double rms = std::sqrt(energy / window);
        if (rms > 0.05 && quiet >= 20) result.bursts << audioStartUs + qint64(at) * 1'000'000 / rate;
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
    QString clip_, gapped_, cut_, frameGap_;
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
            // The exact frame sequence: every kept clip frame once, in order.
            QList<int> expected;
            for (int index = 0; index < cutclip::frames; ++index) if (kept.kept(frameUs(index))) expected << index;
            QList<int> shown;
            for (const auto& f : frames) shown << f.source;
            QCOMPARE(shown, expected);
            for (qsizetype i = 0; i < frames.size(); ++i) {
                const qint64 sourceUs = frameUs(expected[i]);
                const bool cover = sourceUs >= spanningFrom && sourceUs < spanningTo;
                QVERIFY2(frames[i].spanningCovered == cover, qPrintable(QStringLiteral("clip frame %1: cover %2, expected %3").arg(expected[i]).arg(frames[i].spanningCovered).arg(cover)));
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
        const int outputFps = format == "gif" ? 15 : fps;
        const qint64 step = 1'000'000 / outputFps;
        // The output lasts exactly the time kept, whole output frames here, and progress runs over it.
        QVERIFY2(std::abs(result.durationUs - expected.outputDuration()) <= 1000, qPrintable(QStringLiteral("%1 us, expected %2").arg(result.durationUs).arg(expected.outputDuration())));
        QVERIFY(!progress.isEmpty() && progress.last() > 0.95);
        for (qsizetype i = 1; i < progress.size(); ++i) QVERIFY(progress[i] >= progress[i - 1]);

        cutclip::Decoded decoded;
        const auto failure = cutclip::analyse(request.output, decoded);
        if (format == "webp" && failure == "cannot set up a decoder") QSKIP("this FFmpeg cannot decode animated WebP: only the duration and progress were checked");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        const auto& shown = decoded.frames;
        QVERIFY(shown.size() > 10);
        const qint64 first = shown.first().us;
        const qint64 length = shown.last().us + step - first;
        QVERIFY2(std::abs(length - expected.outputDuration()) <= step, qPrintable(QStringLiteral("%1 us of video for %2 kept").arg(length).arg(expected.outputDuration())));
        // GIF's 15 fps step keeps the later of two clip frames; the window check below covers it.
        if (format != "gif") QCOMPARE(shown.first().source, onScreen(keptFrom));
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
            // GIF's 15 fps step may keep the frame after the first one there.
            const int opens = onScreen(expected.toSource(seam)), next = onScreen(expected.toSource(seam + step / 2));
            QVERIFY2(after->source == opens || (format == "gif" && after->source == next), qPrintable(QStringLiteral("the seam at %1 us opens on clip frame %2, expected %3").arg(seam).arg(after->source).arg(opens)));
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
        QVERIFY2(decoded.audioEndUs - first <= expected.outputDuration() + audioFrameUs, qPrintable(QStringLiteral("audio runs to %1 us of %2").arg(decoded.audioEndUs - first).arg(expected.outputDuration())));
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
