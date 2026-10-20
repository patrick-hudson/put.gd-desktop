#include "record/ClipExporter.h"
#include "record/Poster.h"
#include <QtTest>
#include <algorithm>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
using namespace putgd::record;

// #112: History's poster for a video is decoded from the file as stored. It
// skips a black lead-in, keeps an export's cover masks, and refuses frames
// over the pixel limit and files that are not video.
class PosterTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString clip_;
    static constexpr int width = 640, height = 360, fps = 30;
    // 3 s of MJPEG in Matroska, black for the first 1.2 s, then bright stripes.
    static bool writeClip(const QString& path) {
        AVFormatContext* out = nullptr;
        if (avformat_alloc_output_context2(&out, nullptr, "matroska", QFile::encodeName(path).constData()) < 0) return false;
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        AVStream* stream = avformat_new_stream(out, nullptr);
        AVCodecContext* context = codec ? avcodec_alloc_context3(codec) : nullptr;
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        bool ok = context && stream && packet && frame;
        if (ok) {
            context->width = width; context->height = height; context->time_base = {1, fps}; context->framerate = {fps, 1};
            context->pix_fmt = AV_PIX_FMT_YUVJ420P; context->color_range = AVCOL_RANGE_JPEG; context->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
            if (out->oformat->flags & AVFMT_GLOBALHEADER) context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            ok = avcodec_open2(context, codec, nullptr) >= 0 && avcodec_parameters_from_context(stream->codecpar, context) >= 0;
            stream->time_base = context->time_base;
        }
        ok = ok && avio_open(&out->pb, QFile::encodeName(path).constData(), AVIO_FLAG_WRITE) >= 0 && avformat_write_header(out, nullptr) >= 0;
        auto drain = [&] {
            while (avcodec_receive_packet(context, packet) == 0) {
                av_packet_rescale_ts(packet, context->time_base, stream->time_base);
                packet->stream_index = stream->index;
                if (av_interleaved_write_frame(out, packet) < 0) return false;
            }
            return true;
        };
        for (int index = 0; ok && index < 3 * fps; ++index) {
            av_frame_unref(frame);
            frame->format = AV_PIX_FMT_YUVJ420P; frame->width = width; frame->height = height;
            ok = av_frame_get_buffer(frame, 0) >= 0;
            const bool lit = index >= fps * 6 / 5;
            for (int y = 0; ok && y < height; ++y) for (int x = 0; x < width; ++x) frame->data[0][qint64(y) * frame->linesize[0] + x] = uint8_t(lit ? ((x / 16) % 2 ? 255 : 170) : 0);
            for (int plane = 1; ok && plane < 3; ++plane) for (int y = 0; y < height / 2; ++y) std::fill_n(frame->data[plane] + qint64(y) * frame->linesize[plane], width / 2, uint8_t(128));
            frame->pts = index;
            ok = ok && avcodec_send_frame(context, frame) >= 0 && drain();
        }
        ok = ok && avcodec_send_frame(context, nullptr) >= 0 && drain() && av_write_trailer(out) >= 0;
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        if (out->pb) avio_closep(&out->pb);
        avformat_free_context(out);
        return ok;
    }
    static double luma(const QImage& image, const QRect& area) {
        double sum = 0;
        for (int y = area.top(); y <= area.bottom(); ++y) for (int x = area.left(); x <= area.right(); ++x) sum += qGray(image.pixel(x, y));
        return sum / (double(area.width()) * area.height());
    }
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        clip_ = dir_.filePath("working.mkv");
        QVERIFY(writeClip(clip_));
    }
    void skipsABlackLeadIn() {
        Poster poster;
        const auto error = renderPoster(clip_, 256, 100'000'000, poster);
        QVERIFY2(!error, qPrintable(error.message));
        QCOMPARE(poster.image.size(), QSize(256, 144));
        QCOMPARE(poster.size, QSize(width, height));
        QVERIFY2(std::abs(poster.durationUs - 3'000'000) <= 100'000, qPrintable(QString::number(poster.durationUs)));
        // 1 s in is still black; half-way is the first lit candidate.
        QVERIFY2(poster.frameUs >= 1'200'000, qPrintable(QString::number(poster.frameUs)));
        QVERIFY(luma(poster.image, poster.image.rect()) > 150);
    }
    void keepsAnExportsCoverMasks_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("codec");
        QTest::newRow("mp4") << "mp4" << "h264";
        QTest::newRow("webm") << "webm" << "vp9";
    }
    void keepsAnExportsCoverMasks() {
        QFETCH(QString, format);
        QFETCH(QString, codec);
        ExportRequest request;
        request.input = clip_;
        request.output = dir_.filePath("masked." + format);
        request.durationUs = 3'000'000;
        request.canvas = {width, height};
        request.config.format = format;
        request.config.codec = codec;
        request.config.audio.system = false;
        putgd::RecordingMask mask;
        mask.kind = "solid";
        mask.rect = QRect(0, 0, width / 2, height / 2);
        mask.start = 0;
        mask.end = 3'000'000;
        request.edit.masks.append(mask);
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto exported = exportClip(request, result, cancelled);
        if (exported.code == "recording_unsupported") QSKIP(qPrintable(format + " export unavailable in this FFmpeg: " + exported.message));
        QVERIFY2(!exported, qPrintable(exported.message));
        Poster poster;
        const auto error = renderPoster(request.output, 256, 100'000'000, poster);
        QVERIFY2(!error, qPrintable(error.message));
        QCOMPARE(poster.image.size(), QSize(256, 144));
        // The covered quarter stays covered in the poster; the rest is the picture.
        const double covered = luma(poster.image, QRect(4, 4, 120, 64)), open = luma(poster.image, QRect(132, 76, 120, 64));
        QVERIFY2(covered < 24 && open > 150, qPrintable(QString("covered %1 open %2").arg(covered).arg(open)));
    }
    void refusesFramesOverThePixelLimit() {
        Poster poster;
        QCOMPARE(renderPoster(clip_, 256, qint64(width) * height - 1, poster).code, QString("input_excessive"));
        QVERIFY(poster.image.isNull());
    }
    void refusesWhatIsNotVideo() {
        const auto text = dir_.filePath("notes.mp4");
        QFile file(text);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nhttp://127.0.0.1:9/never.ts\n#EXT-X-ENDLIST\n");
        file.close();
        Poster poster;
        // A playlist is not an allowed container, so nothing it names is opened.
        QCOMPARE(renderPoster(text, 256, 100'000'000, poster).code, QString("poster_failed"));
        QCOMPARE(renderPoster(dir_.filePath("missing.mp4"), 256, 100'000'000, poster).code, QString("poster_failed"));
    }
};
QTEST_GUILESS_MAIN(PosterTest)
#include "test_poster.moc"
