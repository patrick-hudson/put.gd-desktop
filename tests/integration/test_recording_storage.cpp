#include "record/RecordSession.h"
#include "record/Overlays.h"
#include "fixtures/Waits.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <poll.h>
#include <unistd.h>

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
    // A pipe's write end reports POLLERR once its read end is really closed.
    static bool readerClosed(int writeEnd) { pollfd end{writeEnd, 0, 0}; return ::poll(&end, 1, 0) == 1 && (end.revents & POLLERR); }
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
    void stoppedBarCannotReappearOverReview() {
        QTemporaryDir dir; RecordSession session; setup(session, dir);
        session.phase_ = HelperPhase::Ready;
        session.bar_ = new ControlBar({}, session.config_);
        session.barHidden_ = false;
        session.pipeline_.storageProbe_ = [] { return healthy(); };
        QVERIFY(!start(session));
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(session.pipeline_.retainedUs() > 200000, 8000);
        session.showBar("recording"); QVERIFY(session.bar_->isVisible());
        // Stands in for the portal's PipeWire descriptor, the one part of the share visible without a compositor.
        int share[2];QCOMPARE(::pipe(share),0);session.portalFd_=share[0];
        auto keepWriteEnd=qScopeGuard([&]{::close(share[1]);});
        int replies=0;Error stopError;
        session.stop([&](Error error,QJsonObject){stopError=error;++replies;});
        // A second Stop is refused while the first drains, and must leave the share in use alone.
        QString busy;session.stop([&](Error error,QJsonObject){busy=error.code;});
        QCOMPARE(busy,QString("recording_busy"));QCOMPARE(session.portalFd_,share[0]);QVERIFY(!readerClosed(share[1]));
        PUTGD_TRY_COMPARE(replies,1);QVERIFY2(!stopError,qPrintable(stopError.message));
        QCOMPARE(session.portalFd_,-1);QVERIFY(readerClosed(share[1]));
        QVERIFY(session.phase_==HelperPhase::Recorded);QVERIFY(session.review_&&session.review_->isVisible());QVERIFY(!session.bar_->isVisible());
        session.review_->hide();
        session.command("raise",{},[&](Error error,QJsonObject){QVERIFY(!error);++replies;});
        QCOMPARE(replies,2);QVERIFY(session.review_->isVisible());QVERIFY(!session.bar_->isVisible());
        // The bar's overlay command belongs to the capture: over review it is refused and the bar stays hidden.
        session.command("overlay",{{"visible",true}},[](Error error,QJsonObject){QCOMPARE(error.code,QString("recording_state"));});QVERIFY(!session.bar_->isVisible());
        session.showBar("paused");QVERIFY(!session.bar_->isVisible());
    }
    void pipelineFailureEndsTheShare() {
        QTemporaryDir dir; RecordSession session; setup(session, dir);
        auto& pipeline = session.pipeline_;
        pipeline.storageProbe_ = [] { return healthy(); };
        int share[2]; QCOMPARE(::pipe(share), 0); session.portalFd_ = share[0];
        auto keepWriteEnd = qScopeGuard([&] { ::close(share[1]); });
        QSignalSpy events(&session, &RecordSession::event);
        const auto started = start(session); QVERIFY2(!started, qPrintable(started.message));
        QVERIFY(!readerClosed(share[1]));
        GError* error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, "synthetic media failure");
        gst_element_post_message(pipeline.pipeline_, gst_message_new_error(GST_OBJECT(pipeline.pipeline_), error, "synthetic"));
        g_error_free(error);
        PUTGD_TRY_COMPARE(terminal(events)["event"].toString(), QString("failed"));
        QVERIFY(!pipeline.running()); QCOMPARE(session.portalFd_, -1); QVERIFY(readerClosed(share[1]));
    }
    void automaticStop_data() {
        QTest::addColumn<bool>("paused");
        QTest::addColumn<bool>("byteLimit");
        QTest::addColumn<bool>("diskLow");
        QTest::newRow("recording-disk") << false << false << true;
        QTest::newRow("paused-disk") << true << false << true;
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
        QVERIFY(session.phase_ == HelperPhase::Recorded); QVERIFY(session.review_); QVERIFY(session.review_->isVisible());
        QVERIFY(QFileInfo(session.workingClip_).size() > 0);
        QImage frame;
        QVERIFY(!previewFrame(session.workingClip_, {}, session.config_, 0, 64, session.resolved_.canvas, frame));
        QVERIFY(!frame.isNull());
    }
};
}
QTEST_MAIN(putgd::record::RecordingStorageContract)
#include "test_recording_storage.moc"
