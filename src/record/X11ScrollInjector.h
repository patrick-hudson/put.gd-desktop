#pragma once
#include "ScrollInjector.h"
#include <cstdint>
#include <mutex>
#include <vector>

struct xcb_connection_t;

namespace putgd::record {
// XTest on the helper's own X connection: absolute motion to a root position,
// and button 5 (down) or 4 (up) pressed and released once per notch. The event
// goes to the window under the pointer, which is why the pointer moves first.
// The pointer's place is read before the first move and restored at the end.
class X11ScrollInjector final : public ScrollInjector {
public:
    X11ScrollInjector();
    ~X11ScrollInjector() override;
    QString name() const override { return QStringLiteral("xtest"); }
    QString unavailableReason() const override { return reason_; }
    void setArea(const QRect& desktop, const QSize& frame) override;
    bool movePointer(QPoint streamPixel) override;
    bool scroll(int units) override;
    void restorePointer() override;
    bool userHasPointer() override;
    bool waitReady(const std::atomic<bool>& stop) override;
    // How long waitReady waits for the modifier keys.
    static constexpr int modifierWaitMs = 10000;
private:
    bool query(int* x, int* y, unsigned* mask);
    bool sync();
    // The hand's own events (#1515's acceptance on Cinnamon): XInput 2 raw
    // motion and button presses of every pointer but the XTEST one (the
    // capture's own). A hand that moved just before the capture's absolute
    // move leaves no trace in the pointer's place, but its raw event stays
    // queued. Under mutex_: whether one came since the last look; discarded
    // when the capture has not placed the pointer (nothing to tell apart yet).
    bool handMoved(bool discard);
    uint8_t inputOpcode_ = 0;
    std::vector<uint16_t> xtestDevices_;
    bool raw_ = false;
    xcb_connection_t* connection_ = nullptr;
    unsigned root_ = 0;
    QString reason_;
    std::mutex mutex_;
    QRect area_;
    bool saved_ = false, placed_ = false, taken_ = false;
    int savedX_ = 0, savedY_ = 0, placedX_ = 0, placedY_ = 0;
    // Under mutex_: the pointer is not where movePointer left it.
    bool movedByUser();
};
}
