#pragma once
#include <QColor>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace putgd::record {
// Click highlights (#1364): a ring that opens at the pointer on each button
// press and fades over 0.45 s, drawn into the clip's frames just before the
// encoder. Nothing is drawn on the screen. Clicks arrive on the main thread
// (the X server's raw button events, or the resident's `click` command with
// what the desktop reported); frames are drawn on the encoder's streaming
// thread. Positions are canvas pixels and times pipeline running time, before
// pauses are removed, so a click made while paused is never drawn.
class ClickOverlay {
public:
    static constexpr std::int64_t lifeNs = 450'000'000;
    enum Button { Left = 1, Middle = 2, Right = 3 };
    // The ring's largest radius in canvas pixels: 22 logical pixels at the recording's density.
    void setScale(double devicePixels) { scale_.store(devicePixels > 0.25 ? devicePixels : 1.0); }
    void add(QPointF canvasPoint, int button, std::int64_t runningNs);
    void clear();
    // True while a ring is visible at this time: such frames are drawn and kept
    // even where the recorder would otherwise skip a repeated frame.
    bool active(std::int64_t runningNs) const;
    // One 8-bit plane of the frame: `step` bytes between samples (2 for the
    // interleaved chroma of NV12), subsampled by `subX`/`subY` (1 for luma, 2 for 4:2:0 chroma).
    struct Plane { std::uint8_t* data; int stride, width, height, step, subX, subY; };
    // Draws every visible ring into a frame given as luma and two chroma planes
    // (I420: separate planes; NV12: both chroma planes are the interleaved one).
    void draw(const Plane& luma, const Plane& u, const Plane& v, std::int64_t runningNs) const;
    int drawn() const { return drawn_.load(); }
    int received() const { return received_.load(); }
    // Ring colour per button as limited-range BT.601 Y, U, V.
    static std::array<int, 3> colour(int button);
private:
    struct Click { QPointF at; int button; std::int64_t time; };
    mutable std::mutex mutex_;
    std::vector<Click> clicks_;
    std::atomic<std::int64_t> latest_{-lifeNs * 4};
    std::atomic<double> scale_{1.0};
    mutable std::atomic<int> drawn_{0};
    std::atomic<int> received_{0};
};

// Where a click in desktop coordinates falls in the recorded picture, or
// nothing when it is outside. `area` is what the frames show, in the same
// coordinates as the click (X11 root pixels, logical desktop pixels on
// Wayland, or the recorded window's own rectangle); `canvas` is the frame.
// A window smaller than the canvas after a resize sits centred with borders.
bool mapClick(QPointF desktop, const QRectF& area, const QSize& canvas, QPointF* out);
}
