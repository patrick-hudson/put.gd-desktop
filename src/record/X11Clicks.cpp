#include "XInputSetup.h"
#include "X11Clicks.h"
#include <QSocketNotifier>
#include <cstdlib>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

namespace putgd::record {
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
    xcb_window_t root = XCB_WINDOW_NONE;
    const auto reason = setupXInput(connection_, screenNumber, XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS, opcode_, root, QStringLiteral("the X server refused raw button events"));
    if (!reason.isEmpty()) return fail(reason);
    root_ = root;
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
