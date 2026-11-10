#include "record/RecordSession.h"
#include "record/Overlays.h"
#include "fixtures/Waits.h"
#include <QCheckBox>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
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
        session.bar_ = new ControlBar({},session.config_);session.bar_->show();
        session.capabilities_ = {{"formats", QJsonArray{"mp4", "gif"}}, {"codecs", QJsonObject{{"mp4", QJsonArray{"h264"}}}}};
        session.openReview();QVERIFY(!session.bar_->isVisible());
        QVERIFY(session.review_->drive({{"action", "format"}, {"format", "gif"}}).code.isEmpty());
        QVERIFY(!session.config_.audio.system); QCOMPARE(session.audioCaptured_, captured);
        QCOMPARE(session.config_.toJson()["format"].toString(), QString("gif"));
        QVERIFY(!session.config_.toJson()["audio"].toObject()["system"].toBool());
        QVERIFY(!session.review_->inspect()["audioEffective"].toBool());
        QVERIFY(session.review_->drive({{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}}).code.isEmpty());
        QCOMPARE(session.config_.audio.system, captured); QCOMPARE(session.audioCaptured_, captured);
        const auto config = session.config_.toJson();
        QCOMPARE(config["format"].toString(), QString("mp4"));
        QCOMPARE(config["codec"].toString(), QString("h264"));
        QCOMPARE(config["audio"].toObject()["system"].toBool(), captured);
        QCOMPARE(session.review_->inspect()["audioEffective"].toBool(), captured);
        QCOMPARE(session.review_->inspect()["format"].toObject(), (QJsonObject{{"format", "mp4"}, {"codec", "h264"}}));
        const auto audio = session.review_->findChild<QLabel*>("exportAudioSummary")->text();
        QVERIFY(audio.contains(captured ? QString("System audio") : QString("No audio recorded")));
    }
    // #1364: the production session at Ready follows the sound server's inputs. Only the list of inputs
    // is injected; the session, its bar and its events are the real ones.
    void theReadySessionFollowsMicrophones() {
        QTemporaryDir dir; RecordSession session; setup(session, dir);
        // As on a desktop with nothing plugged in: an onboard input that is not the default, and a default
        // input that is an output's monitor.
        const Microphone onboard{"alsa_input.pci-test.analog-stereo", "Built-in Audio", false};
        MicrophoneList inputs;
        inputs.server = true; inputs.devices = {onboard}; inputs.defaultSource = "alsa_output.test.analog-stereo.monitor"; inputs.defaultIsMonitor = true;
        session.listMicrophones_ = [&inputs] { return inputs; };
        // A window target from a portal: the bar is made and placed without asking an X server for the target.
        session.portalTarget_ = true; session.target_.kind = "window"; session.prepared_ = true;
        session.capabilities_ = QJsonObject{{"formats", QJsonArray{"mp4"}}, {"audio", QJsonObject{{"system", false}, {"microphone", true}}}};
        session.microphone_ = chooseMicrophone(inputs, {});
        QSignalSpy events(&session, &RecordSession::event);
        const auto last = [&](const QString& kind) { for (int i = events.size() - 1; i >= 0; --i) { const auto event = events[i][0].toJsonObject(); if (event["event"] == kind) return event; } return QJsonObject{}; };
        Error prepared{"unanswered", "", {}};
        session.finishPrepare([&](Error error, QJsonObject) { prepared = error; });
        QVERIFY2(!prepared, qPrintable(prepared.message));
        QVERIFY(session.bar_);
        // Ready without a microphone: the default input is an output's monitor.
        QVERIFY(session.microphoneWatch_.isActive());
        QCOMPARE(session.bar_->microphoneState()["available"].toBool(), false);
        QVERIFY(session.bar_->microphoneState()["unavailable"].toString().contains("monitor"));
        // A microphone is plugged in and becomes the default input. The next look finds it.
        inputs.devices = {onboard, {"alsa_input.usb-Test_Microphone-00.mono-fallback", "Test Microphone", true}};
        inputs.defaultSource = inputs.devices.last().id; inputs.defaultIsMonitor = false;
        const int before = events.size();
        session.followMicrophones();
        QVERIFY(events.size() > before);
        auto state = last("progress")["recordingMicrophone"].toObject();
        QCOMPARE(state, QJsonObject({{"requested", false}, {"effective", false}, {"available", true}, {"device", "Test Microphone"}}));
        QCOMPARE(session.bar_->findChild<QCheckBox*>("microphone")->text(), QString("Microphone"));
        QVERIFY(session.bar_->findChild<QCheckBox*>("microphone")->isEnabled());
        // Nothing new: no event.
        const int quiet = events.size();
        session.followMicrophones();
        QCOMPARE(events.size(), quiet);
        // It is switched on without starting over (recording.audio): this session's choice, not one to remember.
        Error set{"unanswered", "", {}}; QJsonObject answer;
        session.command("audio", {{"microphone", true}}, [&](Error error, QJsonObject result) { set = error; answer = result; });
        QVERIFY2(!set, qPrintable(set.message));
        QVERIFY(answer["audio"].toObject()["microphone"].toBool() && answer["microphone"].toObject()["effective"].toBool());
        QVERIFY(session.config_.audio.microphone);
        QVERIFY(last("configChanged")["config"].toObject()["audio"].toObject()["microphone"].toBool());
        QVERIFY(!last("configChanged").contains("microphoneRequested"));
        // It is pulled: the bar says so, nothing of it will be recorded, the choice is kept.
        inputs.devices = {onboard}; inputs.defaultSource = "alsa_output.test.analog-stereo.monitor"; inputs.defaultIsMonitor = true;
        session.followMicrophones();
        QVERIFY(!session.config_.audio.microphone);
        QVERIFY(!last("configChanged")["config"].toObject()["audio"].toObject()["microphone"].toBool());
        state = last("progress")["recordingMicrophone"].toObject();
        QVERIFY(state["requested"].toBool() && !state["effective"].toBool() && !state["available"].toBool());
        QCOMPARE(session.bar_->findChild<QCheckBox*>("microphone")->text(), QString("No microphone"));
        // Asking for it now is refused with the reason, and looks first: one plugged in this instant counts.
        session.command("audio", {{"microphone", true}}, [&](Error error, QJsonObject) { set = error; });
        QCOMPARE(set.code, QString("recording_unsupported"));
        QCOMPARE(set.details["reason"].toString(), QString("default_is_monitor"));
        inputs.devices = {onboard, {"alsa_input.usb-Headset-00.mono-fallback", "Headset", true}};
        inputs.defaultSource = inputs.devices.last().id; inputs.defaultIsMonitor = false;
        session.command("audio", {{"microphone", true}}, [&](Error error, QJsonObject result) { set = error; answer = result; });
        QVERIFY2(!set, qPrintable(set.message));
        QCOMPARE(answer["microphone"].toObject()["device"].toString(), QString("Headset"));
        QCOMPARE(session.microphone_.device.id, QString("alsa_input.usb-Headset-00.mono-fallback"));
        // An output's monitor is never the microphone, however the server lists it.
        inputs.devices = {onboard}; inputs.defaultSource = "bluez_output.headset.monitor"; inputs.defaultIsMonitor = true;
        session.followMicrophones();
        QVERIFY(!session.microphone_.available() && !session.config_.audio.microphone);
        QCOMPARE(session.microphone_.reason, QString("default_is_monitor"));
        // Cancelled: the session stops looking.
        session.cancelLocal();
        session.followMicrophones();
        QVERIFY(!session.microphoneWatch_.isActive());
    }
    void stoppedBarCannotReappearOverReview() {
        QTemporaryDir dir; RecordSession session; setup(session, dir);
        session.prepared_ = true;
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
        QVERIFY(session.recorded_);QVERIFY(session.review_&&session.review_->isVisible());QVERIFY(!session.bar_->isVisible());
        session.review_->hide();
        session.command("raise",{},[&](Error error,QJsonObject){QVERIFY(!error);++replies;});
        QCOMPARE(replies,2);QVERIFY(session.review_->isVisible());QVERIFY(!session.bar_->isVisible());
        session.command("overlay",{{"visible",true}},[](Error error,QJsonObject result){QVERIFY(!error);QVERIFY(!result["visible"].toBool());});
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
