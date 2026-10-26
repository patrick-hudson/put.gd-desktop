#include "RecordSession.h"
#include "DebugLog.h"
#include "MediaProbe.h"
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QStandardPaths>
#include <QSaveFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <cmath>
#include <unistd.h>
#ifdef PUTGD_RECORD_LAYER_SHELL
#include "platform/kde/KdeRegionOverlay.h"
#endif

namespace putgd::record {
namespace {
QJsonArray sizeJson(const QSize& s) { return {s.width(), s.height()}; }
QJsonArray rectJson(const QRect& r) { return {r.x(), r.y(), r.width(), r.height()}; }
bool integer(const QJsonValue& value, qint64 min, qint64 max, qint64* out) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < double(min) || number > double(max)) return false;
    *out = qint64(number);
    return true;
}
// Encoder preference: hardware (VA) first, then x264, then OpenH264.
// The legacy vaapih264enc fails with ximagesrc on Ubuntu 26.04 and Mint 22.3; keep it last.
// GPU encoders first (the va plugin, then gstreamer-vaapi), CPU encoders after.
const QStringList videoEncoders{"vah264lpenc", "vah264enc", "vaapih264enc", "x264enc", "openh264enc"};
const QStringList audioEncoders{"avenc_aac", "fdkaacenc", "voaacenc"};
QStringList available(const QStringList& names) {
    QStringList result;
    for (const auto& name : names) if (GstElementFactory* f = gst_element_factory_find(name.toUtf8().constData())) { gst_object_unref(f); result.append(name); }
    return result;
}
// How long a bar collapsing to its handle gets to leave the screen before capture.
constexpr int handleSettleMs = 150;
QRect desktopRoot() {
    QRect desktop;
    for (QScreen* screen : QGuiApplication::screens()) {
        const qreal dpr = screen->devicePixelRatio();
        desktop |= QRect(QPoint(screen->geometry().topLeft()) * dpr, screen->geometry().size() * dpr);
    }
    return desktop;
}
}

RecordSession::RecordSession(QObject* parent) : QObject(parent), pipeline_(this), progress_(this), barTimer_(this), bounds_(this) {
    progress_.setInterval(1000);
    connect(&progress_, &QTimer::timeout, this, &RecordSession::emitProgress);
    barTimer_.setInterval(200);
    connect(&barTimer_, &QTimer::timeout, this, [this] { if (bar_ && pipeline_.running()) bar_->setRetained(pipeline_.retainedUs()); });
    connect(&pipeline_, &RecordingPipeline::failed, this, [this](Error error) {
        progress_.stop();
        barTimer_.stop();
        bounds_.hideAll();
        if (bar_) bar_->hide();
        emit event({{"event", "failed"}, {"error", error.toJson()}});
    });
    connect(&pipeline_, &RecordingPipeline::budgetReached, this, [this](QJsonObject details) {
        // The admitted byte budget is nearly spent: finish the clip and open review.
        showBar("stopping");
        pipeline_.stop([this, details](Error error, qint64 durationUs) {
            progress_.stop();
            barTimer_.stop();
            bounds_.hideAll();
            if (bar_) bar_->hide();
            // A bounded EOS timeout can leave an unindexed Matroska clip. For a
            // disk stop, verify it decodes before promising private review.
            if (!error && durationUs > 0 && !details.isEmpty()) {
                QImage frame;
                error = previewFrame(workingClip_, {}, config_, 0, 64, resolved_.canvas, frame);
            }
            if (error || durationUs < 1) {
                if (!error) error = {"recording_failed", "The clip could not be finalized", {}};
                if (!details.isEmpty()) {
                    // Keep a drain/decoder failure's actual diagnosis. Low space
                    // explains why we stopped, not necessarily why it failed.
                    error.details["stopDetails"] = details;
                    error.details["finalized"] = false;
                }
                emit event({{"event", "failed"}, {"error", error.toJson()}}); return;
            }
            durationUs_ = durationUs;
            recorded_ = true;
            openReview();
            emit event({{"event", "stopped"}, {"durationUs", double(durationUs)}, {"canvas", sizeJson(resolved_.canvas)}, {"reason", "budget"}, {"stopDetails", details}});
        });
    });
}
RecordSession::~RecordSession() {
    if (exportCancel_) exportCancel_->store(true);
    if (exportThread_) { exportThread_->wait(); delete exportThread_; }
    pipeline_.cancel();
    delete review_;
    delete bar_;
    delete selection_;
#ifdef PUTGD_RECORD_LAYER_SHELL
    delete kdeSelection_;
#endif
}
QString RecordSession::targetLabel() const {
    if (target_.kind == "region") return QStringLiteral("Region %1×%2").arg(resolved_.canvas.width()).arg(resolved_.canvas.height());
    if (target_.kind == "monitor") return QStringLiteral("Monitor %1").arg(target_.identity);
    if (target_.kind == "window") return QStringLiteral("Window %1×%2").arg(resolved_.canvas.width()).arg(resolved_.canvas.height());
    return QStringLiteral("Whole desktop");
}
void RecordSession::showBar(const QString& state) {
    if (!bar_) return;
    bar_->setState(state);
    if (barHidden_) return;
    bar_->show();
    bar_->raise();
}
void RecordSession::command(const QString& command, const QJsonObject& params, Reply reply) {
    if (command == "prepare") { prepare(params, std::move(reply)); return; }
    if (command == "select") { completeSelection(params, std::move(reply)); return; }
    if (!prepared_) { reply({"recording_state", "The helper has no prepared session", {}}, {}); return; }
    if (command == "start") { start(std::move(reply)); return; }
    if (command == "pause") { if (auto e = pipeline_.pause()) reply(e, {}); else { showBar("paused"); emitProgress(); reply({}, pipeline_.status()); } return; }
    if (command == "resume") { if (auto e = pipeline_.resume()) reply(e, {}); else { showBar("recording"); emitProgress(); reply({}, pipeline_.status()); } return; }
    if (command == "stop") { stop(std::move(reply)); return; }
    if (command == "cancel") { cancelLocal(); reply({}, {}); return; }
    if (command == "setEdit") {
        RecordingEdit edit;
        if (!parseEdit(params["edit"].toObject(), edit)) { reply({"invalid_request", "Invalid edit", {}}, {}); return; }
        auto configJson = params["config"].toObject();
        for (const auto* key : {"mime", "budgetBytes"}) configJson.remove(key);
        RecordingConfig config;
        if (!configJson.isEmpty() && !recording::parseConfig(configJson, config)) { config.budgetBytes = config_.budgetBytes; config_ = config; if (review_) review_->setConfig(config_); }
        if (review_) {
            const auto limits = params["limits"].toObject();
            if (!limits.isEmpty()) review_->setLimits(qint64(limits["animationSeconds"].toDouble()) * 1000000, limits["animationEdge"].toInt());
            review_->setEdit(edit);
        }
        reply({}, {});
        return;
    }
    if (command == "raise") {
        barHidden_ = false;
        if (bar_) { bar_->show(); bar_->raise(); }
        if (review_) review_->reopen(params["error"].toString());
        reply({}, {});
        return;
    }
    if (command == "notice") {
        // The resident's account of a stop the helper made itself (#225).
        if (review_) review_->setNotice(params["text"].toString().left(1000));
        reply({}, {});
        return;
    }
    if (command == "overlay") {
        // {visible} hides or shows the bar; {handle} (from the resident, when the
        // GNOME Shell had to leave the bar inside the area) collapses it from Record on.
        if (params["handle"].isBool() && bar_) bar_->setHandleWhileRecording(params["handle"].toBool());
        if (params.contains("visible")) { barHidden_ = !params["visible"].toBool(true); if (bar_) bar_->setVisible(!barHidden_); }
        reply({}, {{"visible", bar_ ? bar_->isVisible() : false}});
        return;
    }
    if (command == "review") {
        // #283: recording.review drives the review window as a user does.
        if (!review_) { reply({"recording_state", "The review window is not open", {}}, {}); return; }
        const auto result = review_->drive(params);
        if (!result.code.isEmpty()) reply({result.code, result.message, {}}, {});
        else reply({}, result.state);
        return;
    }
    if (command == "preview") { preview(params, std::move(reply)); return; }
    if (command == "export") { exportClip(params, std::move(reply)); return; }
    reply({"unknown_method", "Unknown recording helper command", {}}, {});
}
void RecordSession::cancelLocal() {
    cancelled_ = true;
    if (exportCancel_) exportCancel_->store(true);
    progress_.stop();
    barTimer_.stop();
    if (selection_) { selection_->disconnect(this); selection_->cancel(); selection_->deleteLater(); selection_ = nullptr; }
#ifdef PUTGD_RECORD_LAYER_SHELL
    if (kdeSelection_) { kdeSelection_->disconnect(this); kdeSelection_->cancel(); kdeSelection_->deleteLater(); kdeSelection_ = nullptr; }
#endif
    pipeline_.cancel();
    portal_.close();
    mutter_.close();
#ifdef PUTGD_RECORD_KDE_SCREENCAST
    kde_.close();
#endif
    if (portalFd_ >= 0) { ::close(portalFd_); portalFd_ = -1; }
    bounds_.hideAll();
    if (bar_) bar_->hide();
    if (review_) review_->hide();
    QFile::remove(workingClip_);
}
void RecordSession::prepare(const QJsonObject& params, Reply reply) {
    if (prepared_ || selection_ || kdeSelection_) { reply({"recording_state", "The helper already has a session", {}}, {}); return; }
    RecordingTarget target;
    RecordingConfig config;
    if (auto e = recording::parseTarget(params["target"], target)) { reply(e, {}); return; }
    auto configJson = params["config"].toObject();
    qint64 budget = 0;
    integer(configJson["budgetBytes"], 0, 1LL << 50, &budget);
    for (const auto* key : {"mime", "budgetBytes"}) configJson.remove(key);
    if (auto e = recording::parseConfig(configJson, config)) { reply(e, {}); return; }
    config.budgetBytes = budget;
    const auto staging = params["staging"].toString();
    if (staging.isEmpty() || !QFileInfo(staging).isDir()) { reply({"recording_io", "The staging directory is missing", {}}, {}); return; }
    id_ = params["id"].toString();
    target_ = target;
    reviewOptions_ = params["reviewOptions"].toObject();
    config_ = config;
    staging_ = staging;
    workingClip_ = staging + "/working.mkv";
    capabilities_ = probeMedia();
    if (target_.kind == "region" && target_.rect.isEmpty()) { select(std::move(reply)); return; }
    acquire(std::move(reply));
}
void RecordSession::acquire(Reply reply) {
    // A cancel that arrived while the selector was closing opens nothing.
    if (cancelled_) { reply({"cancelled", "Recording cancelled before the source opened", {}}, {}); return; }
    const auto acquisition = capabilities_["acquisition"].toString();
    if (acquisition == "kde-screencast" || acquisition == "mutter-screencast") { prepareNative(std::move(reply)); return; }
    if (acquisition == "portal") { preparePortal(std::move(reply)); return; }
    finishPrepare(std::move(reply));
}
void RecordSession::select(Reply reply) {
    // Release keeps the bounds and shows the controls; nothing streams or records
    // until the rectangle exists, so Escape leaves no compositor session behind.
    QPointer<RecordSession> guard(this);
    auto cancelled = [guard, reply](const Error& error) {
        if (!guard) return;
        reply(error.code == "cancelled" ? Error{"cancelled", "Region selection cancelled; nothing was recorded", {}} : error, {});
    };
    const auto mode = surfaceMode();
    if (mode == SurfaceMode::X11) {
        selection_ = new SelectionOverlay(this);
        connect(selection_, &SelectionOverlay::selected, this, [guard, reply](const QRect& root) {
            if (!guard) return;
            guard->selection_->deleteLater();
            guard->selection_ = nullptr;
            guard->target_.rect = root;
            guard->acquire(reply);
        });
        connect(selection_, &SelectionOverlay::cancelled, this, [guard, cancelled](const Error& error) {
            if (!guard) return;
            guard->selection_->deleteLater();
            guard->selection_ = nullptr;
            cancelled(error);
        });
        selection_->start();
        emit event({{"event", "selecting"}, {"selector", "x11"}});
        return;
    }
#ifdef PUTGD_RECORD_LAYER_SHELL
    if (mode == SurfaceMode::LayerShell) {
        // The KDE screenshot selector: crosshair and outline on overlay layer
        // surfaces, emitted only after KWin has rendered and unmapped them.
        QList<CaptureFrame> frames;
        for (QScreen* screen : QGuiApplication::screens()) {
            CaptureFrame frame;
            frame.identity = screen->name();
            frame.logicalRect = screen->geometry();
            frame.rotation = screen->angleBetween(screen->nativeOrientation(), screen->orientation());
            frames.append(frame);
        }
        kdeSelection_ = new KdeRegionOverlay(frames, this, true);
        connect(kdeSelection_, &KdeRegionOverlay::selected, this, [guard, reply](const QRectF& logical) {
            if (!guard || !guard->kdeSelection_) return;
            guard->kdeSelection_->deleteLater();
            guard->kdeSelection_ = nullptr;
            guard->target_.rect = logical.toAlignedRect();
            // Emitted from a Wayland callback: open the stream and the surfaces outside it.
            QTimer::singleShot(0, guard, [guard, reply] { if (guard) guard->acquire(reply); });
        });
        connect(kdeSelection_, &KdeRegionOverlay::cancelled, this, [guard, cancelled](const Error& error) {
            if (!guard || !guard->kdeSelection_) return;
            guard->kdeSelection_->deleteLater();
            guard->kdeSelection_ = nullptr;
            cancelled(error);
        });
        kdeSelection_->start();
        if (kdeSelection_) emit event({{"event", "selecting"}, {"selector", "kde-layer-shell"}});
        return;
    }
#endif
    // GNOME's selector lives in the Shell extension: the resident asks it for the
    // rectangle before the helper starts, and says itself why it cannot (#131).
    // Without one there is no honest region.
    reply({"region_unavailable", "This desktop has no region selector for recordings; pass target.rect", {}}, {});
}
void RecordSession::completeSelection(const QJsonObject& params, Reply reply) {
    const auto r = params["rect"].toArray();
    const QRect rect = r.size() == 4 ? QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt()) : QRect();
    if (rect.width() < 4 || rect.height() < 4) { reply({"invalid_request", "select takes a rectangle of at least 4 x 4 pixels", {}}, {}); return; }
    if (selection_) { selection_->complete(rect); reply({}, {}); return; }
#ifdef PUTGD_RECORD_LAYER_SHELL
    if (kdeSelection_) { kdeSelection_->select(QRectF(rect)); reply({}, {}); return; }
#endif
    reply({"recording_state", "No region selection is open", {}}, {});
}
// Restore tokens live beside the resident's data (the helper inherits its
// XDG directories): one per desktop and source class, replaced on every grant.
static QString portalTokensPath() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/recording-portal-tokens.json"; }
QString RecordSession::portalTokenKey() const {
    const auto desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    return (desktop.isEmpty() ? QStringLiteral("wayland") : desktop) + "/" + (target_.kind == "window" ? QStringLiteral("window") : QStringLiteral("monitor"));
}
QString RecordSession::loadPortalToken() const {
    QFile file(portalTokensPath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object().value(portalTokenKey()).toString();
}
void RecordSession::storePortalToken(const QString& token) const {
    if (token.isEmpty()) return;
    QFile file(portalTokensPath());
    QJsonObject tokens;
    if (file.open(QIODevice::ReadOnly)) { tokens = QJsonDocument::fromJson(file.readAll()).object(); file.close(); }
    tokens[portalTokenKey()] = token;
    QDir().mkpath(QFileInfo(portalTokensPath()).path());
    QSaveFile save(portalTokensPath());
    if (!save.open(QIODevice::WriteOnly)) return;
    save.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    save.write(QJsonDocument(tokens).toJson(QJsonDocument::Compact));
    save.commit();
}
void RecordSession::preparePortal(Reply reply) {
    // The compositor's picker chooses the monitor or window; a region needs an
    // explicit rectangle on the shared monitor (overlays cannot be placed on Wayland).
    const quint32 types = target_.kind == "window" ? PortalScreenCast::Window : PortalScreenCast::Monitor;
    const int offered = capabilities_["portal"].toObject()["sourceTypes"].toInt();
    if (!(offered & int(types))) { reply({"recording_unsupported", "The portal does not offer this kind of screen cast", {{"kind", target_.kind}}}, {}); return; }
    QPointer<RecordSession> guard(this);
    const auto token = loadPortalToken();
    debugLog(token.isEmpty() ? QStringLiteral("portal: no restore token for %1").arg(portalTokenKey()) : QStringLiteral("portal: restoring %1").arg(portalTokenKey()));
    portal_.start(types, token, [guard, reply](Error error, PortalScreenCast::Result result) {
        if (!guard) return;
        if (error) { reply(error, {}); return; }
        guard->storePortalToken(result.restoreToken);
        const auto& stream = result.streams.first();
        QRect shared(stream.position, stream.size);
        if (shared.isEmpty()) shared = QRect(QPoint(0, 0), QSize(1280, 720));
        // The portal shares the whole monitor; a region is cropped out of it.
        guard->finishWayland(stream.node, result.fd, shared, shared, QStringLiteral("portal"), reply);
    });
}
// Dialog-free paths: KWin streams an output or a logical region directly;
// Mutter records a monitor connector or an area. Windows stay on the portal.
void RecordSession::prepareNative(Reply reply) {
    const auto acquisition = capabilities_["acquisition"].toString();
    if (target_.kind == "window") { preparePortal(std::move(reply)); return; }
    QPointer<RecordSession> guard(this);
    const bool region = target_.kind == "region" && !target_.rect.isEmpty();
    QRect rect = target_.rect;
    rect.setWidth(rect.width() & ~1);
    rect.setHeight(rect.height() & ~1);
    if (acquisition == "mutter-screencast") {
        auto done = [guard, reply, acquisition](Error error, MutterScreenCast::Stream stream) {
            if (!guard) return;
            if (error) { reply(error, {}); return; }
            guard->finishWayland(stream.node, -1, stream.logical, stream.logical, acquisition, reply);
        };
        if (region) mutter_.recordArea(rect, done);
        else {
            QString connector = target_.identity;
            QRect logical;
            for (QScreen* screen : QGuiApplication::screens()) if ((connector.isEmpty() && screen == QGuiApplication::primaryScreen()) || screen->name() == connector) { connector = screen->name(); logical = screen->geometry(); }
            if (logical.isEmpty() && QGuiApplication::primaryScreen()) logical = QGuiApplication::primaryScreen()->geometry();
            mutter_.recordMonitor(connector, logical, done);
        }
        return;
    }
#ifdef PUTGD_RECORD_KDE_SCREENCAST
    auto done = [guard, reply, acquisition](Error error, KdeScreencast::Stream stream) {
        if (!guard) return;
        if (error) { reply(error, {}); return; }
        guard->finishWayland(stream.node, -1, stream.logical, stream.logical, acquisition, reply);
    };
    if (region) kde_.streamRegion(rect, done);
    else kde_.streamOutput(target_.identity, done);
#else
    preparePortal(std::move(reply));
#endif
}
// `shared` is the monitor or area the stream covers; `streamRect` is what the
// stream frames show (equal unless a region is cropped out of a portal share).
void RecordSession::finishWayland(quint32 node, int fd, const QRect& shared, const QRect& streamRect, const QString& source, Reply reply) {
    portalFd_ = fd;
    portalNode_ = node;
    portalTarget_ = true;
    ResolvedTarget resolved;
    resolved.rect = streamRect;
    if (source == "portal" && target_.kind == "region" && !target_.rect.isEmpty()) {
        const QRect inside = target_.rect.intersected(shared);
        if (inside.width() < 2 || inside.height() < 2) { cancelLocal(); reply({"recording_target", "The region is not on the shared monitor", {{"shared", rectJson(shared)}}}, {}); return; }
        portalCrop_ = inside.translated(-shared.topLeft());
        portalCrop_.setWidth(portalCrop_.width() & ~1);
        portalCrop_.setHeight(portalCrop_.height() & ~1);
        resolved.rect = portalCrop_.translated(shared.topLeft());
    }
    resolved.canvas = QSize(resolved.rect.width() & ~1, resolved.rect.height() & ~1);
    resolved.layout = QJsonObject{{"acquisition", source}, {"node", double(node)}, {"shared", rectJson(shared)}};
    resolved_ = resolved;
    prepared_ = true;
    finishPrepare(reply);
}
void RecordSession::finishPrepare(Reply reply) {
    if (!portalTarget_) {
        ResolvedTarget resolved;
        if (auto e = resolveX11Target(target_, resolved)) { reply(e, {}); return; }
        resolved_ = resolved;
        prepared_ = true;
    }
    // X11 and KDE Wayland draw the ready ring themselves; on other Wayland
    // desktops the resident's shell (the GNOME extension) draws it.
    const auto mode = surfaceMode();
    if (target_.kind != "desktop" && (!portalTarget_ || (mode == SurfaceMode::LayerShell && target_.kind != "window"))) bounds_.showBounds(resolved_.rect);
    ControlBar::Options options;
    options.targetLabel = targetLabel();
    for (const auto& value : capabilities_["formats"].toArray()) options.formats.append(value.toString());
    options.codecs = capabilities_["codecs"].toObject();
    options.audioSupported = capabilities_["audio"].toObject()["system"].toBool();
    delete bar_;
    bar_ = new ControlBar(options, config_);
    QPointer<RecordSession> guard(this);
    connect(bar_, &ControlBar::configChanged, this, [guard](const RecordingConfig& config) {
        if (!guard || guard->pipeline_.running() || guard->recorded_) return;
        const auto budget = guard->config_.budgetBytes;
        guard->config_ = config;
        guard->config_.budgetBytes = budget;
        emit guard->event({{"event", "configChanged"}, {"config", config.toJson()}});
    });
    connect(bar_, &ControlBar::recordRequested, this, [guard] {
        if (!guard || guard->pipeline_.running() || guard->recorded_) return;
        guard->start([guard](Error error, QJsonObject status) {
            if (!guard) return;
            if (error) { emit guard->event({{"event", "failed"}, {"error", error.toJson()}}); return; }
            status["event"] = "started";
            emit guard->event(status);
        });
    });
    connect(bar_, &ControlBar::pauseRequested, this, [guard] { if (guard && !guard->pipeline_.pause()) { guard->showBar("paused"); auto s = guard->pipeline_.status(); s["event"] = "paused"; emit guard->event(s); } });
    connect(bar_, &ControlBar::resumeRequested, this, [guard] { if (guard && !guard->pipeline_.resume()) { guard->showBar("recording"); auto s = guard->pipeline_.status(); s["event"] = "resumed"; emit guard->event(s); } });
    connect(bar_, &ControlBar::stopRequested, this, [guard] {
        if (!guard || !guard->pipeline_.running()) return;
        guard->stop([guard](Error error, QJsonObject result) {
            if (!guard) return;
            if (error) { emit guard->event({{"event", "failed"}, {"error", error.toJson()}}); return; }
            result["event"] = "stopped";
            result["reason"] = "user";
            emit guard->event(result);
        });
    });
    connect(bar_, &ControlBar::cancelRequested, this, [guard] { if (!guard) return; guard->cancelLocal(); emit guard->event({{"event", "cancelled"}}); });
    if (!portalTarget_) {
        // ximagesrc records every visible pixel of the area, so the bar goes
        // below or above it, or onto another monitor. With nowhere outside
        // (a full single monitor, the whole desktop) it shrinks to a handle
        // inside the clip from Record on.
        overlayExcluded_ = bar_->placeAround(target_.kind == "desktop" ? desktopRoot() : resolved_.rect);
        bar_->setHandleWhileRecording(!overlayExcluded_);
        overlayPlacement_ = overlayExcluded_ ? QStringLiteral("outside") : QStringLiteral("handle");
    } else if (target_.kind == "window") {
        // A window stream carries only that window's pixels, never the bar.
        overlayExcluded_ = true;
        overlayPlacement_ = QStringLiteral("outside");
    } else if (mode == SurfaceMode::LayerShell) {
        // Below or above the recorded area, or on another output; KWin 6.6 and
        // later also leave the requesting client's windows out of its own stream.
        const bool outside = bar_->placeOnLayer(resolved_.rect);
        const bool hidden = resolved_.layout["acquisition"].toString() == "kde-screencast" && capabilities_["kdeScreencast"].toObject()["hidesRequester"].toBool();
        overlayExcluded_ = outside || hidden;
        bar_->setHandleWhileRecording(!overlayExcluded_);
        overlayPlacement_ = outside ? QStringLiteral("outside") : hidden ? QStringLiteral("hidden") : QStringLiteral("handle");
    } else {
        // The compositor places the bar; the resident's shell may move it outside.
        overlayExcluded_ = false;
        overlayPlacement_ = QStringLiteral("compositor");
    }
    showBar("ready");
    reply({}, {{"canvas", sizeJson(resolved_.canvas)}, {"rect", rectJson(resolved_.rect)}, {"window", double(resolved_.window)}, {"overlayExcluded", overlayExcluded_}, {"overlayPlacement", overlayPlacement_}});
}
void RecordSession::start(Reply reply) {
    if (starting_) { reply({"recording_state", "Recording is already starting", {}}, {}); return; }
    if (pipeline_.running() || recorded_) { reply({"recording_state", "The session already recorded", {}}, {}); return; }
    if (!bar_ || !bar_->handleWhileRecording()) { begin(std::move(reply)); return; }
    // The ready bar sits inside the area: shrink it to its handle (a hidden bar
    // stays hidden) and give the X server or compositor a moment to repaint, so
    // the first frame never shows the full bar. KWin applies even a hide from
    // just before Record a frame later.
    starting_ = true;
    bar_->setState("recording");
    QPointer<RecordSession> guard(this);
    QTimer::singleShot(handleSettleMs, this, [guard, reply = std::move(reply)]() mutable {
        if (!guard) return;
        guard->starting_ = false;
        if (guard->cancelled_) { reply({"cancelled", "Recording cancelled before it started", {}}, {}); return; }
        guard->begin(std::move(reply));
    });
}
void RecordSession::begin(Reply reply) {
    RecordingPipeline::Options options;
    options.rect = resolved_.rect;
    options.window = resolved_.window;
    if (portalTarget_) { options.rect = resolved_.layout["shared"].isArray() ? QRect(resolved_.layout["shared"].toArray()[0].toInt(), resolved_.layout["shared"].toArray()[1].toInt(), resolved_.layout["shared"].toArray()[2].toInt(), resolved_.layout["shared"].toArray()[3].toInt()) : resolved_.rect; options.pipewireFd = portalFd_; options.pipewireNode = portalNode_; options.crop = portalCrop_; }
    options.fps = config_.fps;
    options.audio = config_.audio.system && !config_.silent();
    options.audioDevice = "@DEFAULT_MONITOR@";
    options.output = workingClip_;
    options.budgetBytes = config_.budgetBytes;
    options.encoders = available(videoEncoders);
    options.audioEncoders = available(audioEncoders);
    if (options.encoders.isEmpty()) { showBar("ready"); reply({"recording_unsupported", "No H.264 encoder is installed", {}}, {}); return; }
    if (auto e = pipeline_.start(options)) { showBar("ready"); reply(e, {}); return; }
    bounds_.setDimmed(true);
    showBar("recording");
    progress_.start();
    barTimer_.start();
    reply({}, pipeline_.status());
}
void RecordSession::stop(Reply reply) {
    if (!pipeline_.running()) { reply({"recording_state", "Nothing is recording", {}}, {}); return; }
    showBar("stopping");
    QPointer<RecordSession> guard(this);
    pipeline_.stop([guard, reply](Error error, qint64 durationUs) {
        if (!guard) return;
        guard->progress_.stop();
        guard->barTimer_.stop();
        guard->bounds_.hideAll();
        if (guard->bar_) guard->bar_->hide();
        if (error) { reply(error, {}); return; }
        if (durationUs < 1 || !QFileInfo(guard->workingClip_).isFile()) { reply({"recording_failed", "The recording produced no retained frames", {}}, {}); return; }
        guard->durationUs_ = durationUs;
        guard->recorded_ = true;
        // Lab diagnostics only: keep a private copy of the working clip for inspection.
        if (const auto keep = qEnvironmentVariable("PUTGD_RECORD_KEEP_WORKING"); !keep.isEmpty()) QFile::copy(guard->workingClip_, keep + "/" + guard->id_ + ".mkv");
        guard->openReview();
        reply({}, {{"durationUs", double(durationUs)}, {"canvas", sizeJson(guard->resolved_.canvas)}, {"bytes", double(QFileInfo(guard->workingClip_).size())}, {"encoder", guard->pipeline_.encoder()}, {"audioEncoder", guard->pipeline_.audioEncoder()}});
    });
}
void RecordSession::openReview() {
    debugLog("review: open");
    delete review_;
    ReviewWindow::Options options;
    options.workingClip = workingClip_;
    options.destination = reviewOptions_["destination"].toString();
    options.sharingMode = reviewOptions_["sharingMode"].toString();
    options.uploadSummary = reviewOptions_["summary"].toString();
    options.durationUs = durationUs_;
    options.canvas = resolved_.canvas;
    for (const auto& value : capabilities_["formats"].toArray()) options.formats.append(value.toString());
    options.codecs = capabilities_["codecs"].toObject();
    options.audioRecorded = config_.audio.system && !pipeline_.audioEncoder().isEmpty();
    review_ = new ReviewWindow(options, config_);
    QPointer<RecordSession> guard(this);
    connect(review_, &ReviewWindow::editProposed, this, [guard](quint64 revision, const QJsonArray& commands) { if (guard) emit guard->event({{"event", "edit"}, {"revision", double(revision)}, {"commands", commands}}); });
    connect(review_, &ReviewWindow::configChanged, this, [guard](const RecordingConfig& config) {
        if (!guard) return;
        const auto budget = guard->config_.budgetBytes;
        guard->config_ = config;
        guard->config_.budgetBytes = budget;
        emit guard->event({{"event", "configChanged"}, {"config", config.toJson()}});
    });
    connect(review_, &ReviewWindow::uploadRequested, this, [guard](quint64 revision) { if (guard) emit guard->event({{"event", "uploadRequested"}, {"revision", double(revision)}}); });
    connect(review_, &ReviewWindow::saveCopyRequested, this, [guard](const QString& path, quint64 revision) { if (guard) emit guard->event({{"event", "saveCopyRequested"}, {"path", path}, {"revision", double(revision)}}); });
    connect(review_, &ReviewWindow::cancelRequested, this, [guard] { if (!guard) return; guard->cancelLocal(); emit guard->event({{"event", "cancelled"}}); });
    review_->show();
    review_->raise();
    review_->activateWindow();
    debugLog("review: shown");
}
void RecordSession::emitProgress() {
    if (!pipeline_.running()) return;
    auto status = pipeline_.status();
    status["event"] = "progress";
    emit event(status);
}
bool RecordSession::parseEdit(const QJsonObject& json, RecordingEdit& edit) const {
    qint64 start = 0, end = 0, revision = 0;
    if (!integer(json["trimStart"], 0, durationUs_, &start) || !integer(json["revision"], 1, 1LL << 52, &revision) || !json["masks"].isArray()) return false;
    edit.trimStart = start;
    edit.revision = quint64(revision);
    if (json["trimEnd"].isNull() || json["trimEnd"].isUndefined()) edit.trimEnd.reset();
    else if (integer(json["trimEnd"], 0, durationUs_, &end)) edit.trimEnd = end;
    else return false;
    for (const auto& value : json["masks"].toArray()) {
        const auto m = value.toObject();
        RecordingMask mask;
        mask.id = m["id"].toString();
        mask.kind = m["kind"].toString("blur");
        const auto r = m["rect"].toArray();
        if (r.size() != 4) return false;
        mask.rect = QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt());
        qint64 strength = 16;
        if (!integer(m["start"], 0, 1LL << 50, &mask.start) || !integer(m["end"], 0, 1LL << 50, &mask.end)) return false;
        if (m.contains("strength")) integer(m["strength"], 2, 64, &strength);
        mask.strength = int(strength);
        edit.masks.append(mask);
    }
    return true;
}
void RecordSession::preview(const QJsonObject& params, Reply reply) {
    if (!recorded_) { reply({"recording_state", "Nothing to preview before stop", {}}, {}); return; }
    RecordingEdit edit;
    qint64 time = 0, maxEdge = 1024;
    if (!parseEdit(params["edit"].toObject(), edit) || !integer(params["time"], 0, std::max<qint64>(0, durationUs_), &time) || !integer(params["maxEdge"], 16, 8192, &maxEdge)) { reply({"invalid_request", "Invalid preview request", {}}, {}); return; }
    QImage image;
    qint64 actual = 0;
    if (auto e = previewFrame(workingClip_, edit, config_, time, int(maxEdge), resolved_.canvas, image, &actual)) { reply(e, {}); return; }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) { reply({"recording_export_failed", "Cannot encode the preview", {}}, {}); return; }
    reply({}, {{"png", QString::fromLatin1(png.toBase64())}, {"frameTimeUs", double(actual)}, {"width", image.width()}, {"height", image.height()}});
}
void RecordSession::exportClip(const QJsonObject& params, Reply reply) {
    if (!recorded_) { reply({"recording_state", "Nothing to export before stop", {}}, {}); return; }
    if (exportThread_) { reply({"recording_busy", "An export is already running", {}}, {}); return; }
    ExportRequest request;
    request.input = workingClip_;
    request.output = params["output"].toString();
    request.durationUs = durationUs_;
    request.canvas = resolved_.canvas;
    auto configJson = params["config"].toObject();
    for (const auto* key : {"mime", "budgetBytes"}) configJson.remove(key);
    if (!parseEdit(params["edit"].toObject(), request.edit) || request.output.isEmpty() || recording::parseConfig(configJson, request.config)) { reply({"invalid_request", "Invalid export request", {}}, {}); return; }
    request.config.audio.system = request.config.audio.system && config_.audio.system;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    exportCancel_ = cancel;
    auto result = std::make_shared<ExportResult>();
    auto error = std::make_shared<Error>();
    QPointer<RecordSession> guard(this);
    auto* thread = QThread::create([request, result, error, cancel, guard] {
        // The exporter reports every frame; a GPU export runs hundreds a second, more
        // than the control channel accepts, so forward at most ten updates a second.
        QElapsedTimer sinceReport;
        *error = record::exportClip(request, *result, *cancel, [guard, cancel, &sinceReport](double fraction) {
            if (!guard || cancel->load()) return;
            if (sinceReport.isValid() && sinceReport.elapsed() < 100 && fraction < 1.0) return;
            sinceReport.start();
            QMetaObject::invokeMethod(guard, [guard, fraction] { if (guard) emit guard->event({{"event", "progress"}, {"exportFraction", fraction}}); }, Qt::QueuedConnection);
        });
    });
    exportThread_ = thread;
    if (review_) review_->setExporting(true, QStringLiteral("Exporting %1…").arg(request.config.format.toUpper()));
    connect(thread, &QThread::finished, this, [this, thread, result, error, request, reply] {
        thread->deleteLater();
        if (exportThread_ == thread) exportThread_ = nullptr;
        exportCancel_.reset();
        // A failure is an error in the review's status row, kept until dismissed.
        if (review_) review_->setExporting(false, *error ? QString() : QStringLiteral("Exported %1 (%2 KiB)").arg(request.config.format.toUpper()).arg(result->bytes / 1024));
        if (review_ && *error) review_->showError(QStringLiteral("Export failed: %1").arg(error->message));
        if (*error) { QFile::remove(request.output); reply(*error, {}); return; }
        reply({}, {{"bytes", double(result->bytes)}, {"durationUs", double(result->durationUs)}, {"frames", result->frames}, {"videoEncoder", result->videoEncoder}, {"audioEncoder", result->audioEncoder}, {"size", sizeJson(result->size)}});
    });
    thread->start();
}
}
