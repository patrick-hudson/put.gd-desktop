#include "RecordSession.h"
#include <algorithm>
#include <chrono>
#include "EffectiveAudio.h"
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
#include "platform/wayland/LayerShellRegionOverlay.h"
#endif

namespace putgd::record {
namespace {
QJsonArray sizeJson(const QSize& s) { return {s.width(), s.height()}; }
QJsonArray rectJson(const QRect& r) { return {r.x(), r.y(), r.width(), r.height()}; }
QRect rectFromJson(const QJsonValue& value) {
    const auto a = value.toArray();
    if (a.size() != 4) return {};
    for (const auto& v : a) if (!v.isDouble()) return {};
    return QRect(a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt());
}
// A window stream source that failed is reopened at most this often per
// recording, a little after the failure so a closing window's notice wins.
constexpr int sourceReopenLimit = 30, sourceReopenDelayMs = 400;
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

RecordSession::RecordSession(QObject* parent) : QObject(parent), pipeline_(this), monitor_(this), levelTimer_(this), progress_(this), barTimer_(this), bounds_(this) {
    // The meter beside the microphone toggle, about 12 times a second while an input is open.
    levelTimer_.setInterval(80);
    microphoneWatch_.setInterval(1000);
    connect(&microphoneWatch_, &QTimer::timeout, this, &RecordSession::followMicrophones);
    connect(&levelTimer_, &QTimer::timeout, this, [this] {
        if (!bar_) return;
        if (pipeline_.running()) bar_->setMicrophoneLevel(pipeline_.takeMicrophoneLevel());
        else if (monitor_.isOpen()) bar_->setMicrophoneLevel(monitor_.level().take());
    });
    connect(&pipeline_, &RecordingPipeline::microphoneGone, this, [this](const QString& reason) {
        if (bar_) bar_->setMicrophoneLost(reason);
        emitProgress();
    });
    progress_.setInterval(1000);
    connect(&progress_, &QTimer::timeout, this, &RecordSession::emitProgress);
    barTimer_.setInterval(200);
    connect(&barTimer_, &QTimer::timeout, this, [this] { if (bar_ && pipeline_.running()) bar_->setRetained(pipeline_.retainedUs()); });
    connect(&pipeline_, &RecordingPipeline::failed, this, [this](Error error) {
        enterPhase(HelperPhase::Failed);
        progress_.stop();
        barTimer_.stop();
        closeMicrophone();
        bounds_.hideAll();
        if (bar_) bar_->hide();
        releaseAcquisition();
        emit event({{"event", "failed"}, {"error", error.toJson()}});
    });
    connect(&pipeline_, &RecordingPipeline::sourceLost, this, [this](Error error) { sourceLost(error); });
    // wlr-capture: the recorded window closed (targetLost) or its output went away.
    connect(&pipeline_, &RecordingPipeline::imageCopyLost, this, [this](Error error) {
        debugLog("wlr-capture: the source ended: " + error.message);
        if (target_.kind == "window") targetLost();
        else if (pipeline_.running() && !pipeline_.stopping()) endCapture(QStringLiteral("source_lost"));
    });
    connect(&mutter_, &MutterScreenCast::lost, this, [this] { targetLost(); });
#ifdef PUTGD_RECORD_KDE_SCREENCAST
    connect(&kde_, &KdeScreencast::lost, this, [this] { targetLost(); });
#endif
    connect(&pipeline_, &RecordingPipeline::budgetReached, this, [this](QJsonObject details) {
        // The admitted byte budget is nearly spent: finish the clip and open review.
        enterPhase(HelperPhase::Stopping);
        showBar("stopping");
        pipeline_.stop([this, details](Error error, qint64 durationUs) {
            progress_.stop();
            barTimer_.stop();
            closeMicrophone();
            bounds_.hideAll();
            if (bar_) bar_->hide();
            releaseAcquisition();
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
                enterPhase(HelperPhase::Failed);
                emit event({{"event", "failed"}, {"error", error.toJson()}}); return;
            }
            durationUs_ = durationUs;
            enterPhase(HelperPhase::Recorded);
            noteCaptured();
            openReview();
            emit event({{"event", "stopped"}, {"durationUs", double(durationUs)}, {"canvas", sizeJson(resolved_.canvas)}, {"audioEncoder", pipeline_.audioEncoder()}, {"audioSources", audioSources()}, {"reason", "budget"}, {"stopDetails", details}});
        });
    });
}
void RecordSession::targetLost() {
    if (target_.kind != "window" || targetClosed_ || phase_ == HelperPhase::Cancelled) return;
    targetClosed_ = true;
    debugLog("window: the compositor ended the stream (the window closed)");
#ifdef PUTGD_RECORD_KDE_SCREENCAST
    kde_.close();  // the protocol's destructor for the stream KWin closed
#endif
    if (pipeline_.running()) { if (!pipeline_.stopping()) endCapture(QStringLiteral("target_closed")); return; }
    if (phase_ != HelperPhase::Ready && phase_ != HelperPhase::Starting) return;
    // Ready, nothing recorded yet: nothing is left to record.
    cancelLocal();
    emit event({{"event", "failed"}, {"error", Error{"recording_target_closed", "The window closed before recording started", {}}.toJson()}});
}
void RecordSession::sourceLost(const Error& error) {
    if (targetClosed_ || !pipeline_.running() || pipeline_.stopping()) return;
    QPointer<RecordSession> guard(this);
    QTimer::singleShot(sourceReopenDelayMs, this, [guard, error] {
        if (!guard || guard->targetClosed_ || !guard->pipeline_.running() || guard->pipeline_.stopping()) return;
        if (guard->sourceReopens_ < sourceReopenLimit && guard->pipeline_.reopenSource()) { ++guard->sourceReopens_; return; }
        // The stream cannot be reopened: keep what was recorded.
        debugLog("window: the stream could not be reopened: " + error.message);
        guard->endCapture(QStringLiteral("source_lost"));
    });
}
void RecordSession::endCapture(const QString& reason) {
    enterPhase(HelperPhase::Stopping);
    showBar("stopping");
    QPointer<RecordSession> guard(this);
    pipeline_.stop([guard, reason](Error error, qint64 durationUs) {
        if (!guard) return;
        guard->progress_.stop();
        guard->barTimer_.stop();
        guard->closeMicrophone();
        guard->bounds_.hideAll();
        if (guard->bar_) guard->bar_->hide();
        guard->releaseAcquisition();
        if (!error && (durationUs < 1 || !QFileInfo(guard->workingClip_).isFile())) error = {"recording_failed", "The recording produced no retained frames", {}};
        if (error) { error.details["stopReason"] = reason; guard->enterPhase(HelperPhase::Failed); emit guard->event({{"event", "failed"}, {"error", error.toJson()}}); return; }
        guard->durationUs_ = durationUs;
        guard->enterPhase(HelperPhase::Recorded);
        guard->noteCaptured();
        guard->openReview();
        emit guard->event({{"event", "stopped"}, {"durationUs", double(durationUs)}, {"canvas", sizeJson(guard->resolved_.canvas)}, {"audioEncoder", guard->pipeline_.audioEncoder()}, {"audioSources", guard->audioSources()}, {"reason", reason}});
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
void RecordSession::enterPhase(HelperPhase phase) {
    if (phase_ == phase || phase_ == HelperPhase::Cancelled || (phase_ == HelperPhase::Failed && phase != HelperPhase::Cancelled)) return;
    debugLog("session: " + recording::phaseName(phase_) + " -> " + recording::phaseName(phase));
    phase_ = phase;
}
void RecordSession::showBar(const QString& state) {
    if (!bar_) return;
    // Over review, or once the session ended, the bar never comes back.
    if (reviewing() || phase_ == HelperPhase::Cancelled || phase_ == HelperPhase::Failed) { bar_->hide(); return; }
    bar_->setState(state);
    if (barHidden_) return;
    bar_->show();
    bar_->raise();
}
void RecordSession::command(const QString& command, const QJsonObject& params, Reply reply) {
    // The same command table as the resident's, by phase (#1434).
    if (auto error = recording::checkHelperCommand(phase_, command)) { reply(error, {}); return; }
    if (command == "prepare") { prepare(params, std::move(reply)); return; }
    if (command == "select") { completeSelection(params, std::move(reply)); return; }
    if (command == "start") { start(std::move(reply)); return; }
    if (command == "pause") { if (auto e = pipeline_.pause()) reply(e, {}); else { enterPhase(HelperPhase::Paused); showBar("paused"); emitProgress(); reply({}, pipeline_.status()); } return; }
    if (command == "resume") { if (auto e = pipeline_.resume()) reply(e, {}); else { enterPhase(HelperPhase::Recording); showBar("recording"); emitProgress(); reply({}, pipeline_.status()); } return; }
    if (command == "stop") { stop(std::move(reply)); return; }
    if (command == "cancel") { cancelLocal(); reply({}, {}); return; }
    if (command == "audio") {
        // recording.audio: the bar's two toggles, set as a click sets them, while the bar is at Ready.
        if (!bar_) { reply({"recording_state", "The audio sources can be changed only before Record", {}}, {}); return; }
        const bool system = params["system"].toBool(false), microphone = params["microphone"].toBool(false);
        if ((system || microphone) && config_.silent()) { reply({"recording_unsupported", "GIF and WebP are silent", {{"field", "audio"}, {"reason", "silent_format"}}}, {}); return; }
        if (system && !capabilities_["audio"].toObject()["system"].toBool()) { reply({"recording_unsupported", "System audio cannot be recorded here", {{"field", "audio.system"}}}, {}); return; }
        if (microphone) {
            // A microphone plugged in a moment ago counts: look now rather than at the next tick.
            followMicrophones();
            if (!microphone_.available()) { reply({"recording_unsupported", microphone_.message.isEmpty() ? QStringLiteral("No microphone can be opened") : microphone_.message, {{"field", "audio.microphone"}, {"reason", microphone_.reason}}}, {}); return; }
        }
        commandedAudio_ = true;
        bar_->chooseAudio(params["system"].isBool() ? std::optional<bool>(system) : std::nullopt, params["microphone"].isBool() ? std::optional<bool>(microphone) : std::nullopt);
        commandedAudio_ = false;
        reply({}, {{"audio", config_.audio.toJson()}, {"microphone", bar_->microphoneState()}});
        return;
    }
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
        if (bar_) { if (reviewing()) bar_->hide(); else { bar_->show(); bar_->raise(); } }
        if (review_) review_->reopen(params["error"].toString());
        reply({}, {});
        return;
    }
    if (command == "savedCopy") {
        auto json = params["config"].toObject();
        for (const auto* key : {"mime", "budgetBytes"}) json.remove(key);
        RecordingConfig config;
        qint64 revision = 0;
        const QString path = params["path"].toString();
        if (!QDir::isAbsolutePath(path) || !integer(params["revision"], 1, 1LL << 52, &revision) || recording::parseConfig(json, config)) {
            reply({"invalid_request", "Invalid saved-copy result", {}}, {}); return;
        }
        if (review_) review_->setSavedCopy(path, quint64(revision), config);
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
        if (params.contains("visible")) { barHidden_ = !params["visible"].toBool(true); if (bar_) bar_->setVisible(!barHidden_ && !reviewing()); }
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
    if (command == "click" && params["pointer"].toBool()) {
        // #1364: a press whose place only this helper knows (labwc): the capture's own pointer position, in stream pixels.
        QPoint at;
        bool ever = false;
        if (!imageCopy_ || !imageCopyPointer_ || !imageCopy_->pointer(&at, &ever)) {
            // Off this output, or hidden by the application under it: no ring for this press, and nothing wrong.
            // Never reported at all (an output without a hardware cursor): the recording has no highlights, and says so.
            if (ever) reply({}, {});
            else reply({"pointer_position_unavailable", "The compositor has reported no pointer position for this recording (labwc reports it only for an output with a hardware cursor), so clicks cannot be highlighted", {}}, {});
            return;
        }
        const QPointF inCanvas = QPointF(at) - (portalCrop_.isValid() ? QPointF(portalCrop_.topLeft()) : QPointF());
        const int button = params["button"].toInt();
        if (config_.clickHighlights && pipeline_.running() && button >= 1 && button <= 3 && QRectF(QPointF(), QSizeF(resolved_.canvas)).contains(inCanvas)) pipeline_.addClick(inCanvas, button, clickAge(params));
        reply({}, {});
        return;
    }
    if (command == "click") {
        // #1364: a press the desktop reported to the resident, in logical desktop pixels.
        const auto rect = params["window"].toObject()["rect"].toArray();
        const QRectF window = rect.size() == 4 ? QRectF(rect[0].toDouble(), rect[1].toDouble(), rect[2].toDouble(), rect[3].toDouble()) : QRectF();
        if (target_.kind != "window" || params["window"].toObject()["identity"].toString() == target_.identity || target_.identity.isEmpty())
            click(QPointF(params["x"].toDouble(), params["y"].toDouble()), params["button"].toInt(), window, clickAge(params));
        reply({}, {});
        return;
    }
    if (command == "preview") { preview(params, std::move(reply)); return; }
    if (command == "export") { exportClip(params, std::move(reply)); return; }
    reply({"unknown_method", "Unknown recording helper command", {}}, {});
}
void RecordSession::noteCaptured() {
    audioCaptured_ = !pipeline_.audioEncoder().isEmpty();
    capturedSystem_ = pipeline_.recordsSystemAudio();
    capturedMicrophone_ = pipeline_.recordsMicrophone();
}
void RecordSession::closeMicrophone() {
    levelTimer_.stop();
    monitor_.close();
    // The capture is over: nothing more is read from the X server's pointer either.
    x11Clicks_.stop();
}
void RecordSession::startClicks() {
    if (!config_.clickHighlights) return;
    // The ring is sized in logical pixels: canvas pixels per desktop unit of the recorded area.
    // Wayland areas are logical; X11 root pixels are the screen's device pixels.
    double density = 1.0;
    if (portalTarget_) { if (resolved_.rect.width() > 0) density = double(resolved_.canvas.width()) / resolved_.rect.width(); }
    else {
        for (QScreen* screen : QGuiApplication::screens()) {
            const qreal dpr = screen->devicePixelRatio();
            if (QRectF(QPointF(screen->geometry().topLeft()) * dpr, QSizeF(screen->geometry().size()) * dpr).contains(QPointF(resolved_.rect.center()))) density = dpr;
        }
    }
    pipeline_.clicks().setScale(density);
    if (portalTarget_ || surfaceMode() != SurfaceMode::X11) return;
    QString failure;
    QPointer<RecordSession> guard(this);
    // X11: root pixels, or the recorded window's own pixels.
    if (!x11Clicks_.start(resolved_.window, [guard](QPointF at, int button) { if (guard) guard->click(at, button, guard->resolved_.window ? QRectF(QPointF(0, 0), QSizeF(guard->resolved_.canvas)) : QRectF()); }, &failure))
        debugLog("clicks: the X server gives no raw button events: " + failure);
}
// How long ago the desktop reported a press: `atUs` is the resident's CLOCK_MONOTONIC stamp.
qint64 RecordSession::clickAge(const QJsonObject& params) {
    if (!params["atUs"].isDouble()) return 0;
    const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return std::clamp<qint64>(nowUs - qint64(params["atUs"].toDouble()), 0, 2'000'000) * 1000;
}
void RecordSession::click(QPointF desktop, int button, const QRectF& window, qint64 ageNs) {
    if (!config_.clickHighlights || !pipeline_.running() || button < 1 || button > 3) return;
    // What the frames show: the recorded window where one is recorded, else the recorded area.
    const QRectF area = target_.kind == "window" ? window : QRectF(resolved_.rect);
    QPointF at;
    if (!mapClick(desktop, area, resolved_.canvas, &at)) return;
    pipeline_.addClick(at, button, ageNs);
}
void RecordSession::followMicrophones() {
    // Only at Ready: from Record on the device is the one the pipeline opened, and its loss is the pipeline's to report.
    if (!bar_ || reviewing() || phase_ == HelperPhase::Cancelled || phase_ == HelperPhase::Failed) { microphoneWatch_.stop(); return; }
    if (pipeline_.running() || phase_ == HelperPhase::Starting || microphone_.reason == QLatin1String("microphone_elements_missing")) return;
    const auto now = chooseMicrophone(listMicrophones_(), config_.audio.microphoneDevice);
    if (now.device.id == microphone_.device.id && now.device.label == microphone_.device.label && now.reason == microphone_.reason) return;
    debugLog(now.available() ? "microphone: now " + now.device.id : "microphone: now unavailable (" + now.reason + ")");
    microphone_ = now;
    const bool before = bar_->effectiveMicrophone();
    // The bar announces a change of what will be recorded itself (configChanged, handled where the bar is made).
    bar_->setMicrophone(now.available(), now.device.label, now.message);
    if (bar_->effectiveMicrophone() == before) {
        // The toggle's own state changed all the same: available or not, and which device.
        updateMicrophoneMonitor();
        emit event({{"event", "progress"}, {"recordingAudio", bar_->audioState()}, {"recordingMicrophone", bar_->microphoneState()}});
    }
}
void RecordSession::updateMicrophoneMonitor() {
    // The input is open for the meter only while the toggle is on and nothing
    // records yet; the recording pipeline opens its own from Record to Stop.
    const bool wanted = bar_ && (phase_ == HelperPhase::Ready || phase_ == HelperPhase::Starting) && !pipeline_.running() && bar_->effectiveMicrophone() && microphone_.available();
    if (!wanted) { if (!pipeline_.running()) closeMicrophone(); return; }
    QString failure;
    if (!monitor_.open(microphone_.device.id, &failure)) { debugLog("microphone: the meter could not open it: " + failure); return; }
    levelTimer_.start();
}
void RecordSession::cancelLocal() {
    enterPhase(HelperPhase::Cancelled);
    if (exportCancel_) exportCancel_->store(true);
    progress_.stop();
    barTimer_.stop();
    closeMicrophone();
    if (selection_) { selection_->disconnect(this); selection_->cancel(); selection_->deleteLater(); selection_ = nullptr; }
#ifdef PUTGD_RECORD_LAYER_SHELL
    if (kdeSelection_) { kdeSelection_->disconnect(this); kdeSelection_->cancel(); kdeSelection_->deleteLater(); kdeSelection_ = nullptr; }
#endif
    pipeline_.cancel();
    releaseAcquisition();
    bounds_.hideAll();
    if (bar_) bar_->hide();
    if (review_) review_->hide();
    QFile::remove(workingClip_);
}
void RecordSession::releaseAcquisition() {
    // A Stop refused during the drain lands here with frames still flowing.
    if (pipeline_.running()) return;
    // The pipeline has stopped using it; the next prepare opens its own (review #1275).
    imageCopy_.reset();
    portal_.close();
    mutter_.close();
#ifdef PUTGD_RECORD_KDE_SCREENCAST
    kde_.close();
#endif
    if (portalFd_ >= 0) { ::close(portalFd_); portalFd_ = -1; }
}
void RecordSession::prepare(const QJsonObject& params, Reply reply) {
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
    enterPhase(HelperPhase::Preparing);
    id_ = params["id"].toString();
    target_ = target;
    reviewOptions_ = params["reviewOptions"].toObject();
    window_ = target_.kind == "window" ? params["window"].toObject() : QJsonObject{};
    config_ = config;
    staging_ = staging;
    workingClip_ = staging + "/working.mkv";
    capabilities_ = probeMedia();
    // #1364: which input the toggle would open. Listing asks the sound server and opens no device.
    microphone_ = {};
    QString missing;
    if (!capabilities_["audio"].toObject()["microphone"].toBool() || !microphoneElements(&missing)) {
        microphone_.reason = QStringLiteral("microphone_elements_missing");
        microphone_.message = QStringLiteral("Microphone recording is not available on this installation.");
    } else microphone_ = chooseMicrophone(listMicrophones(), config_.audio.microphoneDevice);
    debugLog(microphone_.available() ? "microphone: " + microphone_.device.id : "microphone: unavailable (" + microphone_.reason + ")");
    if (target_.kind == "region" && target_.rect.isEmpty()) { select(std::move(reply)); return; }
    acquire(std::move(reply));
}
void RecordSession::acquire(Reply reply) {
    // A cancel that arrived while the selector was closing opens nothing.
    if (phase_ == HelperPhase::Cancelled) { reply({"cancelled", "Recording canceled before the source opened", {}}, {}); return; }
    const auto acquisition = capabilities_["acquisition"].toString();
    if (acquisition == "kde-screencast" || acquisition == "mutter-screencast") { prepareNative(std::move(reply)); return; }
    if (acquisition == "wlr-capture") { prepareImageCopy(std::move(reply)); return; }
    if (acquisition == "portal") {
        if (target_.kind == "window") debugLog("window: no dialog-free screen cast here (" + capabilities_["kdeScreencast"].toObject()["reason"].toString(capabilities_["mutterScreenCast"].toObject()["reason"].toString(capabilities_["pipewire"].toObject()["socket"].isNull() ? QStringLiteral("no PipeWire socket") : QStringLiteral("none"))) + "); the portal picker chooses the window");
        preparePortal(std::move(reply));
        return;
    }
    finishPrepare(std::move(reply));
}
void RecordSession::select(Reply reply) {
    // Release keeps the bounds and shows the controls; nothing streams or records
    // until the rectangle exists, so Escape leaves no compositor session behind.
    QPointer<RecordSession> guard(this);
    auto cancelled = [guard, reply](const Error& error) {
        if (!guard) return;
        reply(error.code == "cancelled" ? Error{"cancelled", "Region selection canceled; nothing was recorded", {}} : error, {});
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
        kdeSelection_ = new LayerShellRegionOverlay(frames, this, true);
        connect(kdeSelection_, &LayerShellRegionOverlay::selected, this, [guard, reply](const QRectF& logical) {
            if (!guard || !guard->kdeSelection_) return;
            guard->kdeSelection_->deleteLater();
            guard->kdeSelection_ = nullptr;
            guard->target_.rect = logical.toAlignedRect();
            // Emitted from a Wayland callback: open the stream and the surfaces outside it.
            QTimer::singleShot(0, guard, [guard, reply] { if (guard) guard->acquire(reply); });
        });
        connect(kdeSelection_, &LayerShellRegionOverlay::cancelled, this, [guard, cancelled](const Error& error) {
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
// Dialog-free paths: KWin streams an output, a logical region or a window by
// its UUID; Mutter records a monitor connector, an area or a window by its id.
// The resident resolved the window (the active one, or the identity asked for)
// and refused put.gd's own; without that identity the portal picker chooses.
void RecordSession::prepareNative(Reply reply) {
    const auto acquisition = capabilities_["acquisition"].toString();
    QPointer<RecordSession> guard(this);
    if (target_.kind == "window") {
        if (target_.identity.isEmpty()) { debugLog("window: no window identity from the resident; the portal picker chooses the window"); preparePortal(std::move(reply)); return; }
        const QRect logical = rectFromJson(window_["rect"]);
        if (acquisition == "mutter-screencast") {
            bool numeric = false;
            const quint64 id = target_.identity.toULongLong(&numeric);
            if (!numeric) { reply({"recording_target", "A GNOME window identity is Mutter's window id", {{"identity", target_.identity}}}, {}); return; }
            mutter_.recordWindow(id, logical, [guard, reply, acquisition, logical](Error error, MutterScreenCast::Stream stream) {
                if (!guard) return;
                if (error) { reply(error, {}); return; }
                guard->finishWayland(stream.node, -1, logical, logical, acquisition, reply);
            });
            return;
        }
#ifdef PUTGD_RECORD_KDE_SCREENCAST
        kde_.streamWindow(target_.identity, logical, [guard, reply, acquisition, logical](Error error, KdeScreencast::Stream stream) {
            if (!guard) return;
            if (error) { reply(error, {}); return; }
            guard->finishWayland(stream.node, -1, logical, logical, acquisition, reply);
        });
#else
        preparePortal(std::move(reply));
#endif
        return;
    }
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
void RecordSession::prepareImageCopy(Reply reply) {
    // Never the ScreenCast portal here: a missing or refused protocol is the
    // error, with its name, and no chooser ever shows (#1275 decision 9).
    ImageCopySource::Target source;
    QRect shared;
    QScreen* screen = nullptr;
    const auto screens = QGuiApplication::screens();
    if (target_.kind == "window") {
        source.window = true;
        source.identity = target_.identity;
    } else if (target_.kind == "region") {
        for (QScreen* candidate : screens) if (candidate->geometry().contains(target_.rect)) screen = candidate;
        if (!screen) { reply({"recording_unsupported", "A region recording here covers one output; this region spans outputs or leaves them", {{"rect", rectJson(target_.rect)}}}, {}); return; }
    } else if (target_.kind == "monitor") {
        for (QScreen* candidate : screens) if ((target_.identity.isEmpty() && candidate == QGuiApplication::primaryScreen()) || candidate->name() == target_.identity) screen = candidate;
        if (!screen) { reply({"recording_target", "No monitor has this name", {{"identity", target_.identity}}}, {}); return; }
    } else if (target_.kind == "desktop") {
        if (screens.size() != 1) { reply({"recording_unsupported", "Recording the whole desktop here covers one output; record a monitor instead", {{"outputs", int(screens.size())}}}, {}); return; }
        screen = screens.first();
    }
    if (screen) { source.identity = screen->name(); shared = screen->geometry(); }
    imageCopy_ = std::make_unique<ImageCopySource>();
    QSize size; QString identity, appId;
    // #1364: with click highlights the pointer's place has to come from this source (labwc tells
    // nobody else), so it draws the cursor itself; a window source has no cursor to read.
    imageCopyPointer_ = false;
    if (auto e = imageCopy_->open(source, &size, &identity, &appId, config_.clickHighlights && !source.window, &imageCopyPointer_)) { imageCopy_.reset(); reply(e, {}); return; }
    imageCopySize_ = size;
    portalTarget_ = true;
    portalCrop_ = {};
    ResolvedTarget resolved;
    if (target_.kind == "region") {
        // The stream is the output in native pixels; the region is logical.
        const double sx = double(size.width()) / shared.width(), sy = double(size.height()) / shared.height();
        const QRect logical = target_.rect.translated(-shared.topLeft());
        portalCrop_ = QRect(qRound(logical.x() * sx), qRound(logical.y() * sy), qRound(logical.width() * sx) & ~1, qRound(logical.height() * sy) & ~1);
        resolved.rect = target_.rect;
        resolved.canvas = portalCrop_.size();
    } else {
        resolved.rect = shared.isEmpty() ? QRect(QPoint(), size) : shared;
        resolved.canvas = QSize(size.width() & ~1, size.height() & ~1);
    }
    resolved.layout = QJsonObject{{"acquisition", "wlr-capture"}, {"identity", identity}, {"stream", sizeJson(size)}, {"shared", rectJson(shared.isEmpty() ? QRect(QPoint(), size) : shared)}};
    if (!appId.isEmpty()) resolved.layout["appId"] = appId;
    resolved_ = resolved;
    enterPhase(HelperPhase::Ready);
    finishPrepare(std::move(reply));
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
    if (target_.kind == "window") {
        // The window's size when recording was asked for, at its output's scale
        // (the stream carries device pixels); later sizes are letterboxed into it.
        QScreen* screen = QGuiApplication::screenAt(resolved.rect.center());
        if (!screen) screen = QGuiApplication::primaryScreen();
        resolved.canvas = windowCanvas(resolved.rect.size(), source == "portal" ? 1.0 : screen ? screen->devicePixelRatio() : 1.0);
        if (resolved.canvas.isEmpty()) resolved.canvas = QSize(1280, 720);
        if (resolved.rect.isEmpty()) resolved.rect = QRect(QPoint(0, 0), resolved.canvas);
    }
    resolved.layout = QJsonObject{{"acquisition", source}, {"node", double(node)}, {"shared", rectJson(shared)}};
    if (target_.kind == "window") resolved.layout["window"] = QJsonObject{{"identity", target_.identity}, {"title", window_["title"].toString()}};
    resolved_ = resolved;
    enterPhase(HelperPhase::Ready);
    finishPrepare(reply);
}
void RecordSession::finishPrepare(Reply reply) {
    // A stream that opened after a cancel shows no ring and no bar.
    if (phase_ == HelperPhase::Cancelled) { releaseAcquisition(); reply({"cancelled", "Recording canceled before the source opened", {}}, {}); return; }
    if (!portalTarget_) {
        ResolvedTarget resolved;
        if (auto e = resolveX11Target(target_, resolved)) { reply(e, {}); return; }
        resolved_ = resolved;
        enterPhase(HelperPhase::Ready);
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
    options.microphoneAvailable = microphone_.available();
    options.microphoneLabel = microphone_.device.label;
    options.microphoneUnavailable = microphone_.message;
    options.clickHighlights = config_.clickHighlights;
    delete bar_;
    bar_ = new ControlBar(options, config_);
    // What the bar can record is what the session records: a remembered toggle
    // without a microphone prepares a recording without one.
    config_.audio.microphone = bar_->effectiveMicrophone();
    QPointer<RecordSession> guard(this);
    connect(bar_, &ControlBar::configChanged, this, [guard](const RecordingConfig& config) {
        if (!guard || (guard->phase_ != HelperPhase::Ready && guard->phase_ != HelperPhase::Starting)) return;
        const auto budget = guard->config_.budgetBytes;
        guard->config_ = config;
        guard->config_.budgetBytes = budget;
        guard->updateMicrophoneMonitor();
        QJsonObject changed{{"event", "configChanged"}, {"config", config.toJson()}};
        // What the user ticked is remembered for next time; what a command set is not.
        if (!guard->commandedAudio_) changed["microphoneRequested"] = guard->bar_->requestedMicrophone();
        emit guard->event(changed);
        emit guard->event({{"event", "progress"}, {"recordingAudio", guard->bar_->audioState()}, {"recordingMicrophone", guard->bar_->microphoneState()}});
    });
    connect(bar_, &ControlBar::recordRequested, this, [guard] {
        if (!guard || guard->phase_ != HelperPhase::Ready) return;
        guard->start([guard](Error error, QJsonObject status) {
            if (!guard) return;
            if (error) { emit guard->event({{"event", "failed"}, {"error", error.toJson()}}); return; }
            status["event"] = "started";
            emit guard->event(status);
        });
    });
    connect(bar_, &ControlBar::pauseRequested, this, [guard] { if (guard && guard->phase_ == HelperPhase::Recording && !guard->pipeline_.pause()) { guard->enterPhase(HelperPhase::Paused); guard->showBar("paused"); auto s = guard->pipeline_.status(); s["event"] = "paused"; emit guard->event(s); } });
    connect(bar_, &ControlBar::resumeRequested, this, [guard] { if (guard && guard->phase_ == HelperPhase::Paused && !guard->pipeline_.resume()) { guard->enterPhase(HelperPhase::Recording); guard->showBar("recording"); auto s = guard->pipeline_.status(); s["event"] = "resumed"; emit guard->event(s); } });
    connect(bar_, &ControlBar::stopRequested, this, [guard] {
        // A Stop while the clip is already being finished is not a second stop.
        if (!guard || (guard->phase_ != HelperPhase::Recording && guard->phase_ != HelperPhase::Paused)) return;
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
    updateMicrophoneMonitor();
    microphoneWatch_.start();
    emit event({{"event", "progress"}, {"recordingAudio", bar_->audioState()}, {"recordingMicrophone", bar_->microphoneState()}});
    reply({}, {{"canvas", sizeJson(resolved_.canvas)}, {"rect", rectJson(resolved_.rect)}, {"window", double(resolved_.window)}, {"overlayExcluded", overlayExcluded_}, {"overlayPlacement", overlayPlacement_},
               {"audio", config_.audio.toJson()}, {"microphone", bar_->microphoneState()}});
}
void RecordSession::start(Reply reply) {
    if (phase_ == HelperPhase::Starting) { reply({"recording_state", "Recording is already starting", {}}, {}); return; }
    if (phase_ != HelperPhase::Ready) { reply({"recording_state", "The session already recorded", {}}, {}); return; }
    if (!bar_ || !bar_->handleWhileRecording()) { begin(std::move(reply)); return; }
    // The ready bar sits inside the area: shrink it to its handle (a hidden bar
    // stays hidden) and give the X server or compositor a moment to repaint, so
    // the first frame never shows the full bar. KWin applies even a hide from
    // just before Record a frame later.
    enterPhase(HelperPhase::Starting);
    bar_->setState("recording");
    QPointer<RecordSession> guard(this);
    QTimer::singleShot(handleSettleMs, this, [guard, reply = std::move(reply)]() mutable {
        if (!guard) return;
        if (guard->phase_ == HelperPhase::Cancelled) { reply({"cancelled", "Recording canceled before it started", {}}, {}); return; }
        guard->begin(std::move(reply));
    });
}
void RecordSession::begin(Reply reply) {
    RecordingPipeline::Options options;
    options.rect = resolved_.rect;
    options.window = resolved_.window;
    if (imageCopy_) {
        options.imageCopy = imageCopy_.get(); options.rect = QRect(QPoint(), imageCopySize_); options.crop = portalCrop_;
        // 4:2:0 encoders refuse odd sizes: a window or output stream (often odd for a window) loses
        // its last odd column or row to the even canvas, as the region crop already does (review #1275).
        if (!options.crop.isValid() && resolved_.canvas != imageCopySize_) options.crop = QRect(QPoint(), resolved_.canvas);
    }
    else if (portalTarget_) { options.rect = resolved_.layout["shared"].isArray() ? QRect(resolved_.layout["shared"].toArray()[0].toInt(), resolved_.layout["shared"].toArray()[1].toInt(), resolved_.layout["shared"].toArray()[2].toInt(), resolved_.layout["shared"].toArray()[3].toInt()) : resolved_.rect; options.pipewireFd = portalFd_; options.pipewireNode = portalNode_; options.crop = portalCrop_; }
    if (portalTarget_ && !imageCopy_ && target_.kind == "window") {
        // A window stream changes size with the window (KWin, the portal) or
        // moves the window inside a larger buffer (Mutter): follow the crop,
        // keep the canvas, and survive a source that fails to renegotiate.
        options.canvas = resolved_.canvas;
        options.followCrop = true;
        options.reopenableSource = true;
    }
    options.fps = config_.fps;
    options.adaptive = config_.adaptiveFps;
    options.audio = config_.audio.system && !config_.silent();
    options.audioDevice = "@DEFAULT_MONITOR@";
    options.microphone = config_.audio.microphone && !config_.silent() && microphone_.available();
    options.microphoneDevice = microphone_.device.id;
    // The meter's own stream gives the device to the recording pipeline.
    monitor_.close();
    options.output = workingClip_;
    options.budgetBytes = config_.budgetBytes;
    options.encoders = available(videoEncoders);
    options.audioEncoders = available(audioEncoders);
    if (options.encoders.isEmpty()) { enterPhase(HelperPhase::Ready); showBar("ready"); reply({"recording_unsupported", "No H.264 encoder is installed", {}}, {}); return; }
    if (auto e = pipeline_.start(options)) { enterPhase(HelperPhase::Ready); showBar("ready"); updateMicrophoneMonitor(); reply(e, {}); return; }
    enterPhase(HelperPhase::Recording);
    // From here the device is the pipeline's; the bar no longer follows the sound server's inputs.
    microphoneWatch_.stop();
    if (options.microphone) levelTimer_.start();
    startClicks();
    if (pipeline_.microphoneLost() && bar_) bar_->setMicrophoneLost(pipeline_.microphoneLostReason());
    bounds_.setDimmed(true);
    showBar("recording");
    progress_.start();
    barTimer_.start();
    reply({}, pipeline_.status());
}
void RecordSession::stop(Reply reply) {
    if (!pipeline_.running()) { reply({"recording_state", "Nothing is recording", {}}, {}); return; }
    enterPhase(HelperPhase::Stopping);
    showBar("stopping");
    QPointer<RecordSession> guard(this);
    pipeline_.stop([guard, reply](Error error, qint64 durationUs) {
        if (!guard) return;
        guard->progress_.stop();
        guard->barTimer_.stop();
        guard->closeMicrophone();
        guard->bounds_.hideAll();
        if (guard->bar_) guard->bar_->hide();
        guard->releaseAcquisition();
        // A second Stop refused while the first drains leaves the capture running.
        if (error) { if (!guard->pipeline_.running()) guard->enterPhase(HelperPhase::Failed); reply(error, {}); return; }
        if (durationUs < 1 || !QFileInfo(guard->workingClip_).isFile()) { guard->enterPhase(HelperPhase::Failed); reply({"recording_failed", "The recording produced no retained frames", {}}, {}); return; }
        guard->durationUs_ = durationUs;
        guard->enterPhase(HelperPhase::Recorded);
        guard->noteCaptured();
        // Lab diagnostics only: keep a private copy of the working clip for inspection.
        if (const auto keep = qEnvironmentVariable("PUTGD_RECORD_KEEP_WORKING"); !keep.isEmpty()) QFile::copy(guard->workingClip_, keep + "/" + guard->id_ + ".mkv");
        guard->openReview();
        QJsonObject result{{"durationUs", double(durationUs)}, {"canvas", sizeJson(guard->resolved_.canvas)}, {"bytes", double(QFileInfo(guard->workingClip_).size())}, {"encoder", guard->pipeline_.encoder()}, {"audioEncoder", guard->pipeline_.audioEncoder()}, {"audioSources", guard->audioSources()}};
        if (guard->pipeline_.microphoneLost()) result["microphoneLost"] = guard->pipeline_.microphoneLostReason();
        reply({}, result);
    });
}
void RecordSession::openReview() {
    debugLog("review: open");
    if (bar_) bar_->hide();
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
    options.audioRecorded = audioCaptured_;
    options.systemRecorded = capturedSystem_;
    options.microphoneRecorded = capturedMicrophone_;
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
    connect(review_, &ReviewWindow::retryExportRequested, this, [guard] { if (guard) emit guard->event({{"event", "retryExportRequested"}}); });
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
    if (bar_) { status["recordingAudio"] = bar_->audioState(); status["recordingMicrophone"] = bar_->microphoneState(); }
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
    if (phase_ != HelperPhase::Recorded) { reply({"recording_state", "Nothing to preview before stop", {}}, {}); return; }
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
    if (!reviewing()) { reply({"recording_state", "Nothing to export before stop", {}}, {}); return; }
    if (exportThread_) { reply({"recording_busy", "An export is already running", {}}, {}); return; }
    ExportRequest request;
    request.input = workingClip_;
    request.output = params["output"].toString();
    request.durationUs = durationUs_;
    request.canvas = resolved_.canvas;
    auto configJson = params["config"].toObject();
    for (const auto* key : {"mime", "budgetBytes"}) configJson.remove(key);
    if (!parseEdit(params["edit"].toObject(), request.edit) || request.output.isEmpty() || recording::parseConfig(configJson, request.config)) { reply({"invalid_request", "Invalid export request", {}}, {}); return; }
    // The clip's one track holds whatever was captured; an export keeps it or drops it whole.
    const bool keep = effectiveAudio(request.config, audioCaptured_, request.config.audio.any());
    request.config.audio.system = keep && capturedSystem_;
    request.config.audio.microphone = keep && capturedMicrophone_;
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
    enterPhase(HelperPhase::Exporting);
    if (review_) review_->setExporting(true, QStringLiteral("Exporting %1…").arg(request.config.format.toUpper()));
    connect(thread, &QThread::finished, this, [this, thread, result, error, request, reply] {
        thread->deleteLater();
        if (exportThread_ == thread) exportThread_ = nullptr;
        exportCancel_.reset();
        if (phase_ == HelperPhase::Exporting) enterPhase(HelperPhase::Recorded);
        // A failure is an error in the review's status row, kept until dismissed.
        if (review_) review_->setExporting(false, {});
        if (review_ && *error) review_->showError(QStringLiteral("Export failed: %1").arg(error->message));
        if (*error) { QFile::remove(request.output); reply(*error, {}); return; }
        reply({}, {{"bytes", double(result->bytes)}, {"durationUs", double(result->durationUs)}, {"frames", result->frames}, {"videoEncoder", result->videoEncoder}, {"audioEncoder", result->audioEncoder}, {"size", sizeJson(result->size)}});
    });
    thread->start();
}
}
