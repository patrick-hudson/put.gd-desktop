#include "record/AvSync.h"
#include "record/ClipExporter.h"
#include <QtTest>
#include <cmath>
extern "C" {
#include <libavformat/avformat.h>
}
using namespace putgd::record;

// #86: a working clip's audio starts after its first video frame (the sink
// monitor opens after the screen stream; 20-110 ms on the lab desktops). Every
// export with sound must keep that gap rather than rebase the audio to its own
// first sample; silent exports (GIF, WebP, audio off) never read the audio.
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
