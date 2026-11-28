// Scroll capture on X11 (#1376, #1515): the production backend starts the real
// putgd-record as a scroll session on a private Xvfb display against the
// fixture window, and the X server's raw input (XInput2) shows what the helper
// injects and when: nothing at all while the user scrolls; under auto-scroll
// wheel steps and pointer motion only, nothing after Done, Cancel or the end of
// its control pipe, and nothing once another client moved the pointer (#1514).
#include "app/ScrollHelperBackend.h"
#include "core/BuildInfo.h"
#include "fixtures/ScrollRuler.h"
#include "fixtures/Waits.h"
#include "ipc/UiControl.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <algorithm>
#include <memory>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

using namespace putgd;
namespace ruler = putgd::test::ruler;

namespace {
const QRect page(100, 60, 640, 600);
// A sanitizer build does the capture's work several times slower by design (#2099).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool sanitizerBuild = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
constexpr bool sanitizerBuild = true;
#else
constexpr bool sanitizerBuild = false;
#endif
#else
constexpr bool sanitizerBuild = false;
#endif
// A busy host: a thread that fell behind (the loop, or the source's grab or
// copy thread) waited for a CPU at least this share of the time the source
// ran at 60 frames a second. The waits come from the kernel's scheduler
// accounting (/proc/thread-self/schedstat), on in CI's and the lab's
// kernels; with it off they read 0, and a fallback there fails.
constexpr double busyHostWaitShare = 0.1;
// The CPU a frame the loop may take, in frame intervals at 60 a second. On an idle
// aurora-amd core (Ryzen 5 8500GE) the loop takes about 1 ms a frame
// optimized and 6 ms unoptimized (the dev preset), and a busy SMT sibling
// doubles that; a shared runner's slower core adds more. Unoptimized code
// gets twice the interval (#2099).
#ifdef __OPTIMIZE__
constexpr double cpuBudgetFrames = 1;
#else
constexpr double cpuBudgetFrames = 2;
#endif
QJsonArray rectJson(const QRect& r) { return {r.x(), r.y(), r.width(), r.height()}; }

// The fixture window, at `page`.
struct Fixture {
    QProcess process;
    QByteArray seen;
    bool start(QStringList arguments) {
        arguments.prepend(QStringLiteral("--geometry=%1,%2,%3,%4").arg(page.x()).arg(page.y()).arg(page.width()).arg(page.height()));
        process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        process.start(PUTGD_SCROLL_FIXTURE, arguments);
        return putgd::test::waitFor([&] { seen += process.readAllStandardOutput(); return seen.contains("ready "); }, 10000);
    }
    // A fixture started with --self-scroll=PX,MS,-1 begins scrolling now.
    void go() { process.write("go\n"); }
    // The page has settled at this scroll position.
    bool reached(int position) { seen += process.readAllStandardOutput(); return seen.contains(QByteArray("position ") + QByteArray::number(position) + "\n"); }
    ~Fixture() { process.kill(); process.waitForFinished(3000); }
};

// One capture through the production backend.
struct Capture {
    QTemporaryDir staging;
    ScrollHelperBackend backend{PUTGD_RECORD_HELPER};
    QList<QJsonObject> events;
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Error prepareError;
    QJsonObject prepared;
    bool answered = false;
    Capture() { backend.setEventSink([this](QJsonObject event) { events.append(event); }); }
    QJsonObject last(const QString& name) const {
        for (auto it = events.crbegin(); it != events.crend(); ++it) if ((*it)["event"].toString() == name) return *it;
        return {};
    }
    bool has(const QString& name) const { return !last(name).isEmpty(); }
    bool ended() const { return has("finished") || has("failed") || has("cancelled"); }
    void prepare(QJsonObject params) {
        if (!params.contains("selection")) params["selection"] = "helper";
        backend.prepare(id, params, staging.path(), [this](Error error, QJsonObject result) { prepareError = error; prepared = result; answered = true; });
    }
    struct Reply { Error error; QJsonObject result; bool done = false; };
    Reply control(const QString& command, const QJsonObject& params = {}) {
        auto reply = std::make_shared<Reply>();
        backend.control(id, command, params, [reply](Error error, QJsonObject result) { *reply = {error, result, true}; });
        (void)putgd::test::waitFor([&] { return reply->done; }, 40000);
        return *reply;
    }
    // The helper left with a normal exit; a finished PNG is still there for the resident.
    bool leave() {
        const QString png = last("finished")["path"].toString();
        if (!png.isEmpty() && !leaveKeepsPng(png)) return false;
        return left();
    }
    bool leaveKeepsPng(const QString& png) {
        if (!QFileInfo::exists(png)) return false;
        backend.discard(id);
        return putgd::test::waitFor([&] { return !backend.diagnostics()["active"].toBool(); }, 10000) && QFileInfo::exists(png);
    }
    bool left() {
        backend.discard(id);
        return putgd::test::waitFor([&] { return !backend.diagnostics()["active"].toBool(); }, 10000) && backend.diagnostics()["lastExit"].toObject()["status"].toString() == "normal" && backend.diagnostics()["lastExit"].toObject()["code"].toInt() == 0;
    }
};

// Everything the X server's input devices did, as XInput2 raw events on the
// root window: they reach every listener whatever window is under the pointer.
class RawInput {
public:
    int wheel = 0, otherButtons = 0, keys = 0, motions = 0;
    bool open() {
        connection_ = xcb_connect(nullptr, nullptr);
        if (!connection_ || xcb_connection_has_error(connection_)) return false;
        root_ = xcb_setup_roots_iterator(xcb_get_setup(connection_)).data->root;
        const auto* extension = xcb_get_extension_data(connection_, &xcb_input_id);
        if (!extension || !extension->present) return false;
        opcode_ = extension->major_opcode;
        std::unique_ptr<xcb_input_xi_query_version_reply_t, decltype(&free)> version(xcb_input_xi_query_version_reply(connection_, xcb_input_xi_query_version(connection_, 2, 2), nullptr), free);
        if (!version || version->major_version < 2 || (version->major_version == 2 && version->minor_version < 1)) return false;
        struct { xcb_input_event_mask_t head; uint32_t mask; } selection{{XCB_INPUT_DEVICE_ALL_MASTER, 1},
            XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_MOTION | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS};
        auto* error = xcb_request_check(connection_, xcb_input_xi_select_events_checked(connection_, root_, 1, &selection.head));
        if (error) { free(error); return false; }
        return true;
    }
    ~RawInput() { if (connection_) xcb_disconnect(connection_); }
    // Reads what has arrived; the totals are the members.
    void read() {
        // A round trip first: everything the server did before now is in the queue.
        free(xcb_get_input_focus_reply(connection_, xcb_get_input_focus(connection_), nullptr));
        while (auto* event = xcb_poll_for_event(connection_)) {
            const auto* generic = reinterpret_cast<xcb_ge_generic_event_t*>(event);
            if ((event->response_type & 0x7f) == XCB_GE_GENERIC && generic->extension == opcode_) {
                if (generic->event_type == XCB_INPUT_RAW_BUTTON_PRESS) {
                    const auto detail = reinterpret_cast<xcb_input_raw_button_press_event_t*>(event)->detail;
                    if (detail == 4 || detail == 5) ++wheel; else ++otherButtons;
                } else if (generic->event_type == XCB_INPUT_RAW_MOTION) ++motions;
                else if (generic->event_type == XCB_INPUT_RAW_KEY_PRESS) ++keys;
            }
            free(event);
        }
    }
    int total() { read(); return wheel + otherButtons + keys + motions; }
    QPoint pointer() {
        std::unique_ptr<xcb_query_pointer_reply_t, decltype(&free)> reply(xcb_query_pointer_reply(connection_, xcb_query_pointer(connection_, root_), nullptr), free);
        return reply ? QPoint(reply->root_x, reply->root_y) : QPoint(-1, -1);
    }
    void warp(QPoint to) {
        xcb_warp_pointer(connection_, XCB_NONE, root_, 0, 0, 0, 0, int16_t(to.x()), int16_t(to.y()));
        xcb_flush(connection_);
        total();
        wheel = otherButtons = keys = motions = 0;
    }
private:
    xcb_connection_t* connection_ = nullptr;
    xcb_window_t root_ = 0;
    uint8_t opcode_ = 0;
};
}

class ScrollCaptureX11 : public QObject {
    Q_OBJECT
    // The helper's debug log (#1535): a failure says how the session ended.
    QTemporaryDir logs_;
    QString log_;
    QString helperLog() const { return log_; }
    // An event (the result, or the last progress of a capture that never
    // ended), the helper's last "scroll: ended" line and every time auto-scroll
    // let go of the wheel, for a failure message.
    QByteArray endOf(const QJsonObject& event) const {
        QByteArray ended, letGo;
        QFile log(helperLog());
        if (log.open(QIODevice::ReadOnly))
            for (const QByteArray& line : log.readAll().split('\n')) {
                if (line.contains("scroll: ended") || line.contains("scroll: way to the top")) ended = line;
                else if (line.contains("handed the wheel back") || line.contains("auto-scroll paused by")) letGo += "\nhelper " + line;
            }
        return "event " + QJsonDocument(event).toJson(QJsonDocument::Compact) + "\nhelper " + ended + letGo;
    }
private slots:
    void initTestCase() {
        // A log asked for from outside (a lab loop) is kept; otherwise one of the test's own.
        log_ = qEnvironmentVariable("PUTGD_RECORD_DEBUG_LOG");
        if (log_.isEmpty()) {
            QVERIFY(logs_.isValid());
            log_ = logs_.filePath("helper.log");
            qputenv("PUTGD_RECORD_DEBUG_LOG", QFile::encodeName(log_));
        }
    }
    // Auto-scroll: what is injected, and when: wheel steps and pointer motion
    // only, only while capturing, nothing after the Done or Cancel reply, and
    // the pointer back where it was.
    void onlyWheelAndMotionAndOnlyWhileCapturing() {
        RawInput raw;
        QVERIFY2(raw.open(), "the X server offers no XInput 2.1 raw events");
        const QPoint home(900, 700);
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=600", "--notch=100"}));
        raw.warp(home);
        Capture capture;
        capture.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", true}, {"autoStart", false}, {"driver", "putgd"}});
        QVERIFY(putgd::test::waitFor([&] { return capture.answered; }, 20000));
        QVERIFY2(!capture.prepareError, qPrintable(capture.prepareError.message));
        QCOMPARE(capture.prepared["state"].toString(), QString("ready"));
        // Ready, ring and bar on screen: nothing has been injected.
        PUTGD_COMPARE_STEADY(raw.total(), 0, 600);
        QCOMPARE(raw.pointer(), home);
        const auto started = capture.control("start");
        QVERIFY2(!started.error, qPrintable(started.error.message));
        QVERIFY(putgd::test::waitFor([&] { return capture.last("progress")["height"].toInt() > 1200 || capture.ended(); }, 60000));
        raw.read();
        QVERIFY(raw.wheel > 0);
        QVERIFY(raw.motions > 0);
        // #1514: another client moves the pointer, as a user's mouse does.
        // Auto-scroll pauses, says why, hands the page to the user and sends
        // nothing until it is turned on again; the pointer stays where the
        // user put it.
        // A warp gives no raw event, so the helper sees it only by asking the
        // server where the pointer is, which it does before each motion of
        // its own. A warp that the server takes between that question and the
        // motion is overwritten unseen; a starved server takes all three in
        // one go (#1535). A hand goes on moving, and so does this one until
        // the capture says it has paused.
        QVERIFY(!capture.ended());
        {
            const QPoint user(1000, 300);
            QVERIFY(putgd::test::waitFor([&] {
                if (capture.last("progress")["state"].toString() == "paused") return true;
                raw.warp(user);
                return false;
            }, 3000));
            QCOMPARE(capture.last("progress")["pausedBy"].toString(), QString("pointer"));
            QCOMPARE(capture.last("progress")["driver"].toString(), QString("user"));
            const int atPause = raw.total();
            QVERIFY(atPause <= 1);
            PUTGD_COMPARE_STEADY(raw.total(), atPause, 1000);
            QCOMPARE(raw.pointer(), user);
            const auto resumed = capture.control("autoScroll", {{"on", true}});
            QVERIFY2(!resumed.error, qPrintable(resumed.error.message));
            QVERIFY(putgd::test::waitFor([&] { return raw.total() > atPause + 2; }, 10000));
            QVERIFY(!capture.ended());
        }
        const auto stopped = capture.control("stop");
        QVERIFY2(!stopped.error, qPrintable(stopped.error.message));
        // From the stop reply on: not one more event, and the pointer is back.
        const int afterStop = raw.total();
        PUTGD_COMPARE_STEADY(raw.total(), afterStop, 800);
        QCOMPARE(raw.pointer(), home);
        QCOMPARE(raw.otherButtons, 0);
        QCOMPARE(raw.keys, 0);
        QVERIFY(capture.leave());
        QCOMPARE(raw.total(), afterStop);

        // Cancel: the same bound.
        Capture second;
        second.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}, {"driver", "putgd"}});
        QVERIFY(putgd::test::waitFor([&] { return second.last("progress")["height"].toInt() > 1000 || second.ended(); }, 60000));
        QVERIFY2(!second.control("cancel").error, "cancel failed");
        const int afterCancel = raw.total();
        QVERIFY(afterCancel > afterStop);
        PUTGD_COMPARE_STEADY(raw.total(), afterCancel, 800);
        QCOMPARE(raw.pointer(), home);
        QVERIFY(second.leave());
    }
    // #1515: the user scrolls (the page moves by itself here, as under a
    // person's wheel): not one event is injected for the whole capture, the
    // image follows the page, and Done keeps it whole and exact.
    void theUserScrollsAndNothingIsInjected() {
        RawInput raw;
        QVERIFY(raw.open());
        const QPoint home(900, 700);
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=60", "--self-scroll=37,60,-1"}));
        raw.warp(home);
        Capture capture;
        capture.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"autoStart", true}, {"driver", "user"}});
        QVERIFY(putgd::test::waitFor([&] { return capture.answered; }, 20000));
        QVERIFY2(!capture.prepareError, qPrintable(capture.prepareError.message));
        // The page starts to move only once the capture has its first frame,
        // however long the helper took to start (CodeRabbit on #1528).
        QVERIFY(putgd::test::waitFor([&] { return capture.has("progress") || capture.ended(); }, 20000));
        fixture.go();
        // To the page's end: 60 rows of 40 lines in a 600-line window.
        QVERIFY(putgd::test::waitFor([&] { return fixture.reached(60 * ruler::rowHeight - page.height()) || capture.ended(); }, 30000));
        QVERIFY(putgd::test::waitFor([&] { return capture.last("progress")["height"].toInt() >= 60 * ruler::rowHeight || capture.ended(); }, 5000));
        QCOMPARE(capture.last("progress")["driver"].toString(), QString("user"));
        const auto done = capture.control("stop");
        QVERIFY2(!done.error, qPrintable(done.error.message));
        QCOMPARE(raw.total(), 0);
        QCOMPARE(raw.pointer(), home);
        QCOMPARE(done.result["endReason"].toString(), QString("done"));
        QCOMPARE(done.result["truncated"].toBool(true), false);
        QCOMPARE(done.result["scrolledBy"].toString(), QString("user"));
        QCOMPARE(done.result["framesSkipped"].toInt(), 0);
        // #2064: this capture's own source rate, from its result. It asked for
        // 60 frames a second. Kept up, it is still at 60 at the end with 60
        // delivered (within the four fifths below which the tap goes down).
        // It goes down to 30 when the loop or the source cannot keep 60 up
        // (#2099, #1535). That is right on a sanitizer build, whose work is
        // several times slower by design. On any other build it is right
        // only on a busy host: the thread that fell behind needed no more
        // CPU a frame than its budget, the loop's own times account for the
        // frames it missed, and the helper's threads were kept waiting for a
        // CPU. A change that made the capture's work too heavy for 60, that
        // counted frames as missed that were not, or that fell back on an
        // idle host fails here.
        const QJsonObject rate = done.result["stats"].toObject()["sourceRate"].toObject();
        const QByteArray rateText = QJsonDocument(rate).toJson(QJsonDocument::Compact);
        qInfo("source rate: %s", rateText.constData());
        const double delivered = rate["delivered"].toDouble();
        QVERIFY2(rate["asked"].toInt() == 60, rateText.constData());
        if (!rate.contains("fallback")) {
            QVERIFY2(rate["atEnd"].toInt() == 60, rateText.constData());
            QVERIFY2(delivered >= 48 && delivered <= 62, rateText.constData());
        } else {
            const QString why = rate["fallback"].toString();
            const auto source = QRegularExpression(QStringLiteral("^the source sent (\\d+\\.\\d) frames a second$")).match(why);
            const auto loop = QRegularExpression(QStringLiteral("^the loop missed (\\d+)% of the frames$")).match(why);
            QVERIFY2(rate["atEnd"].toInt() == 30, rateText.constData());
            // The reason's rate is rounded to a tenth: a fall at 47.95 reads 48.0.
            QVERIFY2((source.hasMatch() && source.captured(1).toDouble() <= 48) ||
                         (loop.hasMatch() && loop.captured(1).toInt() >= 25 && loop.captured(1).toInt() <= 100),
                     rateText.constData());
            // Over both rates: up to 60 a second, and some delivered.
            QVERIFY2(delivered > 0 && delivered <= 62, rateText.constData());
            if (!sanitizerBuild) {
                const QJsonObject at = rate["atAsked"].toObject();
                const QString text = QString::fromUtf8(rateText);
                QVERIFY2(at.contains("loopCpuMsPerFrame"), qPrintable("no thread times, so a busy host cannot be told from the capture's own work: " + text));
                const double interval = 1000.0 / 60;
                // The threads that fell behind: the loop, or the source's
                // grab and copy threads. Each is judged on its own CPU and wait.
                const QStringList parts = loop.hasMatch() ? QStringList{"loop"} : QStringList{"grab", "copy"};
                double waited = 0;
                for (const QString& part : parts) {
                    const double cpu = at[part + "CpuMsPerFrame"].toDouble();
                    // The grab and the copy run GStreamer's own optimized
                    // code and a row copy, under 1 ms a frame in any build:
                    // one interval whatever this build's optimization.
                    const double budget = part == "loop" ? cpuBudgetFrames * interval : interval;
                    QVERIFY2(cpu < budget, qPrintable(QStringLiteral("the %1 needs %2 ms of CPU a frame, too much for 60 frames a second: %3").arg(part).arg(cpu).arg(text)));
                    waited = std::max(waited, at[part + "WaitedMs"].toDouble());
                }
                if (loop.hasMatch()) {
                    // A missed frame is a frame interval the loop was away
                    // from the mailbox: its CPU time and its wait for a CPU say it was.
                    const double away = at["loopCpuMsPerFrame"].toDouble() + at["loopWaitedMs"].toDouble() / std::max(1.0, at["framesTaken"].toDouble());
                    QVERIFY2(away >= 0.6 * interval, qPrintable(QStringLiteral("the loop was away %1 ms a frame, too little to miss a quarter of the frames: %2").arg(away).arg(text)));
                }
                QVERIFY2(waited >= busyHostWaitShare * at["ms"].toDouble(), qPrintable("it went down to 30 while the threads that fell behind never waited for a CPU: " + text));
            }
        }
        const QImage image(done.result["path"].toString());
        const auto verdict = ruler::verify(image, 0, 0, 0, 59);
        QVERIFY2(verdict.problem.isEmpty(), qPrintable(verdict.problem));
        QVERIFY(capture.leave());
    }
    // Auto-scroll left alone: it turns the page to its end and ends the
    // capture there by itself, end_of_content with every row exact.
    void autoScrollRunsToTheEnd() {
        RawInput raw;
        QVERIFY(raw.open());
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=60", "--notch=100", "--smooth"}));
        raw.warp(QPoint(900, 700));
        Capture capture;
        capture.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"autoStart", true}, {"driver", "putgd"}});
        QVERIFY2(putgd::test::waitFor([&] { return capture.ended(); }, 90000), endOf(capture.last("progress")).constData());
        const auto finished = capture.last("finished");
        QVERIFY2(!finished.isEmpty(), qPrintable(QJsonDocument(capture.last("failed")).toJson(QJsonDocument::Compact)));
        const QImage image(finished["path"].toString());
        const auto verdict = ruler::verify(image, 0, 0, 0, 59);
        QVERIFY2(verdict.problem.isEmpty(), qPrintable(verdict.problem + "\n" + QString::fromUtf8(endOf(finished))));
        // end_of_content promises that no frame was skipped. On a starved
        // runner the tracker may skip a frame it cannot place among the
        // ruler's look-alike rows and find the page in the next, so the
        // helper ends `done` with reachedEnd and frames skipped. That is
        // right too, and the image is exact (checked above). Seen 10 times
        // in 40 runs on one shared core (#1535).
        const QString reason = finished["endReason"].toString();
        QVERIFY2(reason == "end_of_content" || (reason == "done" && finished["reachedEnd"].toBool() && finished["framesSkipped"].toInt() > 0), endOf(finished).constData());
        QCOMPARE(finished["reachedEnd"].toBool(), true);
        QCOMPARE(finished["scrolledBy"].toString(), QString("putgd"));
        raw.read();
        QCOMPARE(raw.otherButtons, 0);
        QCOMPARE(raw.keys, 0);
        QVERIFY(capture.leave());
    }
    // The control pipe closes while the helper captures: it stops injecting at
    // once, does not even put the pointer back, removes its files and leaves.
    void theEndOfTheControlPipeEndsInjection() {
        RawInput raw;
        QVERIFY(raw.open());
        const QPoint home(900, 700);
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=600", "--notch=100"}));
        raw.warp(home);
        QTemporaryDir staging;
        QProcess helper;
        helper.setProcessChannelMode(QProcess::SeparateChannels);
        UiControl control(helper);
        const QJsonObject envelope{{"role", "record"}, {"nonce", "n"}, {"generation", "1"}};
        bool ready = false, capturing = false;
        int height = 0;
        connect(&control, &UiControl::frame, this, [&](const QString& type, const QString&, const QJsonObject& payload) {
            if (type == "ready") ready = true;
            if (type == "commandResult") capturing = payload["result"].toObject()["state"].toString() == "capturing";
            if (type == "event" && payload["event"].toString() == "progress") height = payload["height"].toInt();
        });
        helper.start(PUTGD_RECORD_HELPER, {});
        QVERIFY(helper.waitForStarted(5000));
        auto bootstrap = envelope;
        bootstrap["build"] = QJsonDocument::fromJson(buildManifestJson()).object();
        bootstrap["appearance"] = QJsonObject{{"scheme", "dark"}};
        bootstrap["session"] = "scroll";
        control.send("bootstrap", bootstrap);
        QVERIFY(putgd::test::waitFor([&] { return ready; }, 15000));
        auto prepare = envelope;
        prepare["requestId"] = "prepare";
        prepare["command"] = "prepare";
        prepare["params"] = QJsonObject{{"id", "eof"}, {"staging", staging.path()}, {"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}, {"selection", "helper"}, {"driver", "putgd"}};
        control.send("command", prepare);
        QVERIFY(putgd::test::waitFor([&] { return capturing && height > 1000; }, 60000));
        QVERIFY(raw.pointer() != home);
        helper.closeWriteChannel();
        QVERIFY(putgd::test::waitFor([&] { return helper.state() == QProcess::NotRunning; }, 10000));
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), 5);
        const int afterExit = raw.total();
        PUTGD_COMPARE_STEADY(raw.total(), afterExit, 800);
        // Not even the restore: the pointer stays where the capture parked it.
        QVERIFY(raw.pointer() != home);
        QCOMPARE(raw.otherButtons, 0);
        QCOMPARE(raw.keys, 0);
        QVERIFY(QDir(staging.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }
};

QTEST_GUILESS_MAIN(ScrollCaptureX11)
#include "test_scroll_capture_x11.moc"
