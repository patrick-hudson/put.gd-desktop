#pragma once
#include "ScrollInjector.h"
#include "platform/wayland/VirtualPointer.h"
#include <QJsonObject>
#include <mutex>

namespace putgd::record {
class MutterScreenCast;
// GNOME: the RemoteDesktop session linked to the screen cast (MutterScreenCast::
// recordAreaLinked). Positions are stream pixels of the area stream, as Mutter takes them.
class MutterScrollInjector final : public ScrollInjector {
public:
    explicit MutterScrollInjector(MutterScreenCast& cast);
    QString name() const override { return QStringLiteral("mutter-remote-desktop"); }
    QString unavailableReason() const override { return reason_; }
    void setArea(const QRect&, const QSize&) override {}
    bool movePointer(QPoint streamPixel) override;
    bool movePointerBy(QPointF logicalDelta) override;
    bool scroll(int units) override;
private:
    MutterScreenCast& cast_;
    QString reason_;
};
// Budgie 10.10 (labwc) and Hyprland: a wlroots virtual pointer mapped to the
// captured output. The frames are a crop of that output's stream, so a stream
// pixel of the rectangle is the crop's corner plus that pixel, out of the
// output's stream size.
class WlrScrollInjector final : public ScrollInjector {
public:
    WlrScrollInjector();
    QString name() const override { return QStringLiteral("wlr-virtual-pointer"); }
    QString unavailableReason() const override { return reason_; }
    // The output's wl_output name, the size of its stream and the rectangle's crop in it.
    void setOutput(const QString& name, const QSize& stream, const QRect& crop);
    void setArea(const QRect&, const QSize&) override {}
    bool movePointer(QPoint streamPixel) override;
    bool movePointerBy(QPointF logicalDelta) override;
    bool scroll(int units) override;
private:
    bool open();
    VirtualPointer pointer_;
    QString reason_, output_;
    std::mutex mutex_;
    QSize stream_;
    QRect crop_;
};
// The tallest image a scroll capture may be asked for.
inline constexpr int scrollMaxHeight = 60000, scrollDefaultHeight = 30000, scrollMinEdge = 64;
// What a scroll capture would use on this desktop:
// {schemaVersion, session, available, reason?, message?, injector, frames,
//  selection: "helper"|"shell"|"none", maxHeight}. reason is one of
// injection_unavailable, frame_source_unavailable, region_unavailable.
// It opens no capture source, shows nothing and sends no input.
QJsonObject probeScroll();
}
