#pragma once
#include <QObject>
#include <functional>
#include <vector>

class QSocketNotifier;
struct xcb_connection_t;

namespace putgd::record {
// Escape on X11 while a scroll capture runs (#1376): the capture's bar never
// takes the keyboard, so the key is read as an XInput 2.1 raw key press on the
// root window, which reaches every listener whatever window has the focus and
// whatever grab is active. Nothing is grabbed and the key still goes where it
// was going. Wayland has no such passive key; there Stop is the bar, the
// shortcut or the session method.
class X11EscapeWatch final : public QObject {
    Q_OBJECT
public:
    explicit X11EscapeWatch(QObject* parent = nullptr);
    ~X11EscapeWatch() override;
    // False when the X server offers no raw key events; `pressed` then never runs.
    bool start(std::function<void()> pressed);
    void stop();
private:
    void read();
    xcb_connection_t* connection_ = nullptr;
    QSocketNotifier* notifier_ = nullptr;
    unsigned char opcode_ = 0;
    std::vector<unsigned> keycodes_;
    std::function<void()> pressed_;
};
}
