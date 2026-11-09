#include "ClickOverlay.h"
#include <QSocketNotifier>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

namespace putgd::record {
namespace {
// Coverage of a ring of radius r and stroke `width` at distance d from its
// centre, with a one-pixel soft edge.
double coverage(double d, double r, double width) {
    return std::clamp(width / 2.0 + 0.5 - std::abs(d - r), 0.0, 1.0);
}
void blend(std::uint8_t* sample, int value, double alpha) {
    if (alpha <= 0.0) return;
    *sample = std::uint8_t(std::lround(*sample + (value - *sample) * std::min(alpha, 1.0)));
}
}

std::array<int, 3> ClickOverlay::colour(int button) {
    // The app's amber accent for the main button, a blue of the same weight
    // for the secondary one and the interface's off-white for the middle one;
    // limited-range BT.601, the working clip's matrix.
    const QColor rgb = button == Right ? QColor(0x6d, 0xb6, 0xe8) : button == Middle ? QColor(0xf0, 0xf0, 0xec) : QColor(0xe8, 0xb6, 0x6d);
    const double r = rgb.red(), g = rgb.green(), b = rgb.blue();
    return {int(std::lround(16 + (65.481 * r + 128.553 * g + 24.966 * b) / 255.0)),
            int(std::lround(128 + (-37.797 * r - 74.203 * g + 112.0 * b) / 255.0)),
            int(std::lround(128 + (112.0 * r - 93.786 * g - 18.214 * b) / 255.0))};
}

void ClickOverlay::add(QPointF canvasPoint, int button, std::int64_t runningNs) {
    std::lock_guard lock(mutex_);
    received_.fetch_add(1);
    // Rings already faded make room; a burst of clicks keeps the newest 32.
    clicks_.erase(std::remove_if(clicks_.begin(), clicks_.end(), [runningNs](const Click& c) { return runningNs - c.time > 2 * lifeNs; }), clicks_.end());
    if (clicks_.size() >= 32) clicks_.erase(clicks_.begin());
    clicks_.push_back({canvasPoint, button, runningNs});
    latest_.store(runningNs);
}
void ClickOverlay::clear() {
    std::lock_guard lock(mutex_);
    clicks_.clear();
    received_.store(0);
    drawn_.store(0);
    latest_.store(-lifeNs * 4);
}
bool ClickOverlay::active(std::int64_t runningNs) const {
    const std::int64_t age = runningNs - latest_.load();
    return age >= 0 && age < lifeNs;
}
void ClickOverlay::draw(const Plane& luma, const Plane& u, const Plane& v, std::int64_t runningNs) const {
    std::vector<Click> clicks;
    { std::lock_guard lock(mutex_); clicks = clicks_; }
    const double scale = scale_.load();
    bool any = false;
    for (const auto& click : clicks) {
        const std::int64_t age = runningNs - click.time;
        if (age < 0 || age >= lifeNs) continue;
        any = true;
        const double progress = double(age) / double(lifeNs);
        // Opens fast and slows (ease-out), fading over the second half.
        const double eased = 1.0 - (1.0 - progress) * (1.0 - progress);
        const double radius = (7.0 + 15.0 * eased) * scale;
        const double width = std::max(2.0, 3.0 * scale);
        const double alpha = progress < 0.5 ? 1.0 : 2.0 * (1.0 - progress);
        const auto yuv = colour(click.button);
        const double reach = radius + width + 2.0;
        auto paint = [&](const Plane& plane, int value, int dark, bool outline) {
            const double sx = plane.subX, sy = plane.subY;
            const int x0 = std::max(0, int(std::floor((click.at.x() - reach) / sx))), x1 = std::min(plane.width - 1, int(std::ceil((click.at.x() + reach) / sx)));
            const int y0 = std::max(0, int(std::floor((click.at.y() - reach) / sy))), y1 = std::min(plane.height - 1, int(std::ceil((click.at.y() + reach) / sy)));
            for (int y = y0; y <= y1; ++y) {
                std::uint8_t* row = plane.data + std::ptrdiff_t(y) * plane.stride;
                const double dy = (y + 0.5) * sy - click.at.y();
                for (int x = x0; x <= x1; ++x) {
                    const double dx = (x + 0.5) * sx - click.at.x();
                    const double d = std::sqrt(dx * dx + dy * dy);
                    // A dark rim under the ring keeps it readable on light content.
                    if (outline) blend(row + std::ptrdiff_t(x) * plane.step, dark, 0.55 * alpha * coverage(d, radius, width + 2.0 * std::max(1.0, scale)));
                    blend(row + std::ptrdiff_t(x) * plane.step, value, alpha * coverage(d, radius, width));
                }
            }
        };
        paint(luma, yuv[0], 24, true);
        paint(u, yuv[1], 128, false);
        paint(v, yuv[2], 128, false);
    }
    if (any) drawn_.fetch_add(1);
}

bool mapClick(QPointF desktop, const QRectF& area, const QSize& canvas, QPointF* out) {
    if (area.width() < 1 || area.height() < 1 || canvas.isEmpty() || !area.contains(desktop)) return false;
    // The picture keeps its aspect ratio inside the canvas (borders after a window resize).
    const double scale = std::min(canvas.width() / area.width(), canvas.height() / area.height());
    const double left = (canvas.width() - area.width() * scale) / 2.0, top = (canvas.height() - area.height() * scale) / 2.0;
    if (out) *out = QPointF(left + (desktop.x() - area.x()) * scale, top + (desktop.y() - area.y()) * scale);
    return true;
}

X11ClickSource::X11ClickSource(QObject* parent) : QObject(parent) {}
X11ClickSource::~X11ClickSource() { stop(); }
bool X11ClickSource::available(QString* reason) {
    int screen = 0;
    xcb_connection_t* connection = xcb_connect(nullptr, &screen);
    if (!connection || xcb_connection_has_error(connection)) { if (connection) xcb_disconnect(connection); if (reason) *reason = QStringLiteral("no X server connection"); return false; }
    bool ok = false;
    if (const auto* extension = xcb_get_extension_data(connection, &xcb_input_id); extension && extension->present) {
        if (auto* version = xcb_input_xi_query_version_reply(connection, xcb_input_xi_query_version(connection, 2, 1), nullptr)) {
            ok = version->major_version > 2 || (version->major_version == 2 && version->minor_version >= 1);
            std::free(version);
        }
    }
    xcb_disconnect(connection);
    if (!ok && reason) *reason = QStringLiteral("the X server has no XInput 2.1");
    return ok;
}
bool X11ClickSource::start(unsigned long window, std::function<void(QPointF, int)> press, QString* failure) {
    stop();
    int screenNumber = 0;
    connection_ = xcb_connect(nullptr, &screenNumber);
    auto fail = [&](const QString& why) { if (failure) *failure = why; stop(); return false; };
    if (!connection_ || xcb_connection_has_error(connection_)) return fail(QStringLiteral("no X server connection"));
    const auto* extension = xcb_get_extension_data(connection_, &xcb_input_id);
    if (!extension || !extension->present) return fail(QStringLiteral("the X server has no XInput extension"));
    opcode_ = extension->major_opcode;
    auto* version = xcb_input_xi_query_version_reply(connection_, xcb_input_xi_query_version(connection_, 2, 1), nullptr);
    const bool raw = version && (version->major_version > 2 || (version->major_version == 2 && version->minor_version >= 1));
    std::free(version);
    if (!raw) return fail(QStringLiteral("the X server has no XInput 2.1"));
    auto iterator = xcb_setup_roots_iterator(xcb_get_setup(connection_));
    for (int i = 0; i < screenNumber && iterator.rem; ++i) xcb_screen_next(&iterator);
    if (!iterator.data) return fail(QStringLiteral("no X screen"));
    root_ = iterator.data->root;
    struct { xcb_input_event_mask_t head; std::uint32_t mask; } select{};
    select.head.deviceid = XCB_INPUT_DEVICE_ALL_MASTER;
    select.head.mask_len = 1;
    select.mask = XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS;
    auto* error = xcb_request_check(connection_, xcb_input_xi_select_events_checked(connection_, root_, 1, &select.head));
    if (error) { std::free(error); return fail(QStringLiteral("the X server refused raw button events")); }
    xcb_flush(connection_);
    window_ = window;
    press_ = std::move(press);
    notifier_ = new QSocketNotifier(xcb_get_file_descriptor(connection_), QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this] { read(); });
    return true;
}
void X11ClickSource::stop() {
    delete notifier_;
    notifier_ = nullptr;
    if (connection_) xcb_disconnect(connection_);
    connection_ = nullptr;
    press_ = {};
}
void X11ClickSource::read() {
    if (!connection_) return;
    while (xcb_generic_event_t* event = xcb_poll_for_event(connection_)) {
        const auto* generic = reinterpret_cast<xcb_ge_generic_event_t*>(event);
        if ((event->response_type & 0x7f) == XCB_GE_GENERIC && generic->extension == opcode_ && generic->event_type == XCB_INPUT_RAW_BUTTON_PRESS) {
            const auto* raw = reinterpret_cast<xcb_input_raw_button_press_event_t*>(event);
            // Buttons 1 to 3 are clicks; 4 to 7 are the wheel.
            if (raw->detail >= 1 && raw->detail <= 3) {
                QPointF at;
                bool known = false;
                if (auto* pointer = xcb_query_pointer_reply(connection_, xcb_query_pointer(connection_, root_), nullptr)) {
                    at = QPointF(pointer->root_x, pointer->root_y);
                    known = true;
                    std::free(pointer);
                }
                if (known && window_) {
                    // A window recording shows the window's own pixels: its place on the root at this moment.
                    known = false;
                    if (auto* translated = xcb_translate_coordinates_reply(connection_, xcb_translate_coordinates(connection_, root_, xcb_window_t(window_), std::int16_t(at.x()), std::int16_t(at.y())), nullptr)) {
                        at = QPointF(translated->dst_x, translated->dst_y);
                        known = true;
                        std::free(translated);
                    }
                }
                if (known && press_) press_(at, int(raw->detail));
            }
        }
        std::free(event);
    }
    if (xcb_connection_has_error(connection_)) stop();
}
}
