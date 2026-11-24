#include "KdeFakeInput.h"
#include "DebugLog.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <wayland-client.h>
#include "fake-input-client-protocol.h"

namespace putgd::record {
struct KdeFakeInput::Impl {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    org_kde_kwin_fake_input* fake = nullptr;
    uint32_t version = 0;
    bool authenticated = false;
    QString reason;
    std::mutex mutex;
    QRect area;
    QSize frame;
    static void global(void* data, wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
        auto* self = static_cast<Impl*>(data);
        if (std::strcmp(interface, org_kde_kwin_fake_input_interface.name) != 0 || self->fake) return;
        // Absolute motion is version 3; nothing newer is used.
        self->version = std::min(version, 4u);
        self->fake = static_cast<org_kde_kwin_fake_input*>(wl_registry_bind(registry, id, &org_kde_kwin_fake_input_interface, self->version));
    }
    static void removed(void*, wl_registry*, uint32_t) {}
    void connect() {
        display = wl_display_connect(nullptr);
        if (!display) { reason = QStringLiteral("Cannot connect to the Wayland display"); return; }
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener{global, removed};
        wl_registry_add_listener(registry, &listener, this);
        if (wl_display_roundtrip(display) < 0) { reason = QStringLiteral("The Wayland connection failed"); return; }
        if (!fake) { reason = QStringLiteral("KWin does not offer org_kde_kwin_fake_input to this program (through Plasma 6.7 it needs the putgd-record desktop entry)"); return; }
        if (version < 3) reason = QStringLiteral("KWin's org_kde_kwin_fake_input is too old for absolute pointer motion");
    }
    // The server has read everything sent so far, and the connection is alive.
    bool sync() { return wl_display_roundtrip(display) >= 0 && wl_display_get_error(display) == 0; }
    bool authenticate() {
        if (authenticated) return true;
        org_kde_kwin_fake_input_authenticate(fake, "put.gd", "Scrolling capture");
        authenticated = true;
        debugLog(QStringLiteral("scroll kwin: fake input version %1 authenticated").arg(version));
        return sync();
    }
    ~Impl() {
        // Bound below version 5, which added the destructor request.
        if (fake) wl_proxy_destroy(reinterpret_cast<wl_proxy*>(fake));
        if (registry) wl_registry_destroy(registry);
        if (display) { wl_display_flush(display); wl_display_disconnect(display); }
    }
};

KdeFakeInput::KdeFakeInput() : d_(std::make_unique<Impl>()) { d_->connect(); }
KdeFakeInput::~KdeFakeInput() = default;
QString KdeFakeInput::unavailableReason() const { return d_->reason; }
int KdeFakeInput::version() const { return int(d_->version); }
void KdeFakeInput::setArea(const QRect& desktop, const QSize& frame) {
    std::lock_guard lock(d_->mutex);
    d_->area = desktop;
    d_->frame = frame;
}
bool KdeFakeInput::movePointer(QPoint streamPixel) {
    if (!d_->reason.isEmpty()) return false;
    std::lock_guard lock(d_->mutex);
    if (d_->frame.isEmpty() || d_->area.isEmpty() || !d_->authenticate()) return false;
    // Stream pixels to global logical coordinates: the stream is the logical
    // rectangle at the output's scale.
    const double x = d_->area.x() + streamPixel.x() * double(d_->area.width()) / d_->frame.width();
    const double y = d_->area.y() + streamPixel.y() * double(d_->area.height()) / d_->frame.height();
    org_kde_kwin_fake_input_pointer_motion_absolute(d_->fake, wl_fixed_from_double(x), wl_fixed_from_double(y));
    return d_->sync();
}
bool KdeFakeInput::movePointerBy(QPointF logicalDelta) {
    if (!d_->reason.isEmpty()) return false;
    std::lock_guard lock(d_->mutex);
    if (!d_->authenticate()) return false;
    org_kde_kwin_fake_input_pointer_motion(d_->fake, wl_fixed_from_double(logicalDelta.x()), wl_fixed_from_double(logicalDelta.y()));
    return d_->sync();
}
bool KdeFakeInput::scroll(int units) {
    if (!d_->reason.isEmpty() || units == 0) return false;
    std::lock_guard lock(d_->mutex);
    if (!d_->authenticate()) return false;
    for (int i = 0; i < std::abs(units); ++i)
        org_kde_kwin_fake_input_axis(d_->fake, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(units > 0 ? notchValue : -notchValue));
    return d_->sync();
}
bool KdeFakeInput::rest() {
    if (!d_->reason.isEmpty()) return false;
    std::lock_guard lock(d_->mutex);
    if (!d_->authenticate()) return false;
    org_kde_kwin_fake_input_axis(d_->fake, WL_POINTER_AXIS_VERTICAL_SCROLL, 0);
    return d_->sync();
}
}
