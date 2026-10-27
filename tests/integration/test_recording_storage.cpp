#include "record/RecordSession.h"
#include "fixtures/Waits.h"
#include <QFileInfo>
#include <QLabel>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace putgd::record {
// Only storage values and prepared target metadata are injected. The production
// start, QTimer poll, budget signal, EOS threads, preview decode and review run.
// A private Xvfb root is the real ximagesrc input; this is not desktop evidence.
class RecordingStorageContract : public QObject {
    Q_OBJECT
    QProcess display_;
    GstElement* pattern_ = nullptr;
    static constexpr qint64 MiB = 1LL << 20;
    static RecordingStorage healthy() { return {1536 * MiB, 1536 * MiB, 1234}; }
    static RecordingStorage low() { return {1180 * MiB, 1536 * MiB, 2345}; }
    void setup(RecordSession& session, const QTemporaryDir& dir) {
        session.workingClip_ = dir.filePath("working.mkv");
        session.resolved_.rect = QRect(0, 0, 320, 240);
        session.resolved_.canvas = QSize(320, 240);
        session.config_.fps = 15;
        session.config_.format = "mp4";
        session.barHidden_ = true;
    }
    Error start(RecordSession& session) {
        Error result;
        session.begin([&](Error error, QJsonObject) { result = error; });
        return result;
    }
    static QJsonObject terminal(const QSignalSpy& events) {
        for (const auto& args : events) {
            const auto event = args[0].toJsonObject();
            if (event["event"] == "stopped" || event["event"] == "failed") return event;
        }
        return {};
    }
private slots:
    void initTestCase() {
        gst_init(nullptr, nullptr);
        // Xvfb's dynamically allocated /tmp/.X11-unix/X<n> socket is short;
        // this test never opens a putgd session socket or the user's display.
        display_.start("Xvfb", {"-displayfd", "1", "-screen", "0", "640x480x24", "-nolisten", "tcp", "-ac"});
        QVERIFY2(display_.waitForStarted(), qPrintable(display_.errorString()));
        QVERIFY(display_.waitForReadyRead(5000));
        const auto number = display_.readAllStandardOutput().trimmed();
        bool numeric = false; number.toUInt(&numeric); QVERIFY(numeric);
        qputenv("DISPLAY", ":" + number);
        GError* error = nullptr;
        pattern_ = gst_parse_launch("videotestsrc is-live=true pattern=snow ! video/x-raw,width=320,height=240,framerate=15/1 ! videoconvert ! ximagesink", &error);
        QVERIFY2(!error, error ? error->message : "");
        QVERIFY(pattern_);
        QVERIFY(gst_element_set_state(pattern_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    }
    void cleanupTestCase() {
        if (pattern_) { gst_element_set_state(pattern_, GST_STATE_NULL); gst_object_unref(pattern_); }
        display_.terminate();
        if (!display_.waitForFinished(3000)) { display_.kill(); display_.waitForFinished(); }
    }
    void capturedAudioAvailabilitySurvivesReviewConfigurationChanges_data() {
        QTest::addColumn<bool>("captured");
        QTest::newRow("captured") << true;
        QTest::newRow("not captured") << false;
    }
    void capturedAudioAvailabilitySurvivesReviewConfigurationChanges() {
        QFETCH(bool, captured);
        QTemporaryDir dir; RecordSession session; setup(session, dir);
        session.config_.codec = "h264"; session.config_.audio.system = true;
        session.audioCaptured_ = captured; session.recorded_ = true; session.durationUs_ = 12000000;
        session.capabilities_ = {{"formats", QJsonArray{"mp4", "gif"}}, {"codecs", QJsonObject{{"mp4", QJsonArray{"h264"}}}}};
        session.openReview();
        QVERIFY(session.review_->drive({{"action", "format"}, {"format", "gif"}}).code.isEmpty());
        QVERIFY(!session.config_.audio.system); QCOMPARE(session.audioCaptured_, captured);
        QVERIFY(session.review_->drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}}).code.isEmpty());
        QCOMPARE(session.config_.audio.system, captured); QCOMPARE(session.audioCaptured_, captured);
        QCOMPARE(session.review_->findChild<QLabel*>("exportAudioSummary")->text(), captured ? QString("Silent preview · Export includes system audio") : QString("Silent preview · No audio recorded"));
    }
    void automaticStop_data() {
        QTest::addColumn<bool>("paused");
        QTest::addColumn<bool>("byteLimit");
        QTest::addColumn<bool>("diskLow");
        QTest::newRow("recording-disk") << false << false << true;
        QTest::newRow("paused-disk") << true << false << true;
        QTest::newRow("simultaneous-byte-and-disk") << false << true << true;
        QTest::newRow("byte-only") << false << true << false;
    }
    void automaticStop() {
        QFETCH(bool, paused); QFETCH(bool, byteLimit); QFETCH(bool, diskLow);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        RecordSession session; setup(session, dir);
        auto& pipeline = session.pipeline_;
        RecordingStorage sample = healthy();
        int samples = 0;
        pipeline.storageProbe_ = [&] { ++samples; return sample; };
        QSignalSpy events(&session, &RecordSession::event);
        QSignalSpy budgets(&pipeline, &RecordingPipeline::budgetReached);
        const auto error = start(session); QVERIFY2(!error, qPrintable(error.message));
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(pipeline.retainedUs() > 200000 && pipeline.fileBytes() > 0 && samples >= 2, 8000);
        QVERIFY(terminal(events).isEmpty());
        if (paused) { QVERIFY(!pipeline.pause()); QVERIFY(pipeline.paused()); }
        // Healthy/recovered space must not stop a recording before the next low
        // sample. This happens through the real timer, not a direct poll call.
        const int before = samples;
        sample.freeBytes = 1200 * MiB;
        PUTGD_TRY_VERIFY(samples > before);
        sample = healthy();
        const int recovered = samples;
        PUTGD_TRY_VERIFY(samples > recovered);
        QVERIFY(terminal(events).isEmpty());
        auto expected = low().toJson(); expected["reason"] = "disk_space";
        // Recover as soon as the budget signal is emitted. Review must retain
        // the triggering numbers, not the later healthy sample.
        connect(&pipeline, &RecordingPipeline::budgetReached, this, [&](const QJsonObject&) { sample = healthy(); });
        sample = diskLow ? low() : healthy();
        if (byteLimit) {
            pipeline.budget_ = pipeline.fileBytes();
            // A fresh timer interval would normally defer sampling. The byte
            // limit must force a refresh and prefer the simultaneous disk cause.
            pipeline.storageClock_.start();
        }
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(!terminal(events).isEmpty(), 8000);
        const auto stopped = terminal(events);
        QCOMPARE(stopped["event"].toString(), QString("stopped"));
        QCOMPARE(stopped["reason"].toString(), QString("budget"));
        QCOMPARE(stopped["stopDetails"].toObject(), diskLow ? expected : QJsonObject{});
        QCOMPARE(budgets.count(), 1);
        QVERIFY(!pipeline.running()); QVERIFY(!pipeline.bus_.isActive());
        QVERIFY(session.recorded_); QVERIFY(session.review_); QVERIFY(session.review_->isVisible());
        QVERIFY(QFileInfo(session.workingClip_).size() > 0);
        QImage frame;
        QVERIFY(!previewFrame(session.workingClip_, {}, session.config_, 0, 64, session.resolved_.canvas, frame));
        QVERIFY(!frame.isNull());
    }
    void failureKeepsCause_data() {
        QTest::addColumn<int>("domainKind");
        QTest::addColumn<int>("code");
        QTest::addColumn<bool>("recover");
        QTest::addColumn<QString>("expectedCode");
        QTest::newRow("no-space-even-after-recovery") << 0 << int(GST_RESOURCE_ERROR_NO_SPACE_LEFT) << true << QString("recording_disk_full");
        QTest::newRow("write-with-low-sample") << 0 << int(GST_RESOURCE_ERROR_WRITE) << false << QString("recording_failed");
        QTest::newRow("write-after-recovery") << 0 << int(GST_RESOURCE_ERROR_WRITE) << true << QString("recording_failed");
        QTest::newRow("unrelated-stream-error-during-disk-drain") << 1 << int(GST_STREAM_ERROR_FAILED) << false << QString("recording_failed");
    }
    void failureKeepsCause() {
        QFETCH(int, domainKind); QFETCH(int, code); QFETCH(bool, recover); QFETCH(QString, expectedCode);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        RecordSession session; setup(session, dir);
        auto& pipeline = session.pipeline_;
        bool trigger = false, posted = false;
        const QByteArray diagnostic(800, 'd');
        pipeline.storageProbe_ = [&] {
            if (!trigger) return healthy();
            if (!posted) {
                posted = true;
                GError* error = g_error_new_literal(domainKind ? GST_STREAM_ERROR : GST_RESOURCE_ERROR, code, "synthetic media failure");
                gst_element_post_message(pipeline.pipeline_, gst_message_new_error(GST_OBJECT(pipeline.pipeline_), error, diagnostic.constData()));
                g_error_free(error);
                return low();
            }
            return recover ? healthy() : low();
        };
        QSignalSpy events(&session, &RecordSession::event);
        QSignalSpy budgets(&pipeline, &RecordingPipeline::budgetReached);
        const auto error = start(session); QVERIFY2(!error, qPrintable(error.message));
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(pipeline.retainedUs() > 200000, 8000);
        trigger = true;
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(!terminal(events).isEmpty(), 8000);
        const auto failed = terminal(events);
        QCOMPARE(failed["event"].toString(), QString("failed"));
        const auto result = failed["error"].toObject();
        QCOMPARE(result["code"].toString(), expectedCode);
        const auto details = result["details"].toObject();
        QCOMPARE(details["finalized"], QJsonValue(false));
        auto expected = low().toJson(); expected["reason"] = "disk_space";
        QCOMPARE(details["stopDetails"].toObject(), expected);
        const auto cause = expectedCode == "recording_disk_full" ? details["cause"].toObject() : result;
        QCOMPARE(cause["code"].toString(), QString("recording_failed"));
        QCOMPARE(cause["message"].toString(), QString("Capture failed: synthetic media failure"));
        QCOMPARE(cause["details"].toObject()["debug"].toString(), QString::fromLatin1(diagnostic.left(512)));
        if (expectedCode == "recording_disk_full") QCOMPARE(details["reason"].toString(), QString("disk_space"));
        else QVERIFY(!details.contains("reason"));
        QCOMPARE(budgets.count(), 1);
        QVERIFY(!session.recorded_); QVERIFY(!session.review_);
        QVERIFY(!pipeline.running()); QVERIFY(!pipeline.bus_.isActive());
    }
};
}
QTEST_MAIN(putgd::record::RecordingStorageContract)
#include "test_recording_storage.moc"
