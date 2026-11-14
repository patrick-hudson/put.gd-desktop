#include "ScrollCapture.h"
#include "DebugLog.h"
#include "ScrollInjectors.h"
#include "ScrollSession.h"
#include "ScrollSpool.h"
#include "X11ScrollInjector.h"
#include "X11Targets.h"
#ifdef PUTGD_RECORD_KDE_SCREENCAST
#include "KdeFakeInput.h"
#endif
#ifdef PUTGD_RECORD_LAYER_SHELL
#include "platform/wayland/LayerShellRegionOverlay.h"
#endif
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QPointer>
#include <QEnterEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QRegion>
#include <chrono>
#include <mutex>
#include <QScreen>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <optional>
#include <unistd.h>

namespace putgd::record {
namespace {
QJsonArray rectJson(const QRect& r) { return {r.x(), r.y(), r.width(), r.height()}; }
QRect rectFromJson(const QJsonValue& value) {
    const auto a = value.toArray();
    if (a.size() != 4) return {};
    for (const auto& v : a) if (!v.isDouble() || std::floor(v.toDouble()) != v.toDouble()) return {};
    return QRect(a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt());
}
Error stateError(const QString& message) { return {"scroll_state", message, {}}; }
Error unavailable(const QString& reason, const QString& message) { return {"scroll_unavailable", message, {{"reason", reason}}}; }
// How long the first frame may take, and the pointer's rest outside the region.
constexpr int firstFrameMs = 3000, awayMargin = 16;
// #1514: the rest pad's size, how far from where the capture put it a pointer
// the helper's surface reports is still the capture's (logical pixels, for
// rounding through the stream's scale), how long a visit's leave event may
// come late, and how still the mouse must be after the bar's Resume.
constexpr int padSize = 32, restTolerance = 4, visitLateMs = 1000, resumeQuietMs = 500;
// A hand's motion events come every 8 to 10 ms: one within this long of the
// pointer's arrival at its rest was under way when the capture moved it there.
constexpr int pullBackMs = 30;
// KWin's region stream (seen on 6.3.6, real desktop runs of 2026-11-10) holds
// back or drops a frame that no further damage follows: after one isolated
// repaint the loop saw the old picture for 500 ms and more, four or five steps
// in a capture. With damage somewhere on the output every 30 ms it sent every
// repaint within 90 ms.
constexpr int heartbeatMs = 30;
// The PNG writer may fail before its output exceeds the accepted bound, and
// stops at once when the capture is discarded.
class BoundedFile final : public QFile {
public:
    BoundedFile(qint64 bound, const std::atomic<bool>& abandon) : bound_(bound), abandon_(abandon) {}
    bool exceeded() const { return exceeded_; }
protected:
    qint64 writeData(const char* bytes, qint64 count) override {
        if (abandon_.load()) return -1;
        if (count < 0 || pos() > bound_ || count > bound_ - pos()) { exceeded_ = true; return -1; }
        return QFile::writeData(bytes, count);
    }
private:
    qint64 bound_;
    const std::atomic<bool>& abandon_;
    bool exceeded_ = false;
};
// A private file of the session: created here, 0600, never followed.
int createPrivate(const QString& path) { return ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600); }
// The first frame was pulled to learn the stream's size; the loop gets it again.
struct Primed final : RunFrames {
    RunFrames& inner;
    std::optional<RunFrame> first;
    const std::atomic<bool>& gone;
    Primed(RunFrames& source, RunFrame frame, const std::atomic<bool>& lostElsewhere) : inner(source), first(frame), gone(lostElsewhere) {}
    std::optional<RunFrame> nextFrame(int timeoutMs) override {
        if (first) { RunFrame frame = *first; first.reset(); frame.timestampMs = FrameTap::nowMs(); return frame; }
        auto frame = inner.nextFrame(timeoutMs);
        // Lab timeline: when each frame arrived and whether its picture changed
        // (a sum over every sixteenth row; never the pixels).
        if (frame && debugLogEnabled()) {
            quint64 sum = 0;
            for (int y = 0; y < frame->view.height; y += 16) { const uint8_t* row = frame->view.row(y); for (int x = 0; x < frame->view.width * 4; x += 4) sum = sum * 31 + row[x] + (row[x + 1] << 8) + (row[x + 2] << 16); }
            debugLog(QStringLiteral("scroll frame: t=%1 %2").arg(frame->timestampMs % 1000000).arg(sum == lastSum ? QStringLiteral("same") : QStringLiteral("changed")));
            lastSum = sum;
        }
        return frame;
    }
    quint64 lastSum = 0;
    bool lost() const override { return gone.load() || inner.lost(); }
    std::optional<RunFrame> freshFrame(int timeoutMs) override {
        first.reset();
        auto frame = inner.freshFrame(timeoutMs);
        if (debugLogEnabled()) debugLog(QStringLiteral("scroll fresh frame: %1").arg(frame ? QStringLiteral("t=%1").arg(frame->timestampMs % 1000000) : QStringLiteral("none")));
        return frame;
    }
};
// A clear surface that takes the pointer (#1514): the rest pad and the watch.
class ClearSurface final : public QWidget {
public:
    explicit ClearSurface(const char* name) : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus) {
        setObjectName(QString::fromLatin1(name));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_NoSystemBackground);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
    }
};
// Rows kept clear of the bar where it sits inside the rectangle (#1519): the
// window manager's shadow on X11 reaches further than the bar.
int insideBarMargin() { return surfaceMode() == SurfaceMode::X11 ? 24 : 6; }
}

struct ScrollCapture::Gate final : RunInjector {
    ScrollInjector* injector = nullptr;
    // Closed for good at stop, cancel and the end of the control pipe.
    std::atomic<bool> armed{true};
    // Closed while paused (#1514): set by the main thread the moment it sees
    // the user's motion, before the loop has looked, so nothing slips through.
    std::atomic<bool> paused{false};
    // After Resume the first motion is the capture's own, wherever the user left the pointer.
    std::atomic<bool> skipCheck{false};
    // Where the pointer rests between wheel steps (stream pixels; worker thread only).
    std::optional<QPoint> restAt;
    // What the main thread needs to tell the capture's pointer from the user's:
    // whether the last motion went to the rest spot, whether the pointer has
    // been seen arriving there, and visits into the rectangle whose leave
    // event has not been seen yet.
    std::atomic<bool> resting{false}, detecting{false};
    std::atomic<int> visits{0};
    std::atomic<qint64> visitAt{0};
    // Every pointer motion sent counts: the main thread tells a pointer that
    // moved with nothing sent in between (the user) from one the capture moved.
    std::atomic<quint64> seq{0};
    // Desktop logical coordinates of the rectangle's corner and the stream's
    // scale (set by the worker before the first injection), the last point
    // the wheel was turned at, and where the rest is.
    QPointF origin;
    double sx = 1, sy = 1;
    std::atomic<double> visitX{0}, visitY{0};
    QPointF restGlobal() const { return restAt ? origin + QPointF(restAt->x() / sx, restAt->y() / sy) : QPointF(); }
    // A pointer pulled back from under the user's moving hand (#1514): the
    // main thread knows where the hand had it (base) and follows its motion
    // from the rest since (latest); held() puts it back there, once.
    std::mutex correctionMutex;
    bool correctionPending = false;
    QPointF correctionBase, correctionLatest;
    qint64 correctionSince = 0;
    bool desktopCorrects = false;
    // X11: the pointer is not where XTest left it. Called on the worker.
    std::function<void()> tookPointer;
    bool userHasIt() {
        if (!restAt || skipCheck.load() || !injector->userHasPointer()) return false;
        paused.store(true);
        if (tookPointer) tookPointer();
        return true;
    }
    bool scroll(int notches) override {
        if (!armed.load() || paused.load() || userHasIt()) return false;
        const bool sent = injector->scroll(notches);
        if (debugLogEnabled()) debugLog(QStringLiteral("scroll inject: t=%1 wheel %2 %3").arg(FrameTap::nowMs() % 1000000).arg(notches).arg(sent ? "sent" : "refused"));
        return sent;
    }
    bool movePointer(int x, int y) override {
        if (!armed.load() || paused.load() || userHasIt()) return false;
        skipCheck.store(false);
        const bool toRest = restAt && *restAt == QPoint(x, y);
        // Counted before it is sent: the leave event may come before this returns.
        if (restAt && !toRest && resting.load()) { visitAt.store(FrameTap::nowMs()); visits.fetch_add(1); }
        if (!toRest) { visitX.store(origin.x() + x / sx); visitY.store(origin.y() + y / sy); }
        resting.store(toRest);
        seq.fetch_add(1);
        const bool sent = injector->movePointer(QPoint(x, y));
        if (debugLogEnabled()) debugLog(QStringLiteral("scroll inject: t=%1 pointer %2,%3 %4").arg(FrameTap::nowMs() % 1000000).arg(x).arg(y).arg(sent ? "sent" : "refused"));
        return sent;
    }
    bool rest() override {
        if (!armed.load() || paused.load()) return false;
        const bool sent = injector->rest();
        if (sent && debugLogEnabled()) debugLog(QStringLiteral("scroll inject: t=%1 wheel rest sent").arg(FrameTap::nowMs() % 1000000));
        return sent;
    }
    bool waitReady(const std::atomic<bool>& stop) override { return armed.load() && injector->waitReady(stop); }
    // While paused: the one correction, after 20 ms of following the hand.
    void held() override {
        QPointF target;
        {
            std::lock_guard lock(correctionMutex);
            if (!correctionPending || FrameTap::nowMs() - correctionSince < 20) return;
            correctionPending = false;
            target = correctionBase + (correctionLatest - restGlobal());
        }
        if (!armed.load()) return;
        const QPoint stream(int(std::lround((target.x() - origin.x()) * sx)), int(std::lround((target.y() - origin.y()) * sy)));
        seq.fetch_add(1);
        resting.store(false);
        const bool sent = injector->movePointer(stream);
        debugLog(QStringLiteral("scroll inject: t=%1 pointer put back at %2,%3 (desktop %4,%5) %6").arg(FrameTap::nowMs() % 1000000).arg(stream.x()).arg(stream.y()).arg(target.x(), 0, 'f', 0).arg(target.y(), 0, 'f', 0).arg(sent ? "sent" : "refused"));
    }
};
// What the main thread and the worker share for one capture.
struct ScrollCapture::Job {
    ScrollCapture* owner = nullptr;
    FrameTap* tap = nullptr;
    Gate gate;
    QString spoolPath, pngPath;
    // Desktop coordinates: the captured rectangle, its output, and the bar when it shows.
    QRect rect, screen, bar;
    // Where the pointer rests between wheel steps, on the bar or the rest pad
    // (desktop coordinates); unset where it has no such place (#1514). Set by
    // the main thread in prepareAuto, read by the worker once `prepared` says so.
    std::optional<QPoint> rest;
    // Called on the worker before auto-scroll's first wheel step of a run:
    // the main thread puts up the rest and the watch and then sets `prepared`
    // to the serial it was asked with. The worker waits a bounded time and
    // never blocks on it otherwise, as abandon() joins the worker.
    std::function<void(unsigned)> prepareAuto;
    std::atomic<unsigned> prepared{0};
    // Auto-scroll as the main thread wants it: the wish, and a serial that
    // changes with every request; and the commands, which count against the
    // idle bound.
    std::atomic<bool> wantAuto{false};
    std::atomic<unsigned> autoSerial{0}, touches{0};
    // The bar inside the rectangle (#1519), desktop coordinates: the frames'
    // rows from its top down are not the page's. Empty: it is outside.
    QRect insideBar;
    int maxHeight = 0;
    qint64 maxPixels = 0;
    bool fromTop = false, autoStart = false;
    // The frames may show the mouse pointer although none was asked for (ext-image-copy-capture).
    bool paintedPointer = false;
    std::function<bool()> endedByDesktop;
    std::atomic<bool> stop{false}, discard{false}, sourceGone{false}, layoutChanged{false};
};
struct ScrollCapture::Outcome {
    const Job* job = nullptr;
    SessionEnd end = SessionEnd::NoFrames;
    SessionStats stats;
    Error error;
    QJsonObject descriptor;
    bool nothing = false;  // stopped before any row existed
};

ScrollCapture::ScrollCapture(QObject* parent) : QObject(parent), bounds_(this) {
    // The Shell's screen-sharing indicator was clicked (GNOME): that is Stop.
    connect(&mutter_, &MutterScreenCast::lost, this, [this] { if (job_) job_->stop.store(true); });
#ifdef PUTGD_RECORD_KDE_SCREENCAST
    connect(&kde_, &KdeScreencast::lost, this, [this] { if (job_) job_->sourceGone.store(true); });
#endif
    auto layoutChanged = [this] { if (job_ && state_ == State::Capturing) { job_->layoutChanged.store(true); job_->stop.store(true); } };
    connect(&heartbeat_, &QTimer::timeout, this, [this] { bounds_.beat(); });
    resumeQuiet_.setSingleShot(true);
    connect(&resumeQuiet_, &QTimer::timeout, this, [this] { if (resumePending_) resumeNow(); });
    qApp->installEventFilter(this);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, layoutChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, layoutChanged);
}
ScrollCapture::~ScrollCapture() {
    abandon();
    delete bar_;
    delete pad_;
    delete watch_;
    delete selection_;
#ifdef PUTGD_RECORD_LAYER_SHELL
    delete layerSelection_;
#endif
}
void ScrollCapture::abandon() {
    if (job_) {
        job_->gate.armed.store(false);
        job_->discard.store(true);
        job_->stop.store(true);
    }
    cancelRequested_ = true;
    if (worker_.joinable()) worker_.join();
    job_.reset();
    closeSelector();
    release();
    // A finished image stays: the resident owns the staging directory and takes
    // the PNG from it, often after it has asked this helper to leave.
    if (state_ != State::Done) removeFiles();
    if (state_ != State::Done && state_ != State::Failed) state_ = State::Cancelled;
}
void ScrollCapture::removeFiles() {
    if (staging_.isEmpty()) return;
    QFile::remove(staging_ + "/rows.spool");
    QFile::remove(staging_ + "/scroll.png");
}
void ScrollCapture::release() {
    heartbeat_.stop();
    escape_.stop();
    tap_.stop();
    imageCopy_.reset();
    mutter_.close();
#ifdef PUTGD_RECORD_KDE_SCREENCAST
    kde_.close();
#endif
    injector_.reset();
    bounds_.hideAll();
    if (bar_) bar_->hide();
    delete pad_;
    pad_ = nullptr;
    delete watch_;
    watch_ = nullptr;
    restWindow_.clear();
    watchWindow_.clear();
    resumeQuiet_.stop();
    resumePending_ = false;
}
void ScrollCapture::closeSelector() {
    if (selection_) { selection_->disconnect(this); selection_->cancel(); selection_->deleteLater(); selection_ = nullptr; }
#ifdef PUTGD_RECORD_LAYER_SHELL
    if (layerSelection_) { layerSelection_->disconnect(this); layerSelection_->cancel(); layerSelection_->deleteLater(); layerSelection_ = nullptr; }
#endif
}
void ScrollCapture::fail(const Error& error) {
    closeSelector();
    release();
    removeFiles();
    state_ = State::Failed;
    debugLog("scroll: failed " + error.code + " " + error.message);
    emit event({{"event", "failed"}, {"error", error.toJson()}});
}

void ScrollCapture::command(const QString& command, const QJsonObject& params, Reply reply) {
    if (command == "prepare") { prepare(params, std::move(reply)); return; }
    if (state_ == State::Idle) { reply(stateError("The helper has no prepared scroll capture"), {}); return; }
    if (command == "select") {
        const QRect rect = rectFromJson(params["rect"]);
        if (rect.isEmpty()) { reply({"invalid_request", "select takes rect: [x, y, width, height]", {}}, {}); return; }
        if (state_ != State::Selecting) { reply(stateError("No region selection is open"), {}); return; }
        if (selectReply_) { reply(stateError("A selection is already being applied"), {}); return; }
        selectReply_ = std::move(reply);
        // An open selector completes as a drag release would; without one (the
        // Shell selected) the rectangle is simply taken.
        if (selection_) { selection_->complete(rect); return; }
#ifdef PUTGD_RECORD_LAYER_SHELL
        if (layerSelection_) { layerSelection_->select(QRectF(rect)); return; }
#endif
        selected(rect);
        return;
    }
    if (command == "start") {
        if (state_ != State::Ready) { reply(stateError("Nothing is ready to capture"), {}); return; }
        start([reply](Error error, QJsonObject result) { reply(error, result); });
        return;
    }
    // Auto-scroll on or off; pause and resume are its names from #1514.
    if (command == "autoScroll" || command == "pause" || command == "resume") {
        if (state_ != State::Capturing || !job_ || stopRequested_ || cancelRequested_) { reply(stateError("Nothing is being captured"), {}); return; }
        if (command == "autoScroll" && !params["on"].isBool()) { reply({"invalid_request", "autoScroll takes on: true or false", {}}, {}); return; }
        const bool on = command == "autoScroll" ? params["on"].toBool() : command == "resume";
        job_->touches.fetch_add(1);
        setAutoScroll(on, false);
        reply({}, pauseView());
        return;
    }
    if (command == "stop") { stop(std::move(reply)); return; }
    if (command == "cancel") { cancel(std::move(reply)); return; }
    if (command == "overlay") {
        // GNOME: the Shell placed no bar at all.
        if (params.contains("visible")) barHidden_ = !params["visible"].toBool(true);
        // GNOME: where the Shell put the bar, so the pointer can rest on it;
        // inside: in the rectangle's corner, kept out of the image (#1519).
        if (params.contains("rect")) shellBar_ = rectFromJson(params["rect"]);
        barInside_ = params["inside"].toBool(false) && !shellBar_.isEmpty();
        insideBar_ = barInside_ ? shellBar_ : QRect();
        showBar();
        reply({}, {{"visible", bar_ ? bar_->isVisible() : false}});
        return;
    }
    reply({"unknown_method", "Unknown scroll capture command", {}}, {});
}

void ScrollCapture::prepare(const QJsonObject& params, Reply reply) {
    if (state_ != State::Idle) { reply(stateError("The helper already has a scroll capture"), {}); return; }
    const QString staging = params["staging"].toString();
    if (staging.isEmpty() || !QFileInfo(staging).isDir()) { reply({"invalid_request", "The staging directory is missing", {}}, {}); return; }
    const auto probe = probeScroll();
    if (!probe["available"].toBool()) { reply(unavailable(probe["reason"].toString(), probe["message"].toString()), {}); return; }
    const QString selection = params["selection"].toString(QStringLiteral("helper"));
    const double maxHeight = params["maxHeight"].toDouble(scrollDefaultHeight), maxPixels = params["maxPixels"].toDouble(0);
    const QString driver = params["driver"].toString(QStringLiteral("user"));
    if ((selection != "helper" && selection != "shell") || (driver != "user" && driver != "putgd") || (params.contains("finishKey") && !params["finishKey"].isString()) || (params.contains("rect") && rectFromJson(params["rect"]).isEmpty()) || !std::isfinite(maxHeight) || maxHeight < 1 || !std::isfinite(maxPixels) || maxPixels < 0) {
        reply({"invalid_request", "Invalid scroll capture request", {}}, {}); return;
    }
    id_ = params["id"].toString();
    staging_ = staging;
    frames_ = probe["frames"].toString();
    selectionMode_ = selection;
    maxHeight_ = int(std::clamp(maxHeight, double(scrollMinEdge), double(scrollMaxHeight)));
    maxPixels_ = qint64(std::min(maxPixels, 4e12));
    fromTop_ = params["fromTop"].toBool(false);
    autoStart_ = params["autoStart"].toBool(true);
    autoAtStart_ = driver == "putgd";
    // The key that finishes the capture here, named on the bar: Esc on X11,
    // else the resident's `scroll` hotkey where one is bound.
    finishKey_ = surfaceMode() == SurfaceMode::X11 ? QStringLiteral("Esc") : params["finishKey"].toString().left(40);
    debugLog(QStringLiteral("scroll: prepare frames %1 injector %2 maxHeight %3 fromTop %4 autoStart %5 driver %6").arg(frames_, probe["injector"].toString()).arg(maxHeight_).arg(fromTop_).arg(autoStart_).arg(driver));
    if (params.contains("rect")) {
        if (auto e = setRect(rectFromJson(params["rect"]))) { reply(e, {}); return; }
        if (!autoStart_) { reply({}, {{"state", "ready"}, {"rect", rectJson(rect_)}}); return; }
        start(std::move(reply));
        return;
    }
    state_ = State::Selecting;
    if (selection == "helper") {
        if (auto e = openSelector()) { state_ = State::Failed; reply(e, {}); return; }
    }
    reply({}, {{"state", "selecting"}});
}

Error ScrollCapture::openSelector() {
    // The selector is the recording's: live, with nothing streaming until the
    // rectangle exists, so Escape leaves no compositor session behind.
    auto cancelled = [this](const Error& error) {
        closeSelector();
        const bool user = error.code == "cancelled";
        state_ = user ? State::Cancelled : State::Failed;
        if (auto reply = std::exchange(selectReply_, {})) reply(user ? Error{"cancelled", "Region selection canceled", {}} : error, {});
        if (user) emit event({{"event", "cancelled"}});
        else emit event({{"event", "failed"}, {"error", error.toJson()}});
    };
    const auto mode = surfaceMode();
    if (mode == SurfaceMode::X11) {
        selection_ = new SelectionOverlay(this);
        connect(selection_, &SelectionOverlay::selected, this, [this](const QRect& root) {
            selection_->deleteLater();
            selection_ = nullptr;
            selected(root);
        });
        connect(selection_, &SelectionOverlay::cancelled, this, cancelled);
        selection_->start();
        return {};
    }
#ifdef PUTGD_RECORD_LAYER_SHELL
    if (mode == SurfaceMode::LayerShell) {
        QList<CaptureFrame> frames;
        for (QScreen* screen : QGuiApplication::screens()) {
            CaptureFrame frame;
            frame.identity = screen->name();
            frame.logicalRect = screen->geometry();
            frame.rotation = screen->angleBetween(screen->nativeOrientation(), screen->orientation());
            frames.append(frame);
        }
        layerSelection_ = new LayerShellRegionOverlay(frames, this, true);
        connect(layerSelection_, &LayerShellRegionOverlay::selected, this, [this](const QRectF& logical) {
            if (!layerSelection_) return;
            layerSelection_->deleteLater();
            layerSelection_ = nullptr;
            const QRect rect = logical.toAlignedRect();
            // Emitted from a Wayland callback: open the stream and the surfaces outside it.
            QTimer::singleShot(0, this, [this, rect] { if (state_ == State::Selecting) selected(rect); });
        });
        connect(layerSelection_, &LayerShellRegionOverlay::cancelled, this, [this, cancelled](const Error& error) { if (layerSelection_) cancelled(error); });
        layerSelection_->start();
        return {};
    }
#endif
    return unavailable(QStringLiteral("region_unavailable"), QStringLiteral("This desktop has no region selector here; pass rect"));
}

void ScrollCapture::selected(const QRect& rect) {
    if (state_ != State::Selecting) return;
    auto reply = std::exchange(selectReply_, {});
    if (auto e = setRect(rect)) {
        if (reply) reply(e, {});
        fail(e);
        return;
    }
    emit event({{"event", "selected"}, {"rect", rectJson(rect_)}});
    if (!autoStart_) { if (reply) reply({}, {{"state", "ready"}, {"rect", rectJson(rect_)}}); return; }
    QPointer<ScrollCapture> guard(this);
    start([guard, reply](Error error, QJsonObject result) {
        if (reply) reply(error, result);
        // Nobody asked: the failure is the session's to report.
        else if (error && guard) guard->fail(error);
    });
}

// The region must lie on one output and be at least 64 px each way.
Error ScrollCapture::setRect(const QRect& rect) {
    const auto region = [&](const QString& message) { return Error{"scroll_region", message, {{"rect", rectJson(rect)}, {"minimum", scrollMinEdge}}}; };
    if (rect.width() < scrollMinEdge || rect.height() < scrollMinEdge) return region(QStringLiteral("The region must be at least %1 px in each direction").arg(scrollMinEdge));
    QRect chosen = rect, output;
    QString name;
    if (surfaceMode() == SurfaceMode::X11) {
        // Root pixels, checked against the RandR layout like a region recording.
        RecordingTarget target;
        target.kind = QStringLiteral("region");
        target.rect = rect;
        ResolvedTarget resolved;
        if (auto e = resolveX11Target(target, resolved)) return e.code == "recording_target" ? region(e.message) : e;
        for (const auto& value : resolved.layout["outputs"].toArray()) {
            const auto native = value.toObject()["native"].toArray();
            const QRect candidate = native.size() == 4 ? QRect(native[0].toInt(), native[1].toInt(), native[2].toInt(), native[3].toInt()) : QRect();
            if (candidate.contains(rect)) { output = candidate; name = value.toObject()["identity"].toString(); }
        }
    } else {
        for (QScreen* screen : QGuiApplication::screens()) if (screen->geometry().contains(rect)) { output = screen->geometry(); name = screen->name(); }
    }
    if (output.isEmpty()) return region(QStringLiteral("The region must lie on one output"));
    rect_ = chosen;
    screen_ = output;
    screenName_ = name;
    // X11 and layer-shell desktops draw the ring themselves; elsewhere the
    // resident's shell (the GNOME extension) draws it.
    if (surfaceMode() != SurfaceMode::Compositor) bounds_.showBounds(rect_);
    delete bar_;
    bar_ = new ScrollBar;
    bar_->setFinishKey(finishKey_);
    connect(bar_, &ScrollBar::doneRequested, this, [this] { if (job_) job_->touches.fetch_add(1); stop([](Error, QJsonObject) {}); });
    connect(bar_, &ScrollBar::autoScrollRequested, this, [this](bool on) { if (job_) job_->touches.fetch_add(1); setAutoScroll(on, true); });
    // The event follows the worker's end: nothing is injected after it.
    connect(bar_, &ScrollBar::cancelRequested, this, [this] { cancel([this](Error, QJsonObject) { emit event({{"event", "cancelled"}}); }); });
    barOutside_ = bar_->place(rect_);
    barInside_ = false;
    insideBar_ = {};
    // No room beside the rectangle (#1519): in its corner, its rows kept out of the image.
    if (!barOutside_ && surfaceMode() != SurfaceMode::Compositor && bar_->placeInside(rect_)) {
        barInside_ = true;
        const QRect g = bar_->geometry();
        insideBar_ = surfaceMode() == SurfaceMode::X11 ? QRect(barRootPoint(QPointF(g.topLeft())), barRootPoint(QPointF(g.bottomRight() + QPoint(1, 1))) - QPoint(1, 1)) : g;
        debugLog(QStringLiteral("scroll: no room beside the rectangle, the bar sits inside at %1,%2 %3x%4").arg(insideBar_.x()).arg(insideBar_.y()).arg(insideBar_.width()).arg(insideBar_.height()));
    }
    state_ = State::Ready;
    showBar();
    return {};
}

// The bar is beside the rectangle where there is room, else inside its
// corner with its rows kept out of the image (#1519). GNOME: the Shell moves
// it, which needs it on screen first, so it shows when the capture does not
// start by itself (the resident places it, then starts), and the resident
// hides it (`overlay`) only when the Shell placed no bar at all.
void ScrollCapture::showBar() {
    if (!bar_) return;
    const bool live = state_ == State::Ready || state_ == State::Starting || state_ == State::Capturing;
    const bool allowed = surfaceMode() == SurfaceMode::Compositor ? !autoStart_ : barOutside_ || barInside_;
    if (live && allowed && !barHidden_) { bar_->show(); bar_->raise(); }
    else bar_->hide();
}

void ScrollCapture::start(Reply reply) {
    state_ = State::Starting;
    QPointer<ScrollCapture> guard(this);
    auto refused = [guard, reply](const Error& error) {
        if (!guard) return;
        // A cancel closed the source that was opening.
        if (guard->state_ == State::Cancelled) { reply({"cancelled", "Scroll capture canceled before it started", {}}, {}); return; }
        guard->release();
        guard->state_ = State::Failed;
        reply(error, {});
    };
    FrameTap::Source source;
    if (frames_ == "ximagesrc") {
        injector_ = std::make_unique<X11ScrollInjector>();
        source.x11Rect = rect_;
        begin(source, std::move(reply));
        return;
    }
    if (frames_ == "image-copy") {
        auto injector = std::make_unique<WlrScrollInjector>();
        imageCopy_ = std::make_unique<ImageCopySource>();
        ImageCopySource::Target target;
        target.identity = screenName_;
        QSize size;
        QString identity;
        if (auto e = imageCopy_->open(target, &size, &identity, nullptr, false, nullptr, true)) { refused(unavailable(QStringLiteral("frame_source_unavailable"), e.message)); return; }
        // The stream is the output in its own pixels; the region is logical.
        const double sx = double(size.width()) / screen_.width(), sy = double(size.height()) / screen_.height();
        const QRect logical = rect_.translated(-screen_.topLeft());
        source.imageCopy = imageCopy_.get();
        source.streamSize = size;
        source.crop = QRect(qRound(logical.x() * sx), qRound(logical.y() * sy), qRound(logical.width() * sx), qRound(logical.height() * sy)).intersected(QRect(QPoint(), size));
        injector->setOutput(identity, size, source.crop);
        injector_ = std::move(injector);
        begin(source, std::move(reply));
        return;
    }
    if (frames_ == "mutter-screencast") {
        injector_ = std::make_unique<MutterScrollInjector>(mutter_);
        mutter_.recordAreaLinked(rect_, [guard, reply, refused](Error error, MutterScreenCast::Stream stream) {
            if (!guard) return;
            if (error) { refused(error.code == "scroll_unavailable" ? error : unavailable(QStringLiteral("frame_source_unavailable"), error.message)); return; }
            FrameTap::Source source;
            source.pipewireNode = stream.node;
            guard->begin(source, reply);
        });
        return;
    }
#ifdef PUTGD_RECORD_KDE_SCREENCAST
    if (frames_ == "kde-screencast") {
        injector_ = std::make_unique<KdeFakeInput>();
        // The pointer hidden and the output's own pixels, not one per logical pixel.
        kde_.streamRegion(rect_, [guard, reply, refused](Error error, KdeScreencast::Stream stream) {
            if (!guard) return;
            if (error) { refused(unavailable(QStringLiteral("frame_source_unavailable"), error.message)); return; }
            FrameTap::Source source;
            source.pipewireNode = stream.node;
            guard->begin(source, reply);
        }, true, true);
        return;
    }
#endif
    refused(unavailable(QStringLiteral("frame_source_unavailable"), QStringLiteral("No frame source for this desktop")));
}

void ScrollCapture::begin(const FrameTap::Source& source, Reply reply) {
    auto refused = [this, reply](const Error& error) { release(); state_ = State::Failed; reply(error, {}); };
    // A cancel that arrived while the source was opening starts nothing.
    if (state_ != State::Starting) { release(); reply({"cancelled", "Scroll capture canceled before it started", {}}, {}); return; }
    if (!injector_ || !injector_->unavailableReason().isEmpty()) { refused(unavailable(QStringLiteral("injection_unavailable"), injector_ ? injector_->unavailableReason() : QStringLiteral("No injector for this desktop"))); return; }
    removeFiles();
    if (auto e = tap_.start(source)) { refused(e); return; }
    job_ = std::make_shared<Job>();
    job_->owner = this;
    job_->tap = &tap_;
    job_->gate.injector = injector_.get();
    job_->spoolPath = staging_ + "/rows.spool";
    job_->pngPath = staging_ + "/scroll.png";
    job_->rect = rect_;
    job_->screen = screen_;
    job_->bar = bar_ && bar_->isVisible() && surfaceMode() == SurfaceMode::X11 ? bar_->geometry() : QRect();
    job_->maxHeight = maxHeight_;
    job_->maxPixels = maxPixels_;
    job_->fromTop = fromTop_;
    job_->paintedPointer = frames_ == "image-copy";
    job_->endedByDesktop = [this] { return mutter_.closedByCompositor(); };
    job_->autoStart = autoAtStart_;
    job_->wantAuto.store(autoAtStart_);
    job_->insideBar = barInside_ ? insideBar_ : QRect();
    // Nothing is injected while the user drives.
    job_->gate.paused.store(!autoAtStart_);
    QPointer<ScrollCapture> guard(this);
    const std::weak_ptr<Job> weak = job_;
    job_->prepareAuto = [guard, weak](unsigned serial) {
        QMetaObject::invokeMethod(guard, [guard, weak, serial] {
            if (guard) guard->prepareAuto();
            if (auto job = weak.lock()) job->prepared.store(serial);
        }, Qt::QueuedConnection);
    };
    job_->gate.tookPointer = [guard] { QMetaObject::invokeMethod(guard, [guard] { if (guard) guard->pause(QStringLiteral("pointer")); }, Qt::QueuedConnection); };
    state_ = State::Capturing;
    pausedBy_.clear();
    autoOn_ = autoAtStart_;
    progressState_ = autoAtStart_ ? (fromTop_ ? QStringLiteral("scrollingToTop") : QStringLiteral("autoScrolling")) : QStringLiteral("waiting");
    lastProgress_ = {};
    watchSeqBase_ = 0;
    haveSeen_ = lastWasArrival_ = false;
    arrivedMs_ = -1;
    job_->gate.origin = QPointF(rect_.topLeft());
    // Where the desktop cannot be asked where the pointer is, a pointer pulled
    // back from under the hand is put back (X11 asks before every motion).
    job_->gate.desktopCorrects = surfaceMode() != SurfaceMode::X11;
    if (frames_ == "kde-screencast" && surfaceMode() == SurfaceMode::LayerShell) {
        bounds_.showHeartbeat(screen_.topLeft());
        heartbeat_.start(heartbeatMs);
    }
    if (bar_) { bar_->setState(progressState_); bar_->setAutoScroll(autoOn_); }
    showBar();
    debugLog(QStringLiteral("scroll: capturing %1,%2 %3x%4 with %5").arg(rect_.x()).arg(rect_.y()).arg(rect_.width()).arg(rect_.height()).arg(injector_->name()));
    worker_ = std::thread(&ScrollCapture::work, job_);
    // Escape on X11 is Done: the image so far is kept.
    if (surfaceMode() == SurfaceMode::X11) escape_.start([this] { if (state_ == State::Capturing && !stopRequested_ && !cancelRequested_) stop([](Error, QJsonObject) {}); });
    reply({}, {{"state", "capturing"}, {"rect", rectJson(rect_)}});
}

void ScrollCapture::stop(Reply reply) {
    if (state_ == State::Done) { reply({}, finished_); return; }
    // A Stop behind a Cancel whose worker has not ended yet is answered as the
    // Cancel will end the capture, not as a fault: the resident took the
    // refusal for a failed capture (acceptance of 2026-11-11).
    if (state_ == State::Capturing && job_ && cancelRequested_) { reply({"cancelled", "Scroll capture canceled", {}}, {}); return; }
    if (state_ != State::Capturing || !job_ || cancelRequested_) { reply(stateError("Nothing is being captured"), {}); return; }
    if (stopReply_) { reply(stateError("The capture is already stopping"), {}); return; }
    stopRequested_ = true;
    stopReply_ = std::move(reply);
    job_->stop.store(true);
    if (bar_) bar_->setState(QStringLiteral("finishing"));
}

void ScrollCapture::cancel(Reply reply) {
    if (state_ == State::Capturing && job_) {
        // The worker ends before its next injection; the reply waits for it.
        if (cancelReply_) { reply({}, {}); return; }
        cancelRequested_ = true;
        cancelReply_ = std::move(reply);
        job_->discard.store(true);
        job_->stop.store(true);
        if (bar_) bar_->hide();
        return;
    }
    // Set first: closing a source that is still opening reports through start().
    if (state_ != State::Failed) state_ = State::Cancelled;
    if (auto pending = std::exchange(selectReply_, {})) pending({"cancelled", "Scroll capture canceled", {}}, {});
    closeSelector();
    release();
    removeFiles();
    reply({}, {});
}

// The user moved the mouse while auto-scroll was on (#1514). The gate closes
// at once; the session sees it before its next injection, hands the wheel back
// and goes on following whatever the user does.
void ScrollCapture::pause(const QString& by) {
    if (!job_ || state_ != State::Capturing || stopRequested_ || cancelRequested_) return;
    job_->gate.paused.store(true);
    resumePending_ = false;
    resumeQuiet_.stop();
    if (!autoOn_) return;
    autoOn_ = false;
    pausedBy_ = by;
    // The desktop is the user's again.
    if (watch_) watch_->hide();
    // The pad goes a moment later: a pointer pulled back from under the hand
    // is still being put back where the hand has it (Gate::held).
    QPointer<ScrollCapture> guard(this);
    QTimer::singleShot(300, this, [guard] { if (guard && !guard->autoOn_) guard->dropAutoSurfaces(); });
    debugLog(QStringLiteral("scroll: auto-scroll paused by %1 at t=%2").arg(by).arg(FrameTap::nowMs() % 1000000));
    if (bar_) { bar_->setAutoScroll(false); bar_->setPaused(by); }
    emitProgress();
}
void ScrollCapture::setAutoScroll(bool on, bool quiet) {
    if (!job_ || state_ != State::Capturing || stopRequested_ || cancelRequested_) return;
    if (!on) { autoOff(); return; }
    if (autoOn_ || resumePending_) return;
    resume(quiet);
}
// The bar's Auto-scroll waits until the hand is off the mouse: the click
// itself comes with motion, and a pause the moment it begins helps nobody.
void ScrollCapture::resume(bool quiet) {
    if (!job_ || autoOn_ || state_ != State::Capturing || stopRequested_ || cancelRequested_) return;
    if (!quiet) { resumeNow(); return; }
    resumePending_ = true;
    if (bar_) bar_->setState(QStringLiteral("resuming"));
    resumeQuiet_.start(resumeQuietMs);
}
void ScrollCapture::resumeNow() {
    if (!job_ || autoOn_ || state_ != State::Capturing) return;
    resumePending_ = false;
    resumeQuiet_.stop();
    pausedBy_.clear();
    // The pointer is wherever the user left it: the capture's next motion is
    // its own, and the user is told from the capture again once it has arrived.
    job_->gate.detecting.store(false);
    job_->gate.resting.store(false);
    job_->gate.visits.store(0);
    job_->gate.skipCheck.store(true);
    {
        std::lock_guard lock(job_->gate.correctionMutex);
        job_->gate.correctionPending = false;
    }
    watchSeqBase_ = job_->gate.seq.load();
    haveSeen_ = lastWasArrival_ = false;
    arrivedMs_ = -1;
    autoOn_ = true;
    // The gate opens before the session hears of it, or it would find itself paused.
    job_->gate.paused.store(false);
    job_->wantAuto.store(true);
    job_->autoSerial.fetch_add(1);
    debugLog(QStringLiteral("scroll: auto-scroll on at t=%1").arg(FrameTap::nowMs() % 1000000));
    if (bar_) { bar_->setAutoScroll(true); bar_->setState(QStringLiteral("autoScrolling")); }
    emitProgress();
}
void ScrollCapture::autoOff() {
    if (!job_) return;
    resumePending_ = false;
    resumeQuiet_.stop();
    job_->wantAuto.store(false);
    job_->autoSerial.fetch_add(1);
    job_->gate.paused.store(true);
    const bool was = autoOn_ || !pausedBy_.isEmpty();
    autoOn_ = false;
    pausedBy_.clear();
    dropAutoSurfaces();
    if (was) debugLog(QStringLiteral("scroll: auto-scroll off at t=%1").arg(FrameTap::nowMs() % 1000000));
    if (bar_) {
        bar_->setAutoScroll(false);
        const QString state = lastProgress_["state"].toString();
        bar_->setState(state == "lost" || state == "following" || state == "waiting" ? state : lastProgress_["placed"].toInt() > 1 ? QStringLiteral("following") : QStringLiteral("waiting"));
    }
    emitProgress();
}
// The worker is about to send auto-scroll's first wheel step of a run: the
// pointer's rest, and on layer-shell desktops the watch with holes at the
// wheel points.
void ScrollCapture::prepareAuto() {
    if (!job_ || !autoOn_ || state_ != State::Capturing) return;
    if (!restWindow_) placeRest();
    if (surfaceMode() == SurfaceMode::LayerShell && job_->rest) {
        // The wheel points as the session finds them (ScrollSession::wheelPoints),
        // in desktop logical pixels: the entry just inside the edge the pointer
        // rests beyond, and the three park points.
        const int rows = barInside_ ? std::max(1, insideBar_.top() - rect_.top() - insideBarMargin()) : rect_.height();
        const QPoint at = *job_->rest - rect_.topLeft();
        QList<QPointF> holes;
        if (!QRect(QPoint(), rect_.size()).contains(at)) {
            const RunPoint e = ScrollSession::entryPoint({at.x(), at.y()}, 48, rect_.width(), rows);
            holes << QPointF(rect_.topLeft()) + QPointF(e.x, e.y);
        }
        for (int position = 0; position < 3; ++position) {
            const RunPoint p = ScrollSession::parkPoint(position, rect_.width(), rows);
            holes << QPointF(rect_.topLeft()) + QPointF(p.x, p.y);
        }
        if (!watch_) showWatch(holes);
        else { watch_->show(); watchWindow_ = watch_->windowHandle(); }
    }
}
void ScrollCapture::dropAutoSurfaces() {
    delete pad_;
    pad_ = nullptr;
    delete watch_;
    watch_ = nullptr;
    restWindow_.clear();
    watchWindow_.clear();
    if (job_) job_->rest.reset();
}
// The session's progress (worker thread, queued here). A hand-back the
// session made by itself (it lost the page, the user scrolled, nothing
// scrolled under the wheel) closes the gate and takes the surfaces down;
// progress from before the latest auto-scroll request decides nothing.
void ScrollCapture::sessionProgress(const QJsonObject& p) {
    if (!job_ || state_ != State::Capturing || stopRequested_ || cancelRequested_) return;
    lastProgress_ = {{"state", p["state"]}, {"height", p["height"]}, {"placed", p["placed"]}, {"skipped", p["skipped"]}, {"steps", p["steps"]}, {"pixelsPerNotch", p["pixelsPerNotch"]}};
    const QString driver = p["driver"].toString(), by = p["pausedBy"].toString();
    progressState_ = p["state"].toString();
    const bool current = unsigned(p["serial"].toDouble()) == job_->autoSerial.load();
    if (current && driver == "user" && autoOn_ && !resumePending_) {
        job_->gate.paused.store(true);
        autoOn_ = false;
        if (watch_) watch_->hide();
        dropAutoSurfaces();
        debugLog(QStringLiteral("scroll: auto-scroll handed the wheel back (%1)").arg(by.isEmpty() ? QStringLiteral("off") : by));
    }
    if (current && driver == "user" && !by.isEmpty()) pausedBy_ = by;
    if (bar_) {
        bar_->setHeight(p["height"].toInt());
        bar_->setPicture(p["picture"].toBool());
        if (!resumePending_) {
            bar_->setAutoScroll(autoOn_);
            if (progressState_ == "paused") bar_->setPaused(pausedBy_.isEmpty() ? by : pausedBy_);
            else bar_->setState(progressState_);
        }
    }
    emitProgress();
}
QJsonObject ScrollCapture::pauseView() const {
    QJsonObject view{{"state", "capturing"}, {"driver", autoOn_ ? "putgd" : "user"}, {"paused", !pausedBy_.isEmpty()}};
    if (!pausedBy_.isEmpty()) view["pausedBy"] = pausedBy_;
    return view;
}
void ScrollCapture::emitProgress() {
    QJsonObject progress = lastProgress_;
    progress["event"] = "progress";
    if (!progress.contains("state")) progress["state"] = progressState_;
    if (resumePending_) progress["state"] = QStringLiteral("resuming");
    const auto view = pauseView();
    progress.remove("pausedBy");
    for (auto i = view.begin(); i != view.end(); ++i) if (i.key() != "state") progress[i.key()] = i.value();
    if (!progress.contains("height")) { progress["height"] = 0; progress["placed"] = 0; progress["skipped"] = 0; progress["steps"] = 0; progress["pixelsPerNotch"] = 0; }
    emit event(progress);
}

// Where the pointer rests between wheel steps (#1514): on the bar where it is
// beside the rectangle, in a margin of the bar that holds no button and on its
// edge nearest the rectangle, so the way to the page is short (Plasma's
// "shake cursor", ScrollSession::entryPoint); else at the old rest spot, 16 px
// outside the rectangle, on a clear pad of the helper's own. Where frames show
// a painted pointer the arrow, which hangs down and right from its tip, stays
// out of the rectangle. With no such place (the rectangle fills its output, or
// GNOME without the bar) there is none, and the user's motion is not seen.
void ScrollCapture::placeRest() {
    const auto mode = surfaceMode();
    const bool painted = frames_ == "image-copy";
    const QRect capture = mode == SurfaceMode::X11 ? barLogicalRect(rect_) : rect_;
    const QRect output = mode == SurfaceMode::X11 ? barLogicalRect(screen_) : screen_;
    const QRect guard = capture.adjusted(-2, -2, 2, 2);
    QWidget* surface = nullptr;
    QRect place;
    std::optional<QPoint> rest;
    const bool barShown = bar_ && bar_->isVisible() && !barHidden_;
    const QRect bar = mode == SurfaceMode::Compositor ? shellBar_ : barShown && (barOutside_ || barInside_) ? bar_->geometry() : QRect();
    // The bar inside the rectangle (#1519) covers page rows the image leaves out: the pointer may rest on it.
    const QRect onBar = barInside_ ? bar.adjusted(4, 4, -4, -4) : QRect();
    const auto clear = [&](const QPoint& p) { return onBar.contains(p) || (!guard.contains(p) && (!painted || !QRect(p.x() - 2, p.y() - 2, 26, 34).intersects(guard))); };
    if (barShown && !bar.isEmpty() && (barInside_ || !bar.intersects(capture))) {
        // Over the state text at the bar's left end, never a button, and at
        // least 14 px from every edge of the bar: a hand's first motion
        // event (about 6 px at a brisk pace) then lands on the bar and is
        // seen, where at 3 px from its edge it left the bar at once and its
        // leave was taken for the capture's own (GNOME acceptance, two
        // pull-backs in one try).
        const int inset = 14;
        qint64 best = -1;
        for (int y = bar.top() + inset; y <= bar.bottom() - inset; y += 2)
            for (int x = bar.left() + 16; x <= bar.left() + bar.width() * 35 / 100; x += 4) {
                const QPoint p(x, y);
                if (!clear(p)) continue;
                const int dx = std::max({capture.left() - x, 0, x - capture.right()}), dy = std::max({capture.top() - y, 0, y - capture.bottom()});
                const qint64 d = qint64(dx) * dx + qint64(dy) * dy;
                if (best < 0 || d < best) { best = d; rest = p; }
            }
        // The way into the rectangle stays short (Plasma's shake cursor): a bar
        // whose text end is far from the rectangle is no rest where a pad can be.
        if (rest && best > 60 * 60 && mode != SurfaceMode::Compositor && !barInside_) rest.reset();
        if (rest) { surface = bar_; place = bar; }
    }
    if (!rest && mode != SurfaceMode::Compositor) {
        const QRect& r = capture;
        for (const QPoint& candidate : {QPoint(r.left() - awayMargin, r.top() + r.height() / 3), QPoint(r.right() + 1 + awayMargin, r.top() + r.height() / 3),
                                        QPoint(r.center().x(), r.top() - awayMargin), QPoint(r.center().x(), r.bottom() + 1 + awayMargin)}) {
            QRect pad(candidate - QPoint(padSize / 2, padSize / 2), QSize(padSize, padSize));
            // Kept off the rectangle: moved outward by as much as it overlaps.
            if (candidate.x() < r.left()) pad.moveRight(std::min(pad.right(), r.left() - 1));
            else if (candidate.x() > r.right()) pad.moveLeft(std::max(pad.left(), r.right() + 1));
            else if (candidate.y() < r.top()) pad.moveBottom(std::min(pad.bottom(), r.top() - 1));
            else pad.moveTop(std::max(pad.top(), r.bottom() + 1));
            if (!output.contains(pad) || pad.intersects(capture) || (barShown && bar.intersects(pad)) || !pad.contains(candidate) || !clear(candidate)) continue;
            auto* w = new ClearSurface("putgd-scroll-rest");
            const auto screens = QGuiApplication::screens();
            QScreen* on = nullptr;
            for (QScreen* each : screens) if (each->geometry().contains(pad)) on = each;
            if (mode == SurfaceMode::LayerShell && on) barToLayer(w, on, pad, false);
            else w->setGeometry(pad);
            w->show();
            pad_ = w;
            surface = w;
            place = pad;
            rest = candidate;
            break;
        }
    }
    if (!rest || !surface) {
        debugLog(QStringLiteral("scroll: no rest surface, the user's motion is not watched"));
        return;
    }
    restLocal_ = QPointF(*rest - place.topLeft());
    restOrigin_ = QPointF(place.topLeft());
    restWindow_ = surface->windowHandle();
    job_->rest = mode == SurfaceMode::X11 ? barRootPoint(QPointF(*rest)) : *rest;
    debugLog(QStringLiteral("scroll: rest on the %1 at %2,%3 (local %4,%5)").arg(surface == bar_ ? QStringLiteral("bar") : QStringLiteral("pad")).arg(job_->rest->x()).arg(job_->rest->y()).arg(restLocal_.x()).arg(restLocal_.y()));
}

// A pointer event on one of the helper's surfaces: the rest (the bar or the
// pad) or the watch; local: where on it, none: it left; stamp: the event's
// time. The capture's own: arriving at the rest spot, leaving it for a visit
// it sent. The user: the pointer anywhere else on the rest once it has
// arrived there, leaving the rest with no visit sent, on the watch once the
// capture has moved it, or moved on any of them with nothing sent in between
// (so a hand is seen before the first arrival too: a Hyprland guest's first
// rest came 3.2 s into the capture, after the whole gesture).
void ScrollCapture::pointerSeen(bool watch, std::optional<QPointF> local, quint64 stamp) {
    if (!job_ || state_ != State::Capturing || stopRequested_ || cancelRequested_) return;
    auto& gate = job_->gate;
    const std::optional<QPointF> global = local ? std::optional<QPointF>(*local + (watch ? QPointF(screen_.topLeft()) : restOrigin_)) : std::nullopt;
    if (global && gate.desktopCorrects) {
        // Following the hand for a correction that is still to be made.
        std::lock_guard lock(gate.correctionMutex);
        const QPointF rest = gate.restGlobal();
        if (gate.correctionPending && QLineF(*global, rest).length() < QLineF(*global, gate.correctionBase).length()) gate.correctionLatest = *global;
    }
    if (resumePending_) { resumeQuiet_.start(resumeQuietMs); return; }
    if (gate.paused.load()) return;
    if (!global) {
        if (watch || !gate.detecting.load()) return;
        if (gate.visits.load() > 0) { gate.visits.fetch_sub(1); return; }
        debugLog(QStringLiteral("scroll: pointer left the rest surface with no visit sent"));
        userMoved(arrivedMs_ >= 0 && FrameTap::nowMs() - arrivedMs_ <= pullBackMs, std::nullopt);
        return;
    }
    const quint64 seq = gate.seq.load();
    const bool movedAlone = haveSeen_ && seq == seenSeq_ && QLineF(*global, seen_).length() > restTolerance;
    const bool afterArrival = lastWasArrival_ && stamp - arrivalStamp_ <= quint64(pullBackMs);
    haveSeen_ = true;
    seen_ = *global;
    seenSeq_ = seq;
    lastWasArrival_ = false;
    if (!watch && QLineF(*local, restLocal_).length() <= restTolerance) {
        if (gate.resting.load()) {
            if (!gate.detecting.exchange(true)) debugLog(QStringLiteral("scroll: pointer at rest, watching from t=%1").arg(FrameTap::nowMs() % 1000000));
            // A leave that never came is not waited for.
            if (gate.visits.load() > 0 && FrameTap::nowMs() - gate.visitAt.load() > visitLateMs) gate.visits.store(0);
            lastWasArrival_ = true;
            arrivalStamp_ = stamp;
            arrivedMs_ = FrameTap::nowMs();
        }
        return;
    }
    QString why;
    if (movedAlone) why = QStringLiteral("moved with nothing sent");
    else if (!watch && gate.detecting.load()) why = QStringLiteral("not at its rest");
    else if (watch && seq > watchSeqBase_) why = QStringLiteral("on the watch");
    if (why.isEmpty()) return;
    debugLog(QStringLiteral("scroll: pointer at %1,%2 %3 (%4)").arg(global->x()).arg(global->y()).arg(watch ? QStringLiteral("on the watch") : QStringLiteral("on the rest surface")).arg(why));
    // The hand was moving when the capture put the pointer back on its rest:
    // seen on the watch where the capture believes it at rest (the event left
    // before the move back), or on the rest within a few ms of its arrival.
    if (watch && gate.resting.load()) userMoved(true, global, true);
    else userMoved(!watch && afterArrival, global, false);
}

// The user has the pointer. pulledBack: the capture had just moved it back to
// its rest from under the moving hand. Then, where the desktop cannot be asked
// (Wayland), the pointer is put back once where the hand had it, plus the way
// it has moved since: base is where the watch saw the hand before the move
// back, else the point the wheel was turned at; at is where it is now.
void ScrollCapture::userMoved(bool pulledBack, std::optional<QPointF> at, bool atIsBase) {
    auto& gate = job_->gate;
    if (pulledBack && gate.desktopCorrects) {
        std::lock_guard lock(gate.correctionMutex);
        const QPointF rest = gate.restGlobal();
        gate.correctionBase = atIsBase && at ? *at : QPointF(gate.visitX.load(), gate.visitY.load());
        gate.correctionLatest = !atIsBase && at ? *at : rest;
        gate.correctionSince = FrameTap::nowMs();
        gate.correctionPending = true;
        debugLog(QStringLiteral("scroll: the pointer was pulled back from under the hand; it goes back to %1,%2 plus the hand's way since").arg(gate.correctionBase.x(), 0, 'f', 0).arg(gate.correctionBase.y(), 0, 'f', 0));
    }
    pause(QStringLiteral("pointer"));
}

void ScrollCapture::showWatch(const QList<QPointF>& holes) {
    if (!job_ || state_ != State::Capturing || watch_) return;
    QScreen* on = nullptr;
    for (QScreen* each : QGuiApplication::screens()) if (each->geometry() == screen_) on = each;
    if (!on) return;
    auto* w = new ClearSurface("putgd-scroll-watch");
    QRegion input(QRect(QPoint(), screen_.size()));
    // A wheel step goes to the page only through these holes, 8 px across.
    for (const QPointF& hole : holes) input -= QRect((hole - QPointF(4, 4)).toPoint() - screen_.topLeft(), QSize(8, 8));
    if (bar_ && bar_->isVisible()) input -= bar_->geometry().translated(-screen_.topLeft());
    if (pad_) input -= pad_->geometry().translated(-screen_.topLeft());
    w->setMask(input);
    barToLayer(w, on, screen_, false);
    w->show();
    watch_ = w;
    watchWindow_ = w->windowHandle();
    QStringList where;
    for (const QPointF& hole : holes) where << QStringLiteral("%1,%2").arg(hole.x(), 0, 'f', 1).arg(hole.y(), 0, 'f', 1);
    debugLog(QStringLiteral("scroll: watch over %1 with holes at %2").arg(screenName_, where.join(' ')));
}
bool ScrollCapture::eventFilter(QObject* object, QEvent* e) {
    // The capture's pointer is only ever at its rest (on the bar or the pad,
    // cut out of the watch) or in a hole: see pointerSeen.
    const bool watch = watchWindow_ && object == watchWindow_.data(), rest = restWindow_ && object == restWindow_.data();
    if (watch || rest) {
        switch (e->type()) {
            case QEvent::MouseMove: { auto* m = static_cast<QMouseEvent*>(e); pointerSeen(watch, m->position(), m->timestamp()); break; }
            case QEvent::Enter: { auto* m = static_cast<QEnterEvent*>(e); pointerSeen(watch, m->position(), m->timestamp()); break; }
            case QEvent::Leave: if (rest) pointerSeen(false, std::nullopt, 0); break;
            default: break;
        }
    }
    return QObject::eventFilter(object, e);
}

void ScrollCapture::finish(std::shared_ptr<Outcome> outcome) {
    if (!job_ || outcome->job != job_.get()) return;
    if (worker_.joinable()) worker_.join();
    job_.reset();
    release();
    const SessionStats& st = outcome->stats;
    debugLog(QStringLiteral("scroll: ended %1, %2 rows, placed %3, skipped %4, recovered %5, moved by user %6 by put.gd %7, %8 wheel steps, %9 px per notch, %10 ms%11")
                 .arg(QString::fromLatin1(toString(outcome->end))).arg(st.bottom - st.top).arg(st.placed).arg(st.skipped).arg(st.recovered).arg(st.movedByUser).arg(st.movedByPutgd).arg(st.steps)
                 .arg(st.pxPerNotch, 0, 'f', 1).arg(st.elapsedMs).arg(st.lastError.empty() ? QString() : QStringLiteral(" (%1)").arg(QString::fromStdString(st.lastError))));
    // How fast the desktop's frame source delivered: what a person's scrolling is followed at.
    debugLog(QStringLiteral("scroll: source %1 frames in %2 ms (%3 per second), slowest response %4 ms").arg(st.framesSeen).arg(st.readMs)
                 .arg(st.readMs > 0 ? 1000.0 * (st.framesSeen - 1) / double(st.readMs) : 0.0, 0, 'f', 1).arg(st.maxResponseMs));
    auto stopReply = std::exchange(stopReply_, {});
    auto cancelReply = std::exchange(cancelReply_, {});
    if (cancelRequested_) {
        removeFiles();
        state_ = State::Cancelled;
        if (stopReply) stopReply({"cancelled", "Scroll capture canceled", {}}, {});
        if (cancelReply) cancelReply({}, {});
        return;
    }
    if (outcome->nothing) {
        // Stop before the first frame was stitched: there is no image to keep.
        removeFiles();
        state_ = State::Cancelled;
        emit event({{"event", "cancelled"}});
        if (stopReply) stopReply({"cancelled", "The capture was stopped before anything was captured", {}}, {});
        return;
    }
    if (outcome->error) {
        removeFiles();
        state_ = State::Failed;
        emit event({{"event", "failed"}, {"error", outcome->error.toJson()}});
        if (stopReply) stopReply(outcome->error, {});
        return;
    }
    state_ = State::Done;
    finished_ = outcome->descriptor;
    QJsonObject finished = finished_;
    finished["event"] = "finished";
    emit event(finished);
    if (stopReply) stopReply({}, finished_);
}

// The capture itself, off the main thread: the first frame, the session, the
// pointer's way back, then the spool as one PNG.
void ScrollCapture::work(std::shared_ptr<Job> job) {
    auto outcome = std::make_shared<Outcome>();
    outcome->job = job.get();
    ScrollCapture* owner = job->owner;
    const auto report = [&] { QMetaObject::invokeMethod(owner, [owner, outcome] { owner->finish(outcome); }, Qt::QueuedConnection); };
    // The first frame gives the stream's size: the rectangle at its output's scale.
    std::optional<RunFrame> first;
    const int64_t started = FrameTap::nowMs();
    while (!first && !job->stop.load() && !job->tap->lost() && FrameTap::nowMs() - started < firstFrameMs) first = job->tap->nextFrame(100);
    SessionStats stats;
    SessionEnd end = SessionEnd::NoFrames;
    int width = 0, applied = 0;
    bool spoolFailed = false;
    SpoolCanvas canvas;
    int spoolFd = -1;
    if (first) {
        width = first->view.width;
        const int height = first->view.height;
        job->gate.injector->setArea(job->rect, QSize(width, height));
        applied = job->maxHeight;
        if (job->maxPixels > 0) applied = int(std::min<qint64>(applied, job->maxPixels / width));
        applied = std::max(applied, scrollMinEdge);
        const QRect& r = job->rect;
        const double sx = double(width) / r.width(), sy = double(height) / r.height();
        job->gate.sx = sx;
        job->gate.sy = sy;
        // The bar inside the rectangle (#1519): the frames' rows from just above it down are not the page's.
        int frameRows = 0;
        if (!job->insideBar.isEmpty()) frameRows = std::clamp(int(std::floor((job->insideBar.top() - r.top() - insideBarMargin()) * sy)), 1, height);
        // Room at both ends: the cap's rows above the first frame and below it, and a frame besides.
        spoolFd = createPrivate(job->spoolPath);
        if (spoolFd < 0 || !canvas.open(spoolFd, width, qint64(applied) + height, qint64(applied) + 2LL * height)) spoolFailed = true;
        SessionOptions options;
        options.tracker.maxOutputRows = applied;
        options.autoScroll = job->autoStart;
        options.fromTop = job->fromTop;
        options.frameRows = frameRows;
        options.pointer = true;
        options.entryInset = std::max(24, int(std::lround(48 * sy)));
        // Hyprland paints the pointer into the frames whatever was asked: the
        // frames of a wheel step's visit are not the page's.
        options.blindVisits = job->paintedPointer;
        options.paused = [job] { return job->gate.paused.load(); };
        options.request = [job] { return std::pair<unsigned, bool>(job->autoSerial.load(), job->wantAuto.load()); };
        options.activity = [job] { return job->touches.load(); };
        // Before each run of auto-scroll: the rest and the watch (#1514) on the
        // main thread, then the rest in stream pixels.
        unsigned asked = 0;
        options.beforeAuto = [job, &asked, r, sx, sy, width, height]() -> std::optional<RunPoint> {
            const unsigned serial = ++asked;
            job->prepareAuto(serial);
            const int64_t since = FrameTap::nowMs();
            while (job->prepared.load() != serial && !job->stop.load() && FrameTap::nowMs() - since < 1000) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            // Mapped by the compositor before the first injection.
            if (surfaceMode() == SurfaceMode::LayerShell) std::this_thread::sleep_for(std::chrono::milliseconds(150));
            const auto stream = [&](const QPoint& p) { return RunPoint{int(std::lround((p.x() - r.left()) * sx)), int(std::lround((p.y() - r.top()) * sy))}; };
            if (job->rest) {
                const RunPoint away = stream(*job->rest);
                job->gate.restAt = QPoint(away.x, away.y);
                return away;
            }
            job->gate.restAt.reset();
            // No surface of the helper's to rest on: a place outside the rectangle
            // all the same, where nobody watches for the user's motion.
            for (const QPoint& candidate : {QPoint(r.left() - awayMargin, r.top() + r.height() / 3), QPoint(r.right() + 1 + awayMargin, r.top() + r.height() / 3),
                                            QPoint(r.center().x(), r.top() - awayMargin), QPoint(r.center().x(), r.bottom() + 1 + awayMargin)})
                if (job->screen.contains(candidate) && !job->bar.contains(candidate)) return stream(candidate);
            // A painted arrow hangs down and right of its tip: off the screen in the bottom right corner.
            if (job->paintedPointer) return RunPoint{width - 1, height - 1};
            return std::nullopt;
        };
        options.onProgress = [owner, job](const SessionProgress& p) {
            QJsonObject o{{"state", QString::fromLatin1(toString(p.state))}, {"driver", QString::fromLatin1(toString(p.driver))}, {"pausedBy", QString::fromLatin1(toString(p.pausedBy))},
                          {"height", double(p.height)}, {"placed", p.placed}, {"skipped", p.skipped}, {"steps", p.steps}, {"pixelsPerNotch", std::round(p.pixelsPerNotch * 10) / 10},
                          {"picture", p.picture}, {"serial", double(p.serial)}};
            QMetaObject::invokeMethod(owner, [owner, job, o] { if (owner->job_ == job) owner->sessionProgress(o); }, Qt::QueuedConnection);
        };
        // Geometry of every frame's result for the lab log; never pixels.
        if (debugLogEnabled()) {
            options.onFrame = [](const scroll::TrackResult& tr, const scroll::TrackerHint& hint, int64_t t) {
                debugLog(QStringLiteral("scroll track: t=%1 %2%3 offset %4 position %5 added %6/%7 confidence %8 hint %9 state %10%11")
                             .arg(t % 1000000).arg(QString::fromLatin1(scroll::toString(tr.status)))
                             .arg(tr.reason == scroll::SkipReason::None ? QString() : QStringLiteral(" ") + QString::fromLatin1(scroll::toString(tr.reason)))
                             .arg(tr.offset).arg(tr.position).arg(tr.addedTop).arg(tr.addedBottom).arg(tr.confidence, 0, 'f', 2)
                             .arg(hint.known ? QStringLiteral("%1±%2").arg(hint.offset).arg(hint.uncertainty) : QStringLiteral("none"))
                             .arg(QString::fromLatin1(scroll::toString(tr.state))).arg(tr.recovered ? QStringLiteral(" recovered") : QString()));
            };
            options.onStep = [](int notches, RunPoint at) { debugLog(QStringLiteral("scroll step: %1 notches at %2,%3").arg(notches).arg(at.x).arg(at.y)); };
        }
        debugLog(QStringLiteral("scroll: frames %1x%2 for %3x%4, cap %5 rows, page rows %6, driver %7").arg(width).arg(height).arg(r.width()).arg(r.height()).arg(applied)
                     .arg(frameRows > 0 ? frameRows : height).arg(job->autoStart ? QStringLiteral("putgd") : QStringLiteral("user")));
        Primed frames(*job->tap, *first, job->sourceGone);
        SteadyClock clock;
        ScrollSession session(job->gate, frames, clock, canvas, options);
        if (!spoolFailed) end = session.run(job->stop, job->discard);
        stats = session.stats();
        if (canvas.failed()) spoolFailed = true;
        // X11: the pointer goes back where the user had it, unless the pipe
        // closed or the user has taken it (it then stays where they put it).
        if (job->gate.armed.load() && stats.steps > 0) job->gate.injector->restorePointer();
    } else if (job->stop.load()) end = SessionEnd::Finished;
    else if (job->tap->lost()) end = SessionEnd::SourceLost;
    // Nothing is injected from here on.
    job->gate.armed.store(false);
    // GNOME: the session was ended from the Shell's indicator. That is Done.
    if ((end == SessionEnd::InjectorFailed || end == SessionEnd::SourceLost) && job->endedByDesktop && job->endedByDesktop()) end = SessionEnd::Finished;
    if (job->layoutChanged.load()) end = SessionEnd::FrameSizeChanged;
    outcome->end = end;
    outcome->stats = stats;
    if (first && stats.topAsked)
        debugLog(QStringLiteral("scroll: way to the top %1 steps, %2 ms, %3%4").arg(stats.toTopSteps).arg(stats.toTopMs).arg(stats.topReached ? QStringLiteral("top proved") : QStringLiteral("top not proved"))
                     .arg(stats.toTopGaveUp.empty() ? QString() : QStringLiteral(" (%1)").arg(QString::fromStdString(stats.toTopGaveUp))));
    const bool kept = end == SessionEnd::Finished || end == SessionEnd::Idle || end == SessionEnd::PageEnd || end == SessionEnd::Capped;
    // What ended it (#1515). end_of_content: auto-scroll proved the end of the
    // page and no frame was skipped. A capture finished by the user is whole
    // as far as they scrolled, unless it ended lost or at the cap.
    QString reason;
    if (end == SessionEnd::Capped || stats.capReached) reason = QStringLiteral("height_cap");
    else if (end == SessionEnd::PageEnd && stats.skipped == 0) reason = stats.topAsked && !stats.topReached ? QStringLiteral("top_not_reached") : QStringLiteral("end_of_content");
    else if (stats.endedLost) reason = QStringLiteral("lost_track");
    else reason = QStringLiteral("done");
    const bool truncated = reason == "height_cap" || reason == "lost_track" || reason == "top_not_reached";
    const qint64 rows = canvas.bottom() - canvas.top();
    if (spoolFailed) outcome->error = {"encoding_failed", "The capture could not be written to its staging file", {{"stage", "capture"}, {"check", "spool"}}};
    else if (end == SessionEnd::Discarded) {}
    else if (end == SessionEnd::Finished && (stats.placed == 0 || rows <= 0)) outcome->nothing = true;
    else if (!kept) {
        switch (end) {
            case SessionEnd::InjectorFailed: outcome->error = unavailable(QStringLiteral("injection_unavailable"), QStringLiteral("The desktop refused the scroll input")); outcome->error.details["stage"] = "capture"; break;
            case SessionEnd::NotReady: outcome->error = {"capture_timeout", "A modifier key stayed down, so no wheel step was sent", {{"stage", "capture"}, {"check", "modifiers_released"}}}; break;
            case SessionEnd::FrameSizeChanged: outcome->error = {"display_changed", "The display changed during the capture", {{"stage", "capture"}}}; break;
            case SessionEnd::SourceLost: outcome->error = unavailable(QStringLiteral("frame_source_unavailable"), QStringLiteral("The frame stream ended during the capture")); outcome->error.details["stage"] = "capture"; break;
            default: outcome->error = {"capture_timeout", "The frame stream delivered no frame", {{"stage", "capture"}, {"check", "first_frame"}}}; break;
        }
    } else if (!job->discard.load()) {
        // The spool as one PNG, straight from the file's pages.
        const qint64 raw = rows * qint64(width) * 4;
        const uchar* pixels = rows > 0 ? canvas.image() : nullptr;
        if (!pixels) outcome->error = {"encoding_failed", "The capture's staging file could not be read", {{"stage", "encode"}, {"check", "spool"}}};
        else {
            const QImage image(pixels, width, int(rows), qsizetype(width) * 4, QImage::Format_RGB32);
            BoundedFile png(raw + raw / 1000 + rows + 65536, job->discard);
            const int fd = createPrivate(job->pngPath);
            bool written = fd >= 0 && png.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle);
            if (fd >= 0 && !written) ::close(fd);
            if (written) {
                QImageWriter writer(&png, "png");
                // A fast zlib level: a 30,000-row image must encode inside the stop deadline.
                writer.setQuality(50);
                written = writer.write(image) && png.flush();
            }
            const qint64 bytes = png.size();
            png.close();
            QCryptographicHash hash(QCryptographicHash::Sha256);
            QFile check(job->pngPath);
            if (written && !job->discard.load() && check.open(QIODevice::ReadOnly) && hash.addData(&check) && check.size() == bytes) {
                const QString scrolledBy = stats.movedByUser > 0 && stats.movedByPutgd > 0 ? QStringLiteral("both") : stats.movedByPutgd > 0 ? QStringLiteral("putgd") : stats.movedByUser > 0 ? QStringLiteral("user") : QStringLiteral("none");
                const double fps = stats.readMs > 0 ? std::round(10000.0 * (stats.framesSeen - 1) / double(stats.readMs)) / 10 : 0;
                outcome->descriptor = {{"path", job->pngPath}, {"bytes", double(bytes)}, {"sha256", QString::fromLatin1(hash.result().toHex())}, {"width", width}, {"height", int(rows)},
                    {"endReason", reason}, {"truncated", truncated}, {"appliedMaxHeight", applied},
                    {"scrolledBy", scrolledBy}, {"framesPlaced", stats.placed}, {"framesSkipped", stats.skipped}, {"endedLost", stats.endedLost}, {"reachedEnd", stats.reachedEnd},
                    {"stats", QJsonObject{{"steps", stats.steps}, {"pixelsPerNotch", std::round(stats.pxPerNotch * 10) / 10}, {"lowConfidenceSteps", stats.byExpectation + stats.onHint}, {"elapsedMs", double(stats.elapsedMs)},
                                          {"frames", stats.framesSeen}, {"recovered", stats.recovered}, {"pinnedRegions", stats.pinnedRegions}, {"framesPerSecond", fps}}}};
                if (end == SessionEnd::Idle) outcome->descriptor["note"] = QStringLiteral("nobody scrolled or used the bar for %1 minutes, so the capture ended there").arg(SessionOptions{}.idleLimitMs / 60000);
                else if (reason == "lost_track") outcome->descriptor["note"] = QStringLiteral("the capture was finished while put.gd could not find its place on the page; the image ends where it last could");
                if (stats.topAsked && !stats.topReached) {
                    // Whatever ended the capture: its first row is not the page's.
                    outcome->descriptor["topReached"] = false;
                    if (reason == "top_not_reached" && !stats.toTopGaveUp.empty()) outcome->descriptor["note"] = QString::fromStdString(stats.toTopGaveUp);
                }
            } else if (!job->discard.load()) outcome->error = {png.exceeded() ? "input_excessive" : "encoding_failed", png.exceeded() ? "The PNG exceeded its size bound" : "The PNG could not be written", {{"stage", "encode"}, {"check", "png"}}};
        }
    }
    canvas.close();
    if (spoolFd >= 0) ::close(spoolFd);
    QFile::remove(job->spoolPath);
    if (outcome->error || outcome->nothing || job->discard.load()) QFile::remove(job->pngPath);
    report();
}
}
