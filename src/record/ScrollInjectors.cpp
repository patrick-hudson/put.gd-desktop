#include "ScrollInjectors.h"
#include "ImageCopySource.h"
#include "MediaProbe.h"
#include "MutterScreenCast.h"
#include "WaylandAcquisition.h"
#include "X11ScrollInjector.h"
#include "platform/NativeSession.h"
#ifdef PUTGD_RECORD_KDE_SCREENCAST
#include "KdeFakeInput.h"
#include "KdeScreencast.h"
#endif
#include <QProcessEnvironment>
#include <algorithm>
#include <gst/gst.h>

namespace putgd::record {
MutterScrollInjector::MutterScrollInjector(MutterScreenCast& cast) : cast_(cast) {
    QString why;
    if (!MutterScreenCast::remoteDesktopAvailable(&why)) reason_ = QStringLiteral("Mutter's RemoteDesktop service is not on the session bus (%1)").arg(why);
}
bool MutterScrollInjector::movePointer(QPoint streamPixel) { return cast_.pointerMotion(streamPixel.x(), streamPixel.y()); }
bool MutterScrollInjector::movePointerBy(QPointF logicalDelta) { return cast_.pointerMotionBy(logicalDelta.x(), logicalDelta.y()); }
bool MutterScrollInjector::scroll(int units) { return cast_.pointerAxis(units); }

WlrScrollInjector::WlrScrollInjector() {
    const auto facts = VirtualPointer::probe();
    if (!facts.available) reason_ = facts.reason;
}
void WlrScrollInjector::setOutput(const QString& name, const QSize& stream, const QRect& crop) {
    std::lock_guard lock(mutex_);
    output_ = name;
    stream_ = stream;
    crop_ = crop;
}
bool WlrScrollInjector::open() {
    if (pointer_.isOpen()) return true;
    QString why;
    if (pointer_.open(output_, &why)) return true;
    if (reason_.isEmpty()) reason_ = why;
    return false;
}
bool WlrScrollInjector::movePointer(QPoint streamPixel) {
    std::lock_guard lock(mutex_);
    if (stream_.isEmpty() || !open()) return false;
    const int x = std::clamp(crop_.x() + streamPixel.x(), 0, stream_.width() - 1), y = std::clamp(crop_.y() + streamPixel.y(), 0, stream_.height() - 1);
    return pointer_.moveTo(uint32_t(x), uint32_t(y), uint32_t(stream_.width()), uint32_t(stream_.height()));
}
bool WlrScrollInjector::movePointerBy(QPointF logicalDelta) {
    std::lock_guard lock(mutex_);
    return !stream_.isEmpty() && open() && pointer_.moveBy(logicalDelta.x(), logicalDelta.y());
}
bool WlrScrollInjector::scroll(int units) {
    std::lock_guard lock(mutex_);
    return open() && pointer_.wheel(units);
}

QJsonObject probeScroll() {
    const QString session = sessionType();
    QJsonObject result{{"schemaVersion", 1}, {"session", session}, {"maxHeight", scrollMaxHeight}};
    QString frames, injector, selection = QStringLiteral("none"), reason, message;
    auto refuse = [&](const char* why, const QString& text) { if (reason.isEmpty()) { reason = QString::fromLatin1(why); message = text; } };
    GError* error = nullptr;
    const bool gst = gst_init_check(nullptr, nullptr, &error);
    if (error) g_error_free(error);
    auto has = [gst](const char* name) {
        if (!gst) return false;
        GstElementFactory* factory = gst_element_factory_find(name);
        if (factory) gst_object_unref(factory);
        return factory != nullptr;
    };
    // The frame tap: <source> ! queue ! [videocrop] ! videoconvert ! appsink.
    const bool tap = has("queue") && has("videoconvert") && has("appsink");
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString desktop = env.value("XDG_CURRENT_DESKTOP").toLower();
    if (session == "x11") {
        frames = QStringLiteral("ximagesrc");
        injector = QStringLiteral("xtest");
        selection = QStringLiteral("helper");
        if (!tap || !has("ximagesrc")) refuse("frame_source_unavailable", QStringLiteral("GStreamer's ximagesrc, videoconvert and appsink are needed"));
        const X11ScrollInjector xtest;
        if (!xtest.unavailableReason().isEmpty()) refuse("injection_unavailable", xtest.unavailableReason());
    } else if (session == "wayland" && (budgieCoreSessionEnvironment() || hyprlandCoreSessionEnvironment())) {
        frames = QStringLiteral("image-copy");
        injector = QStringLiteral("wlr-virtual-pointer");
        const auto capture = ImageCopySource::probe();
        if (!capture.available) refuse("frame_source_unavailable", capture.reason);
        else if (!tap || !has("appsrc") || !has("videocrop")) refuse("frame_source_unavailable", QStringLiteral("GStreamer's appsrc, videocrop, videoconvert and appsink are needed"));
        const auto pointer = VirtualPointer::probe();
        if (!pointer.available) refuse("injection_unavailable", pointer.reason);
        else if (pointer.version < 2 && pointer.outputs > 1) refuse("injection_unavailable", QStringLiteral("zwlr_virtual_pointer_manager_v1 version 1 cannot map a pointer to one of several outputs"));
#ifdef PUTGD_RECORD_LAYER_SHELL
        selection = QStringLiteral("helper");
#endif
    } else if (session == "wayland" && desktop.contains("kde")) {
        frames = QStringLiteral("kde-screencast");
        injector = QStringLiteral("kwin-fake-input");
#ifdef PUTGD_RECORD_KDE_SCREENCAST
        QJsonObject facts;
        QString why;
        KdeScreencast cast;
        if (!cast.available(facts, &why)) refuse("frame_source_unavailable", QStringLiteral("KWin screen casting is unavailable: %1").arg(why));
        else if (pipewireSocket(env).isEmpty() || !tap || !has("pipewiresrc")) refuse("frame_source_unavailable", QStringLiteral("The PipeWire session and GStreamer's pipewiresrc are needed"));
        const KdeFakeInput fake;
        if (!fake.unavailableReason().isEmpty()) refuse("injection_unavailable", fake.unavailableReason());
#else
        refuse("frame_source_unavailable", QStringLiteral("This build has no KWin screen cast"));
#endif
#ifdef PUTGD_RECORD_LAYER_SHELL
        selection = QStringLiteral("helper");
#endif
    } else if (session == "wayland" && desktop.contains("gnome")) {
        frames = QStringLiteral("mutter-screencast");
        injector = QStringLiteral("mutter-remote-desktop");
        // The Shell extension selects the region; the resident sends the rectangle.
        selection = QStringLiteral("shell");
        QJsonObject facts;
        QString why;
        if (!MutterScreenCast::available(facts, &why)) refuse("frame_source_unavailable", QStringLiteral("Mutter's screen cast service is unavailable: %1").arg(why));
        else if (pipewireSocket(env).isEmpty() || !tap || !has("pipewiresrc")) refuse("frame_source_unavailable", QStringLiteral("The PipeWire session and GStreamer's pipewiresrc are needed"));
        if (!MutterScreenCast::remoteDesktopAvailable(&why)) refuse("injection_unavailable", QStringLiteral("Mutter's RemoteDesktop service is unavailable: %1").arg(why));
    } else {
        // The portal screen cast is never used for a scroll capture.
        refuse("frame_source_unavailable", session == "wayland" ? QStringLiteral("This desktop has no dialog-free frame stream") : QStringLiteral("No display session"));
    }
    // A build with no region selector has no layer-shell surfaces either: the
    // ring, the bar and the heartbeat would be ordinary windows the compositor
    // places, inside the rectangle among other places, and a capture of a
    // named rectangle was admitted there with the panel and the helper's own
    // windows in it (Ubuntu 24.04 package on KDE, acceptance of 2026-11-12).
    // So the capture is unavailable on such a build, for every caller.
    if (selection == "none") refuse("region_unavailable", QStringLiteral("This desktop has no region selector for a scroll capture"));
    result["frames"] = frames;
    result["injector"] = injector;
    result["selection"] = selection;
    result["available"] = reason.isEmpty();
    if (!reason.isEmpty()) { result["reason"] = reason; result["message"] = message; }
    return result;
}
}
