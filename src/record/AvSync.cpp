#include "AvSync.h"
#include <QFile>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}

namespace putgd::record {
namespace {
struct Sample { double time; double value; };
Error failed(const QString& code, const QString& message) { return {code, message, {}}; }
struct Decoder {
    AVCodecContext* context = nullptr;
    ~Decoder() { avcodec_free_context(&context); }
    Error open(AVStream* stream, int threads) {
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) return failed("av_sync_decoder", "No decoder for the clip's stream");
        context = avcodec_alloc_context3(codec);
        if (!context || avcodec_parameters_to_context(context, stream->codecpar) < 0) return failed("av_sync_decoder", "Cannot configure the decoder");
        context->thread_count = threads;
        context->pkt_timebase = stream->time_base;
        if (avcodec_open2(context, codec, nullptr) < 0) return failed("av_sync_decoder", "Cannot open the decoder");
        return {};
    }
};
double seconds(int64_t pts, AVRational base) { return pts == AV_NOPTS_VALUE ? NAN : double(pts) * av_q2d(base); }
// Mean of the luma plane on a sparse grid (planar and semi-planar YUV only).
bool meanLuma(const AVFrame* frame, double& out) {
    const auto* desc = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
    if (!desc || (desc->flags & AV_PIX_FMT_FLAG_RGB) || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) || desc->comp[0].depth != 8) return false;
    double sum = 0;
    qint64 count = 0;
    for (int y = 0; y < frame->height; y += 4) {
        const uint8_t* row = frame->data[0] + qint64(y) * frame->linesize[0];
        for (int x = 0; x < frame->width; x += 4) { sum += row[x * (desc->comp[0].step)]; ++count; }
    }
    if (!count) return false;
    out = sum / double(count);
    return true;
}
// Rising edges through `high` after the signal fell below `low`, at least `gap` seconds apart.
std::vector<double> onsets(const std::vector<Sample>& signal, double low, double high, double gap) {
    std::vector<double> result;
    bool armed = true;
    for (const auto& s : signal) {
        if (s.value < low) armed = true;
        else if (armed && s.value >= high && (result.empty() || s.time - result.back() >= gap)) { result.push_back(s.time); armed = false; }
    }
    return result;
}
}

Error measureAvSync(const QString& clip, QJsonObject& result) {
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, QFile::encodeName(clip).constData(), nullptr, nullptr) < 0) return failed("av_sync_open", "Cannot open the clip");
    struct Closer { AVFormatContext*& f; ~Closer() { avformat_close_input(&f); } } closer{format};
    if (avformat_find_stream_info(format, nullptr) < 0) return failed("av_sync_open", "Cannot read the clip's streams");
    const int videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (videoIndex < 0) return failed("av_sync_no_video", "The clip has no video stream");
    if (audioIndex < 0) return failed("av_sync_no_audio", "The clip has no audio stream; record with system audio");
    Decoder video, audio;
    if (auto e = video.open(format->streams[videoIndex], 0)) return e;
    if (auto e = audio.open(format->streams[audioIndex], 1)) return e;
    const int rate = audio.context->sample_rate;
    if (rate < 8000) return failed("av_sync_decoder", "Unexpected audio sample rate");
    SwrContext* swr = nullptr;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    if (swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_FLT, rate, &audio.context->ch_layout, audio.context->sample_fmt, rate, 0, nullptr) < 0 || swr_init(swr) < 0) { swr_free(&swr); return failed("av_sync_decoder", "Cannot convert the audio"); }
    struct SwrCloser { SwrContext*& s; ~SwrCloser() { swr_free(&s); } } swrCloser{swr};

    std::vector<Sample> luma, envelope;
    const int window = std::max(1, rate / 500);  // 2 ms RMS windows
    double windowSum = 0, windowStart = NAN, audioSeconds = 0;
    int windowCount = 0;
    std::vector<float> converted;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto drain = [&](Decoder& decoder, int index) -> Error {
        const AVRational base = format->streams[index]->time_base;
        for (;;) {
            const int rc = avcodec_receive_frame(decoder.context, frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
            if (rc < 0) return failed("av_sync_decode", "The clip does not decode");
            const double t = seconds(frame->best_effort_timestamp, base);
            if (index == videoIndex) {
                double value = 0;
                if (!std::isnan(t) && meanLuma(frame, value)) luma.push_back({t, value});
                else if (!std::isnan(t)) { av_frame_unref(frame); return failed("av_sync_pixels", "Unsupported video pixel format"); }
            } else if (!std::isnan(t)) {
                converted.resize(size_t(frame->nb_samples) + 64);
                uint8_t* out[1] = {reinterpret_cast<uint8_t*>(converted.data())};
                const int got = swr_convert(swr, out, int(converted.size()), const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                for (int i = 0; i < got; ++i) {
                    if (windowCount == 0) windowStart = t + double(i) / rate;
                    windowSum += double(converted[size_t(i)]) * converted[size_t(i)];
                    if (++windowCount == window) { envelope.push_back({windowStart, std::sqrt(windowSum / window)}); windowSum = 0; windowCount = 0; }
                }
                audioSeconds += double(std::max(got, 0)) / rate;
            }
            av_frame_unref(frame);
        }
    };
    Error error;
    while (!error && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == videoIndex && avcodec_send_packet(video.context, packet) >= 0) error = drain(video, videoIndex);
        else if (packet->stream_index == audioIndex && avcodec_send_packet(audio.context, packet) >= 0) error = drain(audio, audioIndex);
        av_packet_unref(packet);
    }
    if (!error) { avcodec_send_packet(video.context, nullptr); error = drain(video, videoIndex); }
    if (!error) { avcodec_send_packet(audio.context, nullptr); error = drain(audio, audioIndex); }
    av_frame_free(&frame);
    av_packet_free(&packet);
    if (error) return error;
    if (luma.size() < 4 || envelope.size() < 4) return failed("av_sync_short", "The clip is too short to measure");
    std::sort(luma.begin(), luma.end(), [](const Sample& a, const Sample& b) { return a.time < b.time; });

    double lo = 255, hi = 0, peak = 0;
    for (const auto& s : luma) { lo = std::min(lo, s.value); hi = std::max(hi, s.value); }
    for (const auto& s : envelope) peak = std::max(peak, s.value);
    if (hi - lo < 40) return failed("av_sync_no_flashes", "No flashes in the video: put the marker clip in the recorded area");
    if (peak < 0.01) return failed("av_sync_no_bursts", "The audio is silent: the marker's sound did not reach the recording");
    const double middle = (lo + hi) / 2;
    const auto flashes = onsets(luma, middle - (hi - lo) * 0.1, middle, 0.3);
    const auto bursts = onsets(envelope, peak * 0.1, peak * 0.3, 0.3);
    if (flashes.size() < 2) return failed("av_sync_no_flashes", "Fewer than two flashes in the video");
    std::vector<double> gaps;
    for (size_t i = 1; i < flashes.size(); ++i) gaps.push_back(flashes[i] - flashes[i - 1]);
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
    const double period = gaps[gaps.size() / 2];
    QJsonArray offsets;
    std::vector<double> values;
    for (const double flash : flashes) {
        const auto nearest = std::min_element(bursts.begin(), bursts.end(), [flash](double a, double b) { return std::abs(a - flash) < std::abs(b - flash); });
        if (nearest == bursts.end() || std::abs(*nearest - flash) > period / 2) continue;
        const double ms = (*nearest - flash) * 1000.0;
        values.push_back(ms);
        offsets.append(std::round(ms * 10) / 10);
    }
    if (values.empty()) return failed("av_sync_unpaired", "No flash has a tone burst within half a period");
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    result = {{"flashes", int(flashes.size())}, {"bursts", int(bursts.size())}, {"pairs", int(values.size())}, {"periodMs", std::round(period * 10000) / 10},
        {"offsetsMs", offsets}, {"medianMs", std::round(median * 10) / 10}, {"minMs", std::round(sorted.front() * 10) / 10}, {"maxMs", std::round(sorted.back() * 10) / 10},
        {"spreadMs", std::round((sorted.back() - sorted.front()) * 10) / 10}, {"videoFrames", int(luma.size())}, {"audioSeconds", std::round(audioSeconds * 100) / 100},
        {"peak", std::round(peak * 1000) / 1000}};
    return {};
}

Error writeAvMarker(const QString& path, int seconds, int audioDelayMs) {
    constexpr int fps = 30, rate = 48000, width = 1280, height = 720, chunk = 1024;
    if (seconds < 2 || seconds > 600) return failed("invalid_request", "The marker lasts 2 to 600 seconds");
    if (audioDelayMs < 0 || audioDelayMs >= 1000) return failed("invalid_request", "The audio delay is 0 to 999 ms");
    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "matroska", QFile::encodeName(path).constData()) < 0 || !out) return failed("av_marker_io", "Cannot create the marker file");
    struct Closer { AVFormatContext*& f; ~Closer() { if (f && !(f->oformat->flags & AVFMT_NOFILE)) avio_closep(&f->pb); avformat_free_context(f); } } closer{out};
    auto openEncoder = [&](AVCodecID id, AVStream*& stream, AVCodecContext*& context, auto configure) -> Error {
        const AVCodec* codec = avcodec_find_encoder(id);
        if (!codec) return failed("av_marker_encoder", "A built-in FFmpeg encoder is missing");
        stream = avformat_new_stream(out, nullptr);
        context = avcodec_alloc_context3(codec);
        if (!stream || !context) return failed("av_marker_encoder", "Cannot allocate the encoder");
        configure(context);
        if (out->oformat->flags & AVFMT_GLOBALHEADER) context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(context, codec, nullptr) < 0 || avcodec_parameters_from_context(stream->codecpar, context) < 0) return failed("av_marker_encoder", "Cannot open the encoder");
        stream->time_base = context->time_base;
        return {};
    };
    AVStream *videoStream = nullptr, *audioStream = nullptr;
    AVCodecContext *video = nullptr, *audio = nullptr;
    struct Contexts { AVCodecContext*& v; AVCodecContext*& a; ~Contexts() { avcodec_free_context(&v); avcodec_free_context(&a); } } contexts{video, audio};
    if (auto e = openEncoder(AV_CODEC_ID_MJPEG, videoStream, video, [&](AVCodecContext* c) {
            c->width = width; c->height = height; c->time_base = {1, fps}; c->framerate = {fps, 1};
            c->pix_fmt = AV_PIX_FMT_YUVJ420P; c->color_range = AVCOL_RANGE_JPEG; c->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
            c->flags |= AV_CODEC_FLAG_QSCALE; c->global_quality = FF_QP2LAMBDA * 4; }))
        return e;
    if (auto e = openEncoder(AV_CODEC_ID_PCM_S16LE, audioStream, audio, [&](AVCodecContext* c) {
            c->sample_rate = rate; c->sample_fmt = AV_SAMPLE_FMT_S16; c->time_base = {1, rate};
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO; av_channel_layout_copy(&c->ch_layout, &stereo); }))
        return e;
    if (!(out->oformat->flags & AVFMT_NOFILE) && avio_open(&out->pb, QFile::encodeName(path).constData(), AVIO_FLAG_WRITE) < 0) return failed("av_marker_io", "Cannot write the marker file");
    if (avformat_write_header(out, nullptr) < 0) return failed("av_marker_io", "Cannot write the marker header");
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    struct Frames { AVPacket*& p; AVFrame*& f; ~Frames() { av_packet_free(&p); av_frame_free(&f); } } frames{packet, frame};
    auto encode = [&](AVCodecContext* context, AVStream* stream, AVFrame* input) -> Error {
        if (avcodec_send_frame(context, input) < 0) return failed("av_marker_encoder", "The encoder refused a frame");
        while (avcodec_receive_packet(context, packet) == 0) {
            av_packet_rescale_ts(packet, context->time_base, stream->time_base);
            packet->stream_index = stream->index;
            if (av_interleaved_write_frame(out, packet) < 0) return failed("av_marker_io", "Cannot write a marker packet");
        }
        return {};
    };
    // Interleave by time: one video frame, then the audio up to its end.
    qint64 samples = qint64(audioDelayMs) * rate / 1000;
    for (int index = 0; index < seconds * fps; ++index) {
        const bool flash = index % fps < fps / 10;
        av_frame_unref(frame);
        frame->format = AV_PIX_FMT_YUVJ420P; frame->width = width; frame->height = height;
        if (av_frame_get_buffer(frame, 0) < 0) return failed("av_marker_encoder", "Cannot allocate a frame");
        for (int y = 0; y < height; ++y) std::fill_n(frame->data[0] + qint64(y) * frame->linesize[0], width, uint8_t(flash ? 255 : 0));
        for (int plane = 1; plane < 3; ++plane) for (int y = 0; y < height / 2; ++y) std::fill_n(frame->data[plane] + qint64(y) * frame->linesize[plane], width / 2, uint8_t(128));
        frame->pts = index;
        if (auto e = encode(video, videoStream, frame)) return e;
        const qint64 until = qint64(index + 1) * rate / fps;
        while (samples < until) {
            const int count = int(std::min<qint64>(chunk, until - samples));
            av_frame_unref(frame);
            frame->format = AV_SAMPLE_FMT_S16; frame->sample_rate = rate; frame->nb_samples = count;
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO; av_channel_layout_copy(&frame->ch_layout, &stereo);
            if (av_frame_get_buffer(frame, 0) < 0) return failed("av_marker_encoder", "Cannot allocate audio");
            auto* pcm = reinterpret_cast<int16_t*>(frame->data[0]);
            for (int i = 0; i < count; ++i) {
                const qint64 n = samples + i;
                const bool burst = n % rate < rate / 10;
                const auto value = int16_t(burst ? std::lround(8000.0 * std::sin(2.0 * M_PI * 1000.0 * double(n) / rate)) : 0);
                pcm[2 * i] = pcm[2 * i + 1] = value;
            }
            frame->pts = samples;
            samples += count;
            if (auto e = encode(audio, audioStream, frame)) return e;
        }
    }
    if (auto e = encode(video, videoStream, nullptr)) return e;
    if (auto e = encode(audio, audioStream, nullptr)) return e;
    if (av_write_trailer(out) < 0) return failed("av_marker_io", "Cannot finish the marker file");
    return {};
}
}
