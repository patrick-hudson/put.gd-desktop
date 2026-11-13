#include "XInputSetup.h"
#include "X11EscapeWatch.h"
#include <QSocketNotifier>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xinput.h>

namespace putgd::record {
namespace {
constexpr xcb_keysym_t escapeKeysym = 0xff1b;  // XK_Escape
}
X11EscapeWatch::X11EscapeWatch(QObject* parent) : QObject(parent) {}
X11EscapeWatch::~X11EscapeWatch() { stop(); }
bool X11EscapeWatch::start(std::function<void()> pressed) {
    stop();
    int screenNumber = 0;
    connection_ = xcb_connect(nullptr, &screenNumber);
    auto fail = [&] { stop(); return false; };
    xcb_window_t root = XCB_WINDOW_NONE;
    const auto reason = setupXInput(connection_, screenNumber, XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS, opcode_, root, QStringLiteral("the X server refused raw key events"));
    if (!reason.isEmpty()) return fail();
    // Every keycode the keyboard map gives Escape.
    if (xcb_key_symbols_t* symbols = xcb_key_symbols_alloc(connection_)) {
        if (xcb_keycode_t* codes = xcb_key_symbols_get_keycode(symbols, escapeKeysym)) {
            for (const xcb_keycode_t* code = codes; *code != XCB_NO_SYMBOL; ++code) keycodes_.push_back(*code);
            std::free(codes);
        }
        xcb_key_symbols_free(symbols);
    }
    if (keycodes_.empty()) return fail();
    pressed_ = std::move(pressed);
    notifier_ = new QSocketNotifier(xcb_get_file_descriptor(connection_), QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this] { read(); });
    return true;
}
void X11EscapeWatch::stop() {
    delete notifier_;
    notifier_ = nullptr;
    if (connection_) xcb_disconnect(connection_);
    connection_ = nullptr;
    keycodes_.clear();
    pressed_ = {};
}
void X11EscapeWatch::read() {
    if (!connection_) return;
    bool escape = false;
    while (xcb_generic_event_t* event = xcb_poll_for_event(connection_)) {
        const auto* generic = reinterpret_cast<xcb_ge_generic_event_t*>(event);
        if ((event->response_type & 0x7f) == XCB_GE_GENERIC && generic->extension == opcode_ && generic->event_type == XCB_INPUT_RAW_KEY_PRESS) {
            const auto detail = reinterpret_cast<xcb_input_raw_key_press_event_t*>(event)->detail;
            escape = escape || std::find(keycodes_.begin(), keycodes_.end(), unsigned(detail)) != keycodes_.end();
        }
        std::free(event);
    }
    if (xcb_connection_has_error(connection_)) { stop(); return; }
    // Last: the handler may stop this watch.
    if (escape && pressed_) { const auto pressed = pressed_; pressed(); }
}
}
