#include "Poster.h"
#include <QFile>
#include <QList>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace putgd::record {
namespace {
constexpr AVRational microseconds{1, 1000000};
// A still is not worth decoding a whole long GOP for: stop after this many
// frames past a seek and use the last one decoded.
constexpr int maximumFramesPerSeek = 900;
Error fail(const QString& message) { return {"poster_failed", message, {}}; }

struct Decoder {
    AVFormatContext* format = nullptr;
    AVCodecContext* video = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int index = -1;
    ~Decoder() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        if (video) avcodec_free_context(&video);
        if (format) avformat_close_input(&format);
    }
    Error open(const QString& path, qint64 maxPixels) {
        if (!packet || !frame) return fail("Out of memory");
        AVDictionary* options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        av_dict_set(&options, "format_whitelist", "mov,mp4,m4a,3gp,3g2,mj2,matroska,webm,avi,ogg", 0);
        const int opened = avformat_open_input(&format, QFile::encodeName(path).constData(), nullptr, &options);
        av_dict_free(&options);
        if (opened < 0) return fail("Not a supported video file");
        if (avformat_find_stream_info(format, nullptr) < 0) return fail("Cannot read the video");
        const AVCodec* decoder = nullptr;
        index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (index < 0 || !decoder || (format->streams[index]->disposition & AV_DISPOSITION_ATTACHED_PIC)) return fail("The file has no video");
        const auto* parameters = format->streams[index]->codecpar;
        if (parameters->width < 1 || parameters->height < 1) return fail("The video has no frame size");
        if (qint64(parameters->width) * parameters->height > maxPixels) return {"input_excessive", "The video frame exceeds the pixel limit", {}};
        video = avcodec_alloc_context3(decoder);
        if (!video || avcodec_parameters_to_context(video, parameters) < 0) return fail("Cannot decode the video");
        // The decoder refuses any later frame over the limit, whatever the header said.
        video->max_pixels = maxPixels;
        video->thread_count = 2;
        video->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (avcodec_open2(video, decoder, nullptr) < 0) return fail("No decoder for this video");
        return {};
    }
    qint64 durationUs() const {
        const auto* stream = format->streams[index];
        if (format->duration > 0) return format->duration;
        return stream->duration > 0 ? av_rescale_q(stream->duration, stream->time_base, microseconds) : 0;
    }
    qint64 origin() const { const auto start = format->streams[index]->start_time; return start == AV_NOPTS_VALUE ? 0 : start; }
    // The first frame at or after timeUs, or the last one before the end of the
    // file. A target of zero reads from where the file was opened.
    bool frameAt(qint64 timeUs, AVFrame* chosen) {
        const auto timeBase = format->streams[index]->time_base;
        const auto target = origin() + av_rescale_q(timeUs, microseconds, timeBase);
        if (timeUs > 0) {
            if (av_seek_frame(format, index, target, AVSEEK_FLAG_BACKWARD) < 0) return false;
            avcodec_flush_buffers(video);
        }
        av_frame_unref(chosen);
        int frames = 0;
        bool ended = false, have = false;
        while (true) {
            if (!ended) {
                const int read = av_read_frame(format, packet);
                if (read < 0) { ended = true; avcodec_send_packet(video, nullptr); }
                else if (packet->stream_index != index) { av_packet_unref(packet); continue; }
                else {
                    const int sent = avcodec_send_packet(video, packet);
                    av_packet_unref(packet);
                    if (sent < 0 && sent != AVERROR(EAGAIN)) return have;
                }
            }
            for (;;) {
                const int received = avcodec_receive_frame(video, frame);
                if (received == AVERROR(EAGAIN)) break;
                if (received < 0) return have;
                const auto pts = frame->best_effort_timestamp;
                av_frame_unref(chosen);
                av_frame_move_ref(chosen, frame);
                have = true;
                if (pts == AV_NOPTS_VALUE || pts >= target || ++frames >= maximumFramesPerSeek) return true;
            }
            if (ended) return have;
        }
    }
    double rotation() const {
        const auto* parameters = format->streams[index]->codecpar;
        const auto* matrix = av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        if (!matrix || matrix->size < 9 * sizeof(int32_t)) return 0;
        const double degrees = -av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix->data));
        return std::isnan(degrees) ? 0 : degrees;
    }
};

// Scales straight to the poster size in one conversion. The target is an
// FFmpeg-allocated frame: libswscale's SIMD paths write whole vectors, so a
// tightly sized buffer would be overrun on its last row.
QImage toImage(const AVFrame* source, AVRational aspect, double rotation, int maxEdge) {
    const double stretch = aspect.num > 0 && aspect.den > 0 ? av_q2d(aspect) : 1.0;
    QSize display(std::max(1, int(std::lround(source->width * stretch))), source->height);
    if (display.width() > maxEdge || display.height() > maxEdge) display.scale(maxEdge, maxEdge, Qt::KeepAspectRatio);
    display = display.expandedTo({1, 1});
    AVFrame* rgb = av_frame_alloc();
    if (!rgb) return {};
    rgb->format = AV_PIX_FMT_BGRA;
    rgb->width = display.width();
    rgb->height = display.height();
    QImage image;
    SwsContext* scaler = av_frame_get_buffer(rgb, 64) < 0 ? nullptr : sws_getContext(source->width, source->height, AVPixelFormat(source->format), rgb->width, rgb->height, AV_PIX_FMT_BGRA, SWS_AREA, nullptr, nullptr, nullptr);
    if (scaler) {
        sws_scale(scaler, source->data, source->linesize, 0, source->height, rgb->data, rgb->linesize);
        sws_freeContext(scaler);
        image = QImage(rgb->width, rgb->height, QImage::Format_RGB32);
        for (int y = 0; y < rgb->height; ++y) std::memcpy(image.scanLine(y), rgb->data[0] + qint64(y) * rgb->linesize[0], size_t(rgb->width) * 4);
    }
    av_frame_free(&rgb);
    // Phone videos store portrait frames sideways with a display matrix.
    const int quarter = int(std::lround(std::fmod(std::fmod(rotation, 360.0) + 360.0, 360.0) / 90.0)) % 4;
    if (!image.isNull() && quarter) image = image.transformed(QTransform().rotate(90.0 * quarter));
    return image;
}
// Black, a fade or a solid fill: almost no variation in brightness.
bool flat(const QImage& image) {
    double sum = 0, squares = 0;
    const qint64 count = qint64(image.width()) * image.height();
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) { const int gray = qGray(line[x]); sum += gray; squares += double(gray) * gray; }
    }
    if (count < 1) return true;
    const double mean = sum / count;
    return std::sqrt(std::max(0.0, squares / count - mean * mean)) < 8.0;
}
}

Error renderPoster(const QString& input, int maxEdge, qint64 maxPixels, Poster& out) {
    out = {};
    Decoder decoder;
    if (auto error = decoder.open(input, maxPixels)) return error;
    const auto* stream = decoder.format->streams[decoder.index];
    out.durationUs = decoder.durationUs();
    out.size = QSize(stream->codecpar->width, stream->codecpar->height);
    const auto aspect = av_guess_sample_aspect_ratio(decoder.format, const_cast<AVStream*>(stream), nullptr);
    const auto rotation = decoder.rotation();
    QList<qint64> times{out.durationUs > 0 ? std::min<qint64>(1'000'000, out.durationUs / 2) : 0};
    if (out.durationUs > 0) for (const qint64 t : {out.durationUs / 4, out.durationUs / 2}) if (t > times.last()) times.append(t);
    AVFrame* frame = av_frame_alloc();
    if (!frame) return fail("Out of memory");
    for (const auto t : times) {
        if (!decoder.frameAt(t, frame)) continue;
        const auto image = toImage(frame, aspect, rotation, maxEdge);
        if (image.isNull()) continue;
        const bool usable = !flat(image);
        if (out.image.isNull() || usable) {
            out.image = image;
            out.frameUs = frame->best_effort_timestamp == AV_NOPTS_VALUE ? 0 : av_rescale_q(frame->best_effort_timestamp - decoder.origin(), stream->time_base, microseconds);
        }
        if (usable) break;
    }
    av_frame_free(&frame);
    if (!out.image.isNull()) return {};
    // Every seek failed: the first decodable frame, read from a fresh start.
    Decoder first;
    if (auto error = first.open(input, maxPixels)) return error;
    frame = av_frame_alloc();
    if (frame && first.frameAt(0, frame)) out.image = toImage(frame, aspect, rotation, maxEdge);
    av_frame_free(&frame);
    return out.image.isNull() ? fail("No frame could be decoded") : Error{};
}
}
