// Scroll capture end to end on X11 (#1376): the production backend starts the
// real putgd-record as a scroll session on a private Xvfb display, the helper
// reads the fixture window through ximagesrc and turns its wheel through XTest,
// and the PNG it writes must decode to every line of the bar-coded page once,
// in order. The last cases watch the X server's raw input (XInput2) for what
// the helper injects and when. Xvfb has no compositor and no window manager:
// this proves the helper's own path, never a desktop.
#include "app/ScrollHelperBackend.h"
#include "core/BuildInfo.h"
#include "fixtures/ScrollRuler.h"
#include "fixtures/Waits.h"
#include "ipc/UiControl.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <memory>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xinput.h>
#include <xcb/xtest.h>

using namespace putgd;
namespace ruler = putgd::test::ruler;

namespace {
const QRect page(100, 60, 640, 600);
QJsonArray rectJson(const QRect& r) { return {r.x(), r.y(), r.width(), r.height()}; }

// The fixture window, at `page`.
struct Fixture {
    QProcess process;
    bool start(QStringList arguments) {
        arguments.prepend(QStringLiteral("--geometry=%1,%2,%3,%4").arg(page.x()).arg(page.y()).arg(page.width()).arg(page.height()));
        process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        process.start(PUTGD_SCROLL_FIXTURE, arguments);
        QByteArray seen;
        return putgd::test::waitFor([&] { seen += process.readAllStandardOutput(); return seen.contains("ready "); }, 10000);
    }
    // Where the page is: the fixture prints `position N` whenever its scroll position settles.
    int position() {
        printed += process.readAllStandardOutput();
        const int at = printed.lastIndexOf("position ");
        return at < 0 ? -1 : printed.mid(at + 9, printed.indexOf('\n', at) - at - 9).trimmed().toInt();
    }
    QByteArray printed;
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
    QStringList files() const { return QDir(staging.path()).entryList(QDir::Files | QDir::Hidden); }
};

// The finished descriptor against the file it names; the image, or a null one.
QImage admitted(const QJsonObject& finished, QString* problem) {
    const QString path = finished["path"].toString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *problem = "the PNG cannot be opened"; return {}; }
    const QByteArray bytes = file.readAll();
    if (bytes.size() != qint64(finished["bytes"].toDouble())) { *problem = "the size differs from the descriptor"; return {}; }
    if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != finished["sha256"].toString()) { *problem = "the hash differs from the descriptor"; return {}; }
    if ((QFileInfo(path).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)) != 0) { *problem = "the PNG is readable by others"; return {}; }
    QImage image;
    if (!image.loadFromData(bytes, "PNG")) { *problem = "the PNG does not decode"; return {}; }
    if (image.width() != finished["width"].toInt() || image.height() != finished["height"].toInt()) { *problem = "the image size differs from the descriptor"; return {}; }
    return image;
}

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
private:
    // Starts a capture that begins by itself and waits for its end.
    void run(Capture& capture, const QJsonObject& params) {
        capture.prepare(params);
        QVERIFY(putgd::test::waitFor([&] { return capture.answered; }, 20000));
        QVERIFY2(!capture.prepareError, qPrintable(capture.prepareError.code + ": " + capture.prepareError.message));
        QCOMPARE(capture.prepared["state"].toString(), QString("capturing"));
        QVERIFY(putgd::test::waitFor([&] { return capture.ended(); }, 90000));
    }
    // The finished image is the page's rows firstRow..lastRow, every line once.
    void exact(Capture& capture, int header, int footer, int firstRow, int lastRow, int lastLine = ruler::rowHeight - 1) {
        const auto finished = capture.last("finished");
        QVERIFY2(!finished.isEmpty(), qPrintable("no finished event; failed: " + QString::fromUtf8(QJsonDocument(capture.last("failed")).toJson(QJsonDocument::Compact))));
        QString problem;
        const QImage image = admitted(finished, &problem);
        QVERIFY2(!image.isNull(), qPrintable(problem));
        QCOMPARE(image.width(), page.width());
        const auto verdict = ruler::verify(image, header, footer, firstRow, lastRow, 0, lastLine);
        QVERIFY2(verdict.ok, qPrintable(verdict.problem));
        // The spool is gone: the staging directory holds the PNG and nothing else.
        QCOMPARE(capture.files(), QStringList{"scroll.png"});
    }
private slots:
    void theProbeNamesTheX11Path() {
        ScrollHelperBackend backend(PUTGD_RECORD_HELPER);
        QCOMPARE(backend.capabilities()["reason"].toString(), QString("scroll_not_probed"));
        bool done = false;
        backend.refreshCapabilities([&] { done = true; });
        QVERIFY(putgd::test::waitFor([&] { return done; }, 15000));
        const auto capabilities = backend.capabilities();
        QVERIFY2(capabilities["available"].toBool(), qPrintable(QString::fromUtf8(QJsonDocument(capabilities).toJson(QJsonDocument::Compact))));
        QCOMPARE(capabilities["frames"].toString(), QString("ximagesrc"));
        QCOMPARE(capabilities["injector"].toString(), QString("xtest"));
        QCOMPARE(capabilities["selection"].toString(), QString("helper"));
        QCOMPARE(capabilities["maxHeight"].toInt(), 60000);
        ScrollHelperBackend missing("/nonexistent/putgd-record");
        QCOMPARE(missing.capabilities()["reason"].toString(), QString("recording_helper_missing"));
    }
    void aPlainPageComesOutEveryRowOnceInOrder() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=60", "--notch=100"}));
        Capture capture;
        run(capture, {{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
        exact(capture, 0, 0, 0, 59);
        const auto finished = capture.last("finished");
        QCOMPARE(finished["endReason"].toString(), QString("end_of_content"));
        QCOMPARE(finished["truncated"].toBool(), false);
        QCOMPARE(finished["height"].toInt(), 60 * ruler::rowHeight);
        QCOMPARE(finished["appliedMaxHeight"].toInt(), 30000);
        const auto stats = finished["stats"].toObject();
        QCOMPARE(stats["pixelsPerNotch"].toDouble(), 100.0);
        QCOMPARE(stats["lowConfidenceSteps"].toInt(), 0);
        QVERIFY(stats["steps"].toInt() >= 4);
        // Progress said what the capture was doing, with a growing height.
        const auto progress = capture.last("progress");
        QCOMPARE(progress["state"].toString(), QString("capturing"));
        QVERIFY(progress["height"].toInt() > page.height());
        // A second stop after the end answers with the same descriptor.
        auto again = capture.control("stop");
        QVERIFY2(!again.error, qPrintable(again.error.message));
        QCOMPARE(again.result["sha256"].toString(), finished["sha256"].toString());
        QVERIFY(capture.leave());
    }
    void aStickyHeaderAndFooterComeOutOnce() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=60", "--notch=80", "--header=48", "--footer=32", "--smooth"}));
        Capture capture;
        run(capture, {{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
        exact(capture, 48, 32, 0, 59);
        QCOMPARE(capture.last("finished")["endReason"].toString(), QString("end_of_content"));
        QVERIFY(capture.leave());
    }
    void aPageOpenedHalfwayDownStartsAtRowZeroWithFromTop() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=60", "--notch=100", "--start=30"}));
        Capture capture;
        run(capture, {{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", true}, {"autoStart", true}});
        exact(capture, 0, 0, 0, 59);
        QCOMPARE(capture.last("finished")["endReason"].toString(), QString("end_of_content"));
        // The step before the capture was reported as its own state.
        bool sawToTop = false;
        for (const auto& event : capture.events) sawToTop = sawToTop || event["state"].toString() == "scrollingToTop";
        QVERIFY(sawToTop);
        QVERIFY(capture.leave());
    }
    void withoutFromTopItStartsAtTheRowOnScreen() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=60", "--notch=100", "--start=20"}));
        Capture capture;
        run(capture, {{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
        exact(capture, 0, 0, 20, 59);
        QVERIFY(capture.leave());
    }
    void aTailThatLoadsLateIsStillCaptured() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=60", "--notch=100", "--lazy-tail=20", "--lazy-ms=200"}));
        Capture capture;
        run(capture, {{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
        exact(capture, 0, 0, 0, 59);
        QVERIFY(capture.leave());
    }
    void theHeightCapKeepsAnExactPrefix() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=200", "--notch=100"}));
        {
            Capture capture;
            run(capture, {{"rect", rectJson(page)}, {"maxHeight", 2000}, {"fromTop", false}, {"autoStart", true}});
            exact(capture, 0, 0, 0, 49);
            const auto finished = capture.last("finished");
            QCOMPARE(finished["endReason"].toString(), QString("height_cap"));
            QCOMPARE(finished["truncated"].toBool(), true);
            QCOMPARE(finished["height"].toInt(), 2000);
            QVERIFY(capture.leave());
        }
        // The pixel bound decides the height once the width is known: 1,500 lines here.
        Capture capture;
        run(capture, {{"rect", rectJson(page)}, {"maxHeight", 30000}, {"maxPixels", double(page.width()) * 1500}, {"fromTop", true}, {"autoStart", true}});
        exact(capture, 0, 0, 0, 37, 19);
        QCOMPARE(capture.last("finished")["appliedMaxHeight"].toInt(), 1500);
        QCOMPARE(capture.last("finished")["endReason"].toString(), QString("height_cap"));
        QVERIFY(capture.leave());
    }
    void stopKeepsTheImageSoFarAndCancelLeavesNothing() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=600", "--notch=100"}));
        {
            Capture capture;
            capture.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
            QVERIFY(putgd::test::waitFor([&] { return capture.last("progress")["height"].toInt() > 1500 || capture.ended(); }, 60000));
            const auto stopped = capture.control("stop");
            QVERIFY2(!stopped.error, qPrintable(stopped.error.code + ": " + stopped.error.message));
            QCOMPARE(stopped.result["endReason"].toString(), QString("stopped"));
            // The reply carries what the finished event carries.
            auto finished = capture.last("finished");
            QVERIFY(!finished.isEmpty());
            finished.remove("event");
            finished.remove("id");
            QCOMPARE(stopped.result, finished);
            const int height = stopped.result["height"].toInt();
            QVERIFY(height > 1500 && height < 600 * ruler::rowHeight);
            exact(capture, 0, 0, 0, (height - 1) / ruler::rowHeight, (height - 1) % ruler::rowHeight);
            QVERIFY(capture.leave());
        }
        Fixture again;
        QVERIFY(again.start({"--rows=600", "--notch=100"}));
        Capture capture;
        capture.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
        QVERIFY(putgd::test::waitFor([&] { return capture.last("progress")["height"].toInt() > 1000 || capture.ended(); }, 60000));
        QCOMPARE(capture.files(), QStringList{"rows.spool"});
        const auto cancelled = capture.control("cancel");
        QVERIFY2(!cancelled.error, qPrintable(cancelled.error.message));
        // Nothing is kept: no image, no spool, and no finished event.
        QVERIFY(capture.files().isEmpty());
        PUTGD_VERIFY_STEADY(!capture.has("finished"), 400);
        QCOMPARE(capture.control("stop").error.code, QString("scroll_state"));
        QVERIFY(capture.leave());
    }
    // Stop while the page is still on its way to the top used to keep nothing
    // (real desktop round of 2026-11-10). The image is the viewport as the
    // page came to rest, and nothing is injected after the stop.
    void stopOnTheWayToTheTopKeepsTheViewport() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=600", "--notch=100", "--start=500"}));
        RawInput input;
        QVERIFY(input.open());
        Capture capture;
        capture.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", true}, {"autoStart", true}});
        QVERIFY(putgd::test::waitFor([&] { return capture.last("progress")["state"].toString() == "scrollingToTop" || capture.ended(); }, 30000));
        // A few steps up, well short of the 20 the way takes.
        QVERIFY(putgd::test::waitFor([&] { const int at = fixture.position(); return (at > 0 && at <= 17000) || capture.ended(); }, 30000));
        const auto stopped = capture.control("stop");
        QVERIFY2(!stopped.error, qPrintable(stopped.error.code + ": " + stopped.error.message));
        QCOMPARE(stopped.result["endReason"].toString(), QString("stopped"));
        QCOMPARE(stopped.result["height"].toInt(), page.height());
        input.read();
        const int wheel = input.wheel;
        const int at = fixture.position();
        QVERIFY2(at > 0 && at < 500 * ruler::rowHeight, qPrintable(QString::number(at)));
        QCOMPARE(at % ruler::rowHeight, 0);
        exact(capture, 0, 0, at / ruler::rowHeight, (at + page.height() - 1) / ruler::rowHeight, (at + page.height() - 1) % ruler::rowHeight);
        // From the stop reply on the page stays where it is: no more wheel steps.
        putgd::test::settle(400);
        input.read();
        QCOMPARE(input.wheel, wheel);
        QCOMPARE(fixture.position(), at);
        QVERIFY(capture.leave());
    }
    // Escape on X11, whatever window has the keyboard: Stop, with the image kept.
    void escapeStopsAndKeepsTheImage() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=600", "--notch=100"}));
        Capture capture;
        capture.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
        QVERIFY(putgd::test::waitFor([&] { return capture.last("progress")["height"].toInt() > 1200 || capture.ended(); }, 60000));
        QVERIFY(!capture.ended());
        // The test's own key press: the helper sends keys never.
        xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
        QVERIFY(connection && !xcb_connection_has_error(connection));
        xcb_key_symbols_t* symbols = xcb_key_symbols_alloc(connection);
        xcb_keycode_t* codes = xcb_key_symbols_get_keycode(symbols, 0xff1b);
        QVERIFY(codes && codes[0] != XCB_NO_SYMBOL);
        xcb_test_fake_input(connection, XCB_KEY_PRESS, codes[0], XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_test_fake_input(connection, XCB_KEY_RELEASE, codes[0], XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_flush(connection);
        free(codes);
        xcb_key_symbols_free(symbols);
        QVERIFY(putgd::test::waitFor([&] { return capture.ended(); }, 30000));
        xcb_disconnect(connection);
        const auto finished = capture.last("finished");
        QCOMPARE(finished["endReason"].toString(), QString("stopped"));
        const int height = finished["height"].toInt();
        QVERIFY(height > 1200 && height < 600 * ruler::rowHeight);
        exact(capture, 0, 0, 0, (height - 1) / ruler::rowHeight, (height - 1) % ruler::rowHeight);
        QVERIFY(capture.leave());
    }
    void theSelectorTakesARectangleFromSelect() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=40", "--notch=100"}));
        Capture capture;
        capture.prepare({{"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
        QVERIFY(putgd::test::waitFor([&] { return capture.answered; }, 20000));
        QVERIFY2(!capture.prepareError, qPrintable(capture.prepareError.message));
        QCOMPARE(capture.prepared["state"].toString(), QString("selecting"));
        // The open selector completes as a drag release would.
        const auto selected = capture.control("select", {{"rect", rectJson(page)}});
        QVERIFY2(!selected.error, qPrintable(selected.error.code + ": " + selected.error.message));
        QCOMPARE(selected.result["state"].toString(), QString("capturing"));
        QCOMPARE(capture.last("selected")["rect"].toArray(), rectJson(page));
        QVERIFY(putgd::test::waitFor([&] { return capture.ended(); }, 90000));
        exact(capture, 0, 0, 0, 39);
        QVERIFY(capture.leave());
    }
    void aRegionThatIsTooSmallOrOffTheOutputIsRefused() {
        for (const QRect& rect : {QRect(100, 100, 40, 300), QRect(1200, 100, 400, 300)}) {
            Capture capture;
            capture.prepare({{"rect", rectJson(rect)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
            QVERIFY(putgd::test::waitFor([&] { return capture.answered; }, 20000));
            QCOMPARE(capture.prepareError.code, QString("scroll_region"));
            QVERIFY(capture.files().isEmpty());
            QVERIFY(capture.leave());
        }
    }
    void aPageThatDoesNotScrollIsReportedNotCaptured() {
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=10", "--notch=100"}));
        Capture capture;
        run(capture, {{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", true}, {"autoStart", true}});
        const auto failed = capture.last("failed");
        QCOMPARE(failed["error"].toObject()["code"].toString(), QString("scroll_no_movement"));
        QVERIFY(!capture.has("finished"));
        QVERIFY(capture.files().isEmpty());
        QVERIFY(capture.leave());
    }
    // What is injected, and when: wheel steps and pointer motion only, only
    // while capturing, nothing after the stop or cancel reply, and the pointer
    // back where it was.
    void onlyWheelAndMotionAndOnlyWhileCapturing() {
        RawInput raw;
        QVERIFY2(raw.open(), "the X server offers no XInput 2.1 raw events");
        const QPoint home(900, 700);
        Fixture fixture;
        QVERIFY(fixture.start({"--rows=600", "--notch=100"}));
        raw.warp(home);
        Capture capture;
        capture.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", true}, {"autoStart", false}});
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
        second.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
        QVERIFY(putgd::test::waitFor([&] { return second.last("progress")["height"].toInt() > 1000 || second.ended(); }, 60000));
        QVERIFY2(!second.control("cancel").error, "cancel failed");
        const int afterCancel = raw.total();
        QVERIFY(afterCancel > afterStop);
        PUTGD_COMPARE_STEADY(raw.total(), afterCancel, 800);
        QCOMPARE(raw.pointer(), home);
        QVERIFY(second.leave());

        // The helper killed in the middle: a dead process injects nothing.
        Capture third;
        third.prepare({{"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}});
        QVERIFY(putgd::test::waitFor([&] { return third.last("progress")["height"].toInt() > 1000 || third.ended(); }, 60000));
        const qint64 pid = third.backend.processId(third.id);
        QVERIFY(pid > 0);
        QCOMPARE(QProcess::execute("kill", {"-9", QString::number(pid)}), 0);
        QVERIFY(putgd::test::waitFor([&] { return third.has("failed"); }, 10000));
        QCOMPARE(third.last("failed")["error"].toObject()["code"].toString(), QString("scroll_helper_failed"));
        const int afterKill = raw.total();
        PUTGD_COMPARE_STEADY(raw.total(), afterKill, 800);
        QCOMPARE(raw.otherButtons, 0);
        QCOMPARE(raw.keys, 0);
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
        prepare["params"] = QJsonObject{{"id", "eof"}, {"staging", staging.path()}, {"rect", rectJson(page)}, {"maxHeight", 30000}, {"fromTop", false}, {"autoStart", true}, {"selection", "helper"}};
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
