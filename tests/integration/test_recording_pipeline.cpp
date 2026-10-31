// The live pipeline's frame throughput on a private Xvfb root: capture,
// conversion and encoding in separate threads hold the configured frame rate
// and produce a constant-rate clip (#6 recorder choppiness). Not desktop proof.
#include "record/RecordingPipeline.h"
#include "fixtures/Waits.h"
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <gst/gst.h>
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace putgd::record {
class RecordingPipelineThroughput : public QObject {
    Q_OBJECT
    QProcess display_;
    GstElement* pattern_ = nullptr;
    struct Clip { int frames = 0; qint64 durationNs = 0; qint64 maxGapNs = 0, minGapNs = 0; int irregular = 0; };
    // Parses the clip's access units and measures their timestamps; no decoder
    // is needed (and none may be installed where the tests run).
    static Clip inspect(const QString& path, int fps) {
        Clip clip;
        GError* error = nullptr;
        GstElement* pipeline = gst_parse_launch(QStringLiteral("filesrc location=\"%1\" ! matroskademux ! h264parse ! fakesink name=sink sync=false").arg(path).toUtf8().constData(), &error);
        if (!pipeline || error) { if (error) g_error_free(error); if (pipeline) gst_object_unref(pipeline); return clip; }
        std::vector<qint64> times;
        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        GstPad* pad = gst_element_get_static_pad(sink, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad*, GstPadProbeInfo* info, gpointer user) {
            GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
            if (buffer && GST_BUFFER_PTS_IS_VALID(buffer)) static_cast<std::vector<qint64>*>(user)->push_back(qint64(GST_BUFFER_PTS(buffer)));
            return GST_PAD_PROBE_OK;
        }, &times, nullptr);
        gst_object_unref(pad);
        gst_object_unref(sink);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstBus* bus = gst_element_get_bus(pipeline);
        GstMessage* message = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (message) gst_message_unref(message);
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        std::sort(times.begin(), times.end());
        clip.frames = int(times.size());
        if (times.size() < 2) return clip;
        const qint64 frame = GST_SECOND / fps;
        clip.durationNs = times.back() - times.front() + frame;
        clip.minGapNs = clip.maxGapNs = times[1] - times[0];
        for (size_t i = 1; i < times.size(); ++i) {
            const qint64 gap = times[i] - times[i - 1];
            clip.minGapNs = std::min(clip.minGapNs, gap);
            clip.maxGapNs = std::max(clip.maxGapNs, gap);
            // Off the frame grid by more than a quarter frame: a dropped or late frame.
            if (std::llabs(gap - frame) > frame / 4) ++clip.irregular;
        }
        return clip;
    }
private slots:
    void initTestCase() {
        gst_init(nullptr, nullptr);
        display_.start("Xvfb", {"-displayfd", "1", "-screen", "0", "960x600x24", "-nolisten", "tcp", "-ac"});
        QVERIFY2(display_.waitForStarted(), qPrintable(display_.errorString()));
        QVERIFY(display_.waitForReadyRead(5000));
        const auto number = display_.readAllStandardOutput().trimmed();
        bool numeric = false; number.toUInt(&numeric); QVERIFY(numeric);
        qputenv("DISPLAY", ":" + number);
        // Motion on the root at the display's pace: the recorder must keep up
        // with a moving picture, not only with a still one.
        GError* error = nullptr;
        pattern_ = gst_parse_launch("videotestsrc is-live=true pattern=ball motion=hsweep animation-mode=running-time ! video/x-raw,width=960,height=600,framerate=60/1 ! videoconvert ! ximagesink sync=true", &error);
        QVERIFY2(!error, error ? error->message : "");
        QVERIFY(pattern_);
        QVERIFY(gst_element_set_state(pattern_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    }
    void cleanupTestCase() {
        if (pattern_) { gst_element_set_state(pattern_, GST_STATE_NULL); gst_object_unref(pattern_); }
        display_.terminate();
        if (!display_.waitForFinished(3000)) { display_.kill(); display_.waitForFinished(); }
    }
    void sixtyFramesPerSecondAreCapturedAndKeptOnTheGrid() {
        QStringList encoders;
        for (const char* name : {"x264enc", "openh264enc"}) if (GstElementFactory* factory = gst_element_factory_find(name)) { encoders << name; gst_object_unref(factory); }
        if (encoders.isEmpty()) QSKIP("No CPU H.264 encoder is installed");
        QTemporaryDir dir;
        RecordingPipeline pipeline;
        RecordingPipeline::Options options;
        options.rect = QRect(0, 0, 640, 480);
        options.fps = 60;
        options.output = dir.filePath("working.mkv");
        options.encoders = encoders;
        QSignalSpy failed(&pipeline, &RecordingPipeline::failed);
        QVERIFY2(!pipeline.start(options), qPrintable(pipeline.start(options).message));
        QVERIFY(pipeline.running());
        putgd::test::settle(3000);
        QCOMPARE(failed.size(), 0);
        bool stopped = false; Error stopError; qint64 retained = 0;
        pipeline.stop([&](Error error, qint64 durationUs) { stopped = true; stopError = error; retained = durationUs; });
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(stopped, RecordingPipeline::stopDrainMs + 2000);
        QVERIFY2(!stopError, qPrintable(stopError.message));
        const qint64 encoded = pipeline.framesEncoded(), dropped = pipeline.framesDropped();
        // About three seconds at 60 fps: a 640x480 capture leaves no frame behind
        // on a loaded CI runner either, and the clip's timestamps stay on the grid.
        const Clip clip = inspect(options.output, 60);
        const QString summary = QStringLiteral("encoded %1, dropped %2, clip frames %3, duration %4 ms, gaps %5..%6 ms, irregular %7, retained %8 ms")
            .arg(encoded).arg(dropped).arg(clip.frames).arg(clip.durationNs / 1000000).arg(clip.minGapNs / 1000000.0, 0, 'f', 1).arg(clip.maxGapNs / 1000000.0, 0, 'f', 1).arg(clip.irregular).arg(retained / 1000);
        qInfo("%s", qPrintable(summary));
        QVERIFY2(encoded >= 120, qPrintable(summary));
        QVERIFY2(clip.frames >= encoded - 2 && clip.frames <= encoded, qPrintable(summary));
        QVERIFY2(double(clip.frames) / (double(clip.durationNs) / GST_SECOND) >= 50, qPrintable(summary));
        QVERIFY2(clip.irregular <= std::max(2, clip.frames / 20), qPrintable(summary));
        QVERIFY2(dropped <= encoded / 10, qPrintable(summary));
        QVERIFY2(std::llabs(retained * 1000 - clip.durationNs) <= 2 * GST_SECOND / 60, qPrintable(summary));
    }
};
}
QTEST_GUILESS_MAIN(putgd::record::RecordingPipelineThroughput)
#include "test_recording_pipeline.moc"
