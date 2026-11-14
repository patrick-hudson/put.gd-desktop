#include "X11ScrollInjector.h"
#include "DebugLog.h"
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <xcb/xcb.h>
#include <xcb/xtest.h>

namespace putgd::record {
X11ScrollInjector::X11ScrollInjector() {
    int screen = 0;
    connection_ = xcb_connect(nullptr, &screen);
    if (!connection_ || xcb_connection_has_error(connection_)) { reason_ = QStringLiteral("Cannot connect to the X11 display"); return; }
    auto roots = xcb_setup_roots_iterator(xcb_get_setup(connection_));
    for (int i = 0; i < screen && roots.rem; ++i) xcb_screen_next(&roots);
    if (!roots.data) { reason_ = QStringLiteral("The X11 display has no screen"); return; }
    root_ = roots.data->root;
    const auto* extension = xcb_get_extension_data(connection_, &xcb_test_id);
    if (!extension || !extension->present) { reason_ = QStringLiteral("The X server has no XTEST extension"); return; }
    std::unique_ptr<xcb_test_get_version_reply_t, decltype(&free)> version(xcb_test_get_version_reply(connection_, xcb_test_get_version(connection_, XCB_TEST_MAJOR_VERSION, XCB_TEST_MINOR_VERSION), nullptr), free);
    if (!version) reason_ = QStringLiteral("The X server did not answer for XTEST");
}
X11ScrollInjector::~X11ScrollInjector() {
    if (connection_) xcb_disconnect(connection_);
}
void X11ScrollInjector::setArea(const QRect& desktop, const QSize&) {
    std::lock_guard lock(mutex_);
    area_ = desktop;  // root pixels are stream pixels: ximagesrc reads the root
}
bool X11ScrollInjector::query(int* x, int* y, unsigned* mask) {
    std::unique_ptr<xcb_query_pointer_reply_t, decltype(&free)> reply(xcb_query_pointer_reply(connection_, xcb_query_pointer(connection_, root_), nullptr), free);
    if (!reply) return false;
    if (x) *x = reply->root_x;
    if (y) *y = reply->root_y;
    if (mask) *mask = reply->mask;
    return true;
}
// A round trip: the server has handled every event sent before it.
bool X11ScrollInjector::sync() {
    std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&free)> reply(xcb_get_input_focus_reply(connection_, xcb_get_input_focus(connection_), nullptr), free);
    return reply && !xcb_connection_has_error(connection_);
}
bool X11ScrollInjector::waitReady(const std::atomic<bool>& stop) {
    if (!reason_.isEmpty()) return false;
    std::lock_guard lock(mutex_);
    constexpr unsigned modifiers = XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(modifierWaitMs);
    for (;;) {
        unsigned mask = 0;
        if (!query(nullptr, nullptr, &mask)) return false;
        if (!(mask & modifiers)) return true;
        if (stop.load() || std::chrono::steady_clock::now() >= deadline) { debugLog(QStringLiteral("scroll xtest: modifier keys still down (mask %1)").arg(mask & modifiers)); return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}
bool X11ScrollInjector::movePointer(QPoint streamPixel) {
    if (!reason_.isEmpty()) return false;
    std::lock_guard lock(mutex_);
    if (!saved_) {
        // Where the user had the pointer, for restorePointer.
        if (!query(&savedX_, &savedY_, nullptr)) return false;
        saved_ = true;
    }
    const QPoint root = area_.topLeft() + streamPixel;
    // detail 0: absolute root coordinates.
    xcb_test_fake_input(connection_, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root_, int16_t(root.x()), int16_t(root.y()), 0);
    placed_ = true;
    taken_ = false;
    placedX_ = root.x();
    placedY_ = root.y();
    return sync();
}
bool X11ScrollInjector::movedByUser() {
    int x = 0, y = 0;
    return placed_ && query(&x, &y, nullptr) && (std::abs(x - placedX_) > 1 || std::abs(y - placedY_) > 1);
}
bool X11ScrollInjector::userHasPointer() {
    if (!reason_.isEmpty()) return false;
    std::lock_guard lock(mutex_);
    const bool moved = movedByUser();
    // The next motion is the capture's again (after Resume): measured from there.
    if (moved) { placed_ = false; taken_ = true; }
    return moved;
}
bool X11ScrollInjector::scroll(int units) {
    if (!reason_.isEmpty() || units == 0) return false;
    std::lock_guard lock(mutex_);
    const uint8_t button = units > 0 ? 5 : 4;  // 5 = wheel down, 4 = wheel up
    for (int i = 0; i < std::abs(units); ++i) {
        xcb_test_fake_input(connection_, XCB_BUTTON_PRESS, button, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        xcb_test_fake_input(connection_, XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
    }
    return sync();
}
void X11ScrollInjector::restorePointer() {
    if (!reason_.isEmpty()) return;
    std::lock_guard lock(mutex_);
    if (!saved_) return;
    saved_ = false;
    // The user took the pointer during the capture: it stays where they put it.
    if (taken_ || movedByUser()) return;
    xcb_test_fake_input(connection_, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root_, int16_t(savedX_), int16_t(savedY_), 0);
    sync();
}
}
