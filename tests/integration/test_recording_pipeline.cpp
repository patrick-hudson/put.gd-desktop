// The live pipeline on a private Xvfb root produces a clip with frames in it, and a
// refused PipeWire request is retried with the plain one. Throughput and cadence are
// measured by physical recording acceptance, not here.
#include "record/RecordingPipeline.h"
#include "fixtures/Waits.h"
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <gst/gst.h>

namespace putgd::record {
namespace {
// A stand-in encoder that refuses to start. A bin changes its sinks first, so the
// pipeline fails on it at NULL->READY before pipewiresrc is opened: no PipeWire
// connection is tried. When pipewiresrc cannot connect, its plugin destroys the
// PipeWire context while the context's loop thread still runs, then joins that
// thread; now and then the thread never wakes and the join waits forever (#1113:
// a 300 s timeout in CI, 4 of 200,000 failed opens on pipewire 1.6.2). This test
// needs no daemon, so it avoids that path; the product can still reach it when
// PipeWire goes away while a source starts or reopens (#1994).
GstElementClass* refusingParent = nullptr;
GstStaticPadTemplate refusingSink = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
GstStaticPadTemplate refusingSource = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-h264"));
GstStateChangeReturn refuseToStart(GstElement* element, GstStateChange transition) {
    if (transition == GST_STATE_CHANGE_NULL_TO_READY) return GST_STATE_CHANGE_FAILURE;
    return refusingParent->change_state(element, transition);
}
void refusingClassInit(gpointer klass, gpointer) {
    auto* element = GST_ELEMENT_CLASS(klass);
    refusingParent = GST_ELEMENT_CLASS(g_type_class_peek_parent(klass));
    gst_element_class_add_static_pad_template(element, &refusingSink);
    gst_element_class_add_static_pad_template(element, &refusingSource);
    gst_element_class_set_static_metadata(element, "Refusing encoder", "Codec/Encoder/Video", "Fails to start (test stand-in)", "put.gd");
    element->change_state = refuseToStart;
}
// Its pads link where an H.264 encoder's do: raw video in, H.264 out.
void refusingInstanceInit(GTypeInstance* instance, gpointer) {
    gst_element_add_pad(GST_ELEMENT(instance), gst_pad_new_from_static_template(&refusingSink, "sink"));
    gst_element_add_pad(GST_ELEMENT(instance), gst_pad_new_from_static_template(&refusingSource, "src"));
}
bool registerRefusingEncoder() {
    static const bool registered = [] {
        const GTypeInfo info{guint16(sizeof(GstElementClass)), nullptr, nullptr, refusingClassInit, nullptr, nullptr, guint16(sizeof(GstElement)), 0, refusingInstanceInit, nullptr};
        const GType type = g_type_register_static(GST_TYPE_ELEMENT, "PutgdRefusingEncoder", &info, GTypeFlags(0));
        return type && gst_element_register(nullptr, "putgdrefusingenc", GST_RANK_NONE, type);
    }();
    return registered;
}
}
class RecordingPipelineThroughput : public QObject {
    Q_OBJECT
    QProcess display_;
    GstElement* pattern_ = nullptr;
    QTemporaryDir logs_;
    // Counts the clip's access units; no decoder is needed (and none may be installed
    // where the tests run).
    static int frames(const QString& path) {
        GError* error = nullptr;
        GstElement* pipeline = gst_parse_launch(QStringLiteral("filesrc location=\"%1\" ! matroskademux ! h264parse ! fakesink name=sink sync=false").arg(path).toUtf8().constData(), &error);
        if (!pipeline || error) { if (error) g_error_free(error); if (pipeline) gst_object_unref(pipeline); return 0; }
        int count = 0;
        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        GstPad* pad = gst_element_get_static_pad(sink, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad*, GstPadProbeInfo*, gpointer user) {
            ++*static_cast<int*>(user);
            return GST_PAD_PROBE_OK;
        }, &count, nullptr);
        gst_object_unref(pad);
        gst_object_unref(sink);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstBus* bus = gst_element_get_bus(pipeline);
        GstMessage* message = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (message) gst_message_unref(message);
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return count;
    }
private slots:
    void initTestCase() {
        // Read once, by the first log line of the process.
        QVERIFY(logs_.isValid());
        qputenv("PUTGD_RECORD_DEBUG_LOG", logs_.filePath("record.log").toUtf8());
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
    void aRecordingProducesAClipWithFrames() {
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
        PUTGD_COMPARE_STEADY(failed.size(), 0, 1500);
        bool stopped = false; Error stopError; qint64 retained = 0;
        pipeline.stop([&](Error error, qint64 durationUs) { stopped = true; stopError = error; retained = durationUs; });
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(stopped, RecordingPipeline::stopDrainMs + 2000);
        QVERIFY2(!stopError, qPrintable(stopError.message));
        QVERIFY(pipeline.framesEncoded() > 0);
        QVERIFY(retained > 0);
        QVERIFY(frames(options.output) > 0);
    }
    // #1115: a compositor that refuses max_framerate 0/1 gets the plain request
    // for the same encoder. The stand-in encoder fails both attempts before any
    // PipeWire connection, and the log keeps what each one asked for.
    void aRefusedPipeWireRequestIsRetriedWithThePlainOne() {
        GstElementFactory* source = gst_element_factory_find("pipewiresrc");
        if (!source) QSKIP("No pipewiresrc is installed");
        gst_object_unref(source);
        QVERIFY(registerRefusingEncoder());
        QTemporaryDir dir;
        // Should the bin ever reach pipewiresrc, it finds no daemon, not the desktop's.
        qputenv("PIPEWIRE_RUNTIME_DIR", dir.path().toUtf8());
        qputenv("PIPEWIRE_REMOTE", "no-daemon");
        const QString log = logs_.filePath("record.log");
        QFile::remove(log);
        RecordingPipeline pipeline;
        RecordingPipeline::Options options;
        options.rect = QRect(0, 0, 640, 480);
        options.pipewireNode = 42;
        options.fps = 15;
        options.output = dir.filePath("working.mkv");
        options.encoders = {QStringLiteral("putgdrefusingenc")};
        const Error refused = pipeline.start(options);
        QCOMPARE(refused.code, QStringLiteral("recording_start_failed"));
        // Both attempts failed on the stand-in, not on the source.
        const QStringList attempts = refused.details.value("attempts").toVariant().toStringList();
        QCOMPARE(attempts.size(), 2);
        for (const auto& attempt : attempts) QVERIFY2(attempt.startsWith("putgdrefusingenc refused to start"), qPrintable(attempt));
        QFile written(log);
        QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
        QStringList requests;
        for (const auto& line : QString::fromUtf8(written.readAll()).split('\n')) if (line.contains("pipeline: ")) requests << line;
        QCOMPARE(requests.size(), 2);
        QVERIFY2(requests[0].contains("max-framerate=0/1"), qPrintable(requests[0]));
        QVERIFY2(!requests[1].contains("max-framerate"), qPrintable(requests[1]));
    }
};
}
QTEST_GUILESS_MAIN(putgd::record::RecordingPipelineThroughput)
#include "test_recording_pipeline.moc"
