#pragma once
#include <QRect>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace putgd::record {
// The scroll capture's bar (#1376): what the capture is doing, the height of
// the image so far, Stop and Cancel. It carries the recording bar's window
// title so the GNOME Shell extension moves it the same way, and it is placed
// by the same rules (BarPlacement), but only ever outside the captured
// rectangle: with no spot outside, the session keeps it hidden while frames
// are read. It never takes the keyboard and has no in-frame handle.
class ScrollBar final : public QWidget {
    Q_OBJECT
public:
    explicit ScrollBar(QWidget* parent = nullptr);
    // Finds a spot outside `captured` (X11 root pixels, or logical desktop
    // pixels on Wayland) and moves there; false when there is none.
    bool place(const QRect& captured);
    bool outside() const { return outside_; }
    // ready | scrollingToTop | capturing | finishing
    void setState(const QString& state);
    QString state() const { return state_; }
    void setHeight(int rows);
    // The height as the bar writes it: "12,340 px".
    static QString heightText(int rows);
signals:
    void stopRequested();
    void cancelRequested();
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void refresh();
    QString state_ = QStringLiteral("ready");
    int rows_ = 0;
    bool outside_ = false;
    QLabel *dot_ = nullptr, *label_ = nullptr, *height_ = nullptr;
    QPushButton *stop_ = nullptr, *cancel_ = nullptr;
};
}
