#pragma once
#include "RegionSelection.h"
#include <QMouseEvent>
#include <QTimer>
#include <functional>
namespace putgd {
// Delayed capture (#1363): what the countdown says, on every desktop.
inline QString countdownText(int remaining) {
    return QStringLiteral("Capturing in %1 · Click to cancel").arg(remaining);
}
// The countdown pill: the region selector's pill, one line, counting down once
// a second. A click anywhere on it cancels. It takes no keyboard focus, so the
// window underneath keeps its hover and key state, and it is small enough to
// leave the rest of the screen to the pointer. Its owner decides the surface:
// an override-redirect window on X11, an overlay layer surface on Wayland.
// done(true) is called at expiry and done(false) on a click, once. The pill is
// still mapped then: its owner takes the surface down the way its window
// system needs (on Wayland a transparent frame first, see blank()) and reads no
// pixel until that has been presented.
class CountdownPill final : public QWidget {
public:
    explicit CountdownPill(int seconds, std::function<void(bool expired)> done, QWidget* parent = nullptr)
        : QWidget(parent), remaining_(seconds), done_(std::move(done)) {
        setObjectName(QStringLiteral("captureCountdown"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setFont(nativeRegionFont());
        describe();
        // Sized for the widest text it will show, so it never moves as it counts.
        const QFontMetrics metrics(font());
        QSize size;
        for (int value = 1; value <= std::max(seconds, 10); ++value) size = size.expandedTo(regionPillSize(metrics, countdownText(value)));
        setFixedSize(size);
        tick_.setInterval(1000);
        tick_.setTimerType(Qt::PreciseTimer);
        QObject::connect(&tick_, &QTimer::timeout, this, [this] {
            if (--remaining_ > 0) { describe(); update(); return; }
            finish(true);
        });
    }
    int remaining() const { return remaining_; }
    // Top centre of a display, where the selector's hint shows.
    static QPoint position(const QRect& display, const QSize& size) {
        return {display.x() + (display.width() - size.width()) / 2, display.y() + Ui::space(Ui::Space::l)};
    }
    void start() { show(); raise(); tick_.start(); }
    // Ends the countdown as a click does.
    void cancel() { finish(false); }
    // Draws nothing from now on and commits that frame at once. A compositor that
    // animates a closing surface starts from its last frame, so the last frame
    // must be empty before the surface is unmapped.
    void blank() { blank_ = true; repaint(); }
protected:
    void paintEvent(QPaintEvent*) override {
        if (blank_) return;
        QPainter painter(this);
        painter.setFont(font());
        paintRegionPill(painter, rect(), countdownText(remaining_));
    }
    void mousePressEvent(QMouseEvent* event) override { event->accept(); }
    void mouseReleaseEvent(QMouseEvent* event) override { event->accept(); if (rect().contains(event->position().toPoint())) finish(false); }
private:
    void describe() {
        setAccessibleName(QStringLiteral("Capture countdown"));
        setAccessibleDescription(countdownText(remaining_));
    }
    void finish(bool expired) {
        if (finished_) return;
        finished_ = true;
        tick_.stop();
        if (auto done = std::exchange(done_, {})) done(expired);
    }
    int remaining_;
    bool finished_ = false, blank_ = false;
    QTimer tick_;
    std::function<void(bool)> done_;
};
}
