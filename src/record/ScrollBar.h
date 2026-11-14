#pragma once
#include <QRect>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace putgd::record {
// The scroll capture's bar (#1376): what the capture is doing, the height of
// the image so far, a line that says moving the mouse pauses it, and Resume
// (while paused), Stop and Cancel. It carries the recording bar's window
// title so the GNOME Shell extension moves it the same way, and it is placed
// beside the rectangle on its own screen where it can be (BarPlacement), but
// only ever outside the captured rectangle: with no spot outside, the session
// keeps it hidden while frames are read. It never takes the keyboard and has
// no in-frame handle. One size for every state, so Stop never moves (#1514).
class ScrollBar final : public QWidget {
    Q_OBJECT
public:
    explicit ScrollBar(QWidget* parent = nullptr);
    // Finds a spot outside `captured` (X11 root pixels, or logical desktop
    // pixels on Wayland) and moves there; false when there is none.
    bool place(const QRect& captured);
    bool outside() const { return outside_; }
    // ready | scrollingToTop | capturing | paused | resuming | finishing
    void setState(const QString& state);
    // paused, and why: "pointer" (the user moved the mouse) or "request" (scroll.pause).
    void setPaused(const QString& by);
    QString state() const { return state_; }
    void setHeight(int rows);
    // The height as the bar writes it: "12,340 px".
    static QString heightText(int rows);
signals:
    void resumeRequested();
    void stopRequested();
    void cancelRequested();
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void refresh();
    QString state_ = QStringLiteral("ready"), pausedBy_;
    int rows_ = 0;
    bool outside_ = false;
    QLabel *dot_ = nullptr, *label_ = nullptr, *height_ = nullptr, *hint_ = nullptr;
    QPushButton *resume_ = nullptr, *stop_ = nullptr, *cancel_ = nullptr;
};
}
