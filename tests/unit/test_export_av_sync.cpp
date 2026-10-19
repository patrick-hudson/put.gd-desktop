#include "record/AvSync.h"
#include "record/ClipExporter.h"
#include <QtTest>
#include <cmath>
#include <cstdarg>
#include <cstdio>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
using namespace putgd::record;

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

// #86: a working clip's audio starts after its first video frame (the sink
// monitor opens after the screen stream; 20-110 ms on the lab desktops). Every
// export with sound must keep that gap rather than rebase the audio to its own
// first sample; silent exports (GIF, WebP, audio off) never read the audio.
// #119: every Opus packet of a WebM export, the end-trimmed last one included,
// decodes without error and the decoded audio lasts as long as the export.
class ExportAvSync : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString clip_;
    static double median(const QString& path) {
        QJsonObject sync;
        const auto error = measureAvSync(path, sync);
        if (error) qWarning() << error.code << error.message;
        return error ? NAN : sync["medianMs"].toDouble();
    }
    static int streams(const QString& path, AVMediaType type) {
        AVFormatContext* format = nullptr;
        if (avformat_open_input(&format, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) return -1;
        int count = avformat_find_stream_info(format, nullptr) < 0 ? -1 : 0;
        for (unsigned i = 0; count >= 0 && i < format->nb_streams; ++i) count += format->streams[i]->codecpar->codec_type == type;
        avformat_close_input(&format);
        return count;
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
    bool exportClip(const QString& name, const QString& format, const QString& codec, bool audio, qint64 trimStart, QString* skip) {
        ExportRequest request;
        request.input = clip_;
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
    }
    void exportsWithSoundKeepTheAudioStart_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("codec");
        QTest::addColumn<qint64>("trimStart");
        QTest::newRow("mp4") << "mp4" << "h264" << qint64(0);
        QTest::newRow("mp4 trimmed") << "mp4" << "h264" << qint64(1'000'000);
        QTest::newRow("webm") << "webm" << "vp9" << qint64(0);
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
    void webmAudioDecodesEveryPacket_data() {
        QTest::addColumn<QString>("decoder");
        QTest::addColumn<qint64>("trimStart");
        QTest::newRow("opus") << "opus" << qint64(0);
        QTest::newRow("opus trimmed") << "opus" << qint64(1'000'000);
        QTest::newRow("libopus") << "libopus" << qint64(0);
        QTest::newRow("libopus trimmed") << "libopus" << qint64(1'000'000);
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
    void silentExportsCarryNoAudio_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("codec");
        QTest::newRow("mp4 without sound") << "mp4" << "h264";
        QTest::newRow("gif") << "gif" << "";
        QTest::newRow("webp") << "webp" << "";
    }
    void silentExportsCarryNoAudio() {
        QFETCH(QString, format);
        QFETCH(QString, codec);
        const auto name = QStringLiteral("silent.%1").arg(format);
        QString skip;
        const bool exported = exportClip(name, format, codec, false, 0, &skip);
        if (!skip.isEmpty()) QSKIP(qPrintable(skip));
        QVERIFY(exported);
        if (format == "webp") return;  // FFmpeg reads no animated WebP; the export succeeding is the check
        QCOMPARE(streams(dir_.filePath(name), AVMEDIA_TYPE_AUDIO), 0);
        QCOMPARE(streams(dir_.filePath(name), AVMEDIA_TYPE_VIDEO), 1);
    }
};
QTEST_GUILESS_MAIN(ExportAvSync)
#include "test_export_av_sync.moc"
