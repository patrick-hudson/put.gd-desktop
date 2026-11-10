#pragma once
#include <QObject>
#include <QPointF>
#include <QString>
#include <functional>

class QSocketNotifier;
struct xcb_connection_t;

namespace putgd::record {
// X11: pointer button presses from the X server's XInput2 raw events, selected
// on the root window over a connection of the helper's own. Raw events reach
// every client that selects them, whatever window the pointer is over and
// whoever holds a grab; they carry no position, so each press asks the server
// for the pointer. Nothing is grabbed and no event is consumed.
class X11ClickSource final : public QObject {
    Q_OBJECT
public:
    explicit X11ClickSource(QObject* parent = nullptr);
    ~X11ClickSource() override;
    // False when the X server has no XInput 2.1 (raw events for every client).
    static bool available(QString* reason = nullptr);
    // `window` (an XID, or 0) makes positions relative to that window.
    bool start(unsigned long window, std::function<void(QPointF position, int button)> press, QString* failure = nullptr);
    void stop();
private:
    void read();
    xcb_connection_t* connection_ = nullptr;
    QSocketNotifier* notifier_ = nullptr;
    unsigned root_ = 0;
    unsigned long window_ = 0;
    int opcode_ = 0;
    std::function<void(QPointF, int)> press_;
};
}
