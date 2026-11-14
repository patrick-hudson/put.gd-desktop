#pragma once
#include "ScrollInjector.h"
#include <memory>

namespace putgd::record {
// KWin's org_kde_kwin_fake_input on the helper's own Wayland connection:
// `authenticate` once (it shows nothing; every other request is ignored until
// then), absolute pointer motion in global logical coordinates, and one axis
// request of 15.0 per notch. KWin forwards that as a continuous delta with no
// discrete step, so an application may scroll 15 pixels or a notch for it; the
// capture loop measures what a unit moved. To the application that is a
// gesture without an end (a touchpad's fingers never lifted): a GTK 4 scrolled
// window fed wheel-up steps at its top stays overscrolled, its edge glow in
// the picture, and spends the next steps down on that. `rest` ends the
// gesture; it is sent on its own, well after the last step, because GTK
// ignores steps that arrive together with their stop (lab runs on Plasma
// 6.3.6, 2026-11-10: a page at 3,000 px did not move for two batches of ten).
// Through Plasma 6.7 KWin announces
// the global only to the executable of a desktop entry that lists it in
// X-KDE-Wayland-Interfaces (putgd-record.desktop, and the AppImage's per-mount
// entry); from 6.8 to every client outside a sandbox.
class KdeFakeInput final : public ScrollInjector {
public:
    KdeFakeInput();
    ~KdeFakeInput() override;
    QString name() const override { return QStringLiteral("kwin-fake-input"); }
    QString unavailableReason() const override;
    void setArea(const QRect& desktop, const QSize& frame) override;
    bool movePointer(QPoint streamPixel) override;
    bool movePointerBy(QPointF logicalDelta) override;
    bool scroll(int units) override;
    // An axis request of 0, which KWin forwards as wl_pointer.axis_stop.
    bool rest() override;
    // The protocol version KWin announced, 0 without the global.
    int version() const;
    // What one notch is sent as.
    static constexpr double notchValue = 15.0;
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
}
