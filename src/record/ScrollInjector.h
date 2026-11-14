#pragma once
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <atomic>

namespace putgd::record {
// How a scroll capture (#1376) turns the wheel and places the pointer on one
// desktop. Only wheel steps and pointer motion exist here: no buttons, no keys.
// Each implementation converts stream pixels of the captured rectangle to its
// own space: X11 root pixels, Mutter stream coordinates, KWin global logical
// coordinates, or a position within a wlroots output. scroll and movePointer
// are called from the capture loop's thread, everything else from the main one.
struct ScrollInjector {
    virtual ~ScrollInjector() = default;
    // "xtest" | "mutter-remote-desktop" | "kwin-fake-input" | "wlr-virtual-pointer"
    virtual QString name() const = 0;
    // Empty when usable; otherwise why not, for scroll_unavailable's details
    // (the reason there is injection_unavailable).
    virtual QString unavailableReason() const = 0;
    // The captured rectangle in desktop coordinates (X11 root pixels; logical
    // pixels on Wayland) and the size of its frames, known with the first frame.
    virtual void setArea(const QRect& desktop, const QSize& frame) = 0;
    // Put the pointer at a point given in stream pixels of the captured
    // rectangle; a point outside the frame lies outside the rectangle.
    virtual bool movePointer(QPoint streamPixel) = 0;
    // Positive scrolls the content down. One unit is one wheel notch where the
    // desktop has such a thing; the loop measures what a unit really moved.
    virtual bool scroll(int units) = 0;
    // The wheel has come to rest. Only where the desktop's wheel steps arrive
    // as a gesture that never ends otherwise (KWin's fake input); true when
    // something was sent.
    virtual bool rest() { return false; }
    // X11 only: put the pointer back where the user had it, unless the user
    // has it again (it is no longer where the capture last put it).
    virtual void restorePointer() {}
    // X11 only: the pointer is not where the capture last put it, so the user
    // has moved it (#1514). Elsewhere nobody can tell and it is false; the
    // helper's own surfaces see the user's motion there.
    virtual bool userHasPointer() { return false; }
    // X11 only: wait until Ctrl, Shift, Alt and Super are up (Ctrl plus wheel
    // zooms). False when they stayed down, or `stop` was set first.
    virtual bool waitReady(const std::atomic<bool>& stop) { Q_UNUSED(stop); return true; }
};
}
