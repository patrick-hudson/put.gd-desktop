#include "record/AvSync.h"
#include "record/ClipExporter.h"
#include <QtTest>
#include <cmath>
using namespace putgd::record;

// #86: a working clip's audio starts after its first video frame (the sink
// monitor opens after the screen stream; 20-110 ms on the lab desktops). The
// export must keep that gap, not rebase the audio to its own first sample.
class ExportAvSync : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString clip_;
    double median(const QString& path) {
        QJsonObject sync;
        const auto error = measureAvSync(path, sync);
        if (error) qWarning() << error.code << error.message;
        return error ? NAN : sync["medianMs"].toDouble();
    }
    double exported(const QString& name, qint64 trimStart) {
        ExportRequest request;
        request.input = clip_;
        request.output = dir_.filePath(name);
        request.durationUs = 4'000'000;
        request.canvas = {1280, 720};
        request.edit.trimStart = trimStart;
        request.config.format = "mp4";
        request.config.audio.system = true;
        ExportResult result;
        std::atomic_bool cancelled{false};
        const auto error = exportClip(request, result, cancelled);
        if (error.code == "recording_unsupported") return INFINITY;  // no H.264 or AAC encoder in this FFmpeg
        if (error) { qWarning() << error.code << error.message; return NAN; }
        if (result.audioEncoder.isEmpty()) return NAN;
        return median(request.output);
    }
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        clip_ = dir_.filePath("working.mkv");
        QVERIFY(!writeAvMarker(clip_, 4, 110));
        // The marker itself is in sync in container time, with audio starting 110 ms in.
        QVERIFY(std::abs(median(clip_)) <= 1.0);
    }
    void exportKeepsTheAudioStart() {
        const double offset = exported("whole.mp4", 0);
        if (std::isinf(offset)) QSKIP("FFmpeg has no MP4 H.264/AAC encoder here");
        // Within half a frame at 30 fps; before the fix this read -110 ms.
        QVERIFY2(std::abs(offset) <= 17.0, qPrintable(QString::number(offset)));
    }
    void trimmedExportStaysInSync() {
        const double offset = exported("trimmed.mp4", 1'000'000);
        if (std::isinf(offset)) QSKIP("FFmpeg has no MP4 H.264/AAC encoder here");
        QVERIFY2(std::abs(offset) <= 17.0, qPrintable(QString::number(offset)));
    }
};
QTEST_GUILESS_MAIN(ExportAvSync)
#include "test_export_av_sync.moc"
