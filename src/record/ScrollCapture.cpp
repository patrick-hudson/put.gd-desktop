#include "ScrollCapture.h"
#include "DebugLog.h"
#include "ScrollInjectors.h"
#include "ScrollRun.h"
#include "X11ScrollInjector.h"
#include "X11Targets.h"
#include "core/ScrollStitcher.h"
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
const char* endReason(RunResult result) {
    switch (result) {
        case RunResult::EndOfContent: return "end_of_content";
        case RunResult::TopNotReached: return "top_not_reached";
        case RunResult::LimitReached: return "height_cap";
        case RunResult::Stopped: return "stopped";
        case RunResult::TimeLimit: return "time_limit";
        case RunResult::LostTrack: return "lost_track";
        default: return nullptr;
    }
}
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
        resting.store(toRest);
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
    // (desktop coordinates); unset where it has no such place (#1514).
    std::optional<QPoint> rest;
    int maxHeight = 0;
    qint64 maxPixels = 0;
    bool fromTop = false;
    // The frames may show the mouse pointer although none was asked for (ext-image-copy-capture).
    bool paintedPointer = false;
    std::function<bool()> endedByDesktop;
    std::atomic<bool> stop{false}, discard{false}, sourceGone{false}, layoutChanged{false};
};
struct ScrollCapture::Outcome {
    const Job* job = nullptr;
    RunResult result = RunResult::NoFrames;
    RunStats stats;
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
    restWindow_.clear();
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
    if (command == "pause" || command == "resume") {
        if (state_ != State::Capturing || !job_ || stopRequested_ || cancelRequested_) { reply(stateError("Nothing is being captured"), {}); return; }
        if (command == "pause") pause(QStringLiteral("request"));
        else if (!job_->gate.paused.load()) { reply(stateError("The capture is not paused"), {}); return; }
        else resumeNow();
        reply({}, pauseView());
        return;
    }
    if (command == "stop") { stop(std::move(reply)); return; }
    if (command == "cancel") { cancel(std::move(reply)); return; }
    if (command == "overlay") {
        // GNOME: the Shell could not place the bar outside the rectangle.
        if (params.contains("visible")) barHidden_ = !params["visible"].toBool(true);
        // GNOME: where the Shell put the bar, so the pointer can rest on it.
        if (params.contains("rect")) shellBar_ = rectFromJson(params["rect"]);
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
    if ((selection != "helper" && selection != "shell") || (params.contains("rect") && rectFromJson(params["rect"]).isEmpty()) || !std::isfinite(maxHeight) || maxHeight < 1 || !std::isfinite(maxPixels) || maxPixels < 0) {
        reply({"invalid_request", "Invalid scroll capture request", {}}, {}); return;
    }
    id_ = params["id"].toString();
    staging_ = staging;
    frames_ = probe["frames"].toString();
    selectionMode_ = selection;
    maxHeight_ = int(std::clamp(maxHeight, double(scrollMinEdge), double(scrollMaxHeight)));
    maxPixels_ = qint64(std::min(maxPixels, 4e12));
    fromTop_ = params["fromTop"].toBool(true);
    autoStart_ = params["autoStart"].toBool(true);
    debugLog(QStringLiteral("scroll: prepare frames %1 injector %2 maxHeight %3 fromTop %4 autoStart %5").arg(frames_, probe["injector"].toString()).arg(maxHeight_).arg(fromTop_).arg(autoStart_));
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
    connect(bar_, &ScrollBar::stopRequested, this, [this] { stop([](Error, QJsonObject) {}); });
    connect(bar_, &ScrollBar::resumeRequested, this, [this] { resume(true); });
    // The event follows the worker's end: nothing is injected after it.
    connect(bar_, &ScrollBar::cancelRequested, this, [this] { cancel([this](Error, QJsonObject) { emit event({{"event", "cancelled"}}); }); });
    barOutside_ = bar_->place(rect_);
    state_ = State::Ready;
    showBar();
    return {};
}

// The bar is never inside the captured rectangle while frames are read. X11
// and layer-shell: it shows only where a spot outside exists. GNOME: the Shell
// moves it, which needs it on screen first, so it shows when the capture does
// not start by itself (the resident places it, then starts), and the resident
// hides it (`overlay`) when the Shell found no spot outside.
void ScrollCapture::showBar() {
    if (!bar_) return;
    const bool live = state_ == State::Ready || state_ == State::Starting || state_ == State::Capturing;
    const bool allowed = surfaceMode() == SurfaceMode::Compositor ? !autoStart_ : barOutside_;
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
    QPointer<ScrollCapture> guard(this);
    job_->gate.tookPointer = [guard] { QMetaObject::invokeMethod(guard, [guard] { if (guard) guard->pause(QStringLiteral("pointer")); }, Qt::QueuedConnection); };
    state_ = State::Capturing;
    pausedBy_.clear();
    progressState_ = fromTop_ ? QStringLiteral("scrollingToTop") : QStringLiteral("capturing");
    lastProgress_ = {};
    if (frames_ == "kde-screencast" && surfaceMode() == SurfaceMode::LayerShell) {
        bounds_.showHeartbeat(screen_.topLeft());
        heartbeat_.start(heartbeatMs);
    }
    if (bar_) bar_->setState(progressState_);
    showBar();
    placeRest();
    debugLog(QStringLiteral("scroll: capturing %1,%2 %3x%4 with %5").arg(rect_.x()).arg(rect_.y()).arg(rect_.width()).arg(rect_.height()).arg(injector_->name()));
    worker_ = std::thread(&ScrollCapture::work, job_);
    // Escape on X11 is Stop: the image so far is kept.
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

// The user moved the mouse, or an agent asked (#1514). The gate closes at once;
// the loop sees it before its next injection and waits where it is.
void ScrollCapture::pause(const QString& by) {
    if (!job_ || state_ != State::Capturing || stopRequested_ || cancelRequested_) return;
    job_->gate.paused.store(true);
    resumePending_ = false;
    resumeQuiet_.stop();
    // Paused already: it keeps the reason it was paused for.
    if (!pausedBy_.isEmpty()) { if (bar_) bar_->setPaused(pausedBy_); return; }
    pausedBy_ = by;
    debugLog(QStringLiteral("scroll: paused by %1 at t=%2").arg(by).arg(FrameTap::nowMs() % 1000000));
    if (bar_) bar_->setPaused(by);
    emitProgress();
}
// The bar's Resume waits until the hand is off the mouse: the click itself
// comes with motion, and a pause the moment the capture goes on helps nobody.
void ScrollCapture::resume(bool quiet) {
    if (!job_ || !job_->gate.paused.load() || state_ != State::Capturing || stopRequested_ || cancelRequested_) return;
    if (!quiet) { resumeNow(); return; }
    resumePending_ = true;
    if (bar_) bar_->setState(QStringLiteral("resuming"));
    resumeQuiet_.start(resumeQuietMs);
}
void ScrollCapture::resumeNow() {
    if (!job_ || !job_->gate.paused.load()) return;
    resumePending_ = false;
    resumeQuiet_.stop();
    pausedBy_.clear();
    // The pointer is wherever the user left it: the capture's next motion is
    // its own, and the user is told from the capture again once it has arrived.
    job_->gate.detecting.store(false);
    job_->gate.resting.store(false);
    job_->gate.visits.store(0);
    job_->gate.skipCheck.store(true);
    job_->gate.paused.store(false);
    debugLog(QStringLiteral("scroll: resumed at t=%1").arg(FrameTap::nowMs() % 1000000));
    if (bar_) bar_->setState(progressState_);
    emitProgress();
}
QJsonObject ScrollCapture::pauseView() const {
    QJsonObject view{{"state", "capturing"}, {"paused", !pausedBy_.isEmpty()}};
    if (!pausedBy_.isEmpty()) view["pausedBy"] = pausedBy_;
    return view;
}
void ScrollCapture::emitProgress() {
    QJsonObject progress = lastProgress_;
    progress["event"] = "progress";
    progress["state"] = pausedBy_.isEmpty() ? progressState_ : QStringLiteral("paused");
    for (auto i = progress.begin(); i != progress.end();) { if (i.key() == "pausedBy") i = progress.erase(i); else ++i; }
    const auto view = pauseView();
    for (auto i = view.begin(); i != view.end(); ++i) if (i.key() != "state") progress[i.key()] = i.value();
    if (!progress.contains("height")) { progress["height"] = 0; progress["steps"] = 0; progress["pixelsPerNotch"] = 0; }
    emit event(progress);
}

// Where the pointer rests between wheel steps (#1514): on the bar where it is
// beside the rectangle, in a margin of the bar that holds no button and on its
// edge nearest the rectangle, so the way to the page is short (Plasma's
// "shake cursor", ScrollRun::wheelPoint); else at the old rest spot, 16 px
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
    const auto clear = [&](const QPoint& p) { return !guard.contains(p) && (!painted || !QRect(p.x() - 2, p.y() - 2, 26, 34).intersects(guard)); };
    QWidget* surface = nullptr;
    QRect place;
    std::optional<QPoint> rest;
    const bool barShown = bar_ && bar_->isVisible() && !barHidden_;
    const QRect bar = mode == SurfaceMode::Compositor ? shellBar_ : barShown && barOutside_ ? bar_->geometry() : QRect();
    if (barShown && !bar.isEmpty() && !bar.intersects(capture)) {
        // The margins above and below the bar's controls (Ui::Space::s).
        const int inset = 3;
        qint64 best = -1;
        for (const int y : {bar.top() + inset, bar.bottom() - inset})
            for (int x = bar.left() + 6; x <= bar.right() - 6; x += 4) {
                const QPoint p(x, y);
                if (!clear(p)) continue;
                const int dx = std::max({capture.left() - x, 0, x - capture.right()}), dy = std::max({capture.top() - y, 0, y - capture.bottom()});
                const qint64 d = qint64(dx) * dx + qint64(dy) * dy;
                if (best < 0 || d < best) { best = d; rest = p; }
            }
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
            auto* w = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus);
            w->setObjectName("putgd-scroll-rest");
            w->setAttribute(Qt::WA_TranslucentBackground);
            w->setAttribute(Qt::WA_ShowWithoutActivating);
            w->setAttribute(Qt::WA_NoSystemBackground);
            w->setAttribute(Qt::WA_TransparentForMouseEvents, false);
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
    restWindow_ = surface->windowHandle();
    job_->rest = mode == SurfaceMode::X11 ? barRootPoint(QPointF(*rest)) : *rest;
    debugLog(QStringLiteral("scroll: rest on the %1 at %2,%3 (local %4,%5)").arg(surface == bar_ ? QStringLiteral("bar") : QStringLiteral("pad")).arg(job_->rest->x()).arg(job_->rest->y()).arg(restLocal_.x()).arg(restLocal_.y()));
}

// A pointer event of the rest surface (at: where; none: it left). The
// capture's own: arriving at the rest spot, leaving for a visit it sent.
// Anything else is the user.
void ScrollCapture::pointerSeen(std::optional<QPointF> at) {
    if (!job_ || state_ != State::Capturing || stopRequested_ || cancelRequested_) return;
    if (resumePending_) { resumeQuiet_.start(resumeQuietMs); return; }
    if (job_->gate.paused.load()) return;
    auto& gate = job_->gate;
    if (at) {
        if (QLineF(*at, restLocal_).length() <= restTolerance) {
            if (gate.resting.load()) {
                if (!gate.detecting.exchange(true)) debugLog(QStringLiteral("scroll: pointer at rest, watching from t=%1").arg(FrameTap::nowMs() % 1000000));
                // A leave that never came is not waited for.
                if (gate.visits.load() > 0 && FrameTap::nowMs() - gate.visitAt.load() > visitLateMs) gate.visits.store(0);
            }
            return;
        }
        if (!gate.detecting.load()) return;
        debugLog(QStringLiteral("scroll: pointer at %1,%2 on the rest surface, not %3,%4").arg(at->x()).arg(at->y()).arg(restLocal_.x()).arg(restLocal_.y()));
        pause(QStringLiteral("pointer"));
        return;
    }
    if (!gate.detecting.load()) return;
    if (gate.visits.load() > 0) { gate.visits.fetch_sub(1); return; }
    debugLog(QStringLiteral("scroll: pointer left the rest surface with no visit sent"));
    pause(QStringLiteral("pointer"));
}
bool ScrollCapture::eventFilter(QObject* object, QEvent* e) {
    if (restWindow_ && object == restWindow_.data()) {
        switch (e->type()) {
            case QEvent::MouseMove: pointerSeen(static_cast<QMouseEvent*>(e)->position()); break;
            case QEvent::Enter: pointerSeen(static_cast<QEnterEvent*>(e)->position()); break;
            case QEvent::Leave: pointerSeen(std::nullopt); break;
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
    debugLog(QStringLiteral("scroll: ended %1, %2 rows, %3 steps, %4 px per notch, %5 ms%6").arg(QString::fromLatin1(toString(outcome->result))).arg(outcome->stats.outputRows).arg(outcome->stats.steps).arg(outcome->stats.pxPerNotch).arg(outcome->stats.elapsedMs)
                 .arg(outcome->stats.lastError.empty() ? QString() : QStringLiteral(" (%1)").arg(QString::fromStdString(outcome->stats.lastError))));
    // How the frame source behaved: fresh frames asked for, those that showed
    // movement the stream had not sent, still steps left unproven, the slowest response.
    debugLog(QStringLiteral("scroll: source fresh %1, quiet %2, unproven %3, slowest response %4 ms, retries %5, scroll-backs %6").arg(outcome->stats.freshFrames).arg(outcome->stats.quietStream).arg(outcome->stats.unprovenStill).arg(outcome->stats.maxResponseMs).arg(outcome->stats.retries).arg(outcome->stats.scrollBacks));
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

// The capture itself, off the main thread: the first frame, the loop, the
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
    RunStats stats;
    RunResult result = RunResult::NoFrames;
    int width = 0, applied = 0;
    qint64 rows = 0;
    bool spoolFailed = false;
    QFile spool;
    if (first) {
        width = first->view.width;
        const int height = first->view.height;
        job->gate.injector->setArea(job->rect, QSize(width, height));
        applied = job->maxHeight;
        if (job->maxPixels > 0) applied = int(std::min<qint64>(applied, job->maxPixels / width));
        applied = std::max(applied, scrollMinEdge);
        const int fd = createPrivate(job->spoolPath);
        if (fd < 0 || !spool.open(fd, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle)) { if (fd >= 0) ::close(fd); spoolFailed = true; }
        RunOptions options;
        options.stitch.maxOutputRows = applied;
        options.pointer = true;
        options.fromTop = job->fromTop;
        options.discarded = [job] { return job->discard.load(); };
        // Rule 1: a place outside the rectangle, on its output and off the bar,
        // for the pointer while the first frame is read. Stream pixels.
        const double sx = double(width) / job->rect.width(), sy = double(height) / job->rect.height();
        const QRect& r = job->rect;
        // #1514: on the helper's own surface, where the user's motion is seen.
        if (job->rest) {
            options.away = RunPoint{int(std::lround((job->rest->x() - r.left()) * sx)), int(std::lround((job->rest->y() - r.top()) * sy))};
            job->gate.restAt = QPoint(options.away->x, options.away->y);
        }
        if (!job->rest) for (const QPoint& candidate : {QPoint(r.left() - awayMargin, r.top() + r.height() / 3), QPoint(r.right() + 1 + awayMargin, r.top() + r.height() / 3),
                                        QPoint(r.center().x(), r.top() - awayMargin), QPoint(r.center().x(), r.bottom() + 1 + awayMargin)}) {
            if (!job->screen.contains(candidate) || job->bar.contains(candidate)) continue;
            options.away = RunPoint{int(std::lround((candidate.x() - r.left()) * sx)), int(std::lround((candidate.y() - r.top()) * sy))};
            break;
        }
        // The pointer waits outside the rectangle while frames are read, on
        // every desktop, and what it called up before the capture gets time
        // to go (a browser hides its link panel a quarter of a second after
        // the pointer has left, and fades it out).
        options.restAway = true;
        options.awayLingerMs = 450;
        options.leaveDelayMs = 30;
        options.entryInset = std::max(24, int(std::lround(48 * sy)));
        // A page's own bar that slides in over the bottom on the way becomes the footer.
        options.stitch.lateBarIsFooter = true;
        if (job->paintedPointer) {
            // A compositor that paints the pointer into the frames whatever was
            // asked (Hyprland 0.56 did; lab guest of 2026-11-10): rows are kept
            // further below the parked pointer than a hover effect needs, and
            // with no place outside the rectangle the first frame is read with
            // the pointer in the bottom right corner, where an arrow lies off
            // the screen.
            options.hoverMarginRows = std::max(options.hoverMarginRows, int(std::lround(48 * sy)));
            if (!options.away) options.away = RunPoint{width - 1, height - 1};
            // And Stop on the way to the top keeps the viewport as it is then:
            // the pointer waits outside between the wheel steps up.
            options.restAwayToTop = true;
        }
        options.paused = [job] { return job->gate.paused.load(); };
        options.onProgress = [owner, job](const RunProgress& progress) {
            QMetaObject::invokeMethod(owner, [owner, job, progress] {
                if (owner->job_ != job || owner->state_ != State::Capturing || owner->stopRequested_ || owner->cancelRequested_) return;
                owner->progressState_ = progress.scrollingToTop ? QStringLiteral("scrollingToTop") : QStringLiteral("capturing");
                owner->lastProgress_ = {{"height", progress.height}, {"steps", progress.steps}, {"pixelsPerNotch", progress.pixelsPerNotch}};
                if (owner->bar_) owner->bar_->setHeight(progress.height);
                // Paused: the bar says so until Resume, and so does the state an agent reads.
                if (!owner->pausedBy_.isEmpty()) return;
                if (owner->bar_ && !owner->resumePending_) owner->bar_->setState(owner->progressState_);
                owner->emitProgress();
            }, Qt::QueuedConnection);
        };
        // Geometry of each stitch attempt for the lab log; never pixels.
        options.onStep = [](const scroll::StepResult& step, int hint, int notches) {
            debugLog(QStringLiteral("scroll step: %1 offset %2 hint %3 notches %4 score %5 evidence %6 appended %7 fixed %8/%9%10").arg(QString::fromLatin1(scroll::toString(step.status))).arg(step.offset).arg(hint).arg(notches)
                         .arg(step.score, 0, 'f', 2).arg(step.evidenceRows).arg(step.appendedRows).arg(step.fixedTop).arg(step.fixedBottom).arg(step.lowConfidence ? QStringLiteral(" low-confidence") : QString()));
        };
        debugLog(QStringLiteral("scroll: frames %1x%2 for %3x%4, cap %5 rows, away %6").arg(width).arg(height).arg(r.width()).arg(r.height()).arg(applied).arg(options.away ? QStringLiteral("%1,%2").arg(options.away->x).arg(options.away->y) : QStringLiteral("none")));
        Primed frames(*job->tap, *first, job->sourceGone);
        SteadyClock clock;
        const scroll::RowSink sink = [&](const uint8_t* row, int w) {
            const qint64 bytes = qint64(w) * 4;
            if (spoolFailed || spool.write(reinterpret_cast<const char*>(row), bytes) != bytes) spoolFailed = true;
            else ++rows;
        };
        // The layout rule takes the last rows back (ScrollStitcher::barVerdict).
        const scroll::RowTakeBack takeBack = [&](long long taken) {
            rows = std::max<qint64>(0, rows - taken);
            if (!spoolFailed && (!spool.resize(rows * qint64(width) * 4) || !spool.seek(rows * qint64(width) * 4))) spoolFailed = true;
        };
        ScrollRun run(job->gate, frames, clock, options, sink, takeBack);
        if (!spoolFailed) result = run.run(&job->stop);
        stats = run.stats();
        // X11: the pointer goes back where the user had it, unless the pipe
        // closed or the user has taken it (it then stays where they put it).
        if (job->gate.armed.load()) job->gate.injector->restorePointer();
        if (stats.pauses > 0) debugLog(QStringLiteral("scroll: %1 pauses, %2 ms paused%3").arg(stats.pauses).arg(stats.pausedMs).arg(stats.pauseExpired ? QStringLiteral(", the last one ended the capture") : QString()));
    } else if (job->stop.load()) result = RunResult::Stopped;
    else if (job->tap->lost()) result = RunResult::SourceLost;
    // Nothing is injected from here on.
    job->gate.armed.store(false);
    // GNOME: the session was ended from the Shell's indicator. That is Stop.
    if ((result == RunResult::InjectorFailed || result == RunResult::SourceLost) && job->endedByDesktop && job->endedByDesktop()) result = RunResult::Stopped;
    if (job->layoutChanged.load()) result = RunResult::LayoutChanged;
    outcome->result = result;
    outcome->stats = stats;
    const char* reason = endReason(result);
    // Scroll-to-top was asked for and the top was not proved: the image starts
    // where the page was then. A capture that then ran to the end of the
    // content is not the whole scroll area and does not say it is (the loop
    // ends it as TopNotReached); one that ended otherwise carries the mark.
    const bool topMissed = job->fromTop && first && !stats.reachedTop;
    if (first && job->fromTop)
        debugLog(QStringLiteral("scroll: way to the top %1 steps, %2 ms, %3%4").arg(stats.toTopSteps).arg(stats.toTopMs).arg(stats.reachedTop ? QStringLiteral("top proved") : QStringLiteral("top not proved"))
                     .arg(stats.toTopGaveUp.empty() ? QString() : QStringLiteral(" (%1)").arg(QString::fromStdString(stats.toTopGaveUp))));
    const QString detail = QString::fromStdString(stats.lastError);
    if (spoolFailed) outcome->error = {"encoding_failed", "The capture could not be written to its staging file", {{"stage", "capture"}, {"check", "spool"}}};
    else if (result == RunResult::Stopped && rows == 0) outcome->nothing = true;
    else if (!reason) {
        switch (result) {
            case RunResult::NoMovement: outcome->error = {"scroll_no_movement", "Nothing under the selection scrolled", {{"parkPositions", 3}}}; break;
            case RunResult::InjectorFailed: outcome->error = unavailable(QStringLiteral("injection_unavailable"), QStringLiteral("The desktop refused the scroll input")); outcome->error.details["stage"] = "capture"; break;
            case RunResult::NotReady: outcome->error = {"capture_timeout", "A modifier key stayed down, so no wheel step was sent", {{"stage", "capture"}, {"check", "modifiers_released"}}}; break;
            case RunResult::LayoutChanged: outcome->error = {"display_changed", "The display changed during the capture", {{"stage", "capture"}}}; break;
            case RunResult::SourceLost: outcome->error = unavailable(QStringLiteral("frame_source_unavailable"), QStringLiteral("The frame stream ended during the capture")); outcome->error.details["stage"] = "capture"; break;
            default: outcome->error = {"capture_timeout", "The frame stream delivered no frame", {{"stage", "capture"}, {"check", "first_frame"}}}; break;
        }
    } else if (!job->discard.load()) {
        // The spool as one PNG, straight from the file's pages.
        const qint64 raw = rows * qint64(width) * 4;
        uchar* pixels = rows > 0 && spool.flush() && spool.size() == raw ? spool.map(0, raw) : nullptr;
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
            spool.unmap(pixels);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            QFile check(job->pngPath);
            if (written && !job->discard.load() && check.open(QIODevice::ReadOnly) && hash.addData(&check) && check.size() == bytes) {
                outcome->descriptor = {{"path", job->pngPath}, {"bytes", double(bytes)}, {"sha256", QString::fromLatin1(hash.result().toHex())}, {"width", width}, {"height", int(rows)},
                    {"endReason", reason}, {"truncated", result == RunResult::LimitReached || result == RunResult::TopNotReached}, {"appliedMaxHeight", applied},
                    {"stats", QJsonObject{{"steps", stats.steps}, {"pixelsPerNotch", stats.pxPerNotch}, {"lowConfidenceSteps", stats.lowConfidenceSteps}, {"elapsedMs", double(stats.elapsedMs)}}}};
                if (!detail.isEmpty() && result == RunResult::LostTrack) outcome->descriptor["note"] = detail;
                if (result == RunResult::Stopped && stats.pauseExpired) outcome->descriptor["note"] = QStringLiteral("the capture was paused for %1 minutes and ended there").arg(RunOptions{}.pauseLimitMs / 60000);
                if (topMissed) {
                    // Whatever ended the capture (the end, the cap, Stop): its first row is not the page's.
                    outcome->descriptor["topReached"] = false;
                    if (result == RunResult::TopNotReached && !stats.toTopGaveUp.empty()) outcome->descriptor["note"] = QString::fromStdString(stats.toTopGaveUp);
                }
            } else if (!job->discard.load()) outcome->error = {png.exceeded() ? "input_excessive" : "encoding_failed", png.exceeded() ? "The PNG exceeded its size bound" : "The PNG could not be written", {{"stage", "encode"}, {"check", "png"}}};
        }
    }
    spool.close();
    QFile::remove(job->spoolPath);
    if (outcome->error || outcome->nothing || job->discard.load()) QFile::remove(job->pngPath);
    report();
}
}
